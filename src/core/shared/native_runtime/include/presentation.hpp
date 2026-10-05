#pragma once
#include "controls.hpp"
#include "engine.hpp"
#include <memory>
namespace astro {
class Presentation {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  explicit Presentation(Engine &);
  ~Presentation();
  void update(Object gi, Object menu, const std::vector<ControlRow> &,
              std::function<SetModuleResult(const std::string &, bool)> change,
              const HUDView &, const std::string &language = "zh-CN",
              std::function<bool(const std::string &)> change_language = {},
              std::function<Value(const std::string &,double)> change_number = {});
  void tick_card();
  void close();
  // Game-thread pre-invalidation cleanup; discard remains the no-touch fallback.
  void suspend();
  void discard();
  Value snapshot();
};
} // namespace astro
