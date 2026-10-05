#pragma once
// Frozen release facts from reference/catalog.py. Installer packaging is a
// separate deliverable; all authored business services compile into one DLL.
namespace astro::catalog {
inline constexpr const char *VERSION = "0.2.0";
inline constexpr const char *CORE =
    "Astro/Binaries/Win64/Mods/AstroConsoleCore";
inline constexpr const char *DLL =
    "Astro/Binaries/Win64/Mods/AstroConsoleCore/dlls/main.dll";
inline constexpr const char *PAK = "Astro/Content/Paks/AstroSupportCard_P.pak";
inline constexpr const char *CARD_REVISION = "ff66722d2bc8a825245a";
inline constexpr const char *CARD_SHA256 =
    "0e9d37ae18216cb9cee939ea63e1e4bedfb49460b64ebfdcac346a1ff0e51ec2";
} // namespace astro::catalog
