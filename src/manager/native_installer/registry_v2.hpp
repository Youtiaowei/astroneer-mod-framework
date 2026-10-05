#pragma once
// Schema-2 installed authority: the single transactional registry/ownership
// ledger for installer + persistent manager (installer-manager-v1).
//
// Every other on-disk list is a PROJECTION regenerated from this authority in
// the same transaction:
//   * Astro/.../AstroConsoleCore/native_config.json      (installed_modules)
//   * AstroModManager/data/installed-mods.json           (lightweight index)
//   * AstroModManager/data/installed-records.json        (auth_record v1 registry)
//   * AstroModManager/manager.pointer                    (manager launcher pointer)
// Projections are never independently editable; read_config keeps its equality
// checks for each of them.
//
// Old R7 refuses this document (it requires schema 1) and therefore cannot
// mutate a migrated installation. The installer itself never writes schema 1.
#include "directory_receipt.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace registry {

inline constexpr int authority_schema = 2;
inline constexpr const char *authority_product = "AstroneerModInstaller";
inline constexpr const char *authority_kind = "install_authority";

// Component roles used in file ownership. Module ids are their own role.
inline constexpr const char *role_core = "core";
inline constexpr const char *role_loader = "loader";
inline constexpr const char *role_manager = "manager";
inline constexpr const char *role_data = "data";

// Closed common component roles (package/common provenance).
inline constexpr const char *common_core = "core";
inline constexpr const char *common_loader = "loader";
inline constexpr const char *common_manager = "manager";

// One installed module: authenticated provenance + own version + persistent
// user intent. Unknown-to-this-package but authenticated identities are kept
// verbatim, including their exact signed envelope bytes.
struct ModuleRecord {
  std::string version;         // installed version (own)
  int64_t revision = 0;        // independently comparable revision
  std::string source_package;  // informational provenance label
  std::string envelope;        // EXACT signed auth_record v1 envelope text
  bool enabled = true;         // persistent desired state (user intent)
};

// One installed shared component. Compared independently of modules.
struct CommonRecord {
  std::string version;
  int64_t revision = 0;
  std::string sha256;  // closure digest over the immutable resource bytes
  std::string rel;     // manager only: installed relative EXE path
  std::string pointer; // manager only: stable launcher/pointer relative path
  bool owned = false;  // manager only: retained intentionally after last module
};

struct FileRow {
  std::string component; // role_core|role_loader|role_manager|role_data|<module id>
  std::string sha256;
  std::string owner;     // module id, or "common"/"loader"/"data"/"manager"
};

// Named construction only. Positional aggregate initialisation of this struct
// silently transposed `component` and `sha256` in the committed authority, so
// every file row must be built here where the three fields are named.
inline FileRow file_row(const std::string &component, const std::string &sha256,
                        const std::string &owner) {
  FileRow r;
  r.component = component;
  r.sha256 = sha256;
  r.owner = owner;
  return r;
}

struct Authority {
  // 0 means "no authority loaded from disk"; a parsed or installed authority
  // is always authority_schema (2). The writer never emits any other value.
  int schema = 0;
  std::string product = authority_product;
  std::string kind = authority_kind;
  std::string version;  // installer/manager product version that wrote it
  std::string game_root;
  int64_t sequence = 0; // monotonically increasing, survives migration
  bool loader_owned = false;
  std::map<std::string, ModuleRecord> modules;
  std::map<std::string, CommonRecord> common;
  std::map<std::string, FileRow> files;
  std::vector<std::string> generated_loader_files;
  std::vector<std::string> owned_directories;
  std::map<std::string, io::DirectoryReceipt> directory_receipts;
};

// Serialize/parse the authority document. Parsing is strict: unknown keys,
// duplicate identities, out-of-range values and incoherent ownership are
// refused before any filesystem effect.
std::vector<uint8_t> to_bytes(const Authority &a);
Authority parse(const std::string &text);

// Projections (all derived, never independently authoritative).
std::string native_config_projection(const Authority &a);
std::string installed_mods_projection(const Authority &a);
std::string installed_records_projection(const Authority &a);
std::string manager_pointer_projection(const Authority &a);

// Version policy (no downgrade). Compares common components independently of
// modules and of each other.
enum class Selection {
  none,         // nothing installed yet -> install offered
  install,      // offered is newer -> replace
  retain,       // installed is newer/equal-compatible -> keep installed bytes
  conflict,     // same version+revision, different immutable bytes
  incompatible, // offered revision/ABI cannot be applied on top of installed
};
struct VersionDecision {
  Selection selection = Selection::none;
  std::string detail;
};
VersionDecision decide_common(const CommonRecord *installed,
                              const CommonRecord &offered);

// Same-version immutable-byte conflict detection for modules.
enum class ModuleDecision { install, retain, conflict };
struct ModuleVerdict {
  ModuleDecision decision = ModuleDecision::install;
  std::string detail;
};
ModuleVerdict decide_module(const ModuleRecord *installed,
                            const ModuleRecord &offered,
                            const std::string &offered_sha256,
                            const std::string &installed_sha256);

} // namespace registry
