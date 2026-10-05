#include "registry_v2.hpp"
#include "json.hpp"
#include <algorithm>
#include <cstring>
#include <set>

namespace registry {
namespace {
void hash_format(const std::string &s) {
  if (s.size() != 64 || s.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("Invalid ownership hash");
}
bool identity(const std::string &id) {
  if (id.empty() || id.size() > 64 || id[0] < 'a' || id[0] > 'z')
    return false;
  return id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") ==
         std::string::npos;
}
bool version_text(const std::string &s) {
  return !s.empty() && s.size() <= 32 &&
         s.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "abcdefghijklmnopqrstuvwxyz._+-") == std::string::npos;
}
bool rel_path(const std::string &r) {
  if (r.empty() || r.size() > 240 || r.front() == '/' || r.back() == '/' ||
      r.find("//") != std::string::npos || r.find('\\') != std::string::npos ||
      r.find(':') != std::string::npos)
    return false;
  size_t start = 0;
  int segments = 0;
  while (start <= r.size()) {
    auto cut = r.find('/', start);
    auto seg = r.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
    if (seg.empty() || seg.size() > 64 || seg == "." || seg == ".." ||
        seg.back() == '.' || seg.back() == ' ')
      return false;
    ++segments;
    if (cut == std::string::npos)
      break;
    start = cut + 1;
  }
  return segments >= 1 && segments <= 8;
}
} // namespace

std::vector<uint8_t> to_bytes(const Authority &a) {
  json::Object modules, common, files, receipts;
  json::Array logs, dirs;
  for (auto &[id, m] : a.modules) {
    if (!identity(id))
      throw std::runtime_error("Invalid module identity in authority");
    modules[id] = json::Object{{"version", m.version},
                               {"revision", m.revision},
                               {"source_package", m.source_package},
                               {"envelope", m.envelope},
                               {"enabled", m.enabled}};
  }
  for (auto &[role, c] : a.common) {
    common[role] = json::Object{{"version", c.version},
                                {"revision", c.revision},
                                {"sha256", c.sha256},
                                {"rel", c.rel},
                                {"pointer", c.pointer},
                                {"owned", c.owned}};
  }
  for (auto &[r, e] : a.files) {
    files[r] = json::Object{
        {"component", e.component}, {"sha256", e.sha256}, {"owner", e.owner}};
  }
  for (auto &r : a.generated_loader_files)
    logs.push_back(r);
  for (auto &r : a.owned_directories)
    dirs.push_back(r);
  for (auto &[r, rec] : a.directory_receipts) {
    json::Object value{{"volume_serial", int64_t(rec.volume_serial)},
                       {"file_index_high", int64_t(rec.file_index_high)},
                       {"file_index_low", int64_t(rec.file_index_low)}};
    if (rec.version != 0) {
      if (!rec.valid())
        throw std::runtime_error("Invalid current directory receipt");
      value["version"] = 2;
      value["filesystem"] = "NTFS";
      value["volume_id"] = io::receipt_hex(rec.volume_id);
      value["file_id"] = io::receipt_file_id(rec);
      value["creation_time"] = io::receipt_hex(rec.creation_time);
    }
    receipts[r] = value;
  }
  json::Value doc = json::Object{{"schema", int64_t(authority_schema)},
                                 {"product", authority_product},
                                 {"kind", authority_kind},
                                 {"version", a.version},
                                 {"game_root", a.game_root},
                                 {"sequence", a.sequence},
                                 {"loader_owned", a.loader_owned},
                                 {"modules", modules},
                                 {"common", common},
                                 {"files", files},
                                 {"generated_loader_files", logs},
                                 {"owned_directories", dirs},
                                 {"directory_receipts", receipts}};
  const auto encoded = json::serialize(doc);
  return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

Authority parse(const std::string &text) {
  if (text.empty() || text.size() > 1024 * 1024)
    throw std::runtime_error("Install authority too large");
  auto v = json::parse(text);
  if (!v.is_object() || v.size() != 13)
    throw std::runtime_error("Install authority malformed");
  static const char *keys[] = {"schema",        "product",      "kind",
                              "version",       "game_root",    "sequence",
                              "loader_owned",  "modules",      "common",
                              "files",         "generated_loader_files",
                              "owned_directories", "directory_receipts"};
  for (auto *k : keys)
    if (!v.has(k))
      throw std::runtime_error("Install authority missing field");
  Authority a;
  if (!v["schema"].is_integer() || v["schema"].as_int64() != authority_schema)
    throw std::runtime_error("Unsupported install authority schema");
  // Record the validated schema only after the version check above accepted it.
  // The Authority default of 0 means "no authority on disk"; leaving it at 0
  // would make a valid installed authority look absent to every caller.
  a.schema = authority_schema;
  if (!v["product"].is_string() || v["product"].as_string() != authority_product ||
      !v["kind"].is_string() || v["kind"].as_string() != authority_kind)
    throw std::runtime_error("Install authority product mismatch");
  a.version = v["version"].as_string();
  if (!version_text(a.version))
    throw std::runtime_error("Invalid authority version");
  a.game_root = v["game_root"].as_string();
  if (a.game_root.empty() || a.game_root.size() > 1024)
    throw std::runtime_error("Invalid authority game root");
  if (!v["sequence"].is_integer() || v["sequence"].as_int64() < 0 ||
      v["sequence"].as_int64() > 1000000000)
    throw std::runtime_error("Invalid authority sequence");
  a.sequence = v["sequence"].as_int64();
  if (!v["loader_owned"].is_bool())
    throw std::runtime_error("Invalid loader ownership flag");
  a.loader_owned = v["loader_owned"].as_bool();

  if (v["modules"].size() > 32)
    throw std::runtime_error("Install authority module bound exceeded");
  for (auto &[id, row] : v["modules"].as_object()) {
    if (!identity(id) || !row.is_object() || row.size() != 5)
      throw std::runtime_error("Invalid installed module record");
    if (!row.has("version") || !row.has("revision") || !row.has("source_package") ||
        !row.has("envelope") || !row.has("enabled"))
      throw std::runtime_error("Invalid installed module record");
    ModuleRecord m;
    m.version = row["version"].as_string();
    if (!version_text(m.version))
      throw std::runtime_error("Invalid installed module version");
    if (!row["revision"].is_integer() || row["revision"].as_int64() < 0 ||
        row["revision"].as_int64() > 1000000000)
      throw std::runtime_error("Invalid installed module revision");
    m.revision = row["revision"].as_int64();
    m.source_package = row["source_package"].as_string();
    if (m.source_package.size() > 64)
      throw std::runtime_error("Invalid module source package");
    m.envelope = row["envelope"].as_string();
    if (m.envelope.empty() || m.envelope.size() > 8192)
      throw std::runtime_error("Invalid module envelope retention");
    if (!row["enabled"].is_bool())
      throw std::runtime_error("Invalid module preference flag");
    m.enabled = row["enabled"].as_bool();
    a.modules[id] = std::move(m);
  }

  if (v["common"].size() > 8)
    throw std::runtime_error("Install authority common bound exceeded");
  for (auto &[role, row] : v["common"].as_object()) {
    if (role != common_core && role != common_loader && role != common_manager)
      throw std::runtime_error("Unknown common component role");
    if (!row.is_object() || row.size() != 6)
      throw std::runtime_error("Invalid common component record");
    CommonRecord c;
    c.version = row["version"].as_string();
    if (!version_text(c.version))
      throw std::runtime_error("Invalid common component version");
    if (!row["revision"].is_integer() || row["revision"].as_int64() < 0 ||
        row["revision"].as_int64() > 1000000000)
      throw std::runtime_error("Invalid common component revision");
    c.revision = row["revision"].as_int64();
    c.sha256 = row["sha256"].as_string();
    hash_format(c.sha256);
    c.rel = row["rel"].as_string();
    c.pointer = row["pointer"].as_string();
    if (!c.rel.empty() && !rel_path(c.rel))
      throw std::runtime_error("Invalid manager install path");
    if (!c.pointer.empty() && !rel_path(c.pointer))
      throw std::runtime_error("Invalid manager pointer path");
    if (!row["owned"].is_bool())
      throw std::runtime_error("Invalid common ownership flag");
    c.owned = row["owned"].as_bool();
    a.common[role] = std::move(c);
  }

  if (v["files"].size() > 256)
    throw std::runtime_error("Install authority file bound exceeded");
  for (auto &[r, row] : v["files"].as_object()) {
    if (!rel_path(r) || !row.is_object() || row.size() != 3)
      throw std::runtime_error("Install authority file row malformed");
    if (!row.has("component") || !row.has("sha256") || !row.has("owner"))
      throw std::runtime_error("Install authority file row malformed");
    FileRow e;
    e.component = row["component"].as_string();
    e.sha256 = row["sha256"].as_string();
    e.owner = row["owner"].as_string();
    hash_format(e.sha256);
    if (e.component.empty() || e.component.size() > 64 ||
        e.owner.empty() || e.owner.size() > 64)
      throw std::runtime_error("Invalid file ownership binding");
    if (e.owner != role_core && e.owner != role_loader && e.owner != role_data &&
        e.owner != role_manager && !identity(e.owner))
      throw std::runtime_error("Invalid file owner identity");
    a.files[r] = std::move(e);
  }

  std::set<std::string> logs;
  for (auto &x : v["generated_loader_files"].as_array()) {
    auto r = x.as_string();
    if (!rel_path(r) || !logs.insert(r).second)
      throw std::runtime_error("Invalid generated loader ownership");
    a.generated_loader_files.push_back(r);
  }
  std::set<std::string> dirs;
  for (auto &x : v["owned_directories"].as_array()) {
    auto r = x.as_string();
    if (!rel_path(r) || !dirs.insert(r).second)
      throw std::runtime_error("Invalid directory ownership");
    a.owned_directories.push_back(r);
  }
  for (auto &[r, val] : v["directory_receipts"].as_object()) {
    if (!dirs.count(r))
      throw std::runtime_error("Receipt for non-owned directory");
    io::DirectoryReceipt rec;
    auto vs = val["volume_serial"].as_int64();
    auto fih = val["file_index_high"].as_int64();
    auto fil = val["file_index_low"].as_int64();
    if (vs < 0 || vs > UINT32_MAX || fih < 0 || fih > UINT32_MAX || fil < 0 ||
        fil > UINT32_MAX)
      throw std::runtime_error("Invalid directory receipt values");
    rec.volume_serial = uint32_t(vs);
    rec.file_index_high = uint32_t(fih);
    rec.file_index_low = uint32_t(fil);
    if (val.has("version")) {
      if (val.size() != 8 || !val["version"].is_integer() ||
          val["version"].as_int64() != 2 || val["filesystem"].as_string() != "NTFS")
        throw std::runtime_error("Unsupported directory receipt version");
      rec.version = 2;
      rec.volume_id = io::receipt_number(val["volume_id"].as_string());
      rec.creation_time = io::receipt_number(val["creation_time"].as_string());
      auto id = val["file_id"].as_string();
      if (id.size() != 32)
        throw std::runtime_error("Invalid directory file ID width");
      for (size_t i = 0; i != rec.file_id.size(); ++i)
        rec.file_id[i] = uint8_t(io::receipt_number(id.substr(i * 2, 2), 2));
      if (!rec.valid())
        throw std::runtime_error("Invalid current directory receipt");
    } else if (val.size() != 3) {
      throw std::runtime_error("Invalid legacy directory receipt");
    }
    a.directory_receipts[r] = rec;
  }
  return a;
}

std::string native_config_projection(const Authority &a) {
  json::Array ids;
  for (auto &[id, m] : a.modules)
    if (m.enabled)
      ids.push_back(id);
  json::Value doc = json::Object{{"schema", 1},
                                 {"product", "AstroConsoleCore"},
                                 {"presentation", true},
                                 {"development", false},
                                 {"installed_modules", ids}};
  return json::serialize(doc);
}

std::string installed_mods_projection(const Authority &a) {
  json::Object mods;
  for (auto &[id, m] : a.modules)
    mods[id] = m.version;
  json::Value doc = json::Object{{"schema", int64_t(authority_schema)},
                                 {"product", authority_product},
                                 {"kind", "installed_mods_index"},
                                 {"sequence", a.sequence},
                                 {"modules", mods}};
  return json::serialize(doc);
}

std::string installed_records_projection(const Authority &a) {
  // auth_record v1 registry aggregate. Each envelope is spliced in as its exact
  // stored bytes so the signed payload string is preserved verbatim.
  std::string out = "{\"schema\":1,\"product\":\"AstroModRecord\",\"kind\":\"module_registry\",\"records\":[";
  bool first = true;
  for (auto &[id, m] : a.modules) {
    (void)id;
    if (!first)
      out.push_back(',');
    out += m.envelope;
    first = false;
  }
  out += "]}";
  return out;
}

std::string manager_pointer_projection(const Authority &a) {
  auto it = a.common.find(common_manager);
  if (it == a.common.end() || it->second.rel.empty())
    return {};
  const auto image = a.files.find(it->second.rel);
  if (image == a.files.end())
    throw std::runtime_error("Manager executable ownership missing");
  json::Value doc = json::Object{{"schema", 1},
                                 {"product", "AstroModManager"},
                                 {"kind", "manager_pointer"},
                                 {"version", it->second.version},
                                 {"revision", it->second.revision},
                                 {"rel", it->second.rel},
                                 {"sha256", image->second.sha256}};
  return json::serialize(doc);
}

VersionDecision decide_common(const CommonRecord *installed,
                              const CommonRecord &offered) {
  if (!installed)
    return {Selection::install, "no installed common component"};
  if (offered.revision > installed->revision)
    return {Selection::install, "offered revision is newer"};
  if (offered.revision < installed->revision)
    return {Selection::retain, "installed revision is newer"};
  if (offered.version != installed->version)
    return {Selection::incompatible, "same revision, different version"};
  if (offered.sha256 != installed->sha256)
    return {Selection::conflict, "same version/revision, different immutable bytes"};
  return {Selection::retain, "installed component is current"};
}

ModuleVerdict decide_module(const ModuleRecord *installed,
                            const ModuleRecord &offered,
                            const std::string &offered_sha256,
                            const std::string &installed_sha256) {
  if (!installed)
    return {ModuleDecision::install, "module not installed"};
  if (offered.revision > installed->revision)
    return {ModuleDecision::install, "offered module revision is newer"};
  if (offered.revision < installed->revision)
    return {ModuleDecision::retain, "installed module revision is newer"};
  if (offered.version != installed->version)
    return {ModuleDecision::conflict, "same module revision, different version"};
  if (offered_sha256 != installed_sha256)
    return {ModuleDecision::conflict, "same module revision, different immutable bytes"};
  return {ModuleDecision::retain, "installed module is current"};
}

} // namespace registry
