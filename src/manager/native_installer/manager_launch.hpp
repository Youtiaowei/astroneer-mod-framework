#pragma once
// Verified newest-compatible persistent manager handoff (installer-manager-v1).
//
// The package entrance must run the verified newest compatible manager for
// actual mutations. It must not merely display a newer version and then keep
// using the old embedded transaction engine.
//
// Guarantees enforced here:
//   * manager provenance/path/hash verified before launch, and the file object
//     identity is held across the CreateProcess window (no write/no delete share)
//   * no ancestor directory is a reparse point
//   * Windows argv is quoted correctly (never a naive space join)
//   * forwarding loops are impossible (explicit handoff marker in argv)
//   * the child's exit code is propagated verbatim
//   * a failed activation keeps the previous manager
//   * a running EXE is never overwritten (versioned manager directories)
#include <cstdint>
#include <string>
#include <vector>

namespace manager_launch {

struct Candidate {
  std::string rel;     // game-relative installed manager EXE path
  std::string version;
  int64_t revision = 0;
  std::string sha256;
};

struct Decision {
  bool handoff = false;      // true => run the installed manager
  std::string reason;        // bounded diagnostic identity
  Candidate chosen;
};

// Select the newest compatible installed manager. `package_revision` is the
// revision of the engine compiled into the current package entrance. A newer
// installed manager wins; an equal revision is a no-op handoff (already the
// newest); an older one is never used.
Decision select(const std::vector<Candidate> &installed, int64_t package_revision);

// Quote one Windows argv element per CommandLineToArgvW rules.
std::wstring quote_argv(const std::wstring &arg);

// Launch `rel` with the forwarded argv. Returns the child exit code, or a
// negative value with `error` set on a failed activation (the previous manager
// must then stay in place).
int handoff(const std::string &game_root, const Candidate &manager,
            const std::vector<std::wstring> &args, std::string &error);

} // namespace manager_launch
