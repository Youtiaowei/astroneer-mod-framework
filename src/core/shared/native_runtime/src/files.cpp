#define NOMINMAX
#include "files.hpp"
#include <cstddef>
#include <cstring>
#include <fstream>
#include <vector>
#include <windows.h>
namespace astro {
NativeFiles::NativeFiles(std::filesystem::path p) {
  if (!p.is_absolute())
    throw std::invalid_argument("Storage root must be absolute");
  root = std::filesystem::weakly_canonical(p);
}
std::filesystem::path NativeFiles::path(const std::string &name) const {
  if (name.empty() || name.size() > 128 ||
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
                             "XYZ0123456789-_.") != std::string::npos ||
      name == "." || name == "..")
    throw std::invalid_argument("Invalid storage filename");
  auto path = root / std::filesystem::path(name);
  auto canonical = std::filesystem::weakly_canonical(path);
  if (canonical.parent_path() != root)
    throw std::invalid_argument("Storage file escapes module-derived root");
  return path;
}
std::optional<std::string> NativeFiles::read(const std::string &name,
                                             size_t limit) {
  std::ifstream file(path(name), std::ios::binary);
  if (!file)
    return {};
  std::string bytes(limit + 1, 0);
  file.read(bytes.data(), bytes.size());
  if (file.bad() || size_t(file.gcount()) > limit)
    return {};
  bytes.resize(size_t(file.gcount()));
  return bytes;
}
bool NativeFiles::write_atomic(const std::string &name,
                               const std::string &content) {
  return write_file(name, content, true);
}
bool NativeFiles::publish_runtime_state(const std::string &content) {
  return write_file("runtime-state.json", content, false);
}
bool NativeFiles::write_file(const std::string &name,
                            const std::string &content, bool durable) {
  if (content.size() > 1024 * 1024)
    return false;
  auto final = path(name);
  auto temp = path(name + ".tmp");
  const auto name_wide = final.wstring();
  if (name_wide.size() > 32767)
    return false;
  const auto rename_size = offsetof(FILE_RENAME_INFO, FileName) +
                           (name_wide.size() + 1) * sizeof(wchar_t);
  // Allocate before acquiring the handle; all operations below are non-throwing.
  std::vector<unsigned char> buffer(rename_size, 0);
  auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
  rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                  FILE_RENAME_FLAG_POSIX_SEMANTICS;
  rename->FileNameLength = DWORD(name_wide.size() * sizeof(wchar_t));
  std::memcpy(rename->FileName, name_wide.c_str(),
              (name_wide.size() + 1) * sizeof(wchar_t));
  HANDLE handle =
      CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL | (durable ? FILE_FLAG_WRITE_THROUGH : 0),
                  nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  bool ok = WriteFile(handle, content.data(), DWORD(content.size()), &written,
                      nullptr) &&
            written == content.size() && (!durable || FlushFileBuffers(handle));
  if (ok) {
    // Keep the source handle through the single namespace commit. Compatible
    // readers retain the old file; a reader denying delete-sharing still blocks.
    ok = SetFileInformationByHandle(handle, FileRenameInfoEx, rename,
                                    DWORD(rename_size)) != 0;
    const auto error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
                error == ERROR_INVALID_FUNCTION)) {
      // Older filesystems/Windows may only support the conservative rename.
      rename->Flags = 0;
      rename->ReplaceIfExists = TRUE;
      ok = SetFileInformationByHandle(handle, FileRenameInfo, rename,
                                      DWORD(rename_size)) != 0;
    }
  }
  const auto error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  // Success means the namespace commit completed; cleanup cannot undo it.
  if (!ok)
    SetLastError(error);
  return ok;
}
RuntimePaths resolve_paths(const std::filesystem::path &dll) {
  if (!dll.is_absolute())
    throw std::invalid_argument("DLL path must be absolute");
  auto full = std::filesystem::weakly_canonical(dll);
  auto walk = full;
  for (auto name : {L"main.dll", L"dlls", L"AstroConsoleCore", L"Mods",
                    L"Win64", L"Binaries", L"Astro"}) {
    if (_wcsicmp(walk.filename().c_str(), name) != 0)
      throw std::invalid_argument(
          "DLL path does not match the packaged CORE namespace");
    walk = walk.parent_path();
  }
  return {walk, full.parent_path().parent_path(),
          walk / L"AstroModManager" / L"data"};
}
std::filesystem::path this_module_path() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&this_module_path),
                          &module))
    throw std::runtime_error("Cannot locate runtime module");
  std::vector<wchar_t> buffer(32768);
  auto size = GetModuleFileNameW(module, buffer.data(), DWORD(buffer.size()));
  if (!size || size >= buffer.size())
    throw std::runtime_error("Cannot resolve module filename");
  return std::filesystem::path(std::wstring(buffer.data(), size));
}
} // namespace astro
