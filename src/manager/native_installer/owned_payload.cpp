#include "owned_payload.hpp"
#include "json.hpp"
#include "payload_catalog.h"
#include <sddl.h>
#include <algorithm>
namespace stage {
namespace {
constexpr uint64_t max_total_bytes = 128ull * 1024 * 1024; // engine payload bound
constexpr size_t max_file_bytes = 64ull * 1024 * 1024;      // engine per-row bound

// Base package rows exactly as the engine will re-read them. Parsing here is a
// fail-fast subset; the engine's signed-manifest verification stays the only
// authority for what may be installed.
struct Row {
  std::string source, target, component, sha256;
  int64_t size = 0;
};
struct BaseManifest {
  json::Value document;
  std::vector<std::string> module_ids;
  std::vector<Row> files;
};
std::string hex64(const json::Value &v, const char *what) {
  if (!v.is_string() || v.as_string().size() != 64 ||
      v.as_string().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error(std::string("基础包清单字段无效：") + what);
  return v.as_string();
}
BaseManifest read_base(const std::string &base_payload) {
  const auto root = io::path(base_payload);
  io::reject_links(root);
  if (!io::directory(root))
    throw std::runtime_error("基础包负载目录缺失。");
  auto document = json::parse(io::text(io::read(root / L"release.json", 1024 * 1024)));
  if (!document.is_object() || document.size() != 11)
    throw std::runtime_error("基础包清单不受支持。");
  for (auto *key : {"schema", "product", "kind", "package_id", "version", "game_version",
                    "card_revision", "payload_kind", "modules", "common", "files"})
    if (!document.has(key))
      throw std::runtime_error("基础包清单不完整。");
  if (!document["schema"].is_integer() || document["schema"].as_int64() != 3)
    throw std::runtime_error("基础包清单版本不受支持。");
  if (!document["modules"].is_array() || !document["common"].is_array() ||
      !document["files"].is_array())
    throw std::runtime_error("基础包清单结构无效。");
  if (document["modules"].size() > 8 || document["common"].size() > 8 ||
      document["files"].size() > 64)
    throw std::runtime_error("基础包清单超出固定上限。");
  BaseManifest base;
  base.document = std::move(document);
  for (const auto &row : base.document["modules"].as_array()) {
    if (!row.is_object() || !row.has("id") || !row["id"].is_string())
      throw std::runtime_error("基础包模块行无效。");
    base.module_ids.push_back(row["id"].as_string());
  }
  for (const auto &row : base.document["files"].as_array()) {
    if (!row.is_object() || row.size() != 5 || !row.has("size") || !row["size"].is_integer() ||
        !row.has("source") || !row.has("target") || !row.has("component") || !row.has("sha256"))
      throw std::runtime_error("基础包文件行无效。");
    Row entry{row["source"].as_string(), row["target"].as_string(),
              row["component"].as_string(), hex64(row["sha256"], "sha256"),
              row["size"].as_int64()};
    io::relative(entry.source);
    io::relative(entry.target);
    if (entry.size < 1 || entry.size > int64_t(max_file_bytes) || entry.component.empty())
      throw std::runtime_error("基础包文件行超出固定上限。");
    base.files.push_back(std::move(entry));
  }
  return base;
}
// Build a private security descriptor (SYSTEM + current user only).
PSECURITY_DESCRIPTOR build_private_sd() {
  HANDLE token_handle = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_handle))
    io::fail("Process token");
  io::Handle token(token_handle);
  DWORD n = 0;
  GetTokenInformation(token_handle, TokenUser, nullptr, 0, &n);
  std::vector<uint8_t> buffer(n);
  if (!GetTokenInformation(token_handle, TokenUser, buffer.data(), n, &n))
    io::fail("Token user");
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid))
    io::fail("User SID");
  std::wstring acl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(sid) + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &sd,
                                                            nullptr))
    io::fail("Private ACL");
  return sd;
}
// Create a private directory atomically and capture its creation receipt.
io::DirectoryReceipt private_dir(const io::fs::path &p) {
  struct Descriptor {
    PSECURITY_DESCRIPTOR value = build_private_sd();
    ~Descriptor() { LocalFree(value); }
  } sd;
  io::DirectoryReceipt receipt;
  if (!io::atomic_create_directory(p, receipt, sd.value))
    throw std::runtime_error("暂存目录已被占用，拒绝复用：" + io::str(p));
  return receipt;
}
void hold_pin(io::DirectoryPins &pins, const io::fs::path &p,
              const io::DirectoryReceipt &receipt) {
  pins.add(p);
  io::Handle h(CreateFileW(io::win32_path(p).c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr));
  if (h.h == INVALID_HANDLE_VALUE)
    io::fail("Inspect staged directory");
  if (!receipt.matches(io::directory_receipt(h.h)))
    throw std::runtime_error("暂存目录在保护交接时变化：" + io::str(p));
}
} // namespace

Snapshot::Snapshot(const std::string &parent, const std::string &base_payload,
                   const modlib::Report &library)
    : pins_(std::make_unique<io::DirectoryPins>(false)) {
  try {
    const BaseManifest base = read_base(base_payload);
    // Derived schema-3 manifest: unchanged base rows plus verified module rows.
    std::vector<Row> rows = base.files;
    json::Array modules = base.document["modules"].as_array();
    json::Array files = base.document["files"].as_array();
    uint64_t total = 0;
    for (const auto &row : rows)
      total += uint64_t(row.size);
    for (const auto &candidate : library.modules) {
      const std::string source = "modules/" + candidate.id + ".dll";
      rows.push_back(Row{source, catalog::module_target(candidate.id), candidate.id,
                         candidate.dll_sha256, int64_t(candidate.dll_size)});
      modules.push_back(json::Object{{"id", candidate.id},
                                     {"version", candidate.version},
                                     {"revision", candidate.revision},
                                     {"envelope", candidate.envelope},
                                     {"source", source}});
      files.push_back(json::Object{{"source", source},
                                   {"target", catalog::module_target(candidate.id)},
                                   {"component", candidate.id},
                                   {"sha256", candidate.dll_sha256},
                                   {"size", int64_t(candidate.dll_size)}});
      total += candidate.dll_size;
    }
    if (modules.size() > 8 || files.size() > 64 || total > max_total_bytes)
      throw std::runtime_error("聚合负载超出固定上限。");
    json::Object derived{{"schema", base.document["schema"]},
                         {"product", base.document["product"]},
                         {"kind", base.document["kind"]},
                         {"package_id", base.document["package_id"]},
                         {"version", base.document["version"]},
                         {"game_version", base.document["game_version"]},
                         {"card_revision", base.document["card_revision"]},
                         {"payload_kind", base.document["payload_kind"]},
                         {"modules", modules},
                         {"common", base.document["common"]},
                         {"files", files}};
    const std::string manifest = json::serialize(derived);
    // Reserve ownership tracking before any filesystem publication. A failed
    // protected-file handoff must not lose the successful atomic-write record.
    files_.reserve(rows.size() + 1);
    held_.reserve(rows.size() + 1);
    // Private create-only stage under the owned parent; never a system temp
    // folder, never a reused cache.
    const auto parent_path = io::absolute(parent);
    io::reject_links(parent_path);
    if (!io::directory(parent_path))
      throw std::runtime_error("暂存父目录不可用。");
    pins_->add(parent_path);
    root_ = parent_path / io::path("astro-stage-" + io::random());
    dir_receipts_.emplace(L"", io::DirectoryReceipt{});
    dir_receipts_.at(L"") = private_dir(root_);
    hold_pin(*pins_, root_, dir_receipts_.at(L""));
    std::set<std::string> directories;
    for (const auto &row : rows) {
      auto rel = io::path(row.source).parent_path();
      while (!rel.empty() && rel != io::path(".")) {
        directories.insert(io::str(rel));
        rel = rel.parent_path();
      }
    }
    std::vector<std::string> ordered(directories.begin(), directories.end());
    std::sort(ordered.begin(), ordered.end(), [](const std::string &a, const std::string &b) {
      return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
    });
    for (const auto &relative : ordered) {
      dir_receipts_.emplace(io::wide(relative), io::DirectoryReceipt{});
      auto &receipt = dir_receipts_.at(io::wide(relative));
      receipt = private_dir(root_ / io::path(relative));
      hold_pin(*pins_, root_ / io::path(relative), receipt);
    }
    // Copy verified bytes only; every published object is then held.
    for (const auto &row : base.files) {
      const auto bytes = io::read(io::path(base_payload) / io::path(row.source), max_file_bytes);
      if (bytes.size() != size_t(row.size) || crypto::sha256_hex(bytes) != row.sha256)
        throw std::runtime_error("待复制资源与清单不一致：" + row.source);
      publish(root_ / io::path(row.source), bytes, row.sha256);
    }
    for (const auto &candidate : library.modules) {
      const auto bytes = io::read(io::path(candidate.dll_path), modlib::max_dll_bytes);
      if (bytes.size() != size_t(candidate.dll_size) ||
          crypto::sha256_hex(bytes) != candidate.dll_sha256)
        throw std::runtime_error("待复制模块与已验证记录不一致：" + candidate.id);
      publish(root_ / io::path("modules/" + candidate.id + ".dll"), bytes, candidate.dll_sha256);
    }
    publish(root_ / L"release.json", io::bytes(manifest), crypto::sha256_hex(manifest));
  } catch (const io::DirectoryCreationFailure &e) {
    // The creator may have retained its FILE_CREATED object before returning a
    // receipt. Never delete that unreceipted path or discard its checked report.
    auto remaining = e.cleanup_detail();
    const auto &tracked = cleanup();
    if (!tracked.empty()) remaining += (remaining.empty() ? "" : "\r\n") + tracked;
    throw PreparationFailure(e.what(), std::move(remaining));
  } catch (const std::exception &e) {
    const auto original = std::string(e.what());
    const auto &remaining = cleanup();
    throw PreparationFailure(original, remaining);
  } catch (...) {
    const auto &remaining = cleanup();
    throw PreparationFailure("暂存构建发生非标准异常。", remaining);
  }
}

void Snapshot::publish(const io::fs::path &target, const io::Bytes &bytes,
                       const std::string &hash) {
  files_.push_back({target, hash, false});
  io::atomic(target, bytes, &files_.back().published, "");
  io::Handle held(CreateFileW(io::win32_path(target).c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (held.h == INVALID_HANDLE_VALUE)
    io::fail("暂存文件无法保持：" + io::str(target));
  io::validate_object(held.h);
  if (crypto::sha256_hex(io::read_via_handle(held.h)) != hash)
    throw std::runtime_error("暂存文件写入后校验失败：" + io::str(target));
  held_.push_back(std::move(held));
}

Snapshot::~Snapshot() {
  if (cleanup_attempted_)
    return;
  try {
    const auto &remaining = cleanup();
    if (!remaining.empty())
      OutputDebugStringW(io::wide(remaining).c_str());
  } catch (...) {
    OutputDebugStringW(L"Staged payload cleanup could not report its outcome.\n");
  }
}

const std::string &Snapshot::cleanup() {
  if (cleanup_attempted_)
    return cleanup_report_;
  cleanup_attempted_ = true;
  auto retain = [this](const io::fs::path &path, const std::string &reason) {
    if (cleanup_report_.empty())
      cleanup_report_ = "暂存清理未完成，保留位置：" + io::str(root_);
    cleanup_report_ += "\r\n" + io::str(path) + "：" + reason;
  };
  // Release held identities first; every deletion is then bound to the exact
  // published hash and every empty directory to its creation receipt. Foreign
  // or changed occupants are retained with an explicit warning.
  held_.clear();
  for (const auto &file : files_) {
    if (!file.published)
      continue;
    try {
      io::erase(file.path, file.hash);
    } catch (const std::exception &e) {
      retain(file.path, e.what());
    }
  }
  pins_.reset();
  std::vector<std::string> order;
  for (const auto &[name, receipt] : dir_receipts_)
    if (!name.empty())
      order.push_back(io::utf8(name));
  std::sort(order.begin(), order.end(),
            [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
  order.push_back("");
  for (const auto &name : order) {
    const auto it = dir_receipts_.find(io::wide(name));
    if (it == dir_receipts_.end() || !it->second.valid())
      continue;
    const auto p = name.empty() ? root_ : root_ / io::path(name);
    try {
      if (!io::safe_remove_empty_directory(p, &it->second))
        retain(p, "目录非空、身份不符或无法确认，未删除");
    } catch (const std::exception &e) {
      retain(p, e.what());
    }
  }
  return cleanup_report_;
}

Active collect(const std::string &base_payload, const std::string &library_root,
               const astro::auth::TrustSet &trust, const std::string &core_version) {
  Active active;
  active.payload = base_payload;
  if (library_root.empty())
    return active;
  if (base_payload.empty())
    throw std::runtime_error("Mod 库需要基础包负载目录。");
  // Never accept a library candidate the opened package already offers.
  // Base admission is mandatory and stays outside this optional-source catch:
  // falling back must never bypass a malformed/unsigned common payload.
  const auto reserved = read_base(base_payload).module_ids;
  bool unavailable = false;
  try {
    auto report = modlib::discover(library_root, trust, core_version, reserved);
    active.diagnostics = std::move(report.diagnostics);
    if (report.modules.empty())
      return active;
    auto snapshot = std::make_shared<Snapshot>(library_root, base_payload, report);
    for (const auto &line : snapshot->diagnostics())
      if (active.diagnostics.size() < modlib::max_diagnostics)
        active.diagnostics.push_back(line);
    active.payload = snapshot->path();
    active.hold = std::move(snapshot);
  } catch (const PreparationFailure &failure) {
    active.preparation_cleanup = failure.cleanup_detail();
    unavailable = true;
  } catch (const std::exception &) {
    unavailable = true;
  }
  if (unavailable) {
    // No external offer is admitted on failure. Keep the intact base and let
    // the unchanged engine verify its signatures and installed authority.
    // Installed-only status/toggle/removal must not depend on a download/cache.
    if (active.diagnostics.size() >= modlib::max_diagnostics)
      active.diagnostics.pop_back();
    active.diagnostics.push_back(
        "Mod 下载库不可用或暂存失败，已跳过外部候选；已安装功能仍可管理。 / "
        "Mod library unavailable or staging failed; external offers skipped; installed Mods remain manageable.");
  }
  return active;
}

void Retirement::record(const std::string &detail) {
  if (detail.empty()) return;
  if (!retained_.empty()) retained_ += "\r\n";
  retained_ += detail;
}

void Retirement::replace(Active &current, Active next) {
  if (current.hold) pending_.push_back(current.hold);
  record(next.preparation_cleanup);
  next.preparation_cleanup.clear();
  current = std::move(next);
  reap();
}

void Retirement::reap() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->use_count() != 1) { ++it; continue; }
    try {
      record((*it)->cleanup());
    } catch (const std::exception &e) {
      record("暂存清理未完成，保留位置：" + (*it)->path() + "\r\n" + e.what());
    } catch (...) {
      record("暂存清理未完成，保留位置：" + (*it)->path());
    }
    it = pending_.erase(it);
  }
}

std::string Retirement::detail() const {
  auto result = retained_;
  for (const auto &snapshot : pending_) {
    if (!result.empty()) result += "\r\n";
    result += "等待仍在使用的快照释放，暂存位置：" + snapshot->path();
  }
  return result;
}

} // namespace stage
