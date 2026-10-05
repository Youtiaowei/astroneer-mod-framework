#pragma once
#include "files.hpp"
#include <map>
#include <optional>
#include <string>

// Two bounded journals for ingame settings. Read schema 1, write schema 2.
// Identical algorithm to reference/runtime/ingame_settings.lua.

namespace astro {

struct Preferences {
  int schema = 2;
  int seq = 0;
  std::string language = "zh-CN";
  bool enabled = false;
  bool visible = true;
  std::map<std::string, bool> modules;
  std::map<std::string, double> parameters;
};

class SettingsManager {
public:
  SettingsManager(Files &files, bool fallback_enabled, bool fallback_visible);

  // Read current preferences (from the latest journal entry).
  Preferences read() const;

  // Save preferences. Returns true on success.
  // Validates, increments seq, writes atomically, verifies readback.
  bool save(const Preferences &prefs);

  const std::string &last_error() const { return last_error_; }

private:
  Files &files_;
  std::string path_a_;
  std::string path_b_;
  Preferences current_;
  std::string last_error_;

  static bool modules_valid(const std::map<std::string, bool> &modules);
  static std::optional<Preferences> parse_journal(const std::string &text);
};

} // namespace astro
