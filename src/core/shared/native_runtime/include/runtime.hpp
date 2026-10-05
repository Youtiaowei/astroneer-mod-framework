#pragma once
#include "controls.hpp"
#include "engine.hpp"
#include "files.hpp"
#include "module_registration.hpp"
#include "module_engine.hpp"
#include "presentation.hpp"
#include "settings.hpp"
#include <deque>
#include <map>
#include <mutex>
namespace astro {
struct RuntimeConfig {
  std::vector<std::string> installed;
  bool presentation = true;
  std::vector<ModuleRegistration> modules;
  bool modules_authoritative = false;
  std::map<std::string, bool> initial_enabled;
};
RuntimeConfig read_config(const std::string &native_config,
                          const std::string &registry,
                          const std::string &authority = {});
// Keyboard threads hold this scalar-only mailbox, never a Runtime pointer.
class InputMailbox {
public:
  struct Event {
    bool authorized = false;
    double deadline = -1;
  };
  std::mutex mutex;
  bool alive = true, ready = false, menu = false, blocked = false,
       proof_down = false, proof_eligible = false;
  std::deque<Event> events;
  uint64_t notices = 0, dropped = 0;
  void key_edge() {
    std::lock_guard lock(mutex);
    if (!alive)
      return;
    ++notices;
    if (ready && !menu && !blocked) {
      if (events.size() < 64)
        events.push_back({proof_down && proof_eligible, -1});
      else
        ++dropped;
    }
  }
  void close() {
    std::lock_guard lock(mutex);
    alive = false;
    events.clear();
    ready = false;
  }
};
class Runtime {
  // Per-module owned instance. Created via the registration's API factory,
  // destroyed via the paired destroy in the same allocating DLL.
  struct ModuleInstance {
    module::Api api{};
    module::NumberApi number{};
    std::shared_ptr<void> code_lifetime;
    std::unique_ptr<ModuleEngine> engine_owner;
    void *instance = nullptr;
    bool enabled = false;
    bool world_active = false;
    uint32_t error_code = 0, cleanup_error = 0, destroy_error = 0;
    uint32_t end_world() noexcept;
    void retire() noexcept;
    ~ModuleInstance();
    ModuleInstance() = default;
    ModuleInstance(ModuleInstance &&) noexcept;
    ModuleInstance &operator=(ModuleInstance &&) noexcept;
    ModuleInstance(const ModuleInstance &) = delete;
    ModuleInstance &operator=(const ModuleInstance &) = delete;
  };
  Engine &e;
  Files &files;
  RuntimeConfig config;
  std::shared_ptr<InputMailbox> mailbox;
  std::map<std::string, ModuleInstance> modules_;
  SettingsManager settings;
  ControlsManager controls;
  Presentation presentation;
  std::string session, token, reply_json, error, wait_reason, save,
      publication_last_error, version = "unknown";
  int64_t publication_failures = 0;
  std::deque<std::string> seen;
  std::map<std::string, bool> desired;
  Object controller, instance;
  std::vector<Object> menus;
  bool loading = false, world_active = false, ready = false, authority = false,
       menus_dirty = true, menu_open = false, input_first = true,
       discovery_pending = false;
  double next_core = 0, next_ui = 0, expires = -1;
  uint64_t generation = 0, accepted = 0, rejected = 0, menu_requests = 0;
  Value state;
  bool installed(const std::string &) const;
  void require_ready();
  void core_tick();
  void ui_tick();
  std::vector<ControlRow> menu_rows();
  void command();
  void publish();
  void publish_cleanup();
  void suspend(bool invalidate);
  void create_module_instances();
  void fail_module(const std::string &, ModuleInstance &, uint32_t);
  uint32_t call_module(const std::string &, ModuleInstance &, module::Operation,
                       Object = {}, bool = false, Value * = nullptr);
  bool accept_module_state(const std::string &, ModuleInstance &, const Value &);

public:
  Runtime(Engine &, Files &, RuntimeConfig, std::string session,
          std::shared_ptr<InputMailbox>);
  ~Runtime();
  void tick();
  void load_map_pre();
  void load_map_post();
  void shutdown() {
    try {
      presentation.close();
    } catch (...) {
      // Finish suspension before the existing loader reports the UI error.
      suspend(false);
      throw;
    }
    suspend(false);
  }
  void object_created(Object);
  void widgets_changed() { menus_dirty = true; }
  void request_discovery() { discovery_pending = true; }
  void reflection_error(const std::string &message) { error = message; }
  Value set_module(const std::string &, bool);
  Value set_number(const std::string &, double);
  Value snapshot() const { return state; }
};
} // namespace astro