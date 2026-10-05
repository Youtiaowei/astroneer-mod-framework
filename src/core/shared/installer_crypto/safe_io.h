#include <windows.h>
#pragma once
#include "sha256.h"
#include "directory_receipt.h"
#include <bcrypt.h>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <winternl.h>

namespace io {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
inline void fail(const std::string &s) {
  throw std::runtime_error(s + " (Win32=" + std::to_string(GetLastError()) + ")");
}
inline std::wstring wide(const std::string &s) {
  if (s.find('\0') != s.npos)
    throw std::runtime_error("NUL in text/path");
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
  if (!n)
    fail("Invalid UTF-8");
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), w.data(), n);
  return w;
}
inline std::string utf8(const std::wstring &w) {
  if (w.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), int(w.size()), nullptr, 0,
                              nullptr, nullptr);
  if (!n)
    fail("Invalid UTF-16");
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), int(w.size()), s.data(), n, nullptr,
                      nullptr);
  return s;
}
inline fs::path path(const std::string &s) { return fs::path(wide(s)); }
inline std::string str(const fs::path &p) { return utf8(p.generic_wstring()); }
inline void component(const std::wstring &s) {
  if (s.empty() || s == L"." || s == L".." || s.back() == L'.' || s.back() == L' ')
    throw std::runtime_error("Invalid path component");
  for (auto c : s)
    if (c < 32 || c == 127 || std::wstring(L"<>:\"|?*").find(c) != std::wstring::npos)
      throw std::runtime_error("Invalid path character");
  auto stem = s.substr(0, s.find(L'.'));
  for (auto &c : stem)
    c = towupper(c);
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" ||
      stem == L"CONOUT$" ||
      (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
       stem[3] >= L'0' && stem[3] <= L'9'))
    throw std::runtime_error("Reserved Windows path");
}
inline void relative(const std::string &s) {
  auto w = wide(s);
  if (w.empty() || w.back() == L'/' || w.find(L'\\') != w.npos || path(s).is_absolute())
    throw std::runtime_error("Invalid relative path");
  size_t i = 0;
  while (i < w.size()) {
    auto e = w.find(L'/', i);
    component(w.substr(i, e == w.npos ? w.size() - i : e - i));
    if (e == w.npos)
      break;
    i = e + 1;
  }
}
inline fs::path absolute(const std::string &s) {
  auto p = path(s);
  if (!p.is_absolute() || p.root_name().wstring().size() != 2)
    throw std::runtime_error("Require absolute local drive path");
  for (auto &c : p.relative_path())
    if (!c.empty())
      component(c.wstring());
  return p.lexically_normal();
}
// Internal adapter only: keep ordinary drive paths in authorities and checks.
// Win32 opens must also support backup/staging names beyond MAX_PATH without
// depending on the user's machine-wide LongPathsEnabled registry policy.
inline std::wstring win32_path(const fs::path &p) {
  auto normal = fs::absolute(p).lexically_normal().make_preferred();
  if (!normal.is_absolute() || normal.root_name().wstring().size() != 2)
    throw std::runtime_error("Require absolute local drive path");
  return L"\\\\?\\" + normal.wstring();
}
inline DWORD attrs(const fs::path &p) {
  DWORD a = GetFileAttributesW(win32_path(p).c_str());
  if (a == INVALID_FILE_ATTRIBUTES) {
    auto e = GetLastError();
    if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
      fail("Cannot inspect " + str(p));
  }
  return a;
}
struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE v) : h(v) {}
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE && h)
      CloseHandle(h);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&v) noexcept : h(v.h) { v.h = INVALID_HANDLE_VALUE; }
};
inline void reject_links(const fs::path &p) {
  for (auto q = p; !q.empty();) {
    auto a = attrs(q);
    if (a != INVALID_FILE_ATTRIBUTES) {
      if (a & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::runtime_error("不支持符号链接或目录联接：" + str(q));
      if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        Handle h(CreateFileW(win32_path(q).c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION f{};
        if (h.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h.h, &f))
          fail("Cannot inspect identity");
        if (f.nNumberOfLinks != 1)
          throw std::runtime_error("Hardlink refused: " + str(q));
      }
    }
    auto parent = q.parent_path();
    if (parent == q)
      break;
    q = parent;
  }
}
inline bool exists(const fs::path &p) {
  reject_links(p);
  return attrs(p) != INVALID_FILE_ATTRIBUTES;
}
inline bool directory(const fs::path &p) {
  reject_links(p);
  auto a = attrs(p);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool file(const fs::path &p) {
  reject_links(p);
  auto a = attrs(p);
  if (a == INVALID_FILE_ATTRIBUTES)
    return false;
  if (a & FILE_ATTRIBUTE_DIRECTORY)
    throw std::runtime_error("Target is directory: " + str(p));
  return true;
}
inline DirectoryReceipt object_receipt(HANDLE handle, bool require_directory);
inline DirectoryReceipt identity_receipt(const fs::path &p) {
  reject_links(p);
  Handle h(CreateFileW(
      win32_path(p).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  return object_receipt(h.h, false);
}
inline std::string identity(const fs::path &p) {
  const auto receipt = identity_receipt(p);
  // Live identity/mutex keys stay stable when timestamps change. Ownership
  // receipts additionally compare creation_time before allowing directory cleanup.
  return "NTFS-v2-" + receipt_hex(receipt.volume_id) + "-" + receipt_file_id(receipt);
}
inline std::string lock_identity(const fs::path &p) {
  const auto receipt = identity_receipt(p); // Still enforce complete supported identity.
  // Preserve the cross-version mutex namespace; this string is not a cleanup receipt.
  return std::to_string(receipt.volume_serial) + "-" + std::to_string(receipt.file_index_high) +
         "-" + std::to_string(receipt.file_index_low);
}
inline fs::path canonical(const fs::path &p) {
  reject_links(p);
  return fs::weakly_canonical(p);
}
inline bool within(const fs::path &child, const fs::path &parent) {
  auto c = io::canonical(child), p = io::canonical(parent);
  auto a = c.begin();
  for (auto b = p.begin(); b != p.end(); ++b, ++a)
    if (a == c.end() || CompareStringOrdinal(a->c_str(), -1, b->c_str(), -1, TRUE) != CSTR_EQUAL)
      return false;
  return true;
}
inline void mkdir(const fs::path &p) {
  reject_links(p);
  fs::create_directories(p);
  reject_links(p);
}
// Read all bytes from an already-open handle (no reject_links or re-open).
inline Bytes read_via_handle(HANDLE h, size_t limit = 256 * 1024 * 1024) {
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN))
    fail("Seek protected file");
  LARGE_INTEGER n{};
  if (!GetFileSizeEx(h, &n) || n.QuadPart < 0 || uint64_t(n.QuadPart) > limit)
    throw std::runtime_error("Invalid file/size via handle");
  Bytes b(size_t(n.QuadPart));
  DWORD got = 0;
  if (!ReadFile(h, b.data(), DWORD(b.size()), &got, nullptr) || got != b.size())
    fail("Short read via handle");
  return b;
}
inline Bytes read(const fs::path &p, size_t limit = 256 * 1024 * 1024) {
  reject_links(p);
  Handle h(CreateFileW(win32_path(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (h.h == INVALID_HANDLE_VALUE)
    fail("Cannot read " + str(p));
  BY_HANDLE_FILE_INFORMATION f{};
  LARGE_INTEGER n{};
  if (!GetFileInformationByHandle(h.h, &f) || f.nNumberOfLinks != 1 ||
      (f.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      !GetFileSizeEx(h.h, &n) || n.QuadPart < 0 || uint64_t(n.QuadPart) > limit)
    throw std::runtime_error("Invalid file/size: " + str(p));
  Bytes b(size_t(n.QuadPart));
  DWORD got = 0;
  if (!ReadFile(h.h, b.data(), DWORD(b.size()), &got, nullptr) || got != b.size())
    fail("Short read");
  return b;
}
inline std::string text(const Bytes &b) { return std::string(b.begin(), b.end()); }
inline Bytes bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }
inline std::string random() {
  uint8_t b[16];
  if (BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw std::runtime_error("Random failed");
  return crypto::sha256_hex(b, sizeof(b)).substr(0, 32);
}
// Pins are acquired from the drive root down. Attribute-only handles do NOT
// enforce the sharing contract; actual read access is required (contract probe).
class DirectoryPins {
  std::vector<Handle> handles;
  std::set<fs::path> seen;
  bool strict;
public:
  explicit DirectoryPins(bool protect_reparse = true) : strict(protect_reparse) {}
  void add(fs::path p, bool create = false) {
    p = fs::absolute(p).lexically_normal().make_preferred();
    std::vector<fs::path> chain;
    for (auto q = p; !q.empty();) {
      chain.push_back(q);
      auto parent = q.parent_path();
      if (parent == q) break;
      q = parent;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (seen.count(*it)) continue;
      if (attrs(*it) == INVALID_FILE_ATTRIBUTES) {
        if (!create) continue;
        if (!CreateDirectoryW(win32_path(*it).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
          fail("Create pinned directory " + str(*it));
      }
      Handle h(CreateFileW(win32_path(*it).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | (strict ? 0 : FILE_SHARE_WRITE), nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (h.h == INVALID_HANDLE_VALUE) fail("Pin directory " + str(*it));
      BY_HANDLE_FILE_INFORMATION info{};
      if (!GetFileInformationByHandle(h.h, &info)) fail("Inspect pinned directory");
      if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
          (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Invalid pinned directory " + str(*it));
      handles.push_back(std::move(h));
      seen.insert(*it);
    }
  }
  // Call only with a protected child file anchoring the leaf as nonempty.
  // Ancestors contain the pinned next directory. FSCTL_SET_REPARSE_POINT cannot
  // convert a nonempty directory; share-delete remains denied without a gap.
  // Rename itself needs write-sharing on its destination directory.
  void allow_file_rename() {
    std::vector<Handle> overlap;
    for (const auto &p : seen) {
      Handle h(CreateFileW(win32_path(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (h.h == INVALID_HANDLE_VALUE) fail("Overlap directory pin " + str(p));
      overlap.push_back(std::move(h));
    }
    handles = std::move(overlap);
    strict = false;
  }
};
inline void validate_object(HANDLE h) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(h, &info)) fail("Inspect protected file");
  if (info.nNumberOfLinks != 1 ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                FILE_ATTRIBUTE_READONLY)))
    throw std::runtime_error("Protected file is linked, reparse, directory or readonly");
}
inline DWORD rename_no_replace(HANDLE h, fs::path destination) {
  auto name = win32_path(destination);
  std::vector<uint8_t> storage(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
  auto info = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
  info->ReplaceIfExists = FALSE;
  info->RootDirectory = nullptr;
  info->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
  memcpy(info->FileName, name.data(), info->FileNameLength);
  return SetFileInformationByHandle(h, FileRenameInfo, info, DWORD(storage.size()))
             ? ERROR_SUCCESS : GetLastError();
}
inline DWORD dispose_object(HANDLE h) {
  FILE_DISPOSITION_INFO info{TRUE};
  return SetFileInformationByHandle(h, FileDispositionInfo, &info, sizeof(info))
             ? ERROR_SUCCESS : GetLastError();
}
struct MutationState {
  bool changed = false, published = false;
  fs::path parked, stage;
  std::string recovery_error;
  bool pending() const { return !parked.empty() || !stage.empty(); }
};
#if defined(INSTALLER_TEST_DRIVER) || defined(INSTALLER_IO_TEST)
namespace testing {
inline void (*hook)(const char *, const fs::path &, const fs::path &, HANDLE) = nullptr;
}
#define IO_CUT(phase, target, object, handle) \
  do { if (io::testing::hook) io::testing::hook(phase, target, object, handle); } while (false)
#else
#define IO_CUT(phase, target, object, handle) ((void)0)
#endif
inline void win32_error(DWORD e, const std::string &message) {
  SetLastError(e);
  fail(message);
}
// Validate that a non-empty ownership hash is a real SHA-256 hex string.
inline void validate_expected_hash(const std::string &h) {
  if (!h.empty() && (h.size() != 64 || h.find_first_not_of("0123456789abcdef") != h.npos))
    throw std::runtime_error("Invalid expected hash (not a SHA-256 hex string)");
}
// Every caller must declare its ownership expectation:
//   ""              = expect target to be absent (new publication)
//   64-hex SHA-256  = verify prior owner's content before replacing
// Omitting the expectation is a compile error; no unchecked writes exist
// in a release build.  Empty-string callers must handle the thrown absence
// conflict if a foreign occupant appears.
inline void atomic(const fs::path &p, const Bytes &b, bool *mutated,
                   const std::string &expected_hash, MutationState *result = nullptr) {
  validate_expected_hash(expected_hash);
  MutationState state;
  auto report = [&] { if (mutated) *mutated = state.changed; if (result) *result = state; };
  report();
  DirectoryPins pins;
  pins.add(p.parent_path(), true);
  reject_links(p);
  auto temp = p.parent_path() / (L".install-" + wide(random()));
  auto park = p.parent_path() / (L".park-" + wide(random()));
  Handle stage(INVALID_HANDLE_VALUE), original(INVALID_HANDLE_VALUE);
  try {
    stage.h = CreateFileW(win32_path(temp).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                          FILE_SHARE_READ, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (stage.h == INVALID_HANDLE_VALUE) fail("Create temporary file");
    state.stage = temp;
    DWORD n = 0;
    if (!WriteFile(stage.h, b.data(), DWORD(b.size()), &n, nullptr) || n != b.size() ||
        !FlushFileBuffers(stage.h)) fail("Write/flush stage");
    validate_object(stage.h);
    if (read_via_handle(stage.h) != b) throw std::runtime_error("Stage readback mismatch");
    IO_CUT("stage-validated", p, temp, stage.h);
    pins.allow_file_rename();
    if (expected_hash.empty()) {
      if (attrs(p) != INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("Expected absent target is occupied: " + str(p));
    } else {
      IO_CUT("before-target-open", p, p, INVALID_HANDLE_VALUE);
      original.h = CreateFileW(win32_path(p).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (original.h == INVALID_HANDLE_VALUE) {
        auto e = GetLastError();
        win32_error(e, "Open protected target " + str(p));
      } else {
        validate_object(original.h);
        if (crypto::sha256_hex(read_via_handle(original.h)) != expected_hash)
          throw std::runtime_error("Target changed before protected replace: " + str(p));
        IO_CUT("replace-validated", p, p, original.h);
        auto e = rename_no_replace(original.h, park);
        if (e) win32_error(e, "Park protected original " + str(p));
        state.parked = park;
        state.changed = true;
        report();
        IO_CUT("after-park", p, park, original.h);
      }
    }
    IO_CUT("before-publish", p, temp, stage.h);
    auto e = rename_no_replace(stage.h, p);
    if (e) win32_error(e, "Publish complete stage without replacement " + str(p));
    state.published = state.changed = true;
    state.stage.clear();
    report();
    IO_CUT("after-publish", p, p, stage.h);
    validate_object(stage.h);
    if (read_via_handle(stage.h) != b) throw std::runtime_error("Protected readback mismatch");
    IO_CUT("after-readback", p, p, stage.h);
    if (!state.parked.empty()) {
      IO_CUT("before-dispose-park", p, park, original.h);
      e = dispose_object(original.h);
      if (e) win32_error(e, "Dispose parked original " + str(park));
      state.parked.clear();
    }
    report();
  } catch (const std::exception &error) {
    std::string message = error.what();
    if (!state.parked.empty() && !state.published) {
      auto e = rename_no_replace(original.h, p);
      if (!e) { state.parked.clear(); state.changed = false; }
      else state.recovery_error += "No-replace original restore failed (Win32=" + std::to_string(e) + "); ";
    }
    // Cleanup addresses only held objects. Never delete a temporary pathname:
    // after rename it can already name a completed external write.
    if (!state.published && stage.h != INVALID_HANDLE_VALUE) {
      auto e = dispose_object(stage.h);
      if (!e) state.stage.clear();
      else state.recovery_error += "Stage cleanup failed (Win32=" + std::to_string(e) + "); ";
    }
    if (!state.parked.empty() && state.published) {
      auto e = dispose_object(original.h);
      if (!e) state.parked.clear();
      else state.recovery_error += "Park cleanup failed (Win32=" + std::to_string(e) + "); ";
    }
    report();
    if (state.pending()) message += "; Recovery required: parked=" + str(state.parked) +
        "; stage=" + str(state.stage) + "; " + state.recovery_error;
    throw std::runtime_error(message);
  }
}
// Empty hash is EXPECTED ABSENCE, never permission to delete an occupant.
// Unchecked cleanup has no overload: its caller must retain its intended hash.
inline void erase(const fs::path &p, const std::string &expected_hash,
                  bool *mutated = nullptr) {
  if (mutated) *mutated = false;
  DirectoryPins pins;
  pins.add(p.parent_path());
  reject_links(p);
  if (expected_hash.empty()) {
    IO_CUT("erase-absent", p, p, INVALID_HANDLE_VALUE);
    if (attrs(p) != INVALID_FILE_ATTRIBUTES)
      throw std::runtime_error("Expected absent deletion target is occupied: " + str(p));
    return;
  }
  IO_CUT("before-target-open", p, p, INVALID_HANDLE_VALUE);
  Handle original(CreateFileW(win32_path(p).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (original.h == INVALID_HANDLE_VALUE) fail("Open protected delete target " + str(p));
  validate_object(original.h);
  if (crypto::sha256_hex(read_via_handle(original.h)) != expected_hash)
    throw std::runtime_error("Target changed before protected delete: " + str(p));
  IO_CUT("delete-validated", p, p, original.h);
  auto e = dispose_object(original.h);
  if (e) win32_error(e, "Dispose protected target " + str(p));
  if (mutated) *mutated = true;
  IO_CUT("after-delete", p, p, original.h);
}
#undef IO_CUT

// A read-only snapshot is not proof of creation. Only FILE_CREATED capture
// supplies ownership; all readers use the same supported-filesystem policy.
inline DirectoryReceipt object_receipt(HANDLE handle, bool require_directory) {
  wchar_t filesystem[MAX_PATH + 1]{};
  DWORD flags = 0;
  if (!GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, &flags,
                                     filesystem, MAX_PATH + 1)) fail("Query directory filesystem");
  if (wcscmp(filesystem, L"NTFS") || !(flags & FILE_PERSISTENT_ACLS) ||
      !(flags & FILE_SUPPORTS_OPEN_BY_FILE_ID))
    throw std::runtime_error("Directory ownership requires supported local NTFS identity");
  wchar_t name[32768]{};
  auto length = GetFinalPathNameByHandleW(handle, name, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  std::wstring path_name(name);
  if (!length || length >= 32768 || path_name.size() < 7 || path_name.substr(0,4) != L"\\\\?\\" ||
      path_name[5] != L':' || path_name[6] != L'\\')
    throw std::runtime_error("Directory ownership requires a local drive");
  auto drive = GetDriveTypeW(path_name.substr(4,3).c_str());
  if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE)
    throw std::runtime_error("Remote directory ownership is unsupported");
  BY_HANDLE_FILE_INFORMATION legacy{};
  FILE_ID_INFO id{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandle(handle, &legacy) ||
      !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
    fail("Query complete directory identity");
  if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      (legacy.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      (require_directory && (!(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                             !(legacy.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))))
    throw std::runtime_error("Object identity has an unsupported type");
  DirectoryReceipt receipt{legacy.dwVolumeSerialNumber, legacy.nFileIndexHigh, legacy.nFileIndexLow};
  receipt.version = 2;
  receipt.volume_id = id.VolumeSerialNumber;
  std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), receipt.file_id.begin());
  receipt.creation_time = uint64_t(basic.CreationTime.QuadPart);
  if (!receipt.valid()) throw std::runtime_error("Missing object-specific directory identity");
  return receipt;
}
inline DirectoryReceipt directory_receipt(HANDLE handle) { return object_receipt(handle, true); }

// Resolve documented user-mode NtCreateFile from ntdll.dll.
// Microsoft documents this as the user-mode equivalent of ZwCreateFile:
// https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile
using NtCreateFile_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                         PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                         ULONG, ULONG, ULONG, PVOID, ULONG);
inline NtCreateFile_t resolve_nt_create_file() {
  static NtCreateFile_t fn = nullptr;
  if (!fn) {
    auto addr = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
    if (!addr) throw std::runtime_error("NtCreateFile not available from ntdll.dll");
    static_assert(sizeof(fn) == sizeof(addr));
    memcpy(&fn, &addr, sizeof(fn));
  }
  return fn;
}

// Atomically create a directory using NtCreateFile FILE_CREATE and capture
// its identity from the creation handle — no path-to-open gap.
// Caller must hold parent pins. The receipt captures the identity of the
// genuinely created object.
// If the directory already exists, returns false with an invalid receipt.
inline bool atomic_create_directory(const fs::path &p, DirectoryReceipt &receipt,
                                    const PSECURITY_DESCRIPTOR sd = nullptr) {
  receipt = {};
  reject_links(p);
  auto parent = p.parent_path();
  auto leaf = p.filename().wstring();
  Handle parent_h(CreateFileW(win32_path(parent).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (parent_h.h == INVALID_HANDLE_VALUE) fail("Open parent for atomic directory create: " + str(parent));
  directory_receipt(parent_h.h); // Reject unsupported filesystems before creating a child.
  UNICODE_STRING name{};
  name.Buffer = const_cast<wchar_t*>(leaf.c_str());
  name.Length = USHORT(leaf.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length + sizeof(wchar_t);
  OBJECT_ATTRIBUTES obj{};
  obj.Length = sizeof(obj);
  obj.RootDirectory = parent_h.h;
  obj.ObjectName = &name;
  if (sd) {
    obj.SecurityDescriptor = sd;
  } else {
    // Inherit security from parent (CreateDirectoryW behavior).
    obj.SecurityDescriptor = nullptr;
  }
  IO_STATUS_BLOCK iosb{};
  HANDLE raw = INVALID_HANDLE_VALUE;
  auto create = resolve_nt_create_file();
  auto access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE | DELETE;
  auto options = FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT;
  auto status = create(&raw, access, &obj, &iosb, nullptr,
                       FILE_ATTRIBUTE_DIRECTORY,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       FILE_CREATE, options, nullptr, 0);
  if (status < 0) {
    // STATUS_OBJECT_NAME_COLLISION (0xC0000035) means directory already exists.
    // All other failures are real errors.
    if (status == static_cast<NTSTATUS>(0xC0000035L)) return false;
    SetLastError(RtlNtStatusToDosError(status));
    fail("NtCreateFile FILE_CREATE directory: " + str(p));
  }
  if (iosb.Information != FILE_CREATED) {
    CloseHandle(raw);
    SetLastError(ERROR_ALREADY_EXISTS);
    return false;
  }
  Handle h(raw);
  try {
    receipt = directory_receipt(h.h);
  } catch (const std::exception &e) {
    // FILE_CREATED proves this still-held object is ours even if identity query fails.
    // Disposition targets only that original handle; it never follows a replacement name.
    auto error = dispose_object(h.h);
    throw std::runtime_error(std::string(e.what()) +
      (error ? "; created directory retained (Win32=" + std::to_string(error) + "): " :
               "; created directory disposed after identity failure: ") + str(p));
  }
  return true;
}

// Safely remove an empty directory.  Opens the target as a specific object
// (handle-bound), validates that it is a non-reparse directory, checks
// emptiness, verifies the path still names the same object (detecting a
// rename+replace between check and delete), and only then removes it.
// Caller must hold parent pins so the target cannot be renamed away.
// Returns true if the directory was removed, false otherwise.
//
// When a receipt is provided, validates that the directory identity matches
// the receipt — a replacement directory at the same path has a different
// identity and will NOT be removed. When receipt is nullptr or invalid,
// the directory is kept (legacy path-only semantics cannot prove ownership).
inline bool safe_remove_empty_directory(const fs::path &p,
                                        const DirectoryReceipt *receipt = nullptr) {
  // Phase 0: a receipt is required to prove ownership. Without it, we cannot
  // distinguish a directory we created from a foreign replacement.
  if (!receipt || !receipt->valid()) return false;
  // Phase 1: open the directory object and validate type.
  Handle h(CreateFileW(win32_path(p).c_str(), DELETE | FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (h.h == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(h.h, &info)) return false;
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return false;
  // Phase 2: validate that the opened directory matches the creation receipt.
  // A replacement directory at the same path will have a different identity.
  try { if (!receipt->matches(directory_receipt(h.h))) return false; }
  catch (...) { return false; }
  // Phase 3: check emptiness via the name, then re-verify that the path
  // still names the same object we opened.  If it was renamed away and a
  // new directory created at the same name, the index will differ.
  {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((p / L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool empty = true;
    do {
      if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
        empty = false;
        break;
      }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (!empty) return false;
  }
  // Re-open the path and compare identity (TOCTOU guard).
  {
    Handle check(CreateFileW(win32_path(p).c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (check.h == INVALID_HANDLE_VALUE) return false;
    try { if (!receipt->matches(directory_receipt(check.h))) return false; }
    catch (...) { return false; }
  }
  // Phase 4: delete via the originally opened handle.
  FILE_DISPOSITION_INFO di{TRUE};
  return SetFileInformationByHandle(h.h, FileDispositionInfo, &di, sizeof(di)) != 0;
}
inline std::string hash_or_absent(const fs::path &p) {
  return file(p) ? crypto::sha256_hex(read(p)) : "";
}
inline void known_tree(const fs::path &base, const std::string &rel,
                       const std::set<std::string> &allowed) {
  auto root = base / path(rel);
  reject_links(root);
  if (!directory(root))
    return;
  std::set<std::string> dirs;
  for (auto &f : allowed) {
    auto p = path(f).parent_path();
    while (!p.empty()) {
      dirs.insert(str(p));
      p = p.parent_path();
    }
  }
  size_t count = 0;
  for (auto i = fs::recursive_directory_iterator(root); i != fs::recursive_directory_iterator();
       ++i) {
    if (++count > 512)
      throw std::runtime_error("Owned tree too large");
    reject_links(i->path());
    auto name = str(i->path().lexically_relative(base));
    if (directory(i->path())) {
      if (!dirs.count(name))
        throw std::runtime_error("未知目录：" + name);
    } else if (!allowed.count(name))
      throw std::runtime_error("未知文件：" + name);
  }
}
} // namespace io
