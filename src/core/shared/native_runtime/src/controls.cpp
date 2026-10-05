#include "controls.hpp"
#include "localization.hpp"
#include <cassert>
#include <cmath>

namespace astro {

ControlsManager::ControlsManager(const Preferences &initial,
                                 ApplyCallback apply, PersistCallback persist,
                                 std::vector<CatalogEntry> catalog)
    : language_(i18n::valid(initial.language) ? initial.language : "zh-CN"),
      apply_(std::move(apply)), persist_(std::move(persist)),
      catalog_(std::move(catalog)) {
  // Initialize values from initial preferences
  for (auto &[id, val] : initial.modules) {
    values_[id] = val;
  }
  if (values_.find("universal_slot") == values_.end()) {
    values_["universal_slot"] = initial.enabled;
  }
  desired_ = values_;

  // Validate and index catalog
  std::map<std::string, bool> seen;
  for (size_t i = 0; i < catalog_.size(); i++) {
    auto &entry = catalog_[i];
    if (entry.id.empty() || seen.count(entry.id))
      throw std::invalid_argument("Duplicate/invalid Mod identity");
    seen[entry.id] = true;
    if (entry.name.empty())
      throw std::invalid_argument("Invalid Mod metadata");
    index_[entry.id] = i;
    if (values_.find(entry.id) == values_.end()) {
      values_[entry.id] = false;
      desired_[entry.id] = false;
    }
    if (!entry.load_error.empty()) {
      values_[entry.id] = false;
      errors_[entry.id] = "加载失败，请使用安装器修复";
    }
  }
}

Preferences ControlsManager::preferences() const {
  Preferences p;
  p.enabled =
      desired_.count("universal_slot") ? desired_.at("universal_slot") : false;
  p.visible = false;
  p.modules = desired_;
  p.language = language_;
  return p;
}

bool ControlsManager::set_language(const std::string &language) {
  if (!i18n::valid(language))
    return false;
  if (language == language_)
    return true;
  auto next = preferences();
  next.language = language;
  try {
    if (!persist_ || !persist_(next))
      return false;
  } catch (const std::exception &) {
    return false;
  }
  language_ = language;
  return true;
}

SetModuleResult ControlsManager::set_module(const std::string &id,
                                            bool requested,
                                            const std::string &source,
                                            double now) {
  auto it = index_.find(id);
  if (it == index_.end()) {
    return {id, false, false, "Unknown Mod or invalid state"};
  }
  if (source != "menu" && source != "hotkey")
    throw std::invalid_argument("Invalid command source");

  if (!catalog_[it->second].load_error.empty()) {
    return {id, false, false, i18n::text(language_, errors_[id])};
  }
  if (!active_) {
    return {id, values_[id], false, i18n::text(language_, "等待游戏就绪")};
  }

  notice_.reset(); // menu operations must not resurrect older hotkey notice

  SetModuleResult result;
  result.id = id;
  result.enabled = values_[id];
  try {
    if (apply_)
      result = apply_(requested, id);
  } catch (const std::exception &ex) {
    result.error = ex.what();
    result.applied = false;
  }
  if (!catalog_[it->second].load_error.empty()) result.enabled = false;
  bool confirmed =
      result.enabled == requested && !result.blocked && result.error.empty();
  values_[id] = result.enabled;
  if (confirmed) desired_[id] = result.enabled;
  errors_[id] = confirmed ? "" : "切换失败";

  if (confirmed && persist_) {
    bool saved = false;
    try {
      saved = persist_(preferences());
    } catch (const std::exception &) {
      saved = false;
    }
    if (!saved) {
      errors_[id] = "设置保存失败";
    }
  }

  if (source == "hotkey") {
    notice_ =
        Notice{now, id, catalog_[it->second].name, values_[id], errors_[id]};
  }

  return {id, values_[id], confirmed, i18n::text(language_, errors_[id])};
}

bool ControlsManager::press(const std::string &action, double now) {
  if (action == "visibility")
    return false; // retired persistent HUD shortcut
  std::string id = (action == "feature") ? "universal_slot" : action;
  if (!index_.count(id))
    return false;
  if (down_[action] || !active_)
    return false;
  down_[action] = true;
  set_module(id, !values_[id], "hotkey", now);
  return true;
}

void ControlsManager::release(const std::string &action) {
  down_.erase(action);
}

void ControlsManager::dismiss() { notice_.reset(); }

void ControlsManager::suspend() {
  active_ = false;
  notice_.reset();
  down_.clear();
}

void ControlsManager::ready(const std::map<std::string, bool> &actual) {
  active_ = true;
  for (auto &[id, val] : actual) {
    if (index_.count(id) && (val == true || val == false)) {
      values_[id] = val;
    }
  }
}

void ControlsManager::unavailable(const std::string &id, const std::string &reason) {
  auto found = index_.find(id);
  if (found == index_.end()) return;
  catalog_[found->second].load_error = reason;
  values_[id] = false;
  errors_[id] = reason;
}

std::vector<ControlRow> ControlsManager::rows() const {
  std::vector<ControlRow> result;
  for (auto &entry : catalog_) {
    ControlRow row;
    row.id = entry.id;
    row.name = i18n::module_name(language_, entry.id, entry.name);
    row.hotkey = entry.hotkey;
    row.description = i18n::text(language_, entry.description);
    row.enabled = values_.count(entry.id) ? values_.at(entry.id) : false;
    row.active = active_ && entry.load_error.empty();
    row.error = i18n::text(language_, errors_.count(entry.id) ? errors_.at(entry.id) : "");
    result.push_back(std::move(row));
  }
  return result;
}

HUDView ControlsManager::view(double now) const {
  if (!active_ || !notice_)
    return {false, 0, "", "", false, false, ""};
  double elapsed = now - notice_->at;
  if (elapsed < 0 || elapsed >= 2.6) {
    return {false, 0, "", "", false, false, ""};
  }
  double opacity = 1.0;
  if (elapsed < 0.25)
    opacity = elapsed / 0.25;
  else if (elapsed >= 2.1)
    opacity = (2.6 - elapsed) / 0.5;

  const auto name = i18n::module_name(language_, notice_->id, notice_->name);
  const char *word = notice_->enabled ? "开启" : "关闭";
  std::string text = name + "：已" + word;
  if (!notice_->error.empty())
    text = name + "：" + notice_->error + "（实际" + word + "）";
  if (language_ == "en") {
    const std::string status = notice_->enabled ? "Enabled" : "Disabled";
    text = name + ": " + (notice_->error.empty() ? status
        : i18n::text(language_, notice_->error) + " (actual: " + status + ")");
  }
  return {true,       opacity,          text,
          "",         notice_->enabled, !notice_->error.empty(),
          notice_->id};
}

} // namespace astro
