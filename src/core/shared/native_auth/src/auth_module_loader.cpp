// Production authenticated module loader (auth_record v1).
//
// C1: the verified absolute top-level image is mapped with
//     LOAD_LIBRARY_SEARCH_SYSTEM32 alone. The candidate directory is NOT a
//     dependency search path (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR is never used),
//     so a sibling DLL dropped next to a module cannot be pulled in. No weaker
//     flag is ever used as a retry.
// C2: the ancestor directory chain is acquired and validated root-down with the
//     proven io::DirectoryPins BEFORE the first candidate file-object open, and
//     the no-replacement leases plus the held file object stay alive through
//     mapping. The post-map identity check is supplemental, not a substitute.
#include "auth_module_loader.hpp"
#include "catalog.hpp"
#include "module_contract.hpp"
#include "number_contract.hpp"
#include "safe_io.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>

namespace astro::auth {
namespace {
// Pins first, held file second, image last. The destructor body unmaps first,
// then the held file object and the directory pins are released in reverse
// order (FreeLibrary -> CloseHandle(file) -> release pins).
struct CodeOwner {
  io::DirectoryPins pins;
  std::optional<io::Handle> file;
  HMODULE image = nullptr;
  ~CodeOwner() {
    if (image) {
      FreeLibrary(image);
      image = nullptr;
    }
    // file and pins release after the image is unmapped.
  }
  CodeOwner() = default;
  CodeOwner(const CodeOwner &) = delete;
  CodeOwner &operator=(const CodeOwner &) = delete;
};
ModuleRegistration unavailable(const std::string &id, Reject error) {
  ModuleRegistration registration;
  registration.catalog.id = id;
  registration.catalog.name = id;
  registration.load_error = reject_text(error);
  registration.catalog.load_error = registration.load_error;
  registration.code_kind = ModuleCodeKind::dynamic_library;
  return registration;
}
std::wstring module_filename(HMODULE image) {
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetModuleFileNameW(image, buffer.data(), DWORD(buffer.size()));
  if (!size || size >= buffer.size())
    return {};
  return std::wstring(buffer.data(), size);
}
// Optional callbacks must belong to the same authenticated, retained mapping.
bool owns_code(HMODULE image, const void *address) {
  MEMORY_BASIC_INFORMATION info{};
  if (!address || !VirtualQuery(address, &info, sizeof(info)) ||
      info.AllocationBase != image || info.State != MEM_COMMIT ||
      info.Type != MEM_IMAGE || (info.Protect & PAGE_GUARD))
    return false;
  const DWORD access = info.Protect & 0xff;
  return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
         access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}
// Read borrowed metadata only within the retained authenticated image. Check
// each bounded byte (including NUL) before dereferencing; strcmp on a signed
// but malformed module pointer is still an uncaught access violation.
bool owns_text(HMODULE image,const char* address,const std::string& expected) {
  if(!address||expected.empty()||expected.size()>64)return false;
  const auto start=reinterpret_cast<uintptr_t>(address);
  if(start>(std::numeric_limits<uintptr_t>::max)()-expected.size())return false;
  for(size_t i=0;i<=expected.size();++i) {
    const auto p=reinterpret_cast<const char*>(start+i);
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(p,&info,sizeof(info))||info.AllocationBase!=image
       ||info.State!=MEM_COMMIT||info.Type!=MEM_IMAGE||(info.Protect&PAGE_GUARD))return false;
    const DWORD access=info.Protect&0xff;
    if(access!=PAGE_READONLY&&access!=PAGE_READWRITE&&access!=PAGE_WRITECOPY
       &&access!=PAGE_EXECUTE_READ&&access!=PAGE_EXECUTE_READWRITE
       &&access!=PAGE_EXECUTE_WRITECOPY)return false;
    if(*p!=(i<expected.size()?expected[i]:'\0'))return false;
  }
  return true;
}
// Supplemental mapped-image identity check. The directory pins and the held
// no-replacement file lease already prevent a validate-then-reopen gap; this
// additionally proves the mapped image is the exact held object.
bool same_object(HANDLE held, HMODULE image) {
  BY_HANDLE_FILE_INFORMATION held_info{}, mapped_info{};
  if (!GetFileInformationByHandle(held, &held_info))
    return false;
  const std::wstring path = module_filename(image);
  if (path.empty())
    return false;
  io::Handle mapped(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (mapped.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(mapped.h, &mapped_info))
    return false;
  return held_info.dwVolumeSerialNumber == mapped_info.dwVolumeSerialNumber &&
         held_info.nFileIndexHigh == mapped_info.nFileIndexHigh &&
         held_info.nFileIndexLow == mapped_info.nFileIndexLow;
}
std::filesystem::path build_candidate(const std::filesystem::path &root,
                                      const std::string &relative) {
  std::filesystem::path candidate = root;
  size_t start = 0;
  for (;;) {
    const size_t slash = relative.find('/', start);
    candidate /= relative.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start);
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  candidate.make_preferred();
  return candidate;
}
// Map one already-authenticated row. Every failure keeps the module visible
// with a fixed identity and releases the mapping/handles; the factory is never
// called here.
ModuleRegistration load_row(const ModuleDescriptor &descriptor,
                            const std::filesystem::path &root) {
  ModuleRegistration registration;
  registration.catalog.id = descriptor.module_id;
  registration.catalog.name = descriptor.name_zh;
  registration.catalog.hotkey = descriptor.hotkey;
  registration.catalog.description = descriptor.description_zh;
  registration.name_en = descriptor.name_en;
  registration.description_en = descriptor.description_en;
  registration.version = descriptor.version;
  registration.code_kind = ModuleCodeKind::dynamic_library;
  auto fail = [&](Reject error) {
    registration.load_error = reject_text(error);
    registration.catalog.load_error = registration.load_error;
    registration.api = {};
    registration.number = {};
    registration.code_lifetime.reset();
    return registration;
  };
  const std::filesystem::path candidate =
      build_candidate(root, descriptor.dll_path);
  const std::filesystem::path directory = candidate.parent_path();
  auto owner = std::make_shared<CodeOwner>();
  try {
    // C2: acquire and validate the ancestor chain root-down BEFORE the first
    // candidate file-object open. The pins cover lookup, hashing and mapping.
    owner->pins.add(directory);
    owner->file.emplace(CreateFileW(
        candidate.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (owner->file->h == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      owner.reset();
      return fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                      ? Reject::module_payload_missing
                      : Reject::module_object_invalid);
    }
    // Object validation on the held handle, never a second path lookup.
    io::validate_object(owner->file->h);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(owner->file->h, &size) || size.QuadPart < 0)
      return fail(Reject::module_object_invalid);
    if (uint64_t(size.QuadPart) > dll_size_limit)
      return fail(Reject::module_payload_oversized);
    if (uint64_t(size.QuadPart) != descriptor.dll_size)
      return fail(Reject::module_size_mismatch);
    // Hash the exact held file; no reopen, no path-based re-read.
    const io::Bytes bytes = io::read_via_handle(owner->file->h, dll_size_limit);
    if (bytes.size() != descriptor.dll_size)
      return fail(Reject::module_size_mismatch);
    if (crypto::sha256_hex(bytes) != descriptor.dll_sha256)
      return fail(Reject::module_hash_mismatch);
    // The validated, no-write/no-delete file anchors the leaf as nonempty;
    // every ancestor contains the next pinned child. Overlap the pins to
    // allow file publication without ever granting directory delete-sharing.
    // Strict pins alone would prevent NativeFiles state/command commits under
    // the shared data root for the whole module lifetime.
    owner->pins.allow_file_rename();
    // Absolute verified image + SYSTEM32 only. Never a candidate/application/
    // current/user directory search and never a weaker-flag retry.
    owner->image =
        LoadLibraryExW(candidate.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!owner->image) {
      const DWORD error = GetLastError();
      owner.reset();
      return fail(error == ERROR_MOD_NOT_FOUND || error == ERROR_DLL_NOT_FOUND ||
                          error == ERROR_PROC_NOT_FOUND
                      ? Reject::module_dependency_unresolved
                      : Reject::module_map_failed);
    }
    if (!same_object(owner->file->h, owner->image))
      return fail(Reject::module_mapped_identity_mismatch);
    auto query = reinterpret_cast<module::Query>(
        GetProcAddress(owner->image, module::export_name));
    if (!query)
      return fail(Reject::module_export_missing);
    module::Api api{};
    if (query(module::protocol, module::fingerprint, &api, sizeof(api)) != 0 ||
        api.size != sizeof(module::Api) || api.version != module::protocol ||
        !api.create || !api.destroy || !api.call)
      return fail(Reject::module_abi_rejected);
    if (!owns_text(owner->image,api.id,descriptor.module_id) ||
        !owns_text(owner->image,api.module_version,descriptor.version))
      return fail(Reject::module_export_mismatch);
    auto number_query = reinterpret_cast<module::NumberQuery>(
        GetProcAddress(owner->image, module::number_export));
    if (number_query) {
      if (!owns_code(owner->image, reinterpret_cast<const void *>(number_query)))
        return fail(Reject::module_export_mismatch);
      module::NumberApi number{};
      if (number_query(module::protocol, module::fingerprint, &number,
                       sizeof(number)) != 0 || number.size != sizeof(number) ||
          number.version != 1 || !number.set)
        return fail(Reject::module_abi_rejected);
      if (!owns_text(owner->image,number.id,descriptor.module_id) ||
          !owns_code(owner->image, reinterpret_cast<const void *>(number.set)))
        return fail(Reject::module_export_mismatch);
      registration.number = number;
    }
    registration.api = api;
    registration.code_lifetime = owner;
    return registration;
  } catch (const std::exception &) {
    owner.reset();
    return fail(Reject::module_object_invalid);
  }
}
} // namespace

LoadResult load_installed_modules(const LoadRequest &request) {
  LoadResult result;
  // Fail closed: an unprovisioned trust root never maps a business image and
  // never falls back to a fixture key or an unsigned record.
  if (request.trusted.empty()) {
    result.index_error = Reject::trust_root_not_provisioned;
    for (const auto &id : request.expected_ids)
      result.modules.push_back(
          unavailable(id, Reject::trust_root_not_provisioned));
    return result;
  }
  RegistryOutcome registry =
      parse_registry(request.registry_utf8, request.trusted, request.policy);
  result.index_ok = registry.index_ok;
  result.index_error = registry.error;
  result.dropped_rows = registry.dropped_rows;
  if (!registry.index_ok) {
    for (const auto &id : request.expected_ids)
      result.modules.push_back(unavailable(id, registry.error));
    return result;
  }
  std::filesystem::path root;
  try {
    root = io::absolute(io::str(request.module_root));
  } catch (const std::exception &) {
    result.index_ok = false;
    result.index_error = Reject::namespace_invalid;
    for (const auto &id : request.expected_ids)
      result.modules.push_back(unavailable(id, Reject::namespace_invalid));
    return result;
  }
  for (const auto &row : registry.rows) {
    // Signature validity is provenance, not installed-module authorization.
    // Intersect with the caller's already reconciled installed identities BEFORE
    // even opening the image. An empty installed set authorizes no mappings.
    if (std::find(request.expected_ids.begin(), request.expected_ids.end(),
                  row.descriptor.module_id) == request.expected_ids.end())
      continue;
    if (row.error != Reject::none) {
      result.modules.push_back(unavailable(row.descriptor.module_id, row.error));
      continue;
    }
    result.modules.push_back(load_row(row.descriptor, root));
  }
  // Both installed identity and an authenticated record are required. Mere
  // presence of a DLL never enrolls it; a missing record stays visibly inactive.
  for (const auto &id : request.expected_ids) {
    bool present = false;
    for (const auto &module : result.modules)
      if (module.catalog.id == id) {
        present = true;
        break;
      }
    if (!present)
      result.modules.push_back(unavailable(id, Reject::module_record_not_installed));
  }
  return result;
}

LoadResult load_production_modules(const std::filesystem::path &module_root,
                                   const std::string &registry_utf8,
                                   const std::vector<std::string> &expected_ids) {
  LoadRequest request;
  request.module_root = module_root;
  request.registry_utf8 = registry_utf8;
  request.trusted = compiled_trust_anchors(); // compiled set only
  request.policy.now_unix = current_unix();
  request.policy.abi_fingerprint = module::fingerprint;
  request.policy.core_version = catalog::VERSION;
  request.expected_ids = expected_ids;
  return load_installed_modules(request);
}
} // namespace astro::auth
