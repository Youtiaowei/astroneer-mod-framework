#pragma once
#include "catalog.h"
#include "safe_io.h"

namespace engine {
// Coexistence only: this log is never a payload/ledger row or a deletion target.
// Detect it even after the module leaves the ledger, so retained diagnostics do
// not poison the next install/toggle/removal. All other paths stay closed.
inline void check_state_tree(const std::filesystem::path &base,
                             const std::set<std::string> &allowed) {
  const std::string log =
      "AstroModManager/data/modules/planet_flattening/SmoothSphere-P1.log";
  auto coexist = allowed;
  // file() rejects directories, reparse points and hardlinks before admission.
  if (io::file(base / io::path(log)))
    coexist.insert(log);
  io::known_tree(base, catalog::STATE, coexist);
}
} // namespace engine
