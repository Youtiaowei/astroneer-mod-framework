#pragma once
// Native R7 ownership is unsigned historical metadata, not a module record.
// Parse it separately; never synthesize envelopes or publish schema 1.
#include "payload_catalog.h"
#include "registry_v2.hpp"
#include "json.hpp"
#include <set>
#include <optional>

namespace legacy {
struct NativeAuthority {
  registry::Authority ownership;
  std::map<std::string, std::string> modules;
  bool slot_enabled = false;
};
inline std::optional<std::pair<int64_t, bool>> journal_intent(const json::Value &v) {
  try {
    if (!v.is_object() || !v["schema"].is_integer() || !v["seq"].is_integer()) return {};
    const auto schema = v["schema"].as_int64(), seq = v["seq"].as_int64();
    if ((schema != 1 && schema != 2) || seq < 1 || seq >= 1000000000) return {};
    const auto on = v["enabled"].as_bool();
    (void)v["visible"].as_bool();
    if (schema == 2) {
      const auto &mods = v["modules"].as_object();
      if (mods.empty() || mods.size() > 32 || !mods.count("universal_slot") ||
          mods.at("universal_slot").as_bool() != on) return {};
      for (const auto &[id, flag] : mods) {
        if (id.empty() || id.size() > 64 || id.front() < 'a' || id.front() > 'z' ||
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) return {};
        (void)flag.as_bool();
      }
    }
    return std::pair{seq, on};
  } catch (const std::exception &) { return {}; }
}
inline bool recognized_core(const std::string &hash) {
  // R7 shipping Core and the source/receipt-bound presentation-only cold update.
  static const std::set<std::string> hashes{
      "4b168d84311ef24935be460b50839b8ad1ca4e65121442dcdc760ad74c8e19a7",
      "810c3c21d23f8672fc417c4371017866402e36d3483621fe54dbd1d425686a27"};
  return hashes.count(hash) != 0;
}
inline NativeAuthority parse_native(const json::Value &v) {
  static const std::set<std::string> fields{
      "schema", "product", "version", "game_root", "loader_owned", "modules",
      "files", "generated_loader_files", "owned_directories", "directory_receipts"};
  if (!v.is_object() || v.size() != fields.size())
    throw std::runtime_error("Unsupported legacy native authority");
  for (const auto &key : fields)
    if (!v.has(key)) throw std::runtime_error("Incomplete legacy native authority");
  if (v["schema"].as_int64() != 1 ||
      v["product"].as_string() != registry::authority_product ||
      v["version"].as_string() != "0.2.0")
    throw std::runtime_error("Unsupported legacy native authority");
  NativeAuthority result;
  for (const auto &[id, version] : v["modules"].as_object()) {
    if (id != "universal_slot" || version.as_string() != "0.2.0")
      throw std::runtime_error("Unsupported legacy native module");
    result.modules[id] = version.as_string();
  }
  std::map<std::string, std::string> targets{
      {catalog::NATIVE_DLL_TARGET, registry::role_core},
      {catalog::CONFIG_TARGET, registry::role_core},
      {catalog::PAK, registry::role_core},
      {std::string(catalog::CORE) + "/enabled.txt", registry::role_core}};
  for (const auto &name : catalog::MUTABLE())
    targets[std::string(catalog::STATE) + "/data/" + name] = registry::role_data;
  for (const auto &path : catalog::COMMON_ALLOWED().at(registry::common_loader))
    targets[path] = registry::role_loader;
  json::Object rows;
  for (const auto &[path, row] : v["files"].as_object()) {
    const auto target = targets.find(path);
    if (target == targets.end() || !row.is_object() || row.size() != 2 ||
        !row.has("component") || !row.has("sha256") ||
        row["component"].as_string() != target->second)
      throw std::runtime_error("Invalid legacy native ownership");
    rows[path] = json::Object{{"component", target->second},
                             {"owner", target->second}, {"sha256", row["sha256"]}};
  }
  // Reuse strict typed path/hash/receipt validation, with no signed modules.
  auto physical = v.as_object();
  physical["schema"] = 2;
  physical["kind"] = registry::authority_kind;
  physical["sequence"] = 0;
  physical["modules"] = json::Object{};
  physical["common"] = json::Object{};
  physical["files"] = rows;
  result.ownership = registry::parse(json::serialize(physical));
  for (const auto &path : {std::string(catalog::NATIVE_DLL_TARGET),
                          std::string(catalog::CONFIG_TARGET), std::string(catalog::PAK),
                          std::string(catalog::CORE) + "/enabled.txt",
                          std::string(catalog::STATE) + "/data/bridge-token"})
    if (!result.ownership.files.count(path))
      throw std::runtime_error("Incomplete legacy native ownership");
  const std::set<std::string> directories{
      catalog::CORE, catalog::SCRIPTS, std::string(catalog::CORE) + "/dlls",
      catalog::STATE, std::string(catalog::STATE) + "/data",
      std::string(catalog::STATE) + "/notices"};
  for (const auto &path : result.ownership.owned_directories)
    if (!directories.count(path))
      throw std::runtime_error("Invalid legacy native directory ownership");
  for (const auto &path : result.ownership.generated_loader_files)
    if (!result.ownership.loader_owned || path != std::string(catalog::WIN64) + "/UE4SS.log")
      throw std::runtime_error("Invalid legacy native loader ownership");
  if (result.ownership.loader_owned)
    for (const auto &path : catalog::COMMON_ALLOWED().at(registry::common_loader))
      if (!result.ownership.files.count(path))
        throw std::runtime_error("Incomplete legacy native loader ownership");
  for (const auto &[path, row] : result.ownership.files)
    if (row.owner == registry::role_loader && !result.ownership.loader_owned)
      throw std::runtime_error("Invalid legacy native loader ownership");
  return result;
}
} // namespace legacy
