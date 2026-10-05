#include "engine.h"
#include "payload_catalog.h"
#include "embedded_manifest.h"
#include "safe_io.h"
#include "auth_record.hpp"
#include <algorithm>
#include <set>
using namespace catalog;
namespace engine {
namespace {
void text_bound(const std::string &s, size_t limit, const char *what) {
  if (s.empty() || s.size() > limit)
    throw InstallError(std::string("Invalid package ") + what);
}
bool version_text(const std::string &s) {
  return !s.empty() && s.size() <= 32 &&
         s.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "abcdefghijklmnopqrstuvwxyz._+-") == std::string::npos;
}
bool module_identity(const std::string &id) {
  if (id.empty() || id.size() > 64 || id[0] < 'a' || id[0] > 'z')
    return false;
  return id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") ==
         std::string::npos;
}
} // namespace

Manifest Manager::load_manifest(const std::string &p) {
  auto raw = io::read(io::path(p), 1024 * 1024);
  auto v = json::parse(io::text(raw));
  if (!v.is_object() || v.size() != 11)
    throw InstallError("Unsupported native manifest");
  Manifest m;
  for (auto *k : {"schema", "product", "kind", "package_id", "version",
                  "game_version", "card_revision", "payload_kind", "modules",
                  "common", "files"})
    if (!v.has(k))
      throw InstallError("Incomplete native manifest");
  if (!v["schema"].is_integer() || v["schema"].as_int64() != 3 ||
      v["product"].as_string() != "AstroneerModInstaller" ||
      v["kind"].as_string() != "package")
    throw InstallError("Unsupported native manifest");
  m.schema = 3;
  m.product = v["product"].as_string();
  text_bound(v["package_id"].as_string(), 64, "package id");
  if (!module_identity(v["package_id"].as_string()))
    throw InstallError("Invalid package id");
  m.package_id = v["package_id"].as_string();
  m.version = v["version"].as_string();
  if (!version_text(m.version) || m.version != VERSION)
    throw InstallError("Package version mismatch");
  m.game_version = v["game_version"].as_string();
  m.card_revision = v["card_revision"].as_string();
  if (m.card_revision != CARD_REVISION)
    throw InstallError("Unsupported card revision");
  m.payload_kind = v["payload_kind"].as_string();
  // The payload kind is bound at build time: a fixture build only accepts a
  // fixture-marked package, and vice versa.
  if (m.payload_kind != EMBEDDED_PAYLOAD_KIND)
    throw InstallError("Payload kind mismatch");
  if (v["files"].size() > 64 || v["modules"].size() > 8 || v["common"].size() > 8)
    throw InstallError("Package bound exceeded");
  for (auto &r : v["modules"].as_array()) {
    if (!r.is_object() || r.size() != 5)
      throw InstallError("Invalid module offer row");
    OfferedModule o;
    o.id = r["id"].as_string();
    o.version = r["version"].as_string();
    o.envelope = r["envelope"].as_string();
    o.source = r["source"].as_string();
    if (!module_identity(o.id) || !version_text(o.version) || o.envelope.empty() ||
        o.envelope.size() > astro::auth::envelope_byte_limit)
      throw InstallError("Invalid module offer row");
    if (!r["revision"].is_integer() || r["revision"].as_int64() < 0 ||
        r["revision"].as_int64() > 1000000000)
      throw InstallError("Invalid module offer revision");
    o.revision = r["revision"].as_int64();
    for (auto &existing : m.modules)
      if (existing.id == o.id)
        throw InstallError("Duplicate module offer");
    m.modules.push_back(std::move(o));
  }
  for (auto &r : v["common"].as_array()) {
    if (!r.is_object() || r.size() != 5)
      throw InstallError("Invalid common offer row");
    OfferedCommon c;
    c.role = r["role"].as_string();
    c.version = r["version"].as_string();
    c.envelope = r["envelope"].as_string();
    c.sha256 = r["sha256"].as_string();
    if (!astro::pkg::closed_component(c.role) || !version_text(c.version) ||
        c.envelope.empty() || c.envelope.size() > astro::pkg::envelope_byte_limit ||
        c.sha256.size() != 64 ||
        c.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw InstallError("Invalid common offer row");
    if (!r["revision"].is_integer() || r["revision"].as_int64() < 0 ||
        r["revision"].as_int64() > 1000000000)
      throw InstallError("Invalid common offer revision");
    c.revision = r["revision"].as_int64();
    for (auto &existing : m.common)
      if (existing.role == c.role)
        throw InstallError("Duplicate common offer");
    m.common.push_back(std::move(c));
  }
  for (auto &r : v["files"].as_array()) {
    if (!r.is_object() || r.size() != 5 || !r["size"].is_integer())
      throw InstallError("Invalid file row");
    FileEntry e{r["source"].as_string(), r["target"].as_string(),
                r["component"].as_string(), r["sha256"].as_string(),
                r["size"].as_int64()};
    if (e.size < 1 || e.size > 64 * 1024 * 1024)
      throw InstallError("Payload size limit");
    if (e.sha256.size() != 64 ||
        e.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw InstallError("Invalid payload hash");
    m.files.push_back(std::move(e));
  }
  return m;
}

void Manager::verify_payload() {
  io::reject_links(io::path(payload_));
  auto m = load_manifest(io::str(io::path(payload_) / L"release.json"));
  std::set<std::string> targets, sources{"release.json"};
  int64_t total = 0;
  std::map<std::string, FileEntry> rows;
  for (auto &r : m.files) {
    io::relative(r.source);
    io::relative(r.target);
    // Independent path/role allowlist: the component binding decides the closed
    // destination namespace. A signed record never widens it.
    bool module_row = false;
    for (auto &o : m.modules)
      if (o.id == r.component)
        module_row = true;
    bool common_row = false;
    for (auto &c : m.common)
      if (c.role == r.component)
        common_row = true;
    if (!module_row && !common_row)
      throw InstallError("Payload row has no authenticated owner");
    if (module_row) {
      if (r.target != module_target(r.component))
        throw InstallError("Module payload outside its namespace");
    } else {
      const auto &allowed = COMMON_ALLOWED().at(r.component);
      if (std::find(allowed.begin(), allowed.end(), r.target) == allowed.end())
        throw InstallError("Common payload outside its namespace");
    }
    if (!targets.insert(r.target).second || !sources.insert(r.source).second)
      throw InstallError("Duplicate payload identity");
    if ((total += r.size) > 128 * 1024 * 1024)
      throw InstallError("Payload size limit");
    auto b = io::read(io::path(payload_) / io::path(r.source), 64 * 1024 * 1024);
    if (b.size() != static_cast<size_t>(r.size) || crypto::sha256_hex(b) != r.sha256)
      throw InstallError("Payload hash/size mismatch: " + r.source);
    rows.emplace(r.target, r);
  }
  // Module provenance: auth_record v1, unchanged contract, exact descriptor bytes.
  astro::auth::RecordPolicy policy;
  policy.now_unix = astro::auth::current_unix();
  policy.core_version = VERSION;
  for (auto &o : m.modules) {
    auto outcome = astro::auth::verify_record(o.envelope, module_trust_, policy);
    if (!outcome.verified || outcome.error != astro::auth::Reject::none)
      throw InstallError(std::string("Module record rejected: ") +
                         astro::auth::reject_text(outcome.error));
    const auto &d = outcome.descriptor;
    if (d.module_id != o.id || d.version != o.version || d.revision != o.revision ||
        d.dll_path != module_record_path(o.id))
      throw InstallError("Module record identity mismatch");
    auto it = rows.find(module_target(o.id));
    if (it == rows.end() || it->second.sha256 != d.dll_sha256 ||
        uint64_t(it->second.size) != d.dll_size)
      throw InstallError("Module record payload closure mismatch");
  }
  // Package/common provenance: package_record v1, exact descriptor bytes.
  astro::pkg::Policy pkg_policy;
  for (auto &c : m.common) {
    auto outcome = astro::pkg::verify_package_record(c.envelope, package_trust_, pkg_policy);
    if (!outcome.verified || outcome.error != astro::pkg::Reject::none)
      throw InstallError(std::string("Package record rejected: ") +
                         astro::pkg::reject_text(outcome.error));
    const auto &d = outcome.descriptor;
    if (d.component != c.role || d.version != c.version || d.revision != c.revision)
      throw InstallError("Package record identity mismatch");
    size_t role_rows = 0;
    for (const auto &entry : rows)
      if (entry.second.component == c.role)
        ++role_rows;
    // The descriptor resource set and the assembled payload rows must describe
    // exactly the same resources. Missing and surplus are different defects and
    // report different reasons.
    if (d.resources.size() < role_rows)
      throw InstallError("Package record resource closure incomplete");
    if (d.resources.size() > role_rows)
      throw InstallError("Package record resource closure has surplus resources");
    if (!catalog::common_resource_namespace_unambiguous())
      throw InstallError("Closed common namespace is ambiguous");
    uint64_t closure = 0;
    for (auto &res : d.resources) {
      if (res.role != c.role)
        throw InstallError("Package record resource role mismatch");
      // Record-relative namespace -> closed installed target for that role.
      // validate_closure already enforced strictly ascending `path`, so this
      // sequence is the single deterministic ordering of the closure digest.
      auto target = catalog::common_resource_target(c.role, res.path);
      if (target.empty())
        throw InstallError("Package record resource outside closed namespace");
      auto it = rows.find(target);
      if (it == rows.end() || it->second.component != c.role ||
          it->second.sha256 != res.sha256 || uint64_t(it->second.size) != res.size)
        throw InstallError("Package record resource bytes mismatch");
      closure += res.size;
    }
    if (closure == 0)
      throw InstallError("Package record resource closure empty");
    // Closure digest binds the compared identity of this component: the
    // ordered (path|sha256) tuple of the authenticated resource list.
    std::string combined;
    for (auto &res : d.resources)
      combined += res.path + "|" + res.sha256 + "|";
    if (crypto::sha256_hex(combined) != c.sha256)
      throw InstallError("Package record closure digest mismatch");
    // Agreement between a signed descriptor and its payload is insufficient
    // when both omit a required loader member. Admit the whole closed target
    // set, or no loader offer at all, before a transaction can be planned.
    if (c.role == registry::common_loader)
      for (const auto &target : COMMON_ALLOWED().at(registry::common_loader)) {
        auto it = rows.find(target);
        if (it == rows.end() || it->second.component != registry::role_loader)
          throw InstallError("Loader package resource closure incomplete");
      }
  }
  // Every file row must belong to exactly one authenticated offer.
  for (auto &[target, row] : rows) {
    bool owned = false;
    for (auto &o : m.modules)
      owned = owned || row.component == o.id;
    for (auto &c : m.common)
      owned = owned || row.component == c.role;
    if (!owned)
      throw InstallError("Unowned payload row: " + target);
  }
  io::known_tree(io::path(payload_), "", sources);
  rows_ = std::move(rows);
  manifest_ = std::move(m);
  has_payload_ = true;
}

void Manager::verify_no_payload() {
  rows_.clear();
  manifest_ = {};
  has_payload_ = false;
}
} // namespace engine
