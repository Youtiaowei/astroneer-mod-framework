#pragma once
// Package/common provenance roles and CLOSED destination namespaces.
//
// installer-manager-v1: this is no longer a divergent hardcoded file list. The
// authoritative per-package file/record set is the package's own generated
// release.json, authenticated by:
//   * module rows      -> auth_record v1 module envelope (unchanged contract)
//   * common rows      -> package_record v1 envelope (package_record.hpp)
// The tables below are the CLOSED namespace allowlist that both kinds of signed
// record are still bound to. A signature authenticates bytes and ownership; it
// never grants a game path outside these namespaces.
#include "catalog.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace catalog {

// Historical short names kept so unrelated installer code is unchanged.
inline constexpr const char *CONFIG = CONFIG_TARGET;
inline constexpr const char *NATIVE_DLL = NATIVE_DLL_TARGET;

// Closed destination namespace for one shared component role.
inline const std::map<std::string, std::vector<std::string>> &COMMON_ALLOWED() {
  static const std::map<std::string, std::vector<std::string>> m = {
      {"core", {NATIVE_DLL_TARGET, CONFIG_TARGET, PAK}},
      {"loader", {LOADER_DLL_TARGET, PROXY_DLL_TARGET, LOADER_INI_TARGET,
                  LICENSE_TARGET}},
      {"manager", {MANAGER_EXE_TARGET, MANAGER_POINTER_TARGET}},
  };
  return m;
}

// Deterministic module payload destination (auth_record v1 namespace, resolved
// against AstroModManager/data). Keep in lockstep with docs/auth_record_contract.md §3.
inline std::string module_target(const std::string &id) {
  return std::string(STATE) + "/data/modules/" + id + "/" + id + ".dll";
}
inline std::string module_record_path(const std::string &id) {
  return "modules/" + id + "/" + id + ".dll";
}

// --- ONE deterministic definition of a common component's resource closure ---
//
// A package/common record authenticates a CLOSED set of resources as the
// ordered `path|sha256|` tuple of its `resources` array. "Ordered" is defined
// here and nowhere else: resources are sorted by their record-relative `path`
// in ascending byte order, with strictly increasing paths (duplicates refused).
//
//   record-relative path : common/<role>/<basename of the closed target>
//   installed target     : the closed COMMON_ALLOWED() entry for that role
//
// Both sides of the boundary consume exactly this:
//   * build_v3.py      -> sorts assembled payload rows by `path`, then signs
//   * payload.cpp      -> verifies the descriptor sequence against the rows
//   * engine.cpp       -> re-derives the same tuple from the installed targets
// The reverse map is required to be injective within a role; an ambiguous
// closed namespace is refused rather than guessed.
inline std::string common_resource_path(const std::string &role,
                                        const std::string &target) {
  return std::string("common/") + role + "/" +
         target.substr(target.find_last_of('/') + 1);
}

// Reverse map. Returns {} when the path is outside the closed namespace OR when
// two closed targets of the same role share a basename (ambiguous, never guess).
inline std::string common_resource_target(const std::string &role,
                                          const std::string &path) {
  std::string found;
  for (auto &t : COMMON_ALLOWED().at(role)) {
    if (path != common_resource_path(role, t))
      continue;
    if (!found.empty())
      return {};
    found = t;
  }
  return found;
}

// Closed-namespace well-formedness: every role's resource paths are distinct.
// A violation is a build/definition defect and is refused before any signing.
inline bool common_resource_namespace_unambiguous() {
  for (auto &[role, targets] : COMMON_ALLOWED()) {
    std::set<std::string> paths;
    for (auto &t : targets)
      if (!paths.insert(common_resource_path(role, t)).second)
        return false;
    if (paths.size() != targets.size())
      return false;
  }
  return true;
}

// Pinned shared loader identity. A pre-existing foreign loader must match one of
// these exact bytes to be shared/adopted; anything else is refused.
inline const std::map<std::string, std::string> &PINNED_LOADER() {
  static const std::map<std::string, std::string> m = {
      {LOADER_DLL_TARGET,
       "91d5444f41d19d0bb502f0fbdc15d76421e20359e898caa39e41e134c8f87958"},
      {PROXY_DLL_TARGET,
       "efc189c2829ec3328e09f946632789a8808bcb6b54d8b38172c332d9d1b79a01"},
      {LOADER_INI_TARGET,
       "8ae70f02d1540b5a3610ce9d33e0beaf31b51e867c85ba66d9ba43259a5037e3"},
  };
  return m;
}

inline std::string common_role_for(const std::string &target) {
  for (auto &[role, targets] : COMMON_ALLOWED())
    for (auto &t : targets)
      if (t == target)
        return role;
  return {};
}

} // namespace catalog
