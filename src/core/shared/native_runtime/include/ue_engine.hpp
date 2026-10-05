#pragma once
#include "engine.hpp"
#include <memory>
namespace RC::Unreal {
class UObject;
}
namespace astro {
class UEEngine final : public Engine {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  UEEngine();
  ~UEEngine();
  // Only the native game-thread callback may establish the initial thread.
  void enter_game_thread();
  bool is_game_thread() const;
  void close_callbacks();
  Object reference(RC::Unreal::UObject *);
  bool valid(Object) const override;
  std::string name(Object) override;
  Object find(const std::string &) override;
  std::vector<Object> all(const std::string &) override;
  Object construct(const std::string &, Object, const std::string &) override;
  bool has(Object, const std::string &) override;
  bool is_a(Object, const std::string &) override;
  int enum_value(const std::string &, const std::string &) override;
  Value get(Object, const std::string &) override;
  size_t array_size(Object, const std::string &) override;
  void set(Object, const std::string &, const Value &) override;
  CallResult invoke(Object, const std::string &,
                    const std::vector<Value> &) override;
  HookId hook(const std::string &, HookCallback, HookCallback) override;
  void unhook(HookId) override;
  double now() const override;
  uint64_t epoch() const override;
  void invalidate_world() override;
  std::string last_error() const;
};
} // namespace astro
