#pragma once
// Production authenticated module loader for auth_record v1. Populates the
// existing ModuleRegistration/RuntimeConfig surface; it never calls a plugin
// factory (factory ownership stays with the Runtime).
//
// Loading order before ANY image code runs (docs §6):
//   parse registry -> verify signature -> validate namespace/ABI -> intersect
//   authoritative installed identities -> acquire and
//   validate the ancestor directory pins root-down -> open the candidate file
//   object with no write/no-delete sharing -> hash/size THAT held object ->
//   map the verified absolute image with LOAD_LIBRARY_SEARCH_SYSTEM32 only.
// The candidate directory is never a dependency search path.
#include "auth_record.hpp"
#include "module_registration.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace astro::auth {
struct LoadRequest {
  // Controlled absolute module root (production: the install data directory).
  // It may contain Chinese text; it must be an absolute local-drive path.
  std::filesystem::path module_root;
  std::string registry_utf8;
  TrustSet trusted;   // explicit trusted-key set from a trusted caller
  RecordPolicy policy;
  // Authoritative installed identities, already reconciled by the caller.
  // Only their exact intersection with authenticated records may be mapped;
  // an empty set authorizes nothing. Missing records remain visible as
  // module_record_not_installed instead of disappearing.
  std::vector<std::string> expected_ids;
};
struct LoadResult {
  std::vector<ModuleRegistration> modules;
  bool index_ok = false;
  Reject index_error = Reject::none;
  size_t dropped_rows = 0;
};
// Generic entry point. The caller owns the trust set; there is no
// disable-verify switch.
LoadResult load_installed_modules(const LoadRequest &request);
// Production entry point: passes ONLY the compiled public anchors. When no
// production anchor is provisioned this fails closed and maps nothing.
LoadResult load_production_modules(const std::filesystem::path &module_root,
                                   const std::string &registry_utf8,
                                   const std::vector<std::string> &expected_ids);
} // namespace astro::auth
