#include "embedded_payload.h"
#include "embedded_manifest.h"
#include <compressapi.h>
#include <sddl.h>
#include <algorithm>
#pragma comment(lib, "cabinet.lib")
static io::Bytes resource(int id) {
  auto mod = GetModuleHandleW(nullptr);
  auto r = FindResourceW(mod, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!r)
    io::fail("Missing embedded payload");
  auto n = SizeofResource(mod, r);
  auto h = LoadResource(mod, r);
  auto p = static_cast<const uint8_t *>(LockResource(h));
  if (!p || n < 8 || n > 128 * 1024 * 1024)
    throw std::runtime_error("Invalid compressed resource");
  uint64_t size = 0;
  memcpy(&size, p, 8);
  if (size > 64 * 1024 * 1024)
    throw std::runtime_error("Decompression bound");
  DECOMPRESSOR_HANDLE dc = nullptr;
  if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &dc))
    io::fail("Create decompressor");
  io::Bytes out(static_cast<size_t>(size), uint8_t{});
  SIZE_T actual = 0;
  auto ok = Decompress(dc, p + 8, n - 8, out.data(), out.size(), &actual);
  CloseDecompressor(dc);
  if (!ok || actual != size)
    throw std::runtime_error("Decompression failed");
  return out;
}
// Build a private security descriptor (SYSTEM + current user only).
static PSECURITY_DESCRIPTOR build_private_sd() {
  HANDLE t = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t))
    io::fail("Process token");
  io::Handle token(t);
  DWORD n = 0;
  GetTokenInformation(t, TokenUser, nullptr, 0, &n);
  std::vector<uint8_t> b(n);
  if (!GetTokenInformation(t, TokenUser, b.data(), n, &n))
    io::fail("Token user");
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(b.data())->User.Sid, &sid))
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
// Throws if the directory already exists (collision with a foreign occupant).
static io::DirectoryReceipt private_dir(const io::fs::path &p) {
  struct Descriptor {
    PSECURITY_DESCRIPTOR value = build_private_sd();
    ~Descriptor() { LocalFree(value); }
  } sd;
  io::DirectoryReceipt rec;
  bool created = io::atomic_create_directory(p, rec, sd.value);
  if (!created)
    throw std::runtime_error("Private extraction root already exists: " + io::str(p));
  return rec;
}
EmbeddedPayload::EmbeddedPayload() {
  wchar_t t[32768];
  auto n = GetTempPathW(32768, t);
  if (!n || n >= 32768)
    io::fail("Temp path");
  root_ = io::absolute(io::str(io::fs::path(t))) / io::wide("AstroInstaller-" + io::random());
  // Reserve receipt slots before any creation so allocation cannot lose provenance.
  for (const auto &name : embedded_directories())
    dir_receipts_.emplace(io::wide(name), io::DirectoryReceipt{});
  dir_receipts_.emplace(L"", io::DirectoryReceipt{});
  pins_ = std::make_unique<io::DirectoryPins>(false);
  pins_->add(root_.parent_path());
  try {
    // Root and every intermediate are create-only; receipt-to-pin handoff is checked.
    std::vector<std::string> ordered{"", };
    for (const auto &name : embedded_directories())
      ordered.push_back(name);
    for (const auto &name : ordered) {
      auto p = name.empty() ? root_ : root_ / io::path(name);
      auto &receipt = dir_receipts_.at(io::wide(name));
      receipt = private_dir(p);
      pins_->add(p);
      io::Handle h(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (!receipt.matches(io::directory_receipt(h.h)))
        throw std::runtime_error("Private extraction directory changed before pin: " + io::str(p));
    }
    std::map<std::string, std::string> sources{{"release.json", EMBEDDED_MANIFEST_SHA256}};
    for (const auto &row : embedded_sources())
      sources[row.source] = row.hash;
    // Fully allocate tracking entries before publication. The real atomic writer
    // stores publication directly into its stable entry, including exception paths.
    // Iteration is std::map order, which is the ordinal source order the build
    // used to number the embedded RCDATA resources.
    files_.reserve(sources.size());
    for (const auto &[source, hash] : sources)
      files_.push_back({root_ / io::path(source), hash, false});
    int id = 200;
    for (auto &file : files_) {
      auto b = resource(id++);
      if (crypto::sha256_hex(b) != file.hash)
        throw std::runtime_error("Embedded resource hash mismatch");
      io::atomic(file.path, b, &file.published, "");
    }
  } catch (const std::exception &e) {
    auto original = std::string(e.what());
    const auto &remaining = cleanup();
    throw std::runtime_error(original + (remaining.empty() ? "" : "\r\n" + remaining));
  } catch (...) {
    const auto &remaining = cleanup();
    if (!remaining.empty()) OutputDebugStringW(io::wide(remaining).c_str());
    throw;
  }
}
EmbeddedPayload::~EmbeddedPayload() {
  if (cleanup_attempted_) return;
  try {
    const auto &remaining = cleanup();
    if (!remaining.empty()) OutputDebugStringW(io::wide(remaining).c_str());
  } catch (...) { OutputDebugStringW(L"Temporary extraction cleanup could not report its outcome.\n"); }
}
const std::string &EmbeddedPayload::cleanup() {
  if (cleanup_attempted_) return cleanup_report_;
  cleanup_attempted_ = true;
  auto retain = [&](const io::fs::path &path, const std::string &reason) {
    if (cleanup_report_.empty()) cleanup_report_ = "临时解包清理未完成，保留位置：" + io::str(root_);
    cleanup_report_ += "\r\n" + io::str(path) + "：" + reason;
  };
  // Keep all directory pins while removing registered files. One refusal must
  // not prevent cleanup of independent, still-owned entries.
  for (const auto &file : files_) {
    if (!file.published) continue;
    try { io::erase(file.path, file.hash); }
    catch (const std::exception &e) { retain(file.path, e.what()); }
  }
  pins_.reset();
  std::vector<std::string> cleanup_order = embedded_directories();
  // Deepest paths first, then the root.
  std::sort(cleanup_order.begin(), cleanup_order.end(),
            [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
  cleanup_order.push_back("");
  for (const auto &name : cleanup_order) {
    auto it = dir_receipts_.find(io::wide(name));
    if (it == dir_receipts_.end() || !it->second.valid()) continue;
    auto p = name.empty() ? root_ : root_ / io::path(name);
    try {
      if (!io::safe_remove_empty_directory(p, &it->second))
        retain(p, "目录非空、身份不符或无法确认，未删除");
    } catch (const std::exception &e) { retain(p, e.what()); }
  }
  return cleanup_report_;
}
