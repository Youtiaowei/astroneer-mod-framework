#include "runtime.hpp"
#include "module_engine.hpp"
#include "number_descriptor.hpp"
#include "catalog.hpp"
#include "strict_json.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <limits>
#include <set>
#ifdef ASTRO_RUNTIME_FIXTURE
// Test-only: query the statically linked slot_module for a registration.
// Compiled only when build_r2.ps1 Mode=test defines ASTRO_RUNTIME_FIXTURE.
namespace astro { extern ModuleRegistration fixture_slot_registration(); }
#endif
namespace astro {
namespace {
bool identity(const std::string &id) {
  if (id.empty() || id.size() > 64 || id[0] < 'a' || id[0] > 'z')
    return false;
  return id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") ==
         std::string::npos;
}
std::vector<std::string> module_list(const Value &v) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  if (v.array().size() > 32)
    throw std::runtime_error("Installed module registry exceeds bound");
  for (auto &value : v.array()) {
    auto id = value.string();
    if (!identity(id) || !seen.insert(id).second)
      throw std::runtime_error("Invalid or duplicate installed module");
    out.push_back(id);
  }
  return out;
}
std::map<std::string, bool> old_preferences(Files &files) {
  try {
    auto raw = files.read("settings.json", 4096);
    if (!raw)
      return {};
    auto v = json::parse(*raw, 4096);
    std::map<std::string, bool> values;
    if (!v.record().contains("modules"))
      return {};
    if (v.at("modules").record().size() > 32)
      return {};
    for (auto &[id, on] : v.at("modules").record()) {
      if (!identity(id))
        return {};
      values[id] = on.boolean();
    }
    return values;
  } catch (const std::exception &) {
    return {};
  }
}
bool fallback(Files &files) {
  auto values = old_preferences(files);
  return values.contains("universal_slot") && values.at("universal_slot");
}
// When modules_authoritative, use registration metadata for catalog entries.
// Modules with valid registrations get their own metadata; others show load_error.
// When !modules_authoritative (test fixture path), same logic applies after
// the fixture registration is added in prepare_config.
const ModuleRegistration *registration_for(const RuntimeConfig &cfg,
                                            const std::string &id) {
  for (const auto &registration : cfg.modules)
    if (registration.catalog.id == id) return &registration;
  return nullptr;
}
std::vector<CatalogEntry> menu_catalog(const RuntimeConfig &cfg) {
  std::vector<CatalogEntry> result;
  for (auto &id : cfg.installed) {
    const auto *found = registration_for(cfg, id);
    if (found) {
      auto entry = found->catalog;
      if (entry.name.empty()) entry.name = id;
      entry.load_error = found->load_error;
      result.push_back(std::move(entry));
    } else {
      result.push_back(
          {id, id, "", "", "No compiled module service for " + id});
    }
  }
  return result;
}
RuntimeConfig prepare_config(RuntimeConfig cfg) {
#ifdef ASTRO_RUNTIME_FIXTURE
  if (!cfg.modules_authoritative) {
    auto reg = fixture_slot_registration();
    if (reg.api.create) {
      bool already = false;
      for (auto &r : cfg.modules)
        if (r.catalog.id == reg.catalog.id) {
          already = true;
          break;
        }
      if (!already)
        cfg.modules.push_back(std::move(reg));
    }
  }
#endif
  std::map<std::string, size_t> counts;
  for (const auto &reg : cfg.modules) ++counts[reg.catalog.id];
  for (auto &reg : cfg.modules) {
    if (counts[reg.catalog.id] != 1)
      reg.load_error = "duplicate_module_registration";
    if (!reg.load_error.empty()) continue;
    // Check ownership BEFORE dereferencing the DLL's borrowed metadata strings.
    // The static exception exists only in owned acceptance executables.
    bool static_fixture = false;
#if defined(ASTRO_RUNTIME_FIXTURE) || defined(ASTRO_MODULE_FIXTURE)
    static_fixture = reg.code_kind == ModuleCodeKind::static_fixture;
#endif
    if (!static_fixture && (reg.code_kind != ModuleCodeKind::dynamic_library ||
                           !reg.code_lifetime || reg.code_lifetime.use_count() == 0)) {
      reg.load_error = "module_code_owner_missing";
      continue;
    }
    if (!reg.catalog.load_error.empty())
      reg.load_error = reg.catalog.load_error;
    else if (!reg.api.create || !reg.api.destroy || !reg.api.call ||
             reg.api.size != sizeof(module::Api) || reg.api.version != module::protocol)
      reg.load_error = "invalid_module_api";
    else if (!reg.api.id || reg.catalog.id != reg.api.id ||
             !reg.api.module_version || reg.version.empty() || reg.version != reg.api.module_version)
      reg.load_error = "module_identity_mismatch";
    else if(reg.number.set&&(reg.number.size!=sizeof(module::NumberApi)||reg.number.version!=1
                            ||!reg.number.id||reg.catalog.id!=reg.number.id))
      reg.load_error="module_number_identity_mismatch";
  }
  return cfg;
}
Value preferences_value(const Preferences &p) {
  Value::Record modules;
  for (auto &[id, on] : p.modules)
    modules[id] = on;
  return record(
      {{"enabled", p.enabled}, {"visible", p.visible}, {"modules", modules}, {"language", p.language}});
}
} // namespace
RuntimeConfig read_config(const std::string &native,
                          const std::string &registry,
                          const std::string &authority) {
  if (!authority.empty()) {
    const auto canonical = read_config(native, authority);
    const auto projection = read_config(native, registry);
    if (canonical.installed != projection.installed ||
        canonical.initial_enabled != projection.initial_enabled)
      throw std::runtime_error("Installed index/authority mismatch");
    const auto ledger = json::parse(authority, 1024 * 1024);
    if (ledger.at("schema").number() == 2) {
      const auto index = json::parse(registry, 32768);
      if (index.at("schema").number() != 2 ||
          index.at("kind").string() != "installed_mods_index")
        throw std::runtime_error("Invalid installed module index");
      for (const auto &[id, row] : ledger.at("modules").record())
        if (index.at("modules").at(id).string() != row.at("version").string())
          throw std::runtime_error("Installed index/authority version mismatch");
    }
    return canonical;
  }
  auto v = json::parse(native, 4096);
  if (v.at("schema").number() != 1 ||
      v.at("product").string() != "AstroConsoleCore")
    throw std::runtime_error("Unsupported native configuration");
  // Accept historical metadata without making it an admission requirement.
  if (v.record().contains("game_version"))
    (void)v.at("game_version").string();
  for (auto &[key, value] : v.record())
    if (key != "schema" && key != "product" && key != "game_version" &&
        key != "presentation" && key != "installed_modules" &&
        key != "development")
      throw std::runtime_error("Unknown native configuration field");
  if (v.record().contains("development") && v.at("development").boolean())
    throw std::runtime_error(
        "Development mode is not part of the native runtime");
  RuntimeConfig cfg;
  cfg.installed = module_list(v.at("installed_modules"));
  cfg.presentation = v.at("presentation").boolean();
  auto r = json::parse(registry, 1024 * 1024);
  const auto schema = r.at("schema").number();
  if (schema != 1 && schema != 2)
    throw std::runtime_error("Unsupported installed registry schema");
  std::vector<std::string> listed, enabled;
  if (r.record().contains("installed_modules"))
    listed = module_list(r.at("installed_modules"));
  else {
    if (r.at("product").string() != "AstroneerModInstaller" ||
        r.at("modules").record().size() > 32)
      throw std::runtime_error("Invalid installer ledger");
    const bool index = schema == 2 && r.at("kind").string() == "installed_mods_index";
    if (schema == 2 && !index && r.at("kind").string() != "install_authority")
      throw std::runtime_error("Invalid schema-2 registry kind");
    Value::Array ids;
    for (auto &[id, version] : r.at("modules").record()) {
      const auto &text = schema == 2 && !index ? version.at("version").string() : version.string();
      if (text.empty() || text.size() > 32)
        throw std::runtime_error("Invalid installed module version");
      ids.push_back(id);
      if (schema == 2) {
        const bool on = index ? std::find(cfg.installed.begin(), cfg.installed.end(), id) != cfg.installed.end()
                              : version.at("enabled").boolean();
        cfg.initial_enabled[id] = on;
        if (on) enabled.push_back(id);
      }
    }
    listed = module_list(ids);
  }
  auto expected = cfg.installed;
  std::sort(listed.begin(), listed.end());
  std::sort(expected.begin(), expected.end());
  if ((schema == 2 ? enabled : listed) != expected)
    throw std::runtime_error("Installed registry/config mismatch");
  if (schema == 2) cfg.installed = listed;
  return cfg;
}
// --- ModuleInstance lifetime ---
Runtime::ModuleInstance::~ModuleInstance() {
  retire();
}
uint32_t Runtime::ModuleInstance::end_world() noexcept {
  if (!instance || !world_active) return 0;
  world_active = false; // Claim before invoking, including failed cleanup.
  const auto code = api.call(instance, module::Operation::end_world, {}, false, nullptr);
  if (code && !cleanup_error) cleanup_error = code;
  return code;
}
void Runtime::ModuleInstance::retire() noexcept {
  if (engine_owner) engine_owner->close();
  if (instance) {
    if (!error_code) error_code = engine_owner->fault();
    end_world();
    auto *owned = instance;
    instance = nullptr;
    destroy_error = api.destroy(owned);
  }
}
Runtime::ModuleInstance::ModuleInstance(ModuleInstance &&o) noexcept
    : api(o.api), number(o.number), code_lifetime(std::move(o.code_lifetime)), engine_owner(std::move(o.engine_owner)),
      instance(o.instance), enabled(o.enabled), world_active(o.world_active),
      error_code(o.error_code), cleanup_error(o.cleanup_error), destroy_error(o.destroy_error) {
  o.instance = nullptr;
}
Runtime::ModuleInstance &
Runtime::ModuleInstance::operator=(ModuleInstance &&o) noexcept {
  if (this != &o) {
    this->~ModuleInstance();
    new (this) ModuleInstance(std::move(o));
  }
  return *this;
}
void Runtime::fail_module(const std::string &id, ModuleInstance &mi, uint32_t code) {
  mi.engine_owner->close();
  if (!mi.error_code) mi.error_code = code;
  mi.enabled = false;
  // Claim world cleanup before calling it so a failing cleanup cannot be retried
  // later by suspension/destruction. The original failure remains authoritative.
  mi.end_world();
  controls.unavailable(id, "mod.runtime_error");
}
uint32_t Runtime::call_module(const std::string &id, ModuleInstance &mi,
                             module::Operation op, Object object, bool requested,
                             Value *out) {
  if (auto fault = mi.engine_owner->fault(); fault && !mi.error_code)
    fail_module(id, mi, fault);
  if (mi.error_code && op != module::Operation::end_world) return mi.error_code;
  auto code = mi.api.call(mi.instance, op, object, requested, out);
  if (!code && op != module::Operation::end_world) code = mi.engine_owner->fault();
  if (code) fail_module(id, mi, code);
  return code;
}
bool Runtime::accept_module_state(const std::string &id, ModuleInstance &mi,
                                  const Value &value) {
  try {
    const auto &fields = value.record();
    mi.enabled = value.at("enabled").boolean();
    if (fields.contains("error")) (void)value.at("error").string();
    if(mi.number.set&&fields.contains("number")&&!value.at("number").null())
      (void)NumberDescriptor::parse(value.at("number"));
    return true;
  } catch (const std::exception &) {
    fail_module(id, mi, 3);
    return false;
  }
}
// --- Runtime construction ---
void Runtime::create_module_instances() {
  for (auto &reg : config.modules) {
    if (!installed(reg.catalog.id) || !reg.load_error.empty())
      continue;
    ModuleInstance mi;
    mi.api = reg.api;
    mi.number = reg.number;
    mi.code_lifetime = reg.code_lifetime;
    mi.engine_owner = std::make_unique<ModuleEngine>(e, reg.code_lifetime);
    mi.instance = reg.api.create(mi.engine_owner.get());
    if (!mi.instance) {
      reg.load_error = "module_factory_failed";
      controls.unavailable(reg.catalog.id, "mod.load_error");
      continue;
    }
    auto [it,inserted]=modules_.emplace(reg.catalog.id, std::move(mi));
    auto parameters=settings.read().parameters;
    if(it->second.number.set&&parameters.contains(reg.catalog.id)) {
      Value result;
      const auto code=it->second.number.set(it->second.instance,parameters.at(reg.catalog.id),true,&result);
      const auto fault=it->second.engine_owner->fault();
      // A rejected saved parameter is not a failed module. Keep the native
      // default and the original journal so a later valid slider can fix it.
      if(fault||(code&&code!=3))fail_module(reg.catalog.id,it->second,fault?fault:code);
    }
  }
}
Runtime::Runtime(Engine &engine, Files &storage, RuntimeConfig cfg,
                 std::string session_value, std::shared_ptr<InputMailbox> input)
    : e(engine), files(storage), config(prepare_config(std::move(cfg))),
      mailbox(std::move(input)),
      settings(files, fallback(files), true),
      controls(
          settings.read(),
          [this](bool on, const std::string &id) {
            auto result = set_module(id, on);
            bool blocked = result.record().contains("blocked") &&
                           result.at("blocked").boolean();
            auto it = modules_.find(id);
            bool is_enabled =
                (it != modules_.end()) ? it->second.enabled : false;
            std::string msg;
            if (blocked)
              msg = result.record().contains("reason")
                        ? result.at("reason").string()
                        : "";
            else
              msg = result.record().contains("error")
                        ? result.at("error").string()
                        : "";
            return SetModuleResult{id, is_enabled, !blocked, msg, blocked};
          },
          [this](const Preferences &p) {
            auto merged=p;merged.parameters=settings.read().parameters;
            return settings.save(merged);
          },
          menu_catalog(config)),
      presentation(e), session(std::move(session_value)) {
  auto bytes = files.read("bridge-token", 128);
  if (!bytes || bytes->empty())
    throw std::runtime_error("Bridge token missing or oversized");
  token = *bytes;
  desired = old_preferences(files);
  auto stored = settings.read();
  for (auto &[id, on] : stored.modules)
    desired[id] = on;
  // Disabled by the external manager remains installed, but starts inert even
  // when an older in-game journal still records the previous enabled intent.
  for (const auto &[id, on] : config.initial_enabled)
    if (!on || !desired.contains(id)) desired[id] = on;
  controls.suspend();
  state = record({});
  create_module_instances();
}
Runtime::~Runtime() {
  // Close every admission gate before any module's cleanup can call the engine.
  for (auto &[id, mi] : modules_) mi.engine_owner->close();
  for (auto &[id, mi] : modules_) mi.retire();
  try { publish_cleanup(); }
  catch (...) { std::fputs("AstroConsoleCore: cleanup report could not be published\n", stderr); }
}

void Runtime::publish_cleanup() {
  Value::Record cleanup;
  if (!state.record().contains("modules")) state.record()["modules"] = record({});
  for (auto &[id, mi] : modules_) {
    const auto unhook_errors = mi.engine_owner->cleanup_errors();
    const bool complete = !mi.cleanup_error && !mi.destroy_error && !unhook_errors;
    cleanup[id] = record({{"end_world_error_code", int64_t(mi.cleanup_error)},
                          {"destroy_error_code", int64_t(mi.destroy_error)},
                          {"native_error_code", int64_t(mi.error_code)},
                          {"unhook_error_count", int64_t(unhook_errors)},
                          {"complete", complete}});
    auto &row = state.record()["modules"].record()[id];
    if (row.null()) row = record({});
    row.record()["enabled"] = false;
    row.record()["phase"] = complete ? "stopped" : "cleanup_error";
    if (!complete) row.record()["error"] = "mod.cleanup_error";
    if (state.record().contains("controls")) {
      auto &view = state.record()["controls"].record();
      if (view.contains("mods")) for (auto &item : view["mods"].array())
        if (item.at("id").string() == id) {
          item.record()["enabled"] = false;
          item.record()["active"] = false;
          if (!complete) item.record()["error"] = "mod.cleanup_error";
        }
    }
  }
  state.record()["cleanup"] = cleanup;
  state.record()["ready"] = false;
  state.record()["stopped"] = true;
  publish();
}
bool Runtime::installed(const std::string &id) const {
  return std::find(config.installed.begin(), config.installed.end(), id) !=
         config.installed.end();
}
void Runtime::require_ready() {
  if (loading)
    throw std::runtime_error("Wait for native map loading");
  auto instances = e.all("AstroGameInstance");
  if (instances.empty())
    throw std::runtime_error("Wait for the game instance");
  instance = instances.front();
  if (!e.valid(instance) || e.call(instance, "IsLoadingScreenActive").boolean())
    throw std::runtime_error("Wait for the loading screen");
  Object pc;
  for (auto candidate : e.all("PlayController"))
    if (e.valid(candidate) &&
        e.call(candidate, "IsLocalController").boolean()) {
      if (pc)
        throw std::runtime_error(
            "Multiple local controllers are not supported");
      pc = candidate;
    }
  if (!e.valid(pc) || !e.valid(e.call(pc, "GetAstroCharacter").object()))
    throw std::runtime_error("Wait for the local playable character");
  auto name = e.call(instance, "GetActiveSaveFileDescriptiveName").string();
  authority = e.call(pc, "HasAuthority").boolean();
  if (authority && name.empty())
    throw std::runtime_error("Load a local save first");
  controller = pc;
  save = name;
}
void Runtime::suspend(bool invalidate) {
  discovery_pending = false;
  world_active = false;
  ready = false;
  controller = {};
  instance = {};
  for (auto &[id, mi] : modules_) {
    if (mi.world_active) {
      if (auto code = mi.end_world()) fail_module(id, mi, code);
    }
    mi.enabled = false;
  }
  if (invalidate)
    presentation.suspend();
  else
    presentation.discard(); // Shutdown already attempted close; never retry it.
  controls.suspend();
  menus.clear();
  menus_dirty = true;
  menu_open = false;
  input_first = true;
  expires = -1;
  {
    std::lock_guard lock(mailbox->mutex);
    mailbox->ready = false;
    mailbox->menu = false;
    mailbox->blocked = false;
    mailbox->proof_down = false;
    mailbox->proof_eligible = false;
    mailbox->events.clear();
  }
  if (invalidate) {
    e.invalidate_world();
    ++generation;
  }
}
void Runtime::load_map_pre() {
  loading = true;
  suspend(true);
}
void Runtime::load_map_post() {
  loading = false;
  next_core = 0;
}
void Runtime::object_created(Object o) {
  if (world_active && !loading && ready) {
    for (auto &[id, mi] : modules_) {
      if (mi.instance && mi.enabled)
        call_module(id, mi, module::Operation::object_created, o);
    }
  }
}
Value Runtime::set_module(const std::string &id, bool enabled) {
  if (!ready || loading)
    throw std::runtime_error("Wait for a supported playable world");
  require_ready();
  if (!installed(id))
    throw std::runtime_error("Unknown or unavailable module");
  auto it = modules_.find(id);
  if (it == modules_.end() || !it->second.instance)
    throw std::runtime_error("Unknown or unavailable module");
  Value result;
  auto rc = call_module(id, it->second, module::Operation::set_enabled,
                        {}, enabled, &result);
  if (rc != 0)
    throw std::runtime_error("Module set_enabled call failed");
  try {
    // A normal refusal is not a status record and does not change either the
    // effective state or the persisted intent (e.g. native resource sorting).
    if (result.record().contains("blocked") && result.at("blocked").boolean()) {
      if (result.record().contains("reason")) (void)result.at("reason").string();
      return result;
    }
  } catch (const std::exception &) {
    fail_module(id, it->second, 3);
    throw std::runtime_error("Invalid module toggle response");
  }
  if (!accept_module_state(id, it->second, result))
    throw std::runtime_error("Module set_enabled call failed");
  bool error_empty = !result.record().contains("error") ||
                     result.at("error").string().empty();
  if (it->second.enabled == enabled && error_empty)
    desired[id] = enabled;
  return result;
}
void Runtime::command() {
  auto raw = files.read("request.json", 8192);
  if (!raw)
    return;
  Value request;
  try {
    request = json::parse(*raw);
    if (request.at("protocol").number() != 1 ||
        request.at("token").string() != token ||
        request.at("session").string() != session)
      return;
    auto id = request.at("id").string();
    if (id.empty() || id.size() > 64 ||
        std::find(seen.begin(), seen.end(), id) != seen.end())
      return;
    seen.push_back(id);
    if (seen.size() > 256)
      seen.pop_front();
  } catch (const std::exception &) {
    return;
  }
  auto id = request.at("id").string();
  Value reply;
  try {
    auto action = request.at("action").string();
    Value data;
    if (action == "inspect")
      data = record(
          {{"SlotAcceptsItem",
            e.valid(e.find("/Script/Astro.SlotsComponent:SlotAcceptsItem"))},
           {"ConnectPowerToItem",
            e.valid(
                e.find("/Script/Astro.SlotsComponent:ConnectPowerToItem"))}});
    else if (action == "set_module")
      data = set_module(request.at("module").string(),
                        request.at("enabled").boolean());
    else if (action == "refresh_modules") {
      require_ready();
      if (!ready)
        throw std::runtime_error("World not ready");
      bool failed = false;
      for (auto &[mid, mi] : modules_)
        if (mi.instance)
          failed = call_module(mid, mi, module::Operation::refresh) != 0 || failed;
      if (failed) throw std::runtime_error("Module refresh failed");
      data = record({{"refreshed", true}});
    } else
      throw std::runtime_error("Unsupported action");
    reply = record({{"id", id}, {"ok", true}, {"data", data}});
  } catch (const std::exception &ex) {
    reply = record({{"id", id}, {"ok", false}, {"error", ex.what()}});
  }
  try {
    reply_json = json::encode(reply);
  } catch (const std::exception &ex) {
    reply_json = json::encode(record(
        {{"id", id},
         {"ok", false},
         {"error", "Reply encoding failed: " + std::string(ex.what())}}));
  }
}
void Runtime::core_tick() {
  version = "unknown";
  try {
    auto project =
        e.find("/Script/EngineSettings.Default__GeneralProjectSettings");
    if (e.valid(project))
      version = e.get(project, "ProjectVersion").string();
  } catch (const std::exception &) {
    // Version metadata is diagnostic; missing metadata must not suspend play.
  }
  auto previous = controller;
  auto previous_save = save;
  try {
    require_ready();
    if (world_active && (controller != previous || save != previous_save)) {
      suspend(true);
      require_ready();
    }
    ready = true;
    wait_reason.clear();
  } catch (const std::exception &ex) {
    wait_reason = ex.what();
    if (world_active)
      suspend(true);
    ready = false;
  }
  if (ready) {
    for (auto &[id, mi] : modules_) {
      if (mi.error_code) continue;
      if (!mi.world_active) {
        Value result;
        mi.world_active = true;
        if (call_module(id, mi, module::Operation::start_world, {}, desired[id], &result) ||
            !accept_module_state(id, mi, result)) continue;
      }
      if (discovery_pending)
        call_module(id, mi, module::Operation::request_refresh);
      call_module(id, mi, module::Operation::tick);
    }
    if (discovery_pending)
      discovery_pending = false;
    world_active = true;
    if (config.presentation) {
      std::map<std::string, bool> actual;
      for (auto &[id, mi] : modules_)
        actual[id] = mi.enabled;
      controls.ready(actual);
    }
  }
  command();
  Value::Record modules;
  for (auto &id : config.installed) {
    Value s;
    auto it = modules_.find(id);
    if (it != modules_.end() && it->second.instance) {
      auto &mi = it->second;
      auto code = call_module(id, mi, module::Operation::snapshot, {}, mi.enabled, &s);
      if (code || !accept_module_state(id, mi, s))
        s = record({{"enabled", false}, {"phase", "runtime_error"},
                    {"error", "mod.runtime_error"},
                    {"native_error_code", int64_t(mi.error_code)},
                    {"cleanup_error_code", int64_t(mi.cleanup_error)}});
      if (!s.record().contains("error"))
        s.record()["error"] = "";
    } else {
      const auto *registration = registration_for(config, id);
      const std::string load_error = registration && !registration->load_error.empty()
                                        ? registration->load_error : "mod.load_error";
      s = record({{"enabled", false},
                  {"phase", "load_error"},
                  {"error", load_error}});
    }
    s.record()["desired"] = desired[id];
    if (!ready && desired[id] && s.at("error").string().empty())
      s.record()["phase"] = "waiting_save";
    modules[id] = s;
  }
  state = record(
      {{"protocol", 1},
       {"runtime", "AstroConsoleCore"},
       {"session", session},
       {"modules", modules},
       {"world_generation", int64_t(generation)},
       {"ready", ready},
       {"wait_reason", wait_reason},
       {"save", save},
       {"game_version", version},
       {"local_authority", authority},
       {"local_controller", e.valid(controller) ? e.name(controller) : ""},
       {"scheduler", "engine_tick"},
       {"scheduler_game_thread", true},
       {"heartbeat", int64_t(std::time(nullptr))},
       {"reflection",
        record(
            {{"PhysicalItem", e.valid(e.find("/Script/Astro.PhysicalItem"))}})},
       {"error", error}});
  if (config.presentation) {
    auto view = presentation.snapshot();
    view.record()["preferences"] = preferences_value(controls.preferences());
    view.record()["menu_open"] = menu_open;
    Value::Array rows;
    for (auto &r : controls.rows())
      rows.push_back(record({{"id", r.id},
                             {"name", r.name},
                             {"hotkey", r.hotkey},
                             {"description", r.description},
                             {"enabled", r.enabled},
                             {"active", r.active},
                             {"error", r.error}}));
    view.record()["mods"] = rows;
    uint64_t events, dropped;
    {
      std::lock_guard lock(mailbox->mutex);
      events = mailbox->notices;
      dropped = mailbox->dropped;
    }
    view.record()["input"] =
        record({{"events", record({{"feature", int64_t(events)}})},
                {"rejected_native", record({{"feature", int64_t(rejected)}})},
                {"accepted", record({{"feature", int64_t(accepted)}})},
                {"menu", record({{"universal_slot", int64_t(menu_requests)}})},
                {"dropped", int64_t(dropped)}});
    state.record()["controls"] = view;
  }
  publish();
}
void Runtime::publish() {
  auto publication = [this](bool pending, const std::string &message) {
    return record({{"pending", pending},
                   {"error", message},
                   {"failure_count", publication_failures},
                   {"last_error", publication_last_error}});
  };
  try {
    // The first successfully committed recovery snapshot describes itself as
    // current. History remains visible without overwriting unrelated errors.
    state.record()["publication"] = publication(false, "");
    auto encoded = json::encode(state);
    if (!reply_json.empty()) {
      encoded.pop_back();
      encoded += ",\"reply\":" + reply_json + "}";
    }
    if (!files.publish_runtime_state(encoded))
      throw std::runtime_error("Runtime state publication failed");
  } catch (const std::exception &ex) {
    publication_last_error = ex.what();
    if (publication_failures < std::numeric_limits<int64_t>::max())
      ++publication_failures;
    state.record()["publication"] = publication(true, publication_last_error);
    // Leave the last complete disk snapshot and frozen command reply intact.
    // The existing core cadence retries fresh world state, without sleeping.
  }
}
void Runtime::ui_tick() {
  if (!ready || !config.presentation)
    return;
  if (!e.valid(controller) || !e.valid(instance)) {
    suspend(true);
    return;
  }
  if (menus_dirty) {
    menus = e.all("GameMenuRoot_C");
    menus_dirty = false;
  }
  Object menu;
  for (auto candidate : menus)
    if (e.valid(candidate) && e.call(candidate, "IsInViewport").boolean()) {
      menu = candidate;
      break;
    }
  menu_open = bool(menu);
  double now = e.call(e.find("/Script/Engine.Default__GameplayStatics"),
                      "GetRealTimeSeconds", {instance})
                   .number();
  bool down =
      modules_.count("universal_slot") > 0 &&
      e.call(controller, "IsInputKeyDown", {record({{"KeyName", "F8"}})})
          .boolean();
  bool apply = false;
  {
    std::lock_guard lock(mailbox->mutex);
    mailbox->ready = true;
    mailbox->menu = menu_open;
    if (input_first) {
      mailbox->blocked = down;
      input_first = false;
    }
    if (menu_open && down)
      mailbox->blocked = true;
    if (down && !mailbox->proof_down)
      mailbox->proof_eligible = !menu_open && !mailbox->blocked;
    if (down)
      expires = now + .5;
    if (menu_open || mailbox->blocked)
      mailbox->proof_eligible = false;
    if (mailbox->proof_eligible && now <= expires)
      for (auto &event : mailbox->events)
        event.authorized = true;
    mailbox->proof_down = down;
    if (!down) {
      controls.release("feature");
      mailbox->blocked = false;
    }
    if (menu_open) {
      mailbox->events.clear();
      controls.dismiss();
    }
    if (!mailbox->events.empty()) {
      auto &event = mailbox->events.front();
      if (event.deadline < 0)
        event.deadline = now + .5;
      if (mailbox->blocked)
        mailbox->events.pop_front();
      else if (event.authorized) {
        mailbox->events.pop_front();
        apply = true;
      } else if (now > event.deadline) {
        ++rejected;
        mailbox->events.pop_front();
      }
    }
  }
  if (apply) {
    controls.release("feature");
    if (controls.press("feature", now))
      ++accepted;
    controls.release("feature");
  }
  auto view = controls.view(now);
  if (menu_open)
    view.show = false;
  presentation.update(
      instance, menu, menu_rows(),
      [this, menu, now](const std::string &id, bool enabled) {
        if (!ready || !menu_open || !e.valid(menu) ||
            !e.call(menu, "IsInViewport").boolean())
          throw std::runtime_error("Menu is no longer active");
        ++menu_requests;
        return controls.set_module(id, enabled, "menu", now);
      },
      view, controls.language(),
      [this](const std::string &language) { return controls.set_language(language); },
      [this,menu](const std::string& id,double value) {
        if(!ready||!menu_open||!e.valid(menu)||!e.call(menu,"IsInViewport").boolean())
          throw std::runtime_error("Menu is no longer active");
        return set_number(id,value);
      });
}
void Runtime::tick() {
  double now = e.now();
  if (now >= next_core) {
    next_core = now + .25;
    try {
      core_tick();
    } catch (const std::exception &ex) {
      error = ex.what();
    }
  }
  if (now >= next_ui) {
    next_ui = now + .033;
    try {
      if (ready) {
        for (auto &[id, mi] : modules_)
          if (mi.instance && mi.enabled)
            call_module(id, mi, module::Operation::observe_placement, controller);
      }
    } catch (const std::exception &ex) {
      error = ex.what();
    }
    try {
      ui_tick();
    } catch (const std::exception &ex) {
      error = ex.what();
    }
  }
  if (config.presentation)
    presentation.tick_card();
}
} // namespace astro