#pragma once
#include "forwarding_engine.hpp"
#include <mutex>
namespace astro {
// Core-owned adapter: a copied callback keeps its allocating DLL mapped until
// after the native std::function has been destroyed. Engine ABI is unchanged.
class ModuleEngine final : public ForwardingEngine {
  struct Admission {
    std::recursive_mutex mutex;
    bool open = true;
    uint32_t failure = 0;
    uint32_t cleanup_errors = 0;
  };
  struct RetainedCallback {
    std::shared_ptr<void> code; // destroyed LAST, after the native callable
    HookCallback callback;
  };
  std::shared_ptr<void> code;
  std::shared_ptr<Admission> admission = std::make_shared<Admission>();
  HookCallback pin(HookCallback callback) {
    if (!callback) return {};
    auto owner = std::make_shared<RetainedCallback>(code, std::move(callback));
    return [owner, gate = admission](const ScriptEvent &event) {
      std::lock_guard lock(gate->mutex);
      if (!gate->open) return;
      try { owner->callback(event); }
      catch (const std::exception &) { gate->failure = 1; gate->open = false; }
      catch (...) { gate->failure = 2; gate->open = false; }
    };
  }
public:
  ModuleEngine(Engine &engine, std::shared_ptr<void> lifetime)
      : ForwardingEngine(engine), code(std::move(lifetime)) {}
  ~ModuleEngine() override { close(); }
  uint32_t fault() const noexcept {
    std::lock_guard lock(admission->mutex);
    return admission->failure;
  }
  uint32_t cleanup_errors() const noexcept {
    std::lock_guard lock(admission->mutex);
    return admission->cleanup_errors;
  }
  void close() noexcept {
    std::lock_guard lock(admission->mutex);
    admission->open = false;
  }
  HookId hook(const std::string &p, HookCallback pre, HookCallback post) override {
    return target.hook(p, pin(std::move(pre)), pin(std::move(post)));
  }
  void unhook(HookId id) override {
    // Cleanup must continue through all modules, including noexcept destructors.
    // Keep the failure at the shared host boundary even if a plugin ignores it.
    try { target.unhook(id); }
    catch (...) {
      std::lock_guard lock(admission->mutex);
      ++admission->cleanup_errors;
    }
  }
};
} // namespace astro
