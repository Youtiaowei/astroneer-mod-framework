#include "ue_engine.hpp"
// Load the SDK's fmt dependency before Unreal enables its engine-only warnings.
#include <fmt/chrono.h>
#include <fmt/xchar.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <windows.h>
namespace astro {
namespace U = RC::Unreal;
namespace {
std::wstring wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              int(s.size()), nullptr, 0);
  if (!n)
    throw std::runtime_error("Invalid UTF-8");
  std::wstring r(n, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()),
                      r.data(), n);
  return r;
}
std::string narrow(std::wstring_view s) {
  if (s.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                              int(s.size()), nullptr, 0, nullptr, nullptr);
  if (!n)
    throw std::runtime_error("Invalid engine UTF-16");
  std::string r(n, 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()),
                      r.data(), n, nullptr, nullptr);
  return r;
}
std::vector<U::FProperty *> parameters(U::UFunction *fn) {
  std::vector<U::FProperty *> out;
  for (auto p : fn->ForEachProperty())
    if (p->HasAnyPropertyFlags(U::CPF_Parm))
      out.push_back(p);
  std::stable_sort(out.begin(), out.end(), [](auto a, auto b) {
    return a->GetOffset_Internal() < b->GetOffset_Internal();
  });
  return out;
}
struct Buffer {
  U::FProperty *prop;
  void *data;
  explicit Buffer(U::FProperty *p) : prop(p), data(nullptr) {
    if (p->GetSize() < 0 || p->GetSize() > 1024 * 1024)
      throw std::runtime_error("Reflected property size exceeds bound");
    data = p->AllocateAndInitializeValue();
    if (!data)
      throw std::bad_alloc();
  }
  ~Buffer() { prop->DestroyAndFreeValue(data); }
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
};
struct NativeValue final : Value::Native {
  Buffer storage;
  uint64_t epoch;
  NativeValue(U::FProperty *p, void *source, uint64_t generation)
      : storage(p), epoch(generation) {
    p->CopyCompleteValue(storage.data, source);
  }
};
struct Params {
  void *data = nullptr;
  std::vector<U::FProperty *> props, initialized;
  explicit Params(U::UFunction *fn) : props(parameters(fn)) {
    auto bytes = fn->GetParmsSize();
    if (bytes > 1024 * 1024)
      throw std::runtime_error("Function parameters exceed bound");
    auto alignment = std::max<int>(16, fn->GetMinAlignment());
    data = U::FMemory::Malloc(std::max<int>(bytes, 1), alignment);
    if (!data)
      throw std::bad_alloc();
    std::memset(data, 0, bytes);
    try {
      for (auto p : props) {
        if (p->GetOffset_Internal() < 0 ||
            p->GetOffset_Internal() + p->GetSize() > bytes)
          throw std::runtime_error("Invalid reflected parameter bounds");
        p->InitializeValue_InContainer(data);
        initialized.push_back(p);
      }
    } catch (const std::exception &) {
      destroy();
      throw;
    }
  }
  void destroy() {
    for (auto it = initialized.rbegin(); it != initialized.rend(); ++it)
      (*it)->DestroyValue((*it)->ContainerPtrToValuePtr<void>(data));
    initialized.clear();
    U::FMemory::Free(data);
    data = nullptr;
  }
  ~Params() { destroy(); }
  Params(const Params &) = delete;
};
} // namespace
struct UEEngine::Impl {
  UEEngine &owner;
  DWORD thread = 0;
  uint64_t generation = 1, next_hook = 1;
  std::string error;
  mutable std::mutex error_mutex;
  struct Gate {
    std::recursive_mutex mutex;
    bool alive = true;
  };
  struct BoundHook {
    U::FWeakObjectPtr function;
    std::pair<int, int> ids;
    std::shared_ptr<Gate> alive;
    std::exception_ptr unregister_failure;
  };
  std::map<HookId, BoundHook> hooks;
  explicit Impl(UEEngine &o) : owner(o) {}
  void guard() const {
    if (!thread || GetCurrentThreadId() != thread)
      throw std::runtime_error("Engine access outside native game thread");
  }
  U::UObject *resolve(Object o) const {
    guard();
    if (o.epoch != generation || !o)
      throw std::runtime_error("Expired object epoch");
    U::FWeakObjectPtr weak;
    weak.ObjectIndex = o.index;
    weak.ObjectSerialNumber = o.serial;
    auto p = weak.Get();
    if (!p)
      throw std::runtime_error("Expired weak object");
    return p;
  }
  std::pair<U::FProperty *, void *> locate(Object o, const std::string &path) {
    auto object = resolve(o);
    U::UStruct *type = object->GetClassPrivate();
    void *base = object;
    size_t cursor = 0;
    U::FProperty *prop = nullptr;
    for (int depth = 0; depth < 32 && cursor < path.size(); ++depth) {
      auto end = path.find_first_of(".[", cursor);
      auto name = wide(path.substr(cursor, end - cursor));
      prop = nullptr;
      for (auto p : type->ForEachPropertyInChain())
        if (p->GetName() == name) {
          prop = p;
          break;
        }
      if (!prop)
        throw std::runtime_error("Missing reflected property: " + path);
      base = prop->ContainerPtrToValuePtr<void>(base);
      cursor = end;
      if (cursor == std::string::npos)
        return {prop, base};
      if (path[cursor] == '[') {
        auto close = path.find(']', cursor);
        if (close == std::string::npos)
          throw std::runtime_error("Malformed array path");
        auto index_text = path.substr(cursor + 1, close - cursor - 1);
        if (index_text.empty() ||
            index_text.find_first_not_of("0123456789") != std::string::npos)
          throw std::runtime_error("Invalid array index");
        auto index = std::stoul(index_text);
        auto array = U::CastField<U::FArrayProperty>(prop);
        if (!array)
          throw std::runtime_error("Property is not an array");
        U::FScriptArrayHelper helper(array, base);
        if (index >= size_t(helper.Num()))
          throw std::runtime_error("Reflected array index out of range");
        base = helper.GetRawPtr(int(index));
        prop = array->GetInner();
        cursor = close + 1;
        if (cursor == path.size())
          return {prop, base};
      }
      if (path[cursor] != '.')
        throw std::runtime_error("Malformed field path");
      ++cursor;
      if (auto structure = U::CastField<U::FStructProperty>(prop))
        type = structure->GetStruct().Get();
      else if (auto object_property =
                   U::CastField<U::FObjectPropertyBase>(prop)) {
        auto child = object_property->GetObjectPropertyValue(base);
        auto ref = owner.reference(child);
        base = resolve(ref);
        type = child->GetClassPrivate();
      } else
        throw std::runtime_error("Field path does not name a struct/object");
    }
    throw std::runtime_error("Field path exceeds depth bound");
  }
  std::pair<U::UObject *, U::UFunction *> function(Object object,
                                                   const std::string &name) {
    auto p = resolve(object);
    auto f = p->GetFunctionByNameInChain(wide(name).c_str());
    if (!f)
      throw std::runtime_error("Missing reflected function: " + name);
    return {p, f};
  }
  Value text_read(U::FProperty *prop, void *source) {
    auto lib = owner.find("/Script/Engine.Default__KismetTextLibrary");
    auto [obj, fn] = function(lib, "Conv_TextToString");
    Params params(fn);
    U::FProperty *result = nullptr;
    bool copied = false;
    for (auto p : params.props) {
      if (p->HasAnyPropertyFlags(U::CPF_ReturnParm)) {
        result = p;
        continue;
      }
      if (p->IsA<U::FTextProperty>()) {
        p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(params.data),
                             source);
        copied = true;
      }
    }
    if (!copied || !result || !result->IsA<U::FStrProperty>())
      throw std::runtime_error("Unexpected native text conversion signature");
    obj->ProcessEvent(fn, params.data);
    return read(result, result->ContainerPtrToValuePtr<void>(params.data), 0);
  }
  void text_write(U::FProperty *prop, void *dest, const std::string &text) {
    auto lib = owner.find("/Script/Engine.Default__KismetTextLibrary");
    auto [obj, fn] = function(lib, "Conv_StringToText");
    Params params(fn);
    U::FProperty *result = nullptr;
    bool supplied = false;
    for (auto p : params.props) {
      if (p->HasAnyPropertyFlags(U::CPF_ReturnParm)) {
        result = p;
        continue;
      }
      if (p->IsA<U::FStrProperty>()) {
        write(p, p->ContainerPtrToValuePtr<void>(params.data), Value{text}, 0);
        supplied = true;
      }
    }
    if (!supplied || !result || !result->IsA<U::FTextProperty>())
      throw std::runtime_error("Unexpected native text conversion signature");
    obj->ProcessEvent(fn, params.data);
    prop->CopyCompleteValue(dest,
                            result->ContainerPtrToValuePtr<void>(params.data));
  }
  Value read(U::FProperty *p, void *data, int depth) {
    if (depth > 24)
      throw std::runtime_error("Reflected value exceeds depth bound");
    if (auto b = U::CastField<U::FBoolProperty>(p))
      return b->GetPropertyValue(data);
    if (auto n = U::CastField<U::FNumericProperty>(p))
      return n->IsFloatingPoint()
                 ? Value(n->GetFloatingPointPropertyValue(data))
                 : Value(int64_t(n->GetSignedIntPropertyValue(data)));
    if (auto n = U::CastField<U::FEnumProperty>(p))
      return Value(
          int64_t(n->GetUnderlyingProperty()->GetSignedIntPropertyValue(data)));
    if (auto n = U::CastField<U::FNameProperty>(p))
      return narrow(n->GetPropertyValue(data).ToString());
    if (auto s = U::CastField<U::FStrProperty>(p)) {
      auto &str = s->GetPropertyValue(data);
      if (str.Len() > 1024 * 1024)
        throw std::runtime_error("Engine string exceeds bound");
      return narrow(std::wstring_view(*str, str.Len()));
    }
    if (p->IsA<U::FTextProperty>())
      return text_read(p, data);
    if (auto o = U::CastField<U::FObjectPropertyBase>(p)) {
      if (p->IsA<U::FSoftObjectProperty>() ||
          p->IsA<U::FLazyObjectProperty>()) {
        Value v;
        v.native = std::make_shared<NativeValue>(p, data, generation);
        return v;
      }
      return owner.reference(o->GetObjectPropertyValue(data));
    }
    if (auto s = U::CastField<U::FStructProperty>(p)) {
      Value::Record fields;
      size_t count = 0;
      for (auto f : s->GetStruct()->ForEachPropertyInChain()) {
        if (++count > 512)
          throw std::runtime_error("Struct exceeds field bound");
        fields.emplace(
            narrow(f->GetName()),
            read(f, f->ContainerPtrToValuePtr<void>(data), depth + 1));
      }
      Value value(fields);
      value.native = std::make_shared<NativeValue>(p, data, generation);
      return value;
    }
    if (auto a = U::CastField<U::FArrayProperty>(p)) {
      U::FScriptArrayHelper array(a, data);
      if (array.Num() < 0 || array.Num() > 10000)
        throw std::runtime_error("Array exceeds bound");
      Value::Array values;
      values.reserve(array.Num());
      for (int i = 0; i < array.Num(); ++i)
        values.push_back(read(a->GetInner(), array.GetRawPtr(i), depth + 1));
      return values;
    }
    // Opaque values can only be passed unchanged to the identical reflected
    // type.
    Value value;
    value.native = std::make_shared<NativeValue>(p, data, generation);
    return value;
  }
  void write(U::FProperty *p, void *data, const Value &v, int depth) {
    if (depth > 24)
      throw std::runtime_error("Reflected input exceeds depth bound");
    if (v.native) {
      auto native = std::dynamic_pointer_cast<NativeValue>(v.native);
      if (!native || native->epoch != generation ||
          !p->SameType(native->storage.prop))
        throw std::runtime_error("Mismatched or expired native value");
      p->CopyCompleteValue(data, native->storage.data);
      if (!p->IsA<U::FStructProperty>() ||
          !std::holds_alternative<Value::Record>(v.data))
        return;
    }
    if (v.null())
      return; // omitted/out parameters keep their initialized native default
    if (auto b = U::CastField<U::FBoolProperty>(p)) {
      b->SetPropertyValue(data, v.boolean());
      return;
    }
    auto numeric = [&](U::FNumericProperty *n) {
      double number = v.number();
      if (!std::isfinite(number))
        throw std::runtime_error("Nonfinite native argument");
      if (n->IsFloatingPoint())
        n->SetFloatingPointPropertyValue(data, number);
      else {
        if (std::floor(number) != number || number < -9007199254740991.0 ||
            number > 9007199254740991.0)
          throw std::runtime_error("Invalid integer native argument");
        n->SetIntPropertyValue(data, U::int64(number));
      }
    };
    if (auto n = U::CastField<U::FNumericProperty>(p)) {
      numeric(n);
      return;
    }
    if (auto n = U::CastField<U::FEnumProperty>(p)) {
      numeric(n->GetUnderlyingProperty());
      return;
    }
    if (auto n = U::CastField<U::FNameProperty>(p)) {
      U::FName value(wide(v.string()));
      p->CopyCompleteValue(data, &value);
      return;
    }
    if (p->IsA<U::FStrProperty>()) {
      U::FString text(wide(v.string()));
      p->CopyCompleteValue(data, &text);
      return;
    }
    if (p->IsA<U::FTextProperty>()) {
      text_write(p, data, v.string());
      return;
    }
    if (auto o = U::CastField<U::FObjectPropertyBase>(p)) {
      auto ref = v.object();
      auto object = ref ? resolve(ref) : nullptr;
      if (object && !object->IsA(o->GetPropertyClass().Get()))
        throw std::runtime_error("Incompatible UObject argument");
      o->SetObjectPropertyValue(data, object);
      return;
    }
    if (auto s = U::CastField<U::FStructProperty>(p)) {
      for (auto &[key, value] : v.record()) {
        U::FProperty *field = nullptr;
        for (auto f : s->GetStruct()->ForEachPropertyInChain())
          if (narrow(f->GetName()) == key) {
            field = f;
            break;
          }
        if (!field)
          throw std::runtime_error("Unknown native struct field: " + key);
        write(field, field->ContainerPtrToValuePtr<void>(data), value,
              depth + 1);
      }
      return;
    }
    if (auto a = U::CastField<U::FArrayProperty>(p)) {
      if (v.array().size() > 10000)
        throw std::runtime_error("Native array input exceeds bound");
      U::FScriptArrayHelper array(a, data);
      if (size_t(array.Num()) != v.array().size())
        throw std::runtime_error(
            "Array length changes are not part of the authored runtime");
      for (size_t i = 0; i < v.array().size(); ++i)
        write(a->GetInner(), array.GetRawPtr(int(i)), v.array()[i], depth + 1);
      return;
    }
    throw std::runtime_error("Unsupported reflected write type: " +
                             narrow(p->GetName()));
  }
};
UEEngine::UEEngine() : impl(std::make_unique<Impl>(*this)) {}
UEEngine::~UEEngine() {
  close_callbacks();
  if (is_game_thread()) {
    // Failed registrations remain recorded. Visit each once rather than looping
    // forever on the first retained entry or aborting other hooks' cleanup.
    for (auto it = impl->hooks.begin(); it != impl->hooks.end();) {
      const auto id = (it++)->first;
      try { unhook(id); }
      catch (...) {
        OutputDebugStringA("[AstroConsoleCore] Inert hook unregister failed; registration retained.\n");
      }
    }
  } else if (!impl->hooks.empty())
    OutputDebugStringA("[AstroConsoleCore] Off-thread shutdown: inert script "
                       "hooks retained until process exit.\n");
}
bool UEEngine::is_game_thread() const {
  return impl->thread && GetCurrentThreadId() == impl->thread;
}
void UEEngine::close_callbacks() {
  for (auto &[id, hook] : impl->hooks) {
    std::lock_guard lock(hook.alive->mutex);
    hook.alive->alive = false;
  }
}
void UEEngine::enter_game_thread() {
  if (!impl->thread)
    impl->thread = GetCurrentThreadId();
  impl->guard();
}
Object UEEngine::reference(U::UObject *p) {
  impl->guard();
  if (!p)
    return {};
  U::FWeakObjectPtr weak(p);
  return {weak.ObjectIndex, weak.ObjectSerialNumber, impl->generation};
}
bool UEEngine::valid(Object o) const {
  impl->guard();
  if (!o || o.epoch != impl->generation)
    return false;
  U::FWeakObjectPtr weak;
  weak.ObjectIndex = o.index;
  weak.ObjectSerialNumber = o.serial;
  return weak.Get() != nullptr;
}
std::string UEEngine::name(Object o) {
  return narrow(impl->resolve(o)->GetFullName());
}
Object UEEngine::find(const std::string &path) {
  impl->guard();
  return reference(U::UObjectGlobals::StaticFindObject(nullptr, nullptr,
                                                       wide(path).c_str()));
}
std::vector<Object> UEEngine::all(const std::string &name) {
  impl->guard();
  std::vector<U::UObject *> raw;
  U::UObjectGlobals::FindAllOf(wide(name), raw);
  if (raw.size() > 100000)
    throw std::runtime_error("Object discovery exceeds bound");
  std::vector<Object> result;
  for (auto p : raw) {
    auto r = reference(p);
    if (valid(r))
      result.push_back(r);
  }
  return result;
}
Object UEEngine::construct(const std::string &path, Object outer,
                           const std::string &name) {
  auto cls = U::Cast<U::UClass>(impl->resolve(find(path)));
  if (!cls)
    throw std::runtime_error("Construction target is not a UClass");
  U::FStaticConstructObjectParameters params(cls, impl->resolve(outer));
  params.Name = U::FName(wide(name));
  return reference(U::UObjectGlobals::StaticConstructObject(params));
}
bool UEEngine::has(Object o, const std::string &field) {
  auto p = impl->resolve(o);
  auto n = wide(field);
  return p->GetPropertyByNameInChain(n.c_str()) ||
         p->GetFunctionByNameInChain(n.c_str());
}
bool UEEngine::is_a(Object o, const std::string &path) {
  auto cls = U::Cast<U::UClass>(impl->resolve(find(path)));
  if (!cls)
    throw std::runtime_error("Expected UClass: " + path);
  return impl->resolve(o)->IsA(cls);
}
int UEEngine::enum_value(const std::string &path, const std::string &member) {
  auto value = U::Cast<U::UEnum>(impl->resolve(find(path)));
  if (!value)
    throw std::runtime_error("Expected UEnum: " + path);
  for (auto pair : value->ForEachName()) {
    auto name = narrow(pair.Key.ToString());
    if (name == member || name.ends_with("::" + member))
      return int(pair.Value);
  }
  throw std::runtime_error("Missing enum member: " + path + "::" + member);
}
Value UEEngine::get(Object o, const std::string &path) {
  auto [p, data] = impl->locate(o, path);
  Buffer snapshot(p);
  p->CopyCompleteValue(snapshot.data, data);
  return impl->read(p, snapshot.data, 0);
}
size_t UEEngine::array_size(Object o, const std::string &path) {
  auto [p, data] = impl->locate(o, path);
  auto a = U::CastField<U::FArrayProperty>(p);
  if (!a)
    throw std::runtime_error("Expected reflected array");
  U::FScriptArrayHelper helper(a, data);
  if (helper.Num() < 0 || helper.Num() > 10000)
    throw std::runtime_error("Array exceeds bound");
  return helper.Num();
}
void UEEngine::set(Object o, const std::string &path, const Value &value) {
  auto [p, data] = impl->locate(o, path);
  Buffer temporary(p);
  p->CopyCompleteValue(temporary.data, data);
  impl->write(p, temporary.data, value, 0);
  auto [again, dest] = impl->locate(o, path);
  if (again != p)
    throw std::runtime_error("Reflected property changed during assignment");
  p->CopyCompleteValue(dest, temporary.data);
}
CallResult UEEngine::invoke(Object o, const std::string &name,
                            const std::vector<Value> &args) {
  auto [object, fn] = impl->function(o, name);
  Params params(fn);
  size_t index = 0;
  for (auto p : params.props)
    if (!p->HasAnyPropertyFlags(U::CPF_ReturnParm)) {
      if (index < args.size())
        impl->write(p, p->ContainerPtrToValuePtr<void>(params.data),
                    args[index], 0);
      ++index;
    }
  if (args.size() > index)
    throw std::runtime_error("Too many native arguments for " + name);
  object = impl->resolve(o);
  object->ProcessEvent(fn, params.data);
  impl->resolve(o);
  CallResult result;
  for (auto p : params.props) {
    if (p->HasAnyPropertyFlags(U::CPF_ReturnParm))
      result.result =
          impl->read(p, p->ContainerPtrToValuePtr<void>(params.data), 0);
    else if (p->HasAnyPropertyFlags(U::CPF_OutParm))
      result.out.emplace(
          narrow(p->GetName()),
          impl->read(p, p->ContainerPtrToValuePtr<void>(params.data), 0));
  }
  return result;
}
HookId UEEngine::hook(const std::string &path, HookCallback pre,
                      HookCallback post) {
  impl->guard();
  auto fn = U::Cast<U::UFunction>(impl->resolve(find(path)));
  if (!fn)
    throw std::runtime_error("Hook target is not a UFunction");
  auto gate = std::make_shared<Impl::Gate>();
  auto weak = U::FWeakObjectPtr(fn);
  auto wrap = [this, gate, weak, path](HookCallback cb, bool writable_args) {
    return [this, gate, weak, path, writable_args,
            cb](U::UnrealScriptFunctionCallableContext &context, void *) {
      std::lock_guard lock(gate->mutex);
      if (!gate->alive)
        return;
      try {
        impl->guard();
        auto function = U::Cast<U::UFunction>(weak.Get());
        if (!function)
          throw std::runtime_error("Hook function expired");
        ScriptEvent event{reference(context.Context), {}, writable_args};
        std::vector<std::pair<U::FProperty *, void *>> argument_targets;
        for (auto p : parameters(function)) {
          if (p->HasAnyPropertyFlags(U::CPF_ReturnParm))
            continue;
          void *data =
              p->ContainerPtrToValuePtr<void>(context.TheStack.Locals());
          if (p->HasAnyPropertyFlags(U::CPF_OutParm)) {
            if (auto out = U::FindOutParamValueAddress(context.TheStack, p))
              data = out;
          }
          event.args.push_back(impl->read(p, data, 0));
          argument_targets.emplace_back(p, data);
        }
        if (cb)
          cb(event);
        for (const auto &[index, replacement] : event.object_arg_replacements) {
          if (!writable_args || index >= argument_targets.size())
            throw std::runtime_error("Invalid hook argument replacement");
          const auto [property, destination] = argument_targets[index];
          if (!U::CastField<U::FObjectPropertyBase>(property) ||
              property->HasAnyPropertyFlags(U::CPF_OutParm))
            throw std::runtime_error("Only UObject input arguments are writable");
          impl->write(property, destination, replacement, 0);
        }
      } catch (const std::exception &ex) {
        std::lock_guard error_lock(impl->error_mutex);
        impl->error = path + ": " + ex.what();
        OutputDebugStringA(
            ("[AstroConsoleCore] " + impl->error + "\n").c_str());
      }
    };
  };
  auto ids =
      U::UObjectGlobals::RegisterHook(fn, wrap(pre, true), wrap(post, false), nullptr);
  if (ids.first < 0 || ids.second < 0)
    throw std::runtime_error("Native hook registration failed: " + path);
  auto id = impl->next_hook++;
  impl->hooks.emplace(id, Impl::BoundHook{weak, ids, gate});
  return id;
}
void UEEngine::unhook(HookId id) {
  if (!is_game_thread()) {
    close_callbacks();
    throw std::runtime_error("Hook closed but unregister requires native game thread");
  }
  auto it = impl->hooks.find(id);
  if (it == impl->hooks.end())
    return;
  {
    std::lock_guard lock(it->second.alive->mutex);
    it->second.alive->alive = false;
  }
  auto fn = U::Cast<U::UFunction>(it->second.function.Get());
  // A previous attempt may already have changed the SDK before throwing. Keep
  // its exact failure and never silently retry or reclassify it as success.
  if (it->second.unregister_failure)
    std::rethrow_exception(it->second.unregister_failure);
  if (fn) {
    try {
      U::UObjectGlobals::UnregisterHook(fn, it->second.ids);
    } catch (const std::exception &ex) {
      it->second.unregister_failure = std::current_exception();
      {
        std::lock_guard lock(impl->error_mutex);
        impl->error = std::string("Hook unregister failed: ") + ex.what();
      }
      OutputDebugStringA(
          (std::string("[AstroConsoleCore] Closed hook unregister: ") +
           ex.what() + "\n")
              .c_str());
      throw;
    } catch (...) {
      it->second.unregister_failure = std::current_exception();
      {
        std::lock_guard lock(impl->error_mutex);
        impl->error = "Hook unregister raised a non-standard exception";
      }
      throw;
    }
  }
  impl->hooks.erase(it);
}
double UEEngine::now() const {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
uint64_t UEEngine::epoch() const { return impl->generation; }
void UEEngine::invalidate_world() {
  impl->guard();
  ++impl->generation;
}
std::string UEEngine::last_error() const {
  std::lock_guard lock(impl->error_mutex);
  return impl->error;
}
} // namespace astro
