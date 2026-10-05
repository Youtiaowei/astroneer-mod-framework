// Catalog constants matching reference/src/installer_app/catalog.py.
// All string paths use forward slashes (POSIX style) internally.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace catalog {

inline constexpr const char *VERSION = "0.3.0";
inline constexpr const char *COMMUNITY_URL = "https://qm.qq.com/q/WqpP5KQuwC";
inline constexpr const char *WIN64 = "Astro/Binaries/Win64";
inline constexpr const char *MODS = "Astro/Binaries/Win64/Mods";
inline constexpr const char *CORE = "Astro/Binaries/Win64/Mods/AstroConsoleCore";
inline constexpr const char *SCRIPTS = "Astro/Binaries/Win64/Mods/AstroConsoleCore/Scripts";
inline constexpr const char *STATE = "AstroModManager";
inline constexpr const char *PAK = "Astro/Content/Paks/AstroSupportCard_P.pak";
// Closed common destination targets (see payload_catalog.h).
inline constexpr const char *NATIVE_DLL_TARGET =
    "Astro/Binaries/Win64/Mods/AstroConsoleCore/dlls/main.dll";
inline constexpr const char *CONFIG_TARGET =
    "Astro/Binaries/Win64/Mods/AstroConsoleCore/native_config.json";
inline constexpr const char *LOADER_DLL_TARGET = "Astro/Binaries/Win64/UE4SS.dll";
inline constexpr const char *PROXY_DLL_TARGET = "Astro/Binaries/Win64/dwmapi.dll";
inline constexpr const char *LOADER_INI_TARGET =
    "Astro/Binaries/Win64/UE4SS-settings.ini";
inline constexpr const char *LICENSE_TARGET =
    "AstroModManager/notices/UE4SS-LICENSE.txt";
inline constexpr const char *MANAGER_EXE_TARGET =
    "AstroModManager/bin/manager.exe";
inline constexpr const char *MANAGER_POINTER_TARGET =
    "AstroModManager/manager.pointer";
inline constexpr const char *CARD_REVISION = "ff66722d2bc8a825245a";
inline constexpr const char *CARD_SHA256 =
    "0e9d37ae18216cb9cee939ea63e1e4bedfb49460b64ebfdcac346a1ff0e51ec2";
// Prior accepted card remains explicitly trusted only for authorized legacy
// adoption. Payload validation continues to require the new CARD_SHA256.
inline constexpr const char *LEGACY_CARD_SHA256 =
    "994100e8b9206a934546b15536187a5d7179a947e3080bfa2fdaed1dfd5034c5";
inline constexpr const char *LEGACY_PROXY_SHA256 =
    "f2f9e57f7707c8456b117391b1ab45fd7149332f89a3a94726eda0cc2300e834";

struct ModuleInfo {
  std::string name;
  std::string version;
  std::string entry;
  std::string description;
};

inline const std::map<std::string, ModuleInfo> &MODULES() {
  static const std::map<std::string, ModuleInfo> m = {
      {"universal_slot",
       {"万能插槽", VERSION, "universal_slot.lua",
        "大件放入背包、平台和设备的小插槽。游戏中按 F8 或打开 ESC "
        "菜单切换。"}}};
  return m;
}

inline const std::vector<std::string> &RUNTIME() {
  static const std::vector<std::string> r = {"main.lua",
                                             "universal_slot.lua",
                                             "json.lua",
                                             "ingame_controls.lua",
                                             "ingame_hud.lua",
                                             "ingame_settings.lua",
                                             "ingame_runtime.lua",
                                             "ingame_panel.lua",
                                             "side_panel_layout.lua",
                                             "support_card_config.lua",
                                             "support_card_state.lua",
                                             "support_card_projection.lua",
                                             "support_card_view.lua"};
  return r;
}

inline const std::vector<std::string> &MUTABLE() {
  static const std::vector<std::string> m = {"bridge-token",           "runtime-state.json",
                                             "request.json",           "settings.json",
                                             "ingame-settings-a.json", "ingame-settings-b.json"};
  return m;
}

} // namespace catalog
