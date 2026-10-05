#pragma once
#include "safe_io.h"
#include <regex>
namespace discovery {
inline std::vector<std::string> values(const io::fs::path &p, const std::string &key) {
  std::vector<std::string> out;
  if (!io::file(p))
    return out;
  auto text = io::text(io::read(p, 1024 * 1024));
  std::regex re("\"" + key + "\"\\s*\"((?:\\\\.|[^\"\\r\\n])*)\"");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator();
       ++it) {
    if (out.size() >= 128)
      throw std::runtime_error("Steam library bound");
    auto s = (*it)[1].str();
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\')
        ++i;
      r += s[i];
    }
    out.push_back(r);
  }
  return out;
}
inline std::string registry(HKEY hive, const wchar_t *key, const wchar_t *name) {
  wchar_t b[32768];
  DWORD n = sizeof(b);
  auto e = RegGetValueW(hive, key, name, RRF_RT_REG_SZ, nullptr, b, &n);
  if (e == ERROR_FILE_NOT_FOUND)
    return {};
  if (e != ERROR_SUCCESS)
    throw std::runtime_error("Steam registry read failed: " + std::to_string(e));
  return io::utf8(b);
}
inline std::string find_game() {
  std::vector<std::string> candidates, libraries;
  for (auto key : {L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\S"
                   L"team App 361420",
                   L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion"
                   L"\\Uninstall\\Steam App 361420"}) {
    auto s = registry(HKEY_LOCAL_MACHINE, key, L"InstallLocation");
    if (!s.empty())
      candidates.push_back(s);
  }
  auto steam = registry(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (!steam.empty()) {
    libraries.push_back(steam);
    for (auto &s : values(io::absolute(steam) / L"steamapps/libraryfolders.vdf", "path"))
      libraries.push_back(s);
  }
  for (auto &l : libraries) {
    auto p = io::absolute(l);
    auto names = values(p / L"steamapps/appmanifest_361420.acf", "installdir");
    std::string name = names.empty() ? "ASTRONEER" : names[0];
    io::component(io::wide(name));
    if (name.find_first_of("/\\") != name.npos)
      throw std::runtime_error("Invalid Steam install dir");
    candidates.push_back(io::str(p / L"steamapps/common" / io::path(name)));
  }
  for (auto &c : candidates) {
    auto p = io::absolute(c);
    if (io::file(p / L"Astro/Binaries/Win64/Astro-Win64-Shipping.exe"))
      return io::str(p);
  }
  return {};
}
} // namespace discovery
