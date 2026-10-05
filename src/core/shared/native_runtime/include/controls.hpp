#pragma once
#include "settings.hpp"
#include "engine.hpp"
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Shared commands and feedback. Identical algorithm to
// reference/runtime/ingame_controls.lua.

namespace astro {

struct CatalogEntry {
  std::string id;
  std::string name;
  std::string hotkey;
  std::string description;
  std::string load_error;
};

struct ControlRow {
  std::string id;
  std::string name;
  std::string hotkey;
  std::string description;
  bool enabled = false;
  bool active = false;
  std::string error;
  Value number;
};

struct SetModuleResult {
  std::string id;
  bool enabled = false;
  bool applied = false;
  std::string error;
  bool blocked = false;
};

struct Notice {
  double at = 0;
  std::string id;
  std::string name;
  bool enabled = false;
  std::string error;
};

struct HUDView {
  bool show = false;
  double opacity = 0;
  std::string text;
  std::string hint;
  bool enabled = false;
  bool error = false;
  std::string mod_id;
};

// Module apply callback: (enabled, module_id) -> SetModuleResult
using ApplyCallback =
    std::function<SetModuleResult(bool enabled, const std::string &id)>;

// Preferences save callback
using PersistCallback = std::function<bool(const Preferences &)>;

class ControlsManager {
public:
  ControlsManager(const Preferences &initial, ApplyCallback apply,
                  PersistCallback persist, std::vector<CatalogEntry> catalog);

  Preferences preferences() const;
  const std::string &language() const { return language_; }
  bool set_language(const std::string &language);
  SetModuleResult set_module(const std::string &id, bool requested,
                             const std::string &source, double now);
  bool press(const std::string &action, double now);
  void release(const std::string &action);
  void dismiss();
  void suspend();
  void ready(const std::map<std::string, bool> &actual);
  void unavailable(const std::string &id, const std::string &reason);
  std::vector<ControlRow> rows() const;
  HUDView view(double now) const;

  bool is_active() const { return active_; }

private:
  std::string language_ = "zh-CN";
  ApplyCallback apply_;
  PersistCallback persist_;
  std::vector<CatalogEntry> catalog_;
  std::map<std::string, size_t> index_;
  std::map<std::string, bool> values_;
  std::map<std::string, bool> desired_; // persisted intent, independent of availability
  std::map<std::string, std::string> errors_;
  std::map<std::string, bool> down_;
  bool active_ = true;
  std::optional<Notice> notice_;
};

} // namespace astro
