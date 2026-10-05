#include "engine.h"
#include "legacy_catalog.h"
#include "legacy_parser.h"
#include "payload_catalog.h"
#include "runtime_sidecars.h"
#include "safe_io.h"
#include "test_hooks.h"
#include <algorithm>
#include <sstream>
#include <tlhelp32.h>
using namespace catalog;
namespace engine {
namespace fs = std::filesystem;
using io::Bytes;
static const std::string ledger_rel = std::string(STATE) + "/installed.json";
static const std::string old_config = std::string(SCRIPTS) + "/console_config.lua";
static const std::string enabled_rel = std::string(CORE) + "/enabled.txt";
static const std::string token_rel = std::string(STATE) + "/data/bridge-token";

// Ownership binding for a target path. Unknown paths are NOT silently core:
// the caller must bind them to an authenticated module id or a closed common
// role first (see verify_payload / allowed_files).
static std::string role(const std::string &r) {
  if (r == CONFIG || r == NATIVE_DLL_TARGET || r == PAK || r == enabled_rel ||
      r == old_config || r.rfind(std::string(SCRIPTS) + "/", 0) == 0)
    return registry::role_core;
  if (r == LOADER_DLL_TARGET || r == PROXY_DLL_TARGET || r == LOADER_INI_TARGET ||
      r == LICENSE_TARGET)
    return registry::role_loader;
  if (r == MANAGER_EXE_TARGET || r == MANAGER_POINTER_TARGET)
    return registry::role_manager;
  // Module payloads live under the data root but are never data-owned; they
  // bind to their authenticated module id.
  if (r.rfind(std::string(STATE) + "/data/modules/", 0) == 0)
    return {};
  if (r.rfind(std::string(STATE) + "/data/", 0) == 0 || r == ledger_rel)
    return registry::role_data;
  return {};
}
// Keep loader ownership symmetric between the reader and every transaction
// publication (including deferred ledgers). A partial row set is not an
// unowned loader and must never be serialized as a successful installation.
static void validate_loader_ownership(const Ledger &l) {
  const auto &targets = COMMON_ALLOWED().at(registry::common_loader);
  const auto common = l.common.find(registry::common_loader);
  if (common != l.common.end() && !l.loader_owned)
    throw InstallError("Incomplete loader ownership");
  for (const auto &[r, e] : l.files)
    if (e.owner == registry::role_loader || e.component == registry::role_loader)
      if (!l.loader_owned || e.owner != registry::role_loader ||
          e.component != registry::role_loader ||
          std::find(targets.begin(), targets.end(), r) == targets.end())
        throw InstallError("Invalid file ownership");
  if (l.loader_owned)
    for (const auto &r : targets) {
      const auto entry = l.files.find(r);
      if (entry == l.files.end())
        throw InstallError("Incomplete loader ownership");
      if (entry->second.owner != registry::role_loader ||
          entry->second.component != registry::role_loader)
        throw InstallError("Invalid file ownership");
    }
  // Legacy owned rows can lack a common record. When one exists, use the
  // authenticated package's record-relative ordering, not installed-path order.
  if (common != l.common.end()) {
    std::map<std::string, std::string> by_resource_path;
    for (const auto &r : targets)
      by_resource_path.emplace(common_resource_path(registry::common_loader, r),
                               l.files.at(r).sha256);
    std::string combined;
    for (const auto &[path, sha256] : by_resource_path)
      combined += path + "|" + sha256 + "|";
    if (crypto::sha256_hex(combined) != common->second.sha256)
      throw InstallError("Loader ownership closure mismatch");
  }
}

std::vector<uint8_t> ledger_to_bytes(const Ledger &l) {
  validate_loader_ownership(l);
  return registry::to_bytes(l);
}

// Required rows of a complete ownership ledger once any module is installed.
// Shared verbatim by the pre-commit check in install() and the reader in
// load_ledger(), so the writer and the reader can never disagree about what
// "complete" means.
static const std::set<std::string> &required_ownership_rows() {
  static const std::set<std::string> required{PAK, enabled_rel, token_rel, CONFIG,
                                              NATIVE_DLL_TARGET};
  return required;
}

std::string Manager::normalize_path(const std::string &p) { return io::str(io::absolute(p)); }
std::string Manager::target_path(const std::string &r) {
  io::relative(r);
  auto p = io::path(game_) / io::path(r);
  io::reject_links(p);
  return io::str(p);
}
bool Manager::game_running() {
  io::Handle h(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (h.h == INVALID_HANDLE_VALUE)
    io::fail("Cannot enumerate processes");
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (!Process32FirstW(h.h, &pe))
    io::fail("Cannot enumerate first process");
  do {
    if (_wcsicmp(pe.szExeFile, L"Astro-Win64-Shipping.exe") == 0)
      return true;
  } while (Process32NextW(h.h, &pe));
  if (GetLastError() != ERROR_NO_MORE_FILES)
    io::fail("Process enumeration incomplete");
  return false;
}
Manager::Manager(const std::string &game, const std::string &payload, const std::string &backup,
                 ProcessCheckFn process, const std::vector<std::string> &saves,
                 const astro::auth::TrustSet &module_trust,
                 const astro::pkg::TrustSet &package_trust)
    : game_(normalize_path(game)), payload_(payload.empty() ? std::string{} : normalize_path(payload)),
      backup_root_(normalize_path(backup)),
      process_check_(process ? process : [this] { return game_running(); }), save_dirs_(saves),
      module_trust_(module_trust), package_trust_(package_trust) {
#ifdef INSTALLER_FIXTURE_ROOT
  if (!io::within(io::path(game_), io::path(INSTALLER_FIXTURE_ROOT)) ||
      !io::within(io::path(backup_root_), io::path(INSTALLER_FIXTURE_ROOT)))
    throw InstallError("FIXTURE ONLY: game and backup must stay inside the "
                       "isolated build workspace");
  if (save_dirs_.empty())
    save_dirs_.push_back(io::str(io::path(INSTALLER_FIXTURE_ROOT) / L"no-real-saves"));
  for (auto &d : save_dirs_)
    if (!io::within(io::path(d), io::path(INSTALLER_FIXTURE_ROOT)))
      throw InstallError("FIXTURE ONLY: real saves prohibited");
#endif
  if (save_dirs_.empty()) {
    wchar_t local[32768];
    auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!n || n >= 32768)
      throw InstallError("Cannot resolve default saves");
    save_dirs_.push_back(io::str(fs::path(local) / L"Astro/Saved/SaveGames"));
  }
  for (auto &d : save_dirs_)
    io::absolute(d);
  // An imported package is inert data: when the payload directory is absent the
  // manager still runs and reports installed state, but offers nothing.
  if (!payload_.empty() && io::directory(io::path(payload_)))
    verify_payload();
  else
    verify_no_payload();
}
class Manager::GameLock {
  io::Handle handle;

public:
  explicit GameLock(const std::string &p)
      : handle(CreateMutexW(nullptr, TRUE,
                            io::wide("Local\\AstroneerModInstaller-" +
                                     crypto::sha256_hex(io::lock_identity(io::path(p))))
                                .c_str())) {
    if (!handle.h)
      io::fail("Cannot lock game");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
      throw InstallError("另一个安装器正在操作这个游戏目录。");
  }
  ~GameLock() { ReleaseMutex(handle.h); }
};
std::unique_ptr<Manager::GameLock> Manager::acquire_lock() {
  return std::make_unique<GameLock>(game_);
}
void Manager::validate_game() {
  io::reject_links(io::path(game_));
  if (!io::file(io::path(target_path(std::string(WIN64) + "/Astro-Win64-Shipping.exe"))))
    throw InstallError("请选择 ASTRONEER 游戏目录。");
}
void Manager::preflight() {
  validate_game();
  if (process_check_())
    throw InstallError("游戏正在运行。请保存并退出游戏。");
  if (has_payload_)
    verify_payload();
  validate_backup_roots();
}
void Manager::validate_backup_roots() {
  auto b = io::path(backup_root_), g = io::path(game_);
  io::reject_links(b);
  if (io::within(b, g) || io::within(g, b) ||
      (!payload_.empty() && io::within(b, io::path(payload_))))
    throw InstallError("备份必须放在游戏目录和安装包目录外。");
  for (auto &d : save_dirs_)
    if (io::within(b, io::path(d)))
      throw InstallError("Backup overlaps saves");
}
std::set<std::string> Manager::allowed_files() {
  std::set<std::string> a{ledger_rel,        CONFIG,          old_config,
                          enabled_rel,       installed_mods_rel(),
                          installed_records_rel(),            manager_pointer_rel(),
                          MANAGER_EXE_TARGET};
  for (auto &[role_name, targets] : COMMON_ALLOWED())
    for (auto &t : targets)
      a.insert(t);
  for (auto &[n, h] : legacy_hashes())
    a.insert(std::string(SCRIPTS) + "/" + n);
  for (auto &n : MUTABLE())
    a.insert(std::string(STATE) + "/data/" + n);
  return a;
}
bool Manager::same_game_root(const std::string &r) {
  try {
    auto p = io::absolute(r).make_preferred(), g = io::absolute(game_).make_preferred();
    if (CompareStringOrdinal(p.c_str(), -1, g.c_str(), -1, TRUE) != CSTR_EQUAL)
      return false;
    return io::identity(p) == io::identity(g);
  } catch (...) {
    return false;
  }
}
std::optional<legacy::NativeAuthority> Manager::load_native_legacy() {
  auto p = io::path(target_path(ledger_rel));
  if (!io::file(p)) return std::nullopt;
  auto document = json::parse(io::text(io::read(p, 1024 * 1024)));
  if (!document.has("schema") || !document["schema"].is_integer() ||
      document["schema"].as_int64() != 1) return std::nullopt;
  auto old = legacy::parse_native(document);
  if (!same_game_root(old.ownership.game_root))
    throw InstallError("旧版安装记录属于其他游戏目录；未修改游戏。");
  const auto data = std::string(STATE) + "/data/";
  auto preference = [&](const std::string &name) -> std::optional<json::Value> {
    const auto file = io::path(target_path(data + name));
    if (!io::file(file)) return {};
    try { return json::parse(io::text(io::read(file, 4096))); }
    catch (const std::exception &) { return {}; }
  };
  if (const auto settings = preference("settings.json");
      settings && settings->has("modules") && (*settings)["modules"].has("universal_slot")) {
    try { old.slot_enabled = (*settings)["modules"]["universal_slot"].as_bool(); }
    catch (const std::exception &) {}
  }
  int64_t sequence = 0;
  for (const auto *name : {"ui-preferences.json", "ingame-settings-a.json", "ingame-settings-b.json"})
    if (const auto raw = preference(name))
      if (const auto intent = legacy::journal_intent(*raw); intent && intent->first > sequence) {
        sequence = intent->first;
        old.slot_enabled = intent->second;
      }
  return old;
}
Ledger Manager::load_ledger() {
  if (load_native_legacy())
    throw InstallError("旧版安装需先备份迁移；未修改游戏。请使用安装 / 更新并确认接管。");
  auto p = io::path(target_path(ledger_rel));
  if (!io::file(p))
    return {};
  auto text = io::text(io::read(p, 1024 * 1024));
  Ledger l;
  try {
    l = registry::parse(text);
  } catch (const std::exception &e) {
    // Schema-1 native metadata has a separate explicit migration entrance.
    // A foreign or malformed schema-2 authority is never mutated.
    throw InstallError(std::string("安装记录无效；未修改游戏。") + e.what());
  }
  if (l.product != registry::authority_product || !same_game_root(l.game_root))
    throw InstallError("安装记录无效；未修改游戏。");
  auto allowed = allowed_files();
  for (auto &[id, m] : l.modules)
    allowed.insert(module_target(id));
  for (auto &[r, e] : l.files) {
    target_path(r);
    if (!allowed.count(r) || r == ledger_rel)
      throw InstallError("安装记录包含非本产品文件。");
    // Component and owner are bound to each other; a row may not claim a role
    // that the closed namespace for that path does not allow.
    if (e.component != e.owner)
      throw InstallError("Invalid file ownership");
    auto bound = role(r);
    if (!bound.empty()) {
      if (bound != e.owner)
        throw InstallError("Invalid file ownership");
    } else {
      // Module-owned row: the target must be that module's deterministic path.
      if (e.owner == registry::role_core || e.owner == registry::role_loader ||
          e.owner == registry::role_data || e.owner == registry::role_manager ||
          r != module_target(e.owner))
        throw InstallError("Invalid file ownership");
    }
  }
  for (auto &[id, m] : l.modules) {
    if (m.envelope.empty() || m.version.empty() || m.version.size() > 32)
      throw InstallError("Invalid installed module record");
  }
  if (!l.modules.empty() || l.common.count(registry::common_core)) {
    for (auto &r : required_ownership_rows())
      if (!l.files.count(r))
        throw InstallError("Incomplete ownership ledger");
    for (auto &[id, m] : l.modules)
      if (!l.files.count(module_target(id)))
        throw InstallError("Incomplete module ownership");
  }
  validate_loader_ownership(l);
  return l;
}
std::optional<manager_launch::Candidate> Manager::installed_manager() {
  if (load_native_legacy()) return std::nullopt;
  const auto ledger = load_ledger();
  const auto common = ledger.common.find(registry::common_manager);
  if (common == ledger.common.end()) return std::nullopt;
  const auto &record = common->second;
  const auto file = ledger.files.find(MANAGER_EXE_TARGET);
  if (!record.owned || record.rel != MANAGER_EXE_TARGET || file == ledger.files.end() ||
      file->second.owner != registry::role_manager || file->second.component != registry::role_manager)
    throw InstallError("Invalid manager ownership");
  // A component closure hash is NOT the hash of the executable to launch.
  const auto closure = common_resource_path(registry::common_manager, record.rel) +
                       "|" + file->second.sha256 + "|";
  if (crypto::sha256_hex(closure) != record.sha256)
    throw InstallError("Manager ownership closure mismatch");
  return manager_launch::Candidate{record.rel, record.version, record.revision, file->second.sha256};
}
int64_t Manager::offered_manager_revision() const {
  for (const auto &offer : manifest_.common)
    if (offer.role == registry::common_manager) return offer.revision;
  return 0;
}
static std::string trim(std::string s) {
  auto a = s.find_first_not_of(" \t\r\n");
  if (a == s.npos)
    return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
bool Manager::detect_external_loader(const Ledger *l) {
  auto t = [&](const std::string &n) {
    return io::path(target_path(std::string(WIN64) + "/" + n));
  };
  if (io::exists(t("ue4ss")) || io::exists(t("override.txt")))
    throw InstallError("其他加载器布局或自定义入口，拒绝覆盖。");
  const bool owned = l && l->loader_owned;
  if (owned) {
    validate_loader_ownership(*l);
    // Signed packages may legitimately carry different loader bytes from the
    // foreign-loader allowlist. Their complete installed ownership rows, not
    // a hard-coded distribution fingerprint, bind subsequent operations.
    for (const auto &r : COMMON_ALLOWED().at(registry::common_loader)) {
      auto p = io::path(target_path(r));
      if (!io::file(p) || crypto::sha256_hex(io::read(p)) != l->files.at(r).sha256)
        throw InstallError("已安装文件被修改：" + r);
    }
  }
  bool dll = io::file(t("UE4SS.dll")), proxy = io::file(t("dwmapi.dll"));
  if (!dll && !proxy) {
    if (io::exists(t("UE4SS-settings.ini")))
      throw InstallError("Unmanaged loader configuration");
    return false;
  }
  if (!dll || !proxy)
    throw InstallError("已有加载器不完整。");
  if (!owned) {
    auto dh = crypto::sha256_hex(io::read(t("UE4SS.dll"))),
         ph = crypto::sha256_hex(io::read(t("dwmapi.dll")));
    auto pinned = PINNED_LOADER();
    if (dh != pinned.at(LOADER_DLL_TARGET) ||
        (ph != LEGACY_PROXY_SHA256 && ph != pinned.at(PROXY_DLL_TARGET)))
      throw InstallError("Unknown loader identity");
  }
  auto text = io::text(io::read(t("UE4SS-settings.ini"), 65536));
  if (text.rfind("\xEF\xBB\xBF", 0) == 0)
    text.erase(0, 3);
  io::wide(text);
  std::istringstream ss(text);
  std::string line, section;
  std::map<std::string, std::string> ini;
  while (std::getline(ss, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#')
      continue;
    if (line[0] == '[') {
      if (line.back() != ']')
        throw InstallError("Malformed INI section");
      section = line.substr(1, line.size() - 2);
      continue;
    }
    auto eq = line.find('=');
    if (eq == line.npos || section.empty())
      throw InstallError("Malformed INI entry");
    auto k = trim(line.substr(0, eq));
    for (auto &c : k)
      c = char(tolower((unsigned char)c));
    ini[section + "/" + k] = trim(line.substr(eq + 1));
  }
  auto cache = ini["General/buseuobjectarraycache"];
  for (auto &c : cache)
    c = char(tolower((unsigned char)c));
  if (!ini["Overrides/modsfolderpath"].empty() || (cache != "false" && cache != "0") ||
      ini["EngineVersionOverride/majorversion"] != "4" ||
      ini["EngineVersionOverride/minorversion"] != "27")
    throw InstallError("共享加载器配置与已验证环境不同。");
  return !owned;
}
Manager::LegacyResult Manager::adopt_legacy_files(bool allowed) {
  LegacyResult result;
  if (!io::file(io::path(target_path(std::string(SCRIPTS) + "/main.lua")))) {
    for (auto &[name, hashes] : legacy_hashes())
      if (io::file(io::path(target_path(std::string(SCRIPTS) + "/" + name))))
        throw InstallError("Incomplete legacy installation; refusing orphan business Lua");
    if (io::file(io::path(target_path(old_config))))
      throw InstallError("Unmanaged legacy configuration");
    return result;
  }
  if (!allowed)
    throw InstallError("旧版接管需要明确确认。");
  auto permitted = allowed_files();
  io::known_tree(io::path(game_), CORE, permitted);
  for (auto &[name, hashes] : legacy_hashes()) {
    auto r = std::string(SCRIPTS) + "/" + name;
    auto p = io::path(target_path(r));
    if (!io::file(p))
      continue;
    auto b = io::read(p, 1024 * 1024);
    if (!hashes.count(crypto::sha256_hex(b)))
      throw InstallError("旧版文件未知或被修改：" + r);
    result.retired[r] = b;
  }
  for (auto &r : {old_config, enabled_rel}) {
    auto p = io::path(target_path(r));
    if (io::file(p))
      result.files[r] = io::read(p, 8192);
  }
  auto pak = io::path(target_path(PAK));
  if (io::file(pak)) {
    auto b = io::read(pak);
    const auto digest = crypto::sha256_hex(b);
    if (digest != CARD_SHA256 && digest != LEGACY_CARD_SHA256)
      throw InstallError("Legacy PAK mismatch");
    result.files[PAK] = b;
  }
  auto config = io::path(target_path(old_config));
  if (io::file(config)) {
    auto old = io::absolute(legacy::Parser(io::text(io::read(config, 8192))).root());
    io::reject_links(old);
    for (auto &n : {"ingame-settings-a.json", "ingame-settings-b.json", "bridge-token"}) {
      auto p = old / io::path(n);
      if (!io::file(p))
        continue;
      auto b = io::read(p, 4096);
      if (std::string(n) == "bridge-token") {
        auto s = io::text(b);
        if (s.size() != 64 || s.find_first_not_of("0123456789abcdef") != std::string::npos)
          throw InstallError("Invalid ownership hash");
      } else
        legacy::preferences(b);
      result.preferences[std::string(STATE) + "/data/" + n] = b;
    }
  }
  result.retired.insert({old_config, {}});
  return result;
}

// --- status: the real offered-vs-installed union ---
StatusResult Manager::status() {
  validate_game();
  const auto native_legacy = load_native_legacy();
  auto l = native_legacy ? Ledger{} : load_ledger();
  StatusResult s;
  s.version = VERSION;
  s.has_authority = native_legacy.has_value() || l.schema != 0;
  auto manager = l.common.find(registry::common_manager);
  s.manager_version =
      manager == l.common.end() ? std::string{} : manager->second.version;
  s.game_version = "unknown";
  try {
    std::istringstream text(io::text(io::read(io::path(target_path("build.version")), 4096)));
    std::string version;
    if (text >> version) s.game_version = version;
  } catch (const std::exception &) {
    // Optional diagnostic metadata never blocks status, install or recovery.
  }
  s.running = process_check_();
  s.legacy = native_legacy.has_value() ||
             io::file(io::path(target_path(std::string(SCRIPTS) + "/main.lua")));
  if (native_legacy) {
    for (const auto &[id, version] : native_legacy->modules) {
      auto &st = s.modules[id];
      st.id = id;
      st.installed = true;
      st.installed_version = version;
      st.enabled = native_legacy->slot_enabled;
      st.name_zh = "万能插槽";
      st.name_en = "Universal Slots";
    }
    CommonStatus core;
    core.role = registry::common_core;
    core.installed = true;
    core.installed_version = native_legacy->ownership.version;
    s.common[core.role] = core;
    for (const auto &[path, row] : native_legacy->ownership.files) {
      if (row.owner == registry::role_data) continue;
      const auto current = io::hash_or_absent(io::path(target_path(path)));
      if (current != row.sha256 &&
          !(path == NATIVE_DLL_TARGET && legacy::recognized_core(row.sha256) &&
            legacy::recognized_core(current))) s.issues.push_back(path);
    }
  }

  // Installed records: every one stays visible, including missing/corrupt ones.
  for (auto &[id, m] : l.modules) {
    ModuleStatus st;
    st.id = id;
    st.installed = true;
    st.installed_version = m.version;
    st.installed_revision = m.revision;
    st.enabled = m.enabled;
    auto target = module_target(id);
    auto p = io::path(target_path(target));
    auto it = l.files.find(target);
    if (it == l.files.end()) {
      st.payload_present = false;
      st.load_error = "module_payload_missing";
      st.issue = "missing";
    } else if (!io::file(p)) {
      st.payload_present = false;
      st.load_error = "module_payload_missing";
      st.issue = "missing";
    } else if (io::hash_or_absent(p) != it->second.sha256) {
      st.payload_present = false;
      st.load_error = "module_hash_mismatch";
      st.issue = "corrupt";
    }
    s.modules[id] = st;
  }
  // Offered records from the opened package (if any).
  for (auto &o : manifest_.modules) {
    auto &st = s.modules[o.id];
    st.id = o.id;
    st.offered = true;
    st.available_version = o.version;
    st.available_revision = o.revision;
    if (!st.installed) {
      st.installed = false;
      st.enabled = true;
    } else {
      st.update_available = o.revision > st.installed_revision;
    }
    if (st.name_zh.empty()) {
      astro::auth::RecordPolicy policy;
      policy.now_unix = astro::auth::current_unix();
      policy.core_version = VERSION;
      auto outcome = astro::auth::verify_record(o.envelope, module_trust_, policy);
      if (outcome.verified) {
        st.name_zh = outcome.descriptor.name_zh;
        st.name_en = outcome.descriptor.name_en;
        st.description_zh = outcome.descriptor.description_zh;
        st.description_en = outcome.descriptor.description_en;
      }
    }
  }
  // Display metadata for installed-only entries comes from the retained exact
  // signed envelope, never from an editable ledger field.
  for (auto &[id, st] : s.modules) {
    if (st.name_zh.empty()) {
      auto it = l.modules.find(id);
      if (it != l.modules.end()) {
        astro::auth::RecordPolicy policy;
        policy.now_unix = astro::auth::current_unix();
        policy.core_version = VERSION;
        auto outcome = astro::auth::verify_record(it->second.envelope, module_trust_, policy);
        if (outcome.verified) {
          st.name_zh = outcome.descriptor.name_zh;
          st.name_en = outcome.descriptor.name_en;
          st.description_zh = outcome.descriptor.description_zh;
          st.description_en = outcome.descriptor.description_en;
        } else {
          st.load_error = st.load_error.empty()
                              ? std::string(astro::auth::reject_text(outcome.error))
                              : st.load_error;
        }
      }
    }
    if (st.name_zh.empty())
      st.name_zh = id;
  }
  // Common components, compared independently of modules and of each other.
  for (auto &[role_name, c] : l.common) {
    CommonStatus st;
    st.role = role_name;
    st.installed = true;
    st.installed_version = c.version;
    st.installed_revision = c.revision;
    s.common[role_name] = st;
  }
  for (auto &o : manifest_.common) {
    auto &st = s.common[o.role];
    st.role = o.role;
    st.offered = true;
    st.available_version = o.version;
    st.available_revision = o.revision;
    if (!st.installed)
      continue;
    if (native_legacy) {
      st.update_available = true;
      continue;
    }
    auto decision = registry::decide_common(&l.common.at(o.role),
                                            {o.version, o.revision, o.sha256, {}, {}, false});
    st.update_available = decision.selection == registry::Selection::install;
    if (decision.selection == registry::Selection::conflict ||
        decision.selection == registry::Selection::incompatible)
      st.issue = decision.detail;
  }
  for (auto &[r, e] : l.files)
    if (e.component != registry::role_data && io::hash_or_absent(io::path(target_path(r))) != e.sha256)
      s.issues.push_back(r);
  return s;
}

OperationResult Manager::install(const std::vector<std::string> &selected, bool adopt, bool menu_only) {
  auto lock = acquire_lock();
  TEST_EVENT("after-lock", io::path(game_), io::path(backup_root_));
  preflight();
  capture_before();
  if (!has_payload_)
    throw InstallError("没有可用的安装包负载。");
  if (selected.empty() && !menu_only)
    throw InstallError("请选择有效模块。");
  if (menu_only && !selected.empty())
    throw InstallError("仅安装菜单不能同时选择 Mod。");
  if (menu_only && std::none_of(manifest_.common.begin(), manifest_.common.end(),
                               [](const auto &offer) { return offer.role == registry::common_core; }))
    throw InstallError("本安装包不提供共享菜单。");
  for (auto &id : selected) {
    bool known = false;
    for (auto &o : manifest_.modules)
      known = known || o.id == id;
    if (!known)
      throw InstallError("本安装包不提供该模块：" + id);
  }
  const auto native_legacy = load_native_legacy();
  if (native_legacy && !adopt)
    throw InstallError("旧版接管需要明确确认；将先备份，再迁移已安装模块。");
  auto l = native_legacy ? native_legacy->ownership : load_ledger();
  auto operation_modules = selected;
  if (manifest_.common.empty() && !manifest_.modules.empty() &&
      (native_legacy || !l.common.count(registry::common_core) ||
       !l.common.count(registry::common_manager)))
    throw InstallError("请先安装公共框架包，再安装这个 Mod。");
  if (native_legacy) {
    validate_native_legacy(*native_legacy, l);
    for (const auto &[id, version] : native_legacy->modules) {
      if (std::none_of(manifest_.modules.begin(), manifest_.modules.end(),
                       [&](const auto &offer) { return offer.id == id; }))
        throw InstallError("本安装包缺少旧版已安装模块，不能安全迁移：" + id);
      if (std::find(operation_modules.begin(), operation_modules.end(), id) == operation_modules.end())
        operation_modules.push_back(id);
    }
  }
  // Installed module namespaces (retained ones included) enter the same plan
  // the transaction recheck uses, so a legal second package can commit while
  // unrelated module directories stay closed to the tree check.
  plan_installed_modules(l);
  bool update = l.schema != 0;
  bool external = detect_external_loader(update ? &l : nullptr);
  auto allowed = allowed_files();
  // Module namespaces of every installed and offered identity stay in the
  // closed allowlist; they never widen it to arbitrary game paths.
  for (auto &[id, m] : l.modules)
    allowed.insert(module_target(id));
  for (auto &o : manifest_.modules)
    allowed.insert(module_target(o.id));
  io::known_tree(io::path(game_), CORE, allowed);
  check_state_tree(io::path(game_), allowed);
  std::map<std::string, Bytes> changes;
  std::set<std::string> deletes;
  for (auto &[r, e] : l.files)
    if (e.component != registry::role_data && io::file(io::path(target_path(r))) &&
        io::hash_or_absent(io::path(target_path(r))) != e.sha256)
      throw InstallError("已安装文件被修改：" + r);
  auto old = adopt_legacy_files(adopt);
  for (auto &[r, b] : old.files) {
    if (!l.files.count(r))
      l.files[r] = registry::file_row(role(r), crypto::sha256_hex(b), role(r));
  }
  for (auto &[r, b] : old.retired) {
    auto p = io::path(target_path(r));
    if (io::file(p))
      deletes.insert(r);
    l.files.erase(r);
  }
  for (auto &[r, e] : l.files)
    if (e.component != registry::role_data && io::file(io::path(target_path(r))) &&
        io::hash_or_absent(io::path(target_path(r))) != e.sha256)
      throw InstallError("已安装文件被修改：" + r);

  // Generated targets need prior authority too. Explicit legacy adoption above
  // may establish ownership of enabled.txt; an allowlisted pathname alone may not.
  for (const auto &r : {std::string(CONFIG), enabled_rel, installed_mods_rel(),
                        installed_records_rel(), manager_pointer_rel()})
    if (io::file(io::path(target_path(r))) && !l.files.count(r))
      throw InstallError("存在未受管理的文件：" + r);

  std::vector<std::string> applied;
  for (auto &id : operation_modules) {
    OfferedModule offer;
    for (auto &o : manifest_.modules)
      if (o.id == id)
        offer = o;
    registry::ModuleRecord proposed;
    proposed.version = offer.version;
    proposed.revision = offer.revision;
    proposed.source_package = manifest_.package_id + "@" + manifest_.version;
    proposed.envelope = offer.envelope;
    auto it = l.modules.find(id);
    const registry::ModuleRecord *installed = it == l.modules.end() ? nullptr : &it->second;
    auto target = module_target(id);
    auto row = rows_.find(target);
    if (row == rows_.end())
      throw InstallError("Module payload missing from package");
    std::string offered_sha = row->second.sha256;
    std::string installed_sha;
    if (installed) {
      auto f = l.files.find(target);
      installed_sha = f == l.files.end() ? std::string{} : f->second.sha256;
    }
    auto verdict = registry::decide_module(installed, proposed, offered_sha, installed_sha);
    if (verdict.decision == registry::ModuleDecision::conflict)
      throw InstallError("同一版本的模块内容不一致：" + id + " (" + verdict.detail + ")");
    if (verdict.decision == registry::ModuleDecision::retain) {
      // Retain the installed bytes; the entry is already manageable.
      applied.push_back(id);
      continue;
    }
    auto p = io::path(target_path(target));
    if (io::file(p) && !l.files.count(target))
      throw InstallError("存在未受管理的文件：" + target);
    auto b = io::read(io::path(payload_) / io::path(row->second.source));
    if (crypto::sha256_hex(b) != row->second.sha256)
      throw InstallError("Payload changed after validation");
    changes[target] = b;
    l.files[target] = registry::file_row(id, row->second.sha256, id);
    proposed.enabled = installed ? installed->enabled
        : native_legacy && native_legacy->modules.count(id) ? native_legacy->slot_enabled : true;
    l.modules[id] = proposed;
    applied.push_back(id);
  }

  // Shared components: no downgrade, newest compatible retained.
  for (auto &o : manifest_.common) {
    if (o.role == registry::common_loader && external) continue;
    registry::CommonRecord proposed;
    proposed.version = o.version;
    proposed.revision = o.revision;
    proposed.sha256 = o.sha256;
    auto it = l.common.find(o.role);
    const registry::CommonRecord *installed =
        it == l.common.end() ? nullptr : &it->second;
    auto decision = registry::decide_common(installed, proposed);
    if (decision.selection == registry::Selection::conflict)
      throw InstallError("同一版本的共享组件内容不一致：" + o.role);
    if (decision.selection == registry::Selection::incompatible)
      throw InstallError("共享组件版本无法在此安装上继续：" + o.role);
    if (decision.selection == registry::Selection::retain) {
      // Keep the newer (or identical) installed bytes: an older package opened
      // afterwards must not downgrade shared code.
      continue;
    }
    // One deterministic definition (payload_catalog.h): the closure digest is
    // the `path|sha256|` tuple of this component's resources in ascending
    // record-relative `path` order - the same sequence the signed descriptor
    // was required to be in and payload.cpp verified.
    std::map<std::string, FileEntry> role_rows;
    std::map<std::string, std::string> by_resource_path;
    for (auto &[r, row] : rows_)
      if (row.component == o.role) {
        auto res_path = catalog::common_resource_path(o.role, r);
        if (catalog::common_resource_target(o.role, res_path) != r)
          throw InstallError("共享组件命名空间不明确：" + o.role);
        if (!by_resource_path.emplace(res_path, r).second)
          throw InstallError("共享组件资源路径重复：" + o.role);
        role_rows[r] = row;
      }
    std::string combined;
    for (auto &[res_path, target] : by_resource_path)
      combined += res_path + "|" + rows_.at(target).sha256 + "|";
    if (role_rows.empty() || crypto::sha256_hex(combined) != o.sha256)
      throw InstallError("共享组件闭包与包记录不一致：" + o.role);
    for (auto &[r, row] : role_rows) {
      auto p = io::path(target_path(r));
      if (io::file(p) && !l.files.count(r))
        throw InstallError("存在未受管理的文件：" + r);
      auto b = io::read(io::path(payload_) / io::path(row.source));
      if (crypto::sha256_hex(b) != row.sha256)
        throw InstallError("Payload changed after validation");
      changes[r] = b;
      l.files[r] = registry::file_row(row.component, row.sha256, row.component);
    }
    proposed.rel = o.role == registry::common_manager ? std::string(MANAGER_EXE_TARGET)
                                                     : std::string{};
    proposed.pointer = o.role == registry::common_manager
                           ? std::string(MANAGER_POINTER_TARGET)
                           : std::string{};
    proposed.owned = o.role == registry::common_manager;
    l.common[o.role] = proposed;
  }

  // Projections of the authority, written in the same transaction.
  auto config_bytes = io::bytes(registry::native_config_projection(l));
  changes[CONFIG] = config_bytes;
  l.files[CONFIG] = registry::file_row(registry::role_core,
                                       crypto::sha256_hex(config_bytes), registry::role_core);
  auto mods_bytes = io::bytes(registry::installed_mods_projection(l));
  changes[installed_mods_rel()] = mods_bytes;
  l.files[installed_mods_rel()] = registry::file_row(
      registry::role_data, crypto::sha256_hex(mods_bytes), registry::role_data);
  auto records_bytes = io::bytes(registry::installed_records_projection(l));
  changes[installed_records_rel()] = records_bytes;
  l.files[installed_records_rel()] = registry::file_row(
      registry::role_data, crypto::sha256_hex(records_bytes), registry::role_data);
  auto pointer = registry::manager_pointer_projection(l);
  if (!pointer.empty()) {
    changes[manager_pointer_rel()] = io::bytes(pointer);
    l.files[manager_pointer_rel()] = registry::file_row(
        registry::role_manager, crypto::sha256_hex(pointer), registry::role_manager);
  }
  changes[enabled_rel] = Bytes{};
  l.files[enabled_rel] = registry::file_row(registry::role_core, crypto::sha256_hex(Bytes{}),
                                            registry::role_core);
  for (auto &[r, b] : old.preferences) {
    auto p = io::path(target_path(r));
    if (io::file(p)) {
      if (io::read(p) != b)
        throw InstallError("Unknown migration destination");
    } else
      changes[r] = b;
    l.files[r] = registry::file_row(registry::role_data, crypto::sha256_hex(b),
                                    registry::role_data);
  }
  if (io::file(io::path(target_path(token_rel)))) {
    if (!l.files.count(token_rel))
      throw InstallError("Unmanaged bridge token");
  } else if (!changes.count(token_rel)) {
    auto b = io::bytes(io::random() + io::random());
    changes[token_rel] = b;
    l.files[token_rel] = registry::file_row(registry::role_data, crypto::sha256_hex(b),
                                            registry::role_data);
  }
  // The authority must be complete before it is committed. This runs AFTER the
  // generated rows (config projection, enabled state, bridge token) exist: on a
  // first install those rows are created here, so the check can only be
  // satisfied by a real complete payload plus those generated rows - never by
  // an incomplete one slipping through, and never by being unsatisfiable.
  if (!l.modules.empty() || l.common.count(registry::common_core))
    for (auto &r : required_ownership_rows())
      if (!l.files.count(r))
        throw InstallError("Incomplete ownership ledger");
  DirPlan dir_plan;
  dir_plan.create_rels = l.owned_directories;
  if (native_legacy)
    for (const auto &[path, row] : native_legacy->ownership.files)
      dir_plan.backup_rels.insert(path);
  for (auto &r : {std::string(CORE), std::string(CORE) + "/dlls", std::string(STATE),
                  std::string(STATE) + "/data", std::string(STATE) + "/notices",
                  std::string(STATE) + "/data/modules", std::string(STATE) + "/bin"})
    if (std::any_of(changes.begin(), changes.end(),
                    [&](const auto &change) { return change.first.rfind(r + "/", 0) == 0; }) &&
        std::find(dir_plan.create_rels.begin(), dir_plan.create_rels.end(), r) ==
            dir_plan.create_rels.end())
      dir_plan.create_rels.push_back(r);
  for (auto &[id, m] : l.modules) {
    auto r = std::string(STATE) + "/data/modules/" + id;
    if (std::any_of(changes.begin(), changes.end(),
                    [&](const auto &change) { return change.first.rfind(r + "/", 0) == 0; }) &&
        std::find(dir_plan.create_rels.begin(), dir_plan.create_rels.end(), r) ==
            dir_plan.create_rels.end())
      dir_plan.create_rels.push_back(r);
  }
  // Loader ownership is claimed only when this installation actually owns the
  // complete closed loader row set. A package that offers no loader records no
  // ownership at all, so load_ledger never demands rows that were never
  // installed; a foreign loader stays foreign.
  bool owns_loader = !external;
  for (auto &t : {LOADER_DLL_TARGET, PROXY_DLL_TARGET, LOADER_INI_TARGET, LICENSE_TARGET})
    owns_loader = owns_loader && l.files.count(t) != 0;
  if (owns_loader && !update && !external &&
      !io::file(io::path(target_path(std::string(WIN64) + "/UE4SS.log"))))
    l.generated_loader_files = {std::string(WIN64) + "/UE4SS.log"};
  l.loader_owned = owns_loader;
  l.schema = registry::authority_schema;
  l.product = registry::authority_product;
  l.kind = registry::authority_kind;
  l.version = VERSION;
  l.game_root = game_;
  l.sequence += 1;
  dir_plan.pending_ledger = l;
  dir_plan.has_pending_ledger = true;
  std::set<fs::path> created_dirs;
  OperationResult r = transaction(changes, deletes, update ? "update" : "install",
                                   &created_dirs, &dir_plan);
  r.modules = applied;
  return r;
}

void Manager::validate_native_legacy(const legacy::NativeAuthority &old, Ledger &l) {
  const auto &old_hash = l.files.at(NATIVE_DLL_TARGET).sha256;
  const auto current = io::hash_or_absent(io::path(target_path(NATIVE_DLL_TARGET)));
  if (!legacy::recognized_core(old_hash) || !legacy::recognized_core(current))
    throw InstallError("旧版文件未知或被修改：" + std::string(NATIVE_DLL_TARGET));
  // The same receipt-bound builds apply to migration and direct removal.
  // Never refresh the separately captured transaction baseline.
  l.files.at(NATIVE_DLL_TARGET).sha256 = current;
  if (l.files.at(PAK).sha256 != CARD_SHA256)
    throw InstallError("旧版文件未知或被修改：" + std::string(PAK));
  for (const auto &[path, row] : l.files)
    if (row.owner != registry::role_data &&
        io::hash_or_absent(io::path(target_path(path))) != row.sha256)
      throw InstallError("旧版文件未知或被修改：" + path);
  const auto config = json::parse(io::text(io::read(io::path(target_path(CONFIG)), 4096)));
  if (config["schema"].as_int64() != 1 ||
      config["product"].as_string() != "AstroConsoleCore" || config["development"].as_bool())
    throw InstallError("旧版菜单配置不受支持；未修改游戏。");
  (void)config["presentation"].as_bool();
  std::set<std::string> expected, listed;
  for (const auto &[id, version] : old.modules) expected.insert(id);
  for (const auto &id : config["installed_modules"].as_array())
    if (!listed.insert(id.as_string()).second)
      throw InstallError("旧版模块配置与安装记录不一致；未修改游戏。");
  if (listed != expected)
    throw InstallError("旧版模块配置与安装记录不一致；未修改游戏。");
  // Both migration and direct removal regenerate these closed projections.
  // A known pathname or captured content hash alone is not ownership.
  for (const auto &path : {std::string(CONFIG), enabled_rel, installed_mods_rel(),
                           installed_records_rel(), manager_pointer_rel()})
    if (io::file(io::path(target_path(path))) && !l.files.count(path))
      throw InstallError("存在未受管理的文件：" + path);
}

OperationResult Manager::uninstall(const std::vector<std::string> &selected, bool risk, bool menu_only) {
  auto lock = acquire_lock();
  preflight();
  // Removal writes/deletes the deterministic namespaces of the selected
  // modules; those identities are not necessarily offered by the open package.
  capture_before(selected);
  if (!risk)
    throw InstallError("请确认存档风险：卸载后尺寸和挂接可能变化。");
  const auto native_legacy = load_native_legacy();
  auto l = native_legacy ? native_legacy->ownership : load_ledger();
  // Neither a standalone module nor the common-only library entrance owns the
  // framework through its selected modules. Aggregating its temporary offers
  // must not turn a module removal into an implicit shared-framework removal.
  // Explicit menu-only removal and older complete packages keep their policy.
  const bool retain_framework = !native_legacy && !menu_only &&
      (!has_payload_ || manifest_.common.empty() || manifest_.package_id == "astro_framework_light") &&
      l.common.count(registry::common_core);
  if (native_legacy) {
    validate_native_legacy(*native_legacy, l);
    if (menu_only && (!selected.empty() || !native_legacy->modules.empty()))
      throw InstallError("仍有已安装 Mod，不能单独卸载共享菜单。");
    for (const auto &id : selected)
      if (!native_legacy->modules.count(id)) throw InstallError("Not installed");
    if (!menu_only && selected.size() != native_legacy->modules.size())
      throw InstallError("请选择已管理模块。");
  }
  // Recognized legacy module code is in its monolithic Core. A modern DLL at
  // the same module's deterministic pathname is not legacy-owned code.
  const auto payload_modules = native_legacy ? std::vector<std::string>{} : selected;
  // Retained and removed module namespaces both enter the plan, before any of
  // them leaves the in-memory authority.
  plan_installed_modules(l);
  for (auto &rel : l.generated_loader_files)
    planned_before_.emplace(rel, io::hash_or_absent(io::path(target_path(rel))));
  if (!l.schema || (selected.empty() && !menu_only))
    throw InstallError("请选择已管理模块。");
  if (menu_only && (!selected.empty() || !l.modules.empty()))
    throw InstallError("仍有已安装 Mod，不能单独卸载共享菜单。");
  if (menu_only && !native_legacy && !l.common.count(registry::common_core))
    throw InstallError("尚未安装共享菜单。");
  for (auto &id : payload_modules)
    if (!l.modules.count(id))
      throw InstallError("Not installed");
  // A selected payload may only be deleted while its bytes are still the code
  // this authority registered. The pre-transaction disk capture proves the bytes
  // did not move while planning; it is not ownership evidence. Refuse a present
  // file that left its registration and keep the bytes, the authority and every
  // fixture tree untouched. An already-missing payload stays supported: it keeps
  // its visible ledger entry until this removal actually retires it, and the
  // receipt-bound empty directory is still eligible for safe cleanup below.
  for (auto &id : payload_modules) {
    auto r = module_target(id);
    auto p = io::path(target_path(r));
    if (!io::file(p))
      continue;
    auto registered = l.files.find(r);
    if (registered == l.files.end())
      throw InstallError("Invalid file ownership");
    if (io::hash_or_absent(p) != registered->second.sha256)
      throw InstallError("文件已被修改，未删除：" + r);
  }
  for (auto &id : payload_modules) {
    l.modules.erase(id);
    l.files.erase(module_target(id));
  }
  auto allowed = allowed_files();
  for (auto &[id, m] : l.modules)
    allowed.insert(module_target(id));
  // The modules being removed are still on disk at this point; their closed
  // namespaces must be known to the tree check or it would refuse the removal.
  for (auto &id : selected)
    allowed.insert(module_target(id));
  io::known_tree(io::path(game_), CORE, allowed);
  check_state_tree(io::path(game_), allowed);
  std::vector<std::string> others;
  auto mods = io::path(target_path(MODS));
  if (io::directory(mods))
    for (auto &e : fs::directory_iterator(mods)) {
      io::reject_links(e.path());
      if (io::directory(e.path()) && e.path().filename() != L"AstroConsoleCore")
        others.push_back(io::str(e.path().filename()));
    }
  bool shared = !others.empty() || !l.modules.empty() || retain_framework;
  std::map<std::string, Bytes> changes;
  std::set<std::string> deletes;
  for (auto &id : payload_modules) {
    auto r = module_target(id);
    auto p = io::path(target_path(r));
    if (io::file(p))
      deletes.insert(r);
    l.files.erase(r);
  }
  std::map<std::string, LedgerEntry> kept;
  bool retired_loader = false;
  for (auto &[r, e] : l.files) {
    bool module_row = false;
    for (auto &[id, m] : l.modules)
      module_row = module_row || e.owner == id;
    bool remove =
        e.component == registry::role_loader
            ? l.loader_owned && !shared
            : (module_row ? false
                          : (l.modules.empty() && !retain_framework &&
                             e.component != registry::role_data &&
                             e.component != registry::role_manager));
    if (remove) {
      auto p = io::path(target_path(r));
      if (io::file(p) && e.component != registry::role_data &&
          io::hash_or_absent(p) != e.sha256)
        throw InstallError("文件已被修改，未删除：" + r);
      deletes.insert(r);
      retired_loader = retired_loader || e.component == registry::role_loader;
    } else
      kept[r] = e;
  }
  // Retiring the owned loader must retire its authority in the same commit: a
  // ledger that still claims loader ownership for rows it no longer carries is
  // refused by the next legitimate read. A loader still shared with another
  // managed module or an external shared user, and a foreign loader this product
  // never claimed, keep both their rows and their ownership record.
  if (retired_loader) {
    l.loader_owned = false;
    l.common.erase(registry::common_loader);
    l.generated_loader_files.clear();
  }
  if (native_legacy && l.loader_owned && shared) {
    // Preserve the shared old loader without inventing a signed modern record
    // for unsigned legacy provenance. Admit only a recognized external loader.
    (void)detect_external_loader(nullptr);
    for (auto it = kept.begin(); it != kept.end();)
      if (it->second.owner == registry::role_loader) it = kept.erase(it);
      else ++it;
    l.loader_owned = false;
    l.generated_loader_files.clear();
  }
  // R25 change vs the historical R7 oracle: final removal RETAINS user settings,
  // prior backups and preferences. Only owned code is removed by reference.
  if (!l.modules.empty() || retain_framework) {
    auto b = io::bytes(registry::native_config_projection(l));
    changes[CONFIG] = b;
    kept[CONFIG] = registry::file_row(registry::role_core, crypto::sha256_hex(b),
                                      registry::role_core);
    auto mods_bytes = io::bytes(registry::installed_mods_projection(l));
    changes[installed_mods_rel()] = mods_bytes;
    kept[installed_mods_rel()] = registry::file_row(
        registry::role_data, crypto::sha256_hex(mods_bytes), registry::role_data);
  } else {
    // Last module removed: the manager is INTENTIONALLY retained with explicit
    // ownership. The game Mod loader is not kept active just to preserve
    // management/preferences. User settings, preferences and prior backups stay.
    l.common.erase(registry::common_core);
    kept.erase(CONFIG);
    kept.erase(enabled_rel);
    auto it = l.common.find(registry::common_manager);
    if (it != l.common.end()) {
      it->second.owned = true;
      if (!kept.count(MANAGER_EXE_TARGET))
        throw InstallError("Manager ownership ledger incomplete");
    }
  }
  auto records_bytes = io::bytes(registry::installed_records_projection(l));
  changes[installed_records_rel()] = records_bytes;
  kept[installed_records_rel()] = registry::file_row(
      registry::role_data, crypto::sha256_hex(records_bytes), registry::role_data);
  auto mods_bytes = io::bytes(registry::installed_mods_projection(l));
  changes[installed_mods_rel()] = mods_bytes;
  kept[installed_mods_rel()] = registry::file_row(
      registry::role_data, crypto::sha256_hex(mods_bytes), registry::role_data);
  l.files = kept;
  DirPlan dir_plan;
  dir_plan.has_pending_ledger = true;
  if (native_legacy)
    for (const auto &[path, row] : native_legacy->ownership.files)
      dir_plan.backup_rels.insert(path);
  dir_plan.erase_ledger = kept.empty();
  if (!kept.empty())
    dir_plan.pending_ledger = l;
  // Cleanup candidates are never discovered: a directory is eligible only when
  // this product recorded creating it (owned_directories) and can bind it to its
  // checked creation receipt. The transaction then removes only an empty object
  // whose identity still matches that receipt.
  auto plan_cleanup = [&](const std::string &rel) {
    if (std::find(l.owned_directories.begin(), l.owned_directories.end(), rel) ==
        l.owned_directories.end())
      return;
    if (std::any_of(dir_plan.cleanup_entries.begin(), dir_plan.cleanup_entries.end(),
                    [&](const DirPlan::CleanupEntry &e) { return e.rel == rel; }))
      return;
    DirPlan::CleanupEntry entry;
    entry.rel = rel;
    auto it = l.directory_receipts.find(rel);
    if (it != l.directory_receipts.end())
      entry.receipt = it->second;
    dir_plan.cleanup_entries.push_back(entry);
  };
  // The deterministic namespace of each validated selected module is a cleanup
  // candidate by ownership, not by a scheduled file deletion: an owned payload
  // that is already missing must still leave no empty directory behind, or the
  // next legitimate transaction fails the closed-tree check on it.
  for (auto &id : payload_modules)
    plan_cleanup(io::str(io::path(module_target(id)).parent_path()));
  for (auto &rel : {std::string(STATE) + "/data/modules", std::string(CORE) + "/dlls",
                    std::string(SCRIPTS), std::string(CORE), std::string(STATE) + "/data",
                    std::string(STATE) + "/notices", std::string(STATE)}) {
    if (std::any_of(deletes.begin(), deletes.end(),
                    [&](const auto &deleted) { return deleted.rfind(rel + "/", 0) == 0; }))
      plan_cleanup(rel);
  }
  auto r = transaction(changes, deletes, "uninstall", nullptr, &dir_plan);
  for (auto &msg : dir_plan.retained)
    r.retained.push_back(std::move(msg));
  if (l.modules.empty())
    r.retained.push_back("已保留设置、偏好与历史备份；管理器按所有权保留。");
  if (!l.loader_owned) {
    // Only claim a retained foreign loader when one is actually present.
    if (io::file(io::path(target_path(LOADER_DLL_TARGET))))
      r.retained.push_back("保留用户原有 UE4SS。");
  } else if (shared)
    r.retained.push_back("保留仍被其他模块使用的共享加载器。");
  r.modules = selected;
  return r;
}

OperationResult Manager::toggle(const std::string &id, bool enabled) {
  auto lock = acquire_lock();
  if (process_check_())
    throw InstallError("游戏正在运行。请在游戏外修改开关。");
  validate_backup_roots();
  capture_before();
  auto l = load_ledger();
  plan_installed_modules(l);
  if (!l.schema)
    throw InstallError("尚未安装。");
  auto it = l.modules.find(id);
  if (it == l.modules.end())
    throw InstallError("Not installed");
  if (it->second.enabled == enabled) {
    OperationResult r;
    r.ok = true;
    r.action = "toggle";
    r.version = VERSION;
    r.modules = {id};
    return r;
  }
  it->second.enabled = enabled;
  l.sequence += 1;
  std::map<std::string, Bytes> changes;
  std::set<std::string> deletes;
  auto config_bytes = io::bytes(registry::native_config_projection(l));
  changes[CONFIG] = config_bytes;
  l.files[CONFIG] = registry::file_row(registry::role_core,
                                       crypto::sha256_hex(config_bytes), registry::role_core);
  auto mods_bytes = io::bytes(registry::installed_mods_projection(l));
  changes[installed_mods_rel()] = mods_bytes;
  l.files[installed_mods_rel()] = registry::file_row(
      registry::role_data, crypto::sha256_hex(mods_bytes), registry::role_data);
  DirPlan dir_plan;
  dir_plan.has_pending_ledger = true;
  dir_plan.pending_ledger = l;
  auto r = transaction(changes, deletes, "toggle", nullptr, &dir_plan);
  r.modules = {id};
  return r;
}
} // namespace engine
