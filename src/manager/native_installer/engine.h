// Core safety engine for Astroneer Mod Installer / persistent manager.
// Install/update/uninstall with bounded ownership and transactional file IO.
// Uses native Win32 APIs: CNG SHA-256, named mutex, atomic writes,
// reparse-point rejection.
//
// installer-manager-v1: the installed authority is the schema-2 registry
// (registry_v2.hpp). Package payload provenance is split into module provenance
// (auth_record v1, consumed unchanged) and package/common provenance
// (package_record v1). Status is the real offered-vs-installed union.
#pragma once
#include "json.hpp"
#include "directory_receipt.h"
#include "registry_v2.hpp"
#include "package_record.hpp"
#include "manager_launch.hpp"
#include "auth_record.hpp"
#include "legacy_authority.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace engine {

class InstallError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct FileEntry {
  std::string source;    // relative path in payload dir
  std::string target;    // relative path under game dir
  std::string component; // common role ("core"/"loader"/"manager") or module id
  std::string sha256;
  int64_t size = 0;
};

// One module offered by the currently opened package.
struct OfferedModule {
  std::string id;
  std::string version;
  int64_t revision = 0;
  std::string envelope; // exact signed auth_record v1 envelope text
  std::string source;   // payload-relative DLL source
};

// One shared component offered by the currently opened package.
struct OfferedCommon {
  std::string role; // core | loader | manager
  std::string version;
  int64_t revision = 0;
  std::string sha256;  // resource closure digest from the package_record
  std::string envelope; // exact signed package_record envelope text
};

struct Manifest {
  int schema = 0;
  std::string product;
  std::string package_id;
  std::string version;
  std::string game_version;
  std::string card_revision;
  std::string payload_kind;
  std::vector<FileEntry> files;
  std::vector<OfferedModule> modules;
  std::vector<OfferedCommon> common;
};

using DirectoryReceipt = io::DirectoryReceipt;
using LedgerEntry = registry::FileRow;
using Ledger = registry::Authority;

// Directory lifecycle owned by the transaction scope. Install captures receipts
// before ledger serialization. Uninstall commits its file/ledger changes before
// best-effort identity-bound directory cleanup. A retained ledger keeps historical
// creation receipts, not a current existence inventory; no post-cleanup rewrite.
struct DirPlan {
  std::vector<std::string> create_rels;
  std::set<std::string> backup_rels; // preserved legacy inputs; never replacement targets

  struct CleanupEntry {
    std::string rel;
    DirectoryReceipt receipt;
  };
  std::vector<CleanupEntry> cleanup_entries;

  Ledger pending_ledger;
  bool has_pending_ledger = false;
  bool erase_ledger = false;

  struct CreatedDir {
    std::string rel;
    DirectoryReceipt receipt;
  };
  std::vector<CreatedDir> created_with_receipts;

  std::vector<std::string> retained;
};

// Ledger path relative to game root.
inline const std::string ledger_rel_path() {
  return "AstroModManager/installed.json";
}
inline const std::string manager_pointer_rel() {
  return "AstroModManager/manager.pointer";
}
inline const std::string installed_mods_rel() {
  return "AstroModManager/data/installed-mods.json";
}
inline const std::string installed_records_rel() {
  return "AstroModManager/data/installed-records.json";
}

// Serialize an installed authority to JSON bytes (schema 2 only).
std::vector<uint8_t> ledger_to_bytes(const Ledger &l);

struct CommonStatus {
  std::string role;
  std::string installed_version;
  std::string available_version;
  int64_t installed_revision = 0;
  int64_t available_revision = 0;
  bool installed = false;
  bool offered = false;
  bool update_available = false;
  std::string issue;
};

struct ModuleStatus {
  std::string id;
  std::string name_zh;
  std::string name_en;
  std::string description_zh;
  std::string description_en;
  std::string installed_version;
  std::string available_version;
  int64_t installed_revision = 0;
  int64_t available_revision = 0;
  bool installed = false;
  bool offered = false;
  bool update_available = false;
  bool enabled = true;      // persistent desired state (user intent)
  bool payload_present = true; // owned bytes still on disk and matching
  std::string load_error;   // bounded diagnostic identity, never a translation
  std::string issue;
};

struct StatusResult {
  std::string version;
  std::string manager_version;
  std::string game_version;
  std::vector<std::string> issues;
  std::map<std::string, ModuleStatus> modules;
  std::map<std::string, CommonStatus> common;
  bool legacy = false;
  bool running = false;
  bool has_authority = false;
};

struct OperationResult {
  bool ok = false;
  std::string action;
  std::string version;
  std::string backup;
  int written = 0;
  std::vector<std::string> retained;
  std::vector<std::string> modules;
  std::string error;
  // Honest final-state publication: when a preference/state write failed the
  // result must say so instead of claiming a fresh on-disk stopped state.
  bool state_persisted = true;
  std::string state_error;
};

using ProcessCheckFn = std::function<bool()>;

class Manager {
public:
  Manager(const std::string &game, const std::string &payload_dir, const std::string &backup_root,
          ProcessCheckFn process_check = nullptr, const std::vector<std::string> &save_dirs = {},
          const astro::auth::TrustSet &module_trust = {},
          const astro::pkg::TrustSet &package_trust = {});

  StatusResult status();
  OperationResult install(const std::vector<std::string> &selected, bool adopt_legacy = false,
                          bool menu_only = false);
  OperationResult uninstall(const std::vector<std::string> &selected,
                            bool accept_save_risk = false, bool menu_only = false);
  // External persistent user intent: toggles exactly one module id. Allowed
  // only while the game is not running; never touches unrelated preferences.
  OperationResult toggle(const std::string &id, bool enabled);

  std::string game_path() const { return game_; }
  std::string payload_path() const { return payload_; }
  bool payload_available() const { return has_payload_; }
  void set_save_backup(bool enabled) { backup_saves_ = enabled; }
  std::optional<manager_launch::Candidate> installed_manager();
  int64_t offered_manager_revision() const;
  std::vector<std::string> protected_paths() const {
    auto paths = save_dirs_;
    paths.insert(paths.end(), {game_, backup_root_});
    if (!payload_.empty()) paths.push_back(payload_);
    return paths;
  }

private:
  std::string game_;
  std::string payload_;
  std::string backup_root_;
  ProcessCheckFn process_check_;
  std::vector<std::string> save_dirs_;
  bool backup_saves_ = false; // Save roots remain protected even when not copied.
  Manifest manifest_;
  std::map<std::string, FileEntry> rows_; // target -> entry
  std::map<std::string, std::string> planned_before_;
  std::set<std::string> planned_paths_;
  astro::auth::TrustSet module_trust_;
  astro::pkg::TrustSet package_trust_;
  bool has_payload_ = false;
  // Snapshot the pre-transaction identity of every path the transaction may
  // touch. `module_ids` adds the deterministic namespaces of modules this
  // operation can write or delete (a module row is never covered by the closed
  // common allowlist alone).
  void capture_before(const std::vector<std::string> &module_ids = {});
  // Additive plan extension, used only AFTER load_ledger() has validated an
  // existing authority: every installed module's deterministic namespace joins
  // the same planned allowlist and hash state, retained identities included.
  // Entries already captured before the authority was read are never replaced -
  // refreshing them would hide an external write made during planning.
  void plan_installed_modules(const Ledger &l);

  static std::string normalize_path(const std::string &p);
  std::string target_path(const std::string &rel);
  bool game_running();

  // Payload verification (inert data only; an imported package is never run).
  void verify_payload();
  Manifest load_manifest(const std::string &path);
  void verify_no_payload();

  // Authority management
  Ledger load_ledger();
  std::optional<legacy::NativeAuthority> load_native_legacy();
  void validate_native_legacy(const legacy::NativeAuthority &old, Ledger &ownership);
  std::set<std::string> allowed_files();
  bool same_game_root(const std::string &recorded);

  // Game validation
  void validate_game();

  class GameLock;
  std::unique_ptr<GameLock> acquire_lock();

  OperationResult transaction(const std::map<std::string, std::vector<uint8_t>> &changes,
                              const std::set<std::string> &deletions, const std::string &action,
                              std::set<std::filesystem::path> *created_directories = nullptr,
                              DirPlan *dir_plan = nullptr);

  bool detect_external_loader(const Ledger *ledger);

  struct LegacyResult {
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<std::string, std::vector<uint8_t>> retired;
    std::map<std::string, std::vector<uint8_t>> preferences;
  };
  LegacyResult adopt_legacy_files(bool allowed);

  void preflight();
  void validate_backup_roots();
};

} // namespace engine
