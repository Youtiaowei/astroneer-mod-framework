// Native UE4SS entry point. No business code is executed on the input thread.
#include "auth_module_loader.hpp"
#include "files.hpp"
#include "runtime.hpp"
#include "strict_json.hpp"
#include "ue_engine.hpp"
#include <fmt/chrono.h>
#include <fmt/xchar.h>

#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <windows.h>
#include <bcrypt.h>
#include "lifecycle_trace.hpp"
#pragma comment(lib, "bcrypt.lib")
namespace {
namespace U = RC::Unreal;
namespace life = astro::lifecycle;
void diagnostic(const std::string &error) {
  OutputDebugStringA(("[AstroConsoleCore] " + error + "\n").c_str());
}
void shutdown_runtime(astro::Runtime &runtime, uint64_t owner) {
  try {
    runtime.shutdown();
  } catch (const std::exception &ex) {
    life::emit(owner, L"shutdown-standard-error");
    diagnostic(ex.what());
  } catch (...) {
    life::emit(owner, L"shutdown-nonstandard-error");
    diagnostic("Native shutdown raised a non-standard exception");
  }
  life::emit(owner, L"shutdown-boundary-returned");
}
std::string session_id() {
  unsigned char bytes[16];
  if (BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw std::runtime_error("Session entropy unavailable");
  std::string result;
  for (auto b : bytes) {
    result += "0123456789abcdef"[b >> 4];
    result += "0123456789abcdef"[b & 15];
  }
  return result;
}
struct CallbackGate {
  std::recursive_mutex mutex;
  bool alive = true;
  std::atomic<DWORD> thread = 0;
  std::atomic<bool> discovery = false;
};
struct Retirement {
  const uint64_t owner;
  explicit Retirement(uint64_t id) : owner(id) {}
  ~Retirement() {
    life::emit(owner, runtime ? L"retirement-pending-destroyed" : L"retirement-empty-destroyed");
  }
  std::mutex mutex;
  std::shared_ptr<astro::UEEngine> engine;
  std::unique_ptr<astro::NativeFiles> data;
  std::unique_ptr<astro::Runtime> runtime;

  bool cleanup_on_game_thread() {
    std::unique_lock lock(mutex);
    if (!runtime || !engine->is_game_thread())
      return false;
    // Take every owner while locked, then release the handoff lock before
    // UObject cleanup. Reentrant/late calls see an empty handoff.
    auto retired_engine = std::move(engine);
    auto retired_data = std::move(data);
    auto retired_runtime = std::move(runtime);
    lock.unlock();
    life::emit(owner, L"cleanup-enter");
    shutdown_runtime(*retired_runtime, owner);
    retired_runtime.reset();
    life::emit(owner, L"runtime-destroyed");
    retired_data.reset();
    life::emit(owner, L"data-destroyed");
    const bool final_engine_owner = retired_engine.use_count() == 1;
    retired_engine.reset();
    life::emit(owner, final_engine_owner ? L"engine-destroyed" : L"engine-owner-released");
    return true;
  }
};
class AstroNativeRuntime final : public RC::CppUserModBase {
  const uint64_t lifecycle_id = life::next_instance();
  // Gate is closed and drained before the services are destroyed. Copied SDK
  // callback closures retain their gate and test it before accessing this.
  std::shared_ptr<CallbackGate> gate = std::make_shared<CallbackGate>();
  std::shared_ptr<astro::InputMailbox> mailbox =
      std::make_shared<astro::InputMailbox>();
  std::shared_ptr<astro::UEEngine> engine = std::make_shared<astro::UEEngine>();
  std::unique_ptr<astro::NativeFiles> data;
  std::unique_ptr<astro::Runtime> runtime;
  std::vector<U::Hook::GlobalCallbackId> hooks;
  std::shared_ptr<Retirement> retirement = std::make_shared<Retirement>(lifecycle_id);
  U::Hook::GlobalCallbackId retirement_hook = U::Hook::ERROR_ID;

  bool attempted = false, loading = false;
  template <class F> auto guarded(F action) {
    return [this, g = gate, action](auto &&...args) {
      std::lock_guard lock(g->mutex);
      if (!g->alive)
        return;
      try {
        action(args...);
      } catch (const std::exception &ex) {
        diagnostic(ex.what());
      }
    };
  }
  U::Hook::FCallbackOptions options(const wchar_t *name) {
    return {false, false, STR("AstroConsoleCore"), name};
  }
  void bootstrap() {
    if (attempted)
      return;
    attempted = true;
    auto paths = astro::resolve_paths(astro::this_module_path());
    astro::NativeFiles mod(paths.mod);
    data = std::make_unique<astro::NativeFiles>(paths.data);
    auto config = mod.read("native_config.json", 4096),
         registry = data->read("installed-mods.json", 8192);
    if (std::filesystem::exists(paths.data / L"installed-mods.json") &&
        !registry)
      throw std::runtime_error("Installed registry is unreadable or oversized");
    astro::NativeFiles installer(paths.data.parent_path());
    auto ledger = installer.read("installed.json", 1024 * 1024);
    if (std::filesystem::exists(paths.data.parent_path() / L"installed.json") &&
        !ledger)
      throw std::runtime_error("Installer ledger is unreadable or oversized");
    if (ledger) {
      auto document = astro::json::parse(*ledger, 1024 * 1024);
      auto recorded =
          std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(
              document.at("game_root").string().c_str())));
      if (!recorded.is_absolute() ||
          _wcsicmp(std::filesystem::weakly_canonical(recorded).c_str(),
                   paths.game.c_str()) != 0)
        throw std::runtime_error(
            "Installer ledger belongs to a different game root");

    }
    if (!registry)
      registry = ledger;
    if (!config || !registry)
      throw std::runtime_error("Native configuration or installed registry "
                               "missing; runtime remains inactive");
    auto cfg = astro::read_config(*config, *registry,
        registry == ledger ? std::string{} : ledger.value_or(std::string{}));
    // Authenticated module index (auth_record v1). The common-file and legacy
    // validation above is unchanged and still authoritative for installed
    // identity; old unsigned metadata never becomes a registration. Missing,
    // unreadable or unprovisioned records fail closed and stay visible.
    auto records =
        data->read("installed-records.json", astro::auth::registry_byte_limit);
    auto loaded = astro::auth::load_production_modules(
        paths.data, records.value_or(std::string{}), cfg.installed);
    cfg.modules = std::move(loaded.modules);
    cfg.modules_authoritative = true;
    if (!loaded.index_ok)
      diagnostic("Authenticated module index rejected: " +
                 std::string(astro::auth::reject_text(loaded.index_error)));
    if (loaded.dropped_rows)
      diagnostic("Authenticated module index dropped unauthenticated rows: " +
                 std::to_string(loaded.dropped_rows));
    life::emit(lifecycle_id, L"authenticated-index-applied");
    runtime = std::make_unique<astro::Runtime>(*engine, *data, cfg,
                                               session_id(), mailbox);
    life::emit(lifecycle_id, L"runtime-created");
    if (cfg.presentation &&
        std::find(cfg.installed.begin(), cfg.installed.end(),
                  "universal_slot") != cfg.installed.end())
      register_keydown_event(RC::Input::Key::F8,
                             [input = mailbox] { input->key_edge(); });
    if (loading)
      runtime->load_map_pre();
  }

public:
  uint64_t instance_id() const noexcept { return lifecycle_id; }
  AstroNativeRuntime() {
    ModName = STR("AstroConsoleCore Native Runtime");
    ModVersion = STR("0.2.0-r2");
    ModDescription =
        STR("Native slot, controls, UMG and authenticated bridge services");
    ModAuthors = STR("Youtiaowei");
    // Input::Handler invokes copies outside its lock. Windows pinning keeps
    // those closure/destructor instructions mapped even after uninstall_mod.
    // It changes neither a file nor persistent system configuration.
    HMODULE resident = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&session_id), &resident))
      throw std::runtime_error("Cannot establish safe callback code lifetime");
    life::emit(lifecycle_id, L"module-created");
  }
  ~AstroNativeRuntime() override {
    life::emit(lifecycle_id, L"uninstall-enter");
    mailbox->close();
    {
      std::lock_guard lock(gate->mutex);
      gate->alive = false;
    }
    engine->close_callbacks();
    life::emit(lifecycle_id, L"engine-gate-closed");
    for (auto id : hooks) {
      if (id != U::Hook::ERROR_ID) {
        const bool removed = U::Hook::UnregisterCallback(id);
        life::emit(lifecycle_id, removed ? L"business-unregistered" : L"business-unregister-failed", id);
        if (!removed)
          diagnostic("Native callback unregister failed; closed gate remains resident");
      }
    }
    // The independent cleanup callback was reserved before business callbacks.
    // Off-thread uninstall only transfers ownership after all old gates drain;
    // it neither allocates nor requests another SDK registration here.
    if (runtime && !engine->is_game_thread()) {
      {
        std::lock_guard lock(retirement->mutex);
        retirement->engine = std::move(engine);
        retirement->data = std::move(data);
        retirement->runtime = std::move(runtime);
      }
      // The game thread may finish before this observation; it is not an arm event.
      life::emit(lifecycle_id, L"handoff-returned");
    } else {
      if (retirement_hook != U::Hook::ERROR_ID &&
          !U::Hook::UnregisterCallback(retirement_hook))
        diagnostic("Native cleanup callback unregister failed");
      if (runtime) {
        shutdown_runtime(*runtime, lifecycle_id);
        runtime.reset();
        life::emit(lifecycle_id, L"runtime-destroyed");
      }
    }
  }
  void on_unreal_init() override {
    retirement_hook = U::Hook::RegisterEngineTickPreCallback(
        [retired = retirement](U::Hook::TCallbackIterationData<void> &iteration,
                               U::UEngine *, float, bool) {
          // Regular UnregisterCallback waits for executors, including self.
          if (retired->cleanup_on_game_thread()) {
            iteration.RemoveSelf();
            life::emit(retired->owner, L"cleanup-invalidated");
          }
        },
        options(L"RetiredGameThreadCleanup"));
    if (retirement_hook == U::Hook::ERROR_ID)
      throw std::runtime_error("Cannot reserve game-thread cleanup; runtime remains inactive");
    hooks.push_back(U::Hook::RegisterLoadMapPreCallback(
        guarded([this](auto &...) {
          engine->enter_game_thread();
          gate->thread = GetCurrentThreadId();
          loading = true;
          if (runtime)
            runtime->load_map_pre();
        }),
        options(L"LoadMapPre")));
    hooks.push_back(U::Hook::RegisterLoadMapPostCallback(
        guarded([this](auto &...) {
          engine->enter_game_thread();
          gate->thread = GetCurrentThreadId();
          loading = false;
          if (runtime)
            runtime->load_map_post();
        }),
        options(L"LoadMapPost")));
    hooks.push_back(U::Hook::RegisterEngineTickPreCallback(
        guarded([this](auto &...) {
          engine->enter_game_thread();
          gate->thread = GetCurrentThreadId();
          bootstrap();
          if (!runtime)
            return;
          if (gate->discovery.exchange(false)) {
            runtime->widgets_changed();
            runtime->request_discovery();
          }
          runtime->tick();
          auto error = engine->last_error();
          if (!error.empty())
            runtime->reflection_error(error);
        }),
        options(L"EngineTick")));
    hooks.push_back(U::Hook::RegisterStaticConstructObjectPostCallback(
        [this, g = gate](U::Hook::TCallbackIterationData<U::UObject *> &result,
                         const U::FStaticConstructObjectParameters &) {
          if (g->thread.load() != GetCurrentThreadId()) {
            g->discovery = true;
            return;
          }
          std::lock_guard lock(g->mutex);
          if (!g->alive || !runtime)
            return;
          try {
            // Async loading provides no safe long-lived UObject pointer. Mark a
            // discovery pass; the next engine tick resolves current weak refs.

            auto object =
                engine->reference(result.GetOriginalFunctionCallResult());
            if (!engine->valid(object))
              return;
            if (engine->is_a(object, "/Script/Astro.PhysicalItem"))
              runtime->object_created(object);
            if (engine->is_a(object, "/Script/UMG.UserWidget"))
              runtime->widgets_changed();
          } catch (const std::exception &ex) {
            diagnostic(ex.what());
          }
        },
        options(L"ObjectCreated")));
    for (auto id : hooks)
      if (id == U::Hook::ERROR_ID)
        throw std::runtime_error(
            "Required native callback registration failed");
  }
};
} // namespace
extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  try {
    return new AstroNativeRuntime();
  } catch (const std::exception &ex) {
    diagnostic(ex.what());
    return nullptr;
  }
}
extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  if (!mod) return;
  const auto owner = static_cast<AstroNativeRuntime*>(mod)->instance_id();
  delete mod;
  life::emit(owner, L"module-destroyed");
}
