#pragma once
#include "engine.hpp"
namespace astro {
// In-process decorator only; the private cross-DLL Engine vtable stays unchanged.
class ForwardingEngine : public Engine {
protected:
  Engine &target;
public:
  explicit ForwardingEngine(Engine &engine) : target(engine) {}
  bool valid(Object o) const override { return target.valid(o); }
  std::string name(Object o) override { return target.name(o); }
  Object find(const std::string &p) override { return target.find(p); }
  std::vector<Object> all(const std::string &p) override { return target.all(p); }
  Object construct(const std::string &p, Object outer, const std::string &n) override { return target.construct(p, outer, n); }
  bool has(Object o, const std::string &p) override { return target.has(o, p); }
  bool is_a(Object o, const std::string &p) override { return target.is_a(o, p); }
  int enum_value(const std::string &p, const std::string &m) override { return target.enum_value(p, m); }
  Value get(Object o, const std::string &p) override { return target.get(o, p); }
  size_t array_size(Object o, const std::string &p) override { return target.array_size(o, p); }
  void set(Object o, const std::string &p, const Value &v) override { target.set(o, p, v); }
  CallResult invoke(Object o, const std::string &f, const std::vector<Value> &args) override { return target.invoke(o, f, args); }
  HookId hook(const std::string &p, HookCallback pre, HookCallback post) override { return target.hook(p, std::move(pre), std::move(post)); }
  void unhook(HookId id) override { target.unhook(id); }
  double now() const override { return target.now(); }
  uint64_t epoch() const override { return target.epoch(); }
  void invalidate_world() override { target.invalidate_world(); }
};
} // namespace astro
