#pragma once
// Bounded external Mod-library discovery (universal entrance / persistent manager).
//
// The library is a set of standalone Mod folders placed beside the framework
// EXE (the distribution root). Each folder carries the ORIGINAL signed module
// envelope `模组信息.json` plus the original `<module_id>.dll`; discovery never
// executes, maps or otherwise identifies a DLL. Only authenticated rows can
// claim display metadata: folder names are untrusted path facts used for
// diagnostics, never for names, versions or ownership.
#include "auth_record.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace modlib {

// Fixed discovery bounds (one scan pass, no directory watcher, no recursion).
inline constexpr size_t max_folders = 32;       // immediate child folders examined
inline constexpr size_t max_modules = 8;        // engine package bound
inline constexpr size_t max_record_bytes = 8192;   // auth_record envelope limit
inline constexpr size_t max_dll_bytes = 64ull * 1024 * 1024;
inline constexpr size_t max_diagnostics = 16;

// One folder that authenticated completely and can be staged.
struct Candidate {
  std::string id;         // authenticated module_id
  std::string version;
  int64_t revision = 0;
  std::string envelope;   // exact signed envelope text, bytes unchanged
  std::string dll_sha256; // authenticated payload closure
  uint64_t dll_size = 0;
  std::string folder;     // absolute source folder (diagnostic origin)
  std::string dll_path;   // absolute <folder>/<id>.dll source
};

struct Report {
  std::vector<Candidate> modules;           // valid, unique, within bounds
  std::vector<std::string> diagnostics;     // bilingual, bounded, actionable
};

// Enumerate immediate child folders of `root` exactly once. Folders without a
// `模组信息.json` (framework payload, caches, notes) are ignored silently. Each
// record is verified with production trust/policy; the source DLL is resolved
// strictly inside its own folder and must match the authenticated hash/size.
// `reserved` lists module ids already offered by the opened base package: such
// a candidate is refused (never overwrites a signed package offer).
Report discover(const std::string &root, const astro::auth::TrustSet &trust,
                const std::string &core_version,
                const std::vector<std::string> &reserved);

} // namespace modlib
