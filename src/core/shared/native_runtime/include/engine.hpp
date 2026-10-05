#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace astro {
// A reference is an engine weak index/serial pair scoped to one world epoch.
// No UObject address or reflected property view crosses this interface.
struct Object {
  int32_t index = -1, serial = 0;
  uint64_t epoch = 0;
  auto operator<=>(const Object &) const = default;
  explicit operator bool() const { return index >= 0 && serial != 0; }
};
struct Value {
  struct Native {
    virtual ~Native() = default;
  };
  std::shared_ptr<Native> native;
  using Record = std::map<std::string, Value>;
  using Array = std::vector<Value>;
  std::variant<std::monostate, bool, int64_t, double, std::string, Object,
               Record, Array>
      data;
  Value() = default;
  Value(bool v) : data(v) {}
  Value(int v) : data(int64_t(v)) {}
  Value(int64_t v) : data(v) {}
  Value(double v) : data(v) {}
  Value(const char *v) : data(std::string(v)) {}
  Value(std::string v) : data(std::move(v)) {}
  Value(Object v) : data(v) {}
  Value(Record v) : data(std::move(v)) {}
  Value(Array v) : data(std::move(v)) {}
  bool null() const { return std::holds_alternative<std::monostate>(data); }
  bool boolean() const { return std::get<bool>(data); }
  double number() const {
    if (auto p = std::get_if<int64_t>(&data))
      return double(*p);
    return std::get<double>(data);
  }
  int integer() const { return static_cast<int>(number()); }
  const std::string &string() const { return std::get<std::string>(data); }
  Object object() const {
    if (null())
      return {};
    return std::get<Object>(data);
  }
  const Record &record() const { return std::get<Record>(data); }
  Record &record() { return std::get<Record>(data); }
  const Array &array() const { return std::get<Array>(data); }
  Array &array() { return std::get<Array>(data); }
  const Value &at(const std::string &key) const { return record().at(key); }
};
inline Value
record(std::initializer_list<std::pair<const std::string, Value>> fields) {
  return Value::Record(fields);
}
inline Value vector(double x, double y, double z) {
  return record({{"X", x}, {"Y", y}, {"Z", z}});
}
struct CallResult {
  Value result;
  Value::Record out;
};
struct ScriptEvent {
  Object context;
  mutable std::vector<Value> args;
  bool writable_object_args = true;
  mutable std::map<size_t, Object> object_arg_replacements;
  // Copied identities only. The adapter commits these synchronously in a pre
  // callback; no borrowed UObject/property/parameter address escapes here.
  void replace_object_arg(size_t index, Object value) const {
    if (!writable_object_args || index >= args.size())
      throw std::runtime_error("Hook object argument is not writable");
    args[index] = value;
    object_arg_replacements[index] = value;
  }
};
using HookCallback = std::function<void(const ScriptEvent &)>;
using HookId = uint64_t;

// All UObject operations, including valid(), require the native game thread.
// Engine values are copied using the reflected property's actual type and
// lifetime methods.
class Engine {
public:
  virtual ~Engine() = default;
  virtual bool valid(Object) const = 0;
  virtual std::string name(Object) = 0;
  virtual Object find(const std::string &path) = 0;
  virtual std::vector<Object> all(const std::string &class_name) = 0;
  virtual Object construct(const std::string &class_path, Object outer,
                           const std::string &name) = 0;
  virtual bool has(Object, const std::string &field_or_function) = 0;
  virtual bool is_a(Object, const std::string &class_path) = 0;
  virtual int enum_value(const std::string &path,
                         const std::string &member) = 0;
  virtual Value get(Object, const std::string &property_path) = 0;
  virtual size_t array_size(Object o, const std::string &path) {
    return get(o, path).array().size();
  }
  virtual void set(Object, const std::string &property_path, const Value &) = 0;
  virtual CallResult invoke(Object, const std::string &function,
                            const std::vector<Value> &args = {}) = 0;
  Value call(Object object, const std::string &function,
             const std::vector<Value> &args = {}) {
    return invoke(object, function, args).result;
  }
  virtual HookId hook(const std::string &path, HookCallback pre,
                      HookCallback post) = 0;
  virtual void unhook(HookId) = 0;
  virtual double now() const = 0;
  virtual uint64_t epoch() const = 0;
  virtual void invalidate_world() = 0;
};
} // namespace astro
