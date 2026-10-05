// Win32 native UI for Astroneer Mod Installer.
// 600x320 logical pixels, Chinese UI, DPI-aware, embedded B icon.
// Uses native controls: static text, edit, buttons, checkbox, listbox,
// progress.
#pragma once
#include "json.hpp"
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace ui {

struct AppState {
  std::string game_path;
  std::string message;
  bool has_error = false;
  bool running = false;
  bool legacy = false;
  bool valid = false;
  bool busy = false;
  bool menu_installed = false;
  bool menu_offered = false;
  bool backup_saves = false;
  bool start_uninstall = false; // Open confirmation only; never execute unattended.
  std::string game_version;
  std::string pending_action;
  std::string pending_text;
  bool requires_ack = false;
  struct ModuleRow {
    std::string id;
    std::string name;
    std::string status;
    bool installed = false;
    bool selected = false;
    std::string description;
    bool enabled = false;
    bool offered = false;
    std::string name_en, description_en;
  };
  std::vector<ModuleRow> modules;
  std::string backup_path;
  json::Value operation_result;
};

using RefreshFn = std::function<void()>;
using ChooseGameFn = std::function<void()>;
using SetGamePathFn = std::function<void(const std::string &)>;
using InstallFn = std::function<void()>;
using UninstallFn = std::function<void()>;
using ConfirmFn = std::function<void(bool)>;
using CancelFn = std::function<void()>;
using OpenBackupsFn = std::function<void()>;
using OpenCommunityFn = std::function<void()>;

struct Callbacks {
  RefreshFn refresh;
  ChooseGameFn choose_game;
  SetGamePathFn set_game_path;
  InstallFn install;
  UninstallFn uninstall;
  InstallFn install_menu;
  UninstallFn uninstall_menu;
  std::function<void(const std::string &, bool)> toggle;
  ConfirmFn confirm;
  CancelFn cancel;
  OpenBackupsFn open_backups;
  OpenCommunityFn open_community;
  std::function<void(const AppState &)> validate_report;
};

// Run the UI. Returns exit code. report_path optional for smoke test.
AppState &state();
std::string language();
void set_smoke_dpi(int dpi);
void set_smoke_action(const std::string &action);
void run_work(std::function<AppState(AppState)> operation);
int run_ui(const AppState &initial, Callbacks cb, const std::string &report_path = "");

// Smoke test: create a window, verify controls, take screenshot, close.
int smoke_test(const AppState &initial, Callbacks cb, const std::string &report_path);

} // namespace ui
