#include "catalog.h"
#include "discovery.h"
#include "embedded_payload.h"
#include "engine.h"
#include "manager_launch.hpp"
#include "package_record.hpp"
#include "safe_io.h"
#include "test_hooks.h"
#include "ui.h"
#include "localization.h"
#include "output_file.h"
#include "auth_record.hpp"
#include "lightweight_payload.hpp"
#include "mod_library.hpp"
#include "owned_payload.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <shellapi.h>
#include <shobjidl.h>
static json::Value result_json(const engine::OperationResult &r) {
  json::Array mods, retained;
  for (auto &m : r.modules)
    mods.push_back(m);
  for (auto &s : r.retained)
    retained.push_back(s);
  return json::Object{{"ok", r.ok},
                      {"action", r.action},
                      {"version", r.version},
                      {"backup", r.backup},
                      {"written", r.written},
                      {"modules", mods},
                      {"retained", retained},
                      {"committed", r.ok},
                      {"state_persisted", r.state_persisted},
                      {"state_error", r.state_error}};
}
static json::Value status_json(const engine::StatusResult &s,
                               const std::vector<std::string> &library = {}) {
  json::Array diagnostics;
  json::Object mods, common;
  json::Array issues;
  for (auto &r : s.issues)
    issues.push_back(r);
  for (auto &line : library)
    diagnostics.push_back(line);
  for (auto &[id, m] : s.modules)
    mods[id] = json::Object{{"id", m.id},
                                {"name", m.name_zh},
                                {"name_en", m.name_en},
                                {"description", m.description_zh},
                                {"description_en", m.description_en},
                                {"installed", m.installed},
                                {"version", m.installed_version},
                                {"available_version", m.available_version},
                                {"installed_revision", m.installed_revision},
                                {"available_revision", m.available_revision},
                                {"offered", m.offered},
                                {"update_available", m.update_available},
                                {"enabled", m.enabled},
                                {"payload_present", m.payload_present},
                                {"load_error", m.load_error},
                                {"issue", m.issue}};
  for (auto &[role, c] : s.common)
    common[role] = json::Object{{"role", c.role},
                                {"installed", c.installed},
                                {"offered", c.offered},
                                {"version", c.installed_version},
                                {"available_version", c.available_version},
                                {"installed_revision", c.installed_revision},
                                {"available_revision", c.available_revision},
                                {"update_available", c.update_available},
                                {"issue", c.issue}};
  return json::Object{{"game_version", s.game_version},
                      {"version", s.version},
                      {"manager_version", s.manager_version},
                      {"legacy", s.legacy},
                      {"running", s.running},
                      {"has_authority", s.has_authority},
                      {"modules", mods},
                      {"common", common},
                      {"issues", issues},
                      {"library_diagnostics", diagnostics}};
}
static void shell(const std::string &p) {
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", io::wide(p).c_str(), nullptr,
                                              nullptr, SW_SHOWNORMAL)) <= 32)
    throw std::runtime_error("无法打开：" + p);
}
// Trust sets. Production consumes only compiled public anchors and fails closed
// while they are not provisioned. A fixture build may load an ephemeral public
// anchor produced by the in-memory test signer; that path is compile-time gated
// and the anchor must live inside the owned fixture root.
static void load_trust(astro::auth::TrustSet &modules, astro::pkg::TrustSet &packages,
                       const std::string &fixture_anchor) {
  modules = astro::auth::compiled_trust_anchors();
  packages = astro::pkg::compiled_package_anchors();
#ifdef INSTALLER_TEST_DRIVER
  if (fixture_anchor.empty())
    return;
#ifdef INSTALLER_FIXTURE_ROOT
  if (!io::within(io::absolute(fixture_anchor), io::path(INSTALLER_FIXTURE_ROOT)))
    throw std::runtime_error("FIXTURE ONLY: anchor must stay inside the build workspace");
#endif
  std::ifstream in(io::path(fixture_anchor), std::ios::binary);
  if (!in)
    throw std::runtime_error("Fixture anchor unreadable");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto v = json::parse(text);
  if (!v.has("kind") || v["kind"].as_string() != "fixture-not-provisioned" ||
      !v.has("key_id") || !v.has("public_key_blob"))
    throw std::runtime_error("Fixture anchor malformed");
  std::string blob_text = v["public_key_blob"].as_string();
  std::vector<uint8_t> blob;
  for (size_t i = 0; i + 1 < blob_text.size(); i += 2)
    blob.push_back(uint8_t(std::strtoul(blob_text.substr(i, 2).c_str(), nullptr, 16)));
  if (blob.size() != 72)
    throw std::runtime_error("Fixture anchor blob width");
  astro::auth::TrustAnchor ma;
  ma.key_id = v["key_id"].as_string();
  ma.public_key_blob = blob;
  astro::pkg::TrustAnchor pa;
  pa.key_id = ma.key_id;
  pa.public_key_blob = blob;
  modules.assign({ma});
  packages.assign({pa});
#else
  if (!fixture_anchor.empty())
    throw std::runtime_error("Fixture trust is not available in this build");
#endif
}
// Read a captured parameter without map::operator[]: a lookup must never create
// an entry, because CLI intent and result publication are decided by presence.
static const std::string &argument(const std::map<std::string, std::string> &args,
                                   const std::string &name) {
  static const std::string empty;
  auto it = args.find(name);
  return it == args.end() ? empty : it->second;
}
// Protected-root parameters. Every value ever supplied for one of these - even a
// later value under a refused duplicate - names a location where an error result
// must never be published.
static bool is_root_option(const std::string &name) {
  return name == "--game-path" || name == "--payload-dir" || name == "--backup-root" ||
         name == "--save-dir" || name == "--package-dir" || name == "--library-root";
}
// Option-likeness is a RAW property of the wide token: an unconvertible operand
// can still be an unknown `--` option, and such a token must never be swallowed
// as some other parameter's value. Known options are pure ASCII and always
// convert, so this predicate cannot misclassify one of them.
static bool option_like(const wchar_t *raw) {
  return raw && raw[0] == L'-' && raw[1] == L'-';
}
// Exact raw-token comparison without any conversion step.
static bool wide_is(const wchar_t *raw, const wchar_t *option) {
  return raw && option && std::wstring(raw) == option;
}
// Convert one raw wide argument for classification. A malformed operand is a
// command error, never an exception that can escape before action intent and the
// requested result target are known.
static bool to_text(const wchar_t *raw, std::string &out, std::string &error) {
  try {
    out = io::utf8(raw);
    return true;
  } catch (const std::exception &e) {
    out.clear();
    error = e.what();
    return false;
  }
}
static int run(int argc, wchar_t **argv) {
  std::map<std::string, std::string> args;
  std::set<std::string> flags{"--install",      "--uninstall",        "--status",
                              "--adopt-legacy", "--accept-save-risk", "--smoke-test",
                              "--manager", "--menu-only", "--backup-saves", "--start-uninstall"};
  std::set<std::string> valued{"--game-path", "--backup-root", "--modules",
                               "--result",    "--report",      "--smoke-dpi",
                               "--toggle"};
#ifdef INSTALLER_PERSISTENT_MANAGER
  valued.insert("--package-dir");
  valued.insert("--library-root");
  const bool persistent_manager = true;
#else
  const bool persistent_manager = false;
#endif
  std::string output;
  // Every protected-root value observed while parsing, including a later value
  // under a refused duplicate option. Used to gate early error publication when
  // the engine's resolved roots are not available yet.
  std::vector<std::string> root_inputs;
  // A root whose operand could not be read is unresolvable: the protected set
  // cannot be completed, so an error result must fail closed instead of being
  // published as if admission had been evaluated.
  bool roots_unresolved = false;
  bool output_admitted = false;
  // Retain resolved/discovered roots even when their Manager cannot be built.
  // Changing the UI root must never reuse an earlier output admission.
  auto remember_root = [&](const std::string &root) {
    output_admitted = false;
    if (!root.empty() && std::find(root_inputs.begin(), root_inputs.end(), root) == root_inputs.end())
      root_inputs.push_back(root);
  };
  std::unique_ptr<EmbeddedPayload> embedded;
  std::unique_ptr<lightweight::Payload> external_package;
  // The currently collected library offer (owned snapshot or base payload).
  // Refresh replaces it; confirm/toggle operate on the copy already displayed.
  stage::Active active_offer;
  stage::Retirement staging_retirement;
  auto staging_cleanup_result = [&]() -> json::Value {
    return json::Object{{"complete", staging_retirement.complete()},
                        {"detail", staging_retirement.detail()}, {"scope", "retired-snapshots"}};
  };
#ifdef INSTALLER_TEST_DRIVER
  // Boolean switches are flags, not valued: the parser is strict and a flag in
  // the valued set would silently consume the next argument as its value.
  flags.insert("--no-manager-handoff");
  valued.insert("--payload-dir");
  valued.insert("--save-dir");
  valued.insert("--scenario");
  valued.insert("--smoke-action");
  valued.insert("--fixture-anchor");
#endif
  // CLI action intent is the SYNTACTIC PRESENCE of an action parameter, never
  // the validity of its operand and never a map lookup that can insert a key.
  // A command that asked for a CLI action is refused as a CLI command; it must
  // never fall through to the interactive UI or a modal error dialog. Intent is
  // read from the RAW wide arguments before any conversion can throw, so a
  // malformed operand cannot hide that this is a CLI command. Known action
  // options are `--`-prefixed and can never be consumed as another parameter's
  // operand, so raw presence really is option-position presence.
  bool cli_action = false;
  bool toggle_present = false;
  for (int i = 1; i < argc; ++i) {
    const bool toggle = wide_is(argv[i], L"--toggle");
    toggle_present = toggle_present || toggle;
    cli_action = cli_action || toggle || wide_is(argv[i], L"--install") ||
                 wide_is(argv[i], L"--uninstall") || wide_is(argv[i], L"--status");
  }
  std::string toggle_id, toggle_error;
  bool toggle_on = false;
  bool toggle_operand_unreadable = false;
  std::string parse_error;
  auto note_error = [&](const std::string &message) {
    if (parse_error.empty())
      parse_error = message;
  };
  // Fail-closed admission for an error result, shared by every entry failure that
  // has no Manager yet (early CLI errors and a refused handoff). The protected
  // set is EVERY supplied or subsequently resolved/discovered root value
  // (including a refused duplicate's later value) plus the defaults the engine
  // would use: the implicit backup directory beside this EXE and the default save
  // tree. A parameter that never received an operand is not a root and is absent
  // from root_inputs, so it cannot abort the check for the roots that DID arrive
  // after the parse error. An unresolvable root fails closed instead.
  auto publish_error = [&](json::Value response) {
    // Recheck at the error sink: UI selection or a later failure can invalidate
    // a prior successful admission, and an existing output must remain intact.
    output_admitted = false;
    if (!output.empty() && !roots_unresolved) {
      try {
        // Missing, truncated or unconvertible defaults are admission failures,
        // never permission to publish with an incomplete protected-root set.
        std::vector<std::string> roots = root_inputs;
        wchar_t exe[32768];
        auto length = GetModuleFileNameW(nullptr, exe, 32768);
        if (!length || length >= 32768)
          throw std::runtime_error("Executable path failed");
        roots.push_back(io::str(io::fs::path(exe).parent_path() / L"AstroneerModBackups"));
#ifdef ASTRO_LIGHTWEIGHT_ENTRANCE
        // Include the implicit external resources even when parsing failed
        // before the normal payload owner could add its protected roots.
        roots.push_back(io::str(io::fs::path(exe).parent_path()));
#endif
        if (persistent_manager) {
          auto root = argument(args, "--game-path");
          if (root.empty()) root = io::str(io::fs::path(exe).parent_path().parent_path().parent_path());
          roots.push_back(root);
          roots.push_back(io::str(io::absolute(root).parent_path() / L"AstroneerModBackups"));
        }
        wchar_t local[32768];
        auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!n || n >= 32768)
          throw std::runtime_error("Cannot resolve default save directory");
        roots.push_back(io::str(io::fs::path(local) / L"Astro/Saved/SaveGames"));
        check_new_output(output);
        check_output_location(output, roots);
        output_admitted = true;
      } catch (const std::exception &location_error) {
        auto with_location = response.as_object();
        with_location["output_location_error"] = std::string(location_error.what());
        response = std::move(with_location);
      }
    }
    write_new_output(response, output, output_admitted);
    std::cout << json::serialize(response) << std::endl;
  };
  try {
    for (int i = 1; i < argc; ++i) {
      std::string a, token_error;
      if (!to_text(argv[i], a, token_error)) {
        // A malformed token is a command error, but it must not end the scan:
        // later and duplicate protected roots and the requested result target
        // still need their protections and their refusal.
        note_error(token_error);
        continue;
      }
      const bool known_flag = flags.count(a) != 0, known_valued = valued.count(a) != 0;
      if (!known_flag && !known_valued) {
        note_error("Unsupported argument: " + a);
        continue;
      }
      if (args.count(a)) {
        note_error("Duplicate argument: " + a);
        // A refused duplicate ROOT value is still a protected location: consume
        // and keep it so a later error result can never publish inside it.
        if (is_root_option(a) && i + 1 < argc && !option_like(argv[i + 1])) {
          std::string next, ignored;
          if (to_text(argv[i + 1], next, ignored))
            root_inputs.push_back(next);
          else
            roots_unresolved = true;
          ++i;
        }
        continue;
      }
      if (known_flag) {
        args[a] = "true";
        continue;
      }
      // A valued parameter consumes an operand only when the next token really
      // is one. A known option, or ANY other `--` option-like token, is never an
      // operand: swallowing an unknown option as a value would let a malformed
      // command perform a real operation. Record presence without inventing a
      // value, then KEEP PARSING so later parameters still reach their own
      // protections.
      std::string value;
      bool supplied = false;
      if (i + 1 < argc) {
        std::string next, next_error;
        const bool readable = to_text(argv[i + 1], next, next_error);
        const bool known_next = readable && (flags.count(next) || valued.count(next));
        if (!option_like(argv[i + 1]) && !known_next) {
          if (!readable) {
            note_error(next_error);
            if (is_root_option(a))
              roots_unresolved = true;
            if (a == "--toggle")
              toggle_operand_unreadable = true;
          }
          value = next;
          supplied = true;
          ++i;
        } else if (readable && !known_next) {
          // Unknown option-like token: it is an unknown option in its own right,
          // not this parameter's operand. Do not consume it; the next scan step
          // reports it at its own position.
          note_error("Unsupported argument: " + next);
        }
      }
      args[a] = value;
      if (is_root_option(a) && !value.empty())
        root_inputs.push_back(value);
      if (a == "--toggle") {
        toggle_present = true;
        if (!supplied)
          toggle_error = "Missing toggle operand";
        else if (value.empty() && !toggle_operand_unreadable)
          toggle_error = "Invalid toggle specification: empty operand";
      } else if (!supplied)
        note_error("Missing operand for parameter: " + a);
    }
    output = argument(args, "--result");
    int actions = int(args.count("--install")) + int(args.count("--uninstall")) +
                  int(args.count("--status")) + int(toggle_present);
    if (actions > 1)
      note_error("Conflicting actions");
    if (args.count("--menu-only") &&
        ((!args.count("--install") && !args.count("--uninstall")) || args.count("--modules")))
      note_error("Menu-only requires install or uninstall without module selection");
    if (toggle_present && !argument(args, "--modules").empty())
      note_error("Conflicting module selection");
    if (toggle_present && toggle_error.empty() && !toggle_operand_unreadable) {
      // Operand is present and nonempty: it must still be a real id=on|off pair
      // before any transaction is attempted.
      const auto &spec = argument(args, "--toggle");
      const auto eq = spec.find('=');
      const auto value = eq == spec.npos ? std::string{} : spec.substr(eq + 1);
      if (eq == spec.npos || eq == 0 || eq + 1 >= spec.size() ||
          (value != "on" && value != "off"))
        toggle_error = "Invalid toggle specification";
      else {
        toggle_id = spec.substr(0, eq);
        toggle_on = value == "on";
      }
    }
    // A syntactically invalid command never executes, however valid its other
    // fields are. A malformed toggle operand is reported as a toggle parameter
    // error so the caller can tell what to fix; the create-new and location
    // checks still gate whether that error may be published anywhere.
    if (!toggle_error.empty())
      throw std::runtime_error(toggle_error);
    if (!parse_error.empty())
      throw std::runtime_error(parse_error);
    check_new_output(output);
    std::vector<std::string> selected;
    std::istringstream ss(argument(args, "--modules"));
    std::string m;
    while (std::getline(ss, m, ','))
      selected.push_back(m);
    wchar_t exe[32768];
    auto length = GetModuleFileNameW(nullptr, exe, 32768);
    if (!length || length >= 32768)
      throw std::runtime_error("Executable path failed");
#ifdef ASTRO_LIGHTWEIGHT_ENTRANCE
    if (!actions && io::fs::path(exe).filename() == L"卸载MOD.exe")
      args.emplace("--start-uninstall", "");
#endif
    std::string payload;
#ifdef INSTALLER_TEST_DRIVER
    payload = argument(args, "--payload-dir");
    test_hooks::scenario = argument(args, "--scenario");
#endif
    if (persistent_manager) payload = argument(args, "--package-dir");
    if (payload.empty() && !persistent_manager) {
#ifdef ASTRO_LIGHTWEIGHT_ENTRANCE
      payload = io::str(io::fs::path(exe).parent_path() / L"payload");
      remember_root(io::str(io::fs::path(exe).parent_path()));
      remember_root(payload);
      external_package = std::make_unique<lightweight::Payload>(io::path(payload));
#else
      embedded = std::make_unique<EmbeddedPayload>();
      payload = embedded->path();
#endif
    }
    // Library routing: the persistent manager scans only the root it is given
    // explicitly; the universal bootstrap forwards its own distribution root at
    // handoff time. With no library argument, package flows stay unchanged.
    std::string library_root;
#ifdef INSTALLER_PERSISTENT_MANAGER
    library_root = argument(args, "--library-root");
#endif
#ifdef ASTRO_UNIVERSAL_ENTRANCE
    library_root = io::str(io::fs::path(exe).parent_path());
#endif
    auto game = argument(args, "--game-path");
    if (game.empty() && persistent_manager)
      game = io::str(io::fs::path(exe).parent_path().parent_path().parent_path());
    auto backup = argument(args, "--backup-root");
    if (backup.empty())
      backup = io::str((persistent_manager ? io::path(game).parent_path()
                                          : io::fs::path(exe).parent_path()) / L"AstroneerModBackups");
    std::vector<std::string> saves;
    engine::ProcessCheckFn process;
#ifdef INSTALLER_TEST_DRIVER
    if (argument(args, "--save-dir").empty())
      throw std::runtime_error("Fixture test driver requires --save-dir");
    saves.push_back(argument(args, "--save-dir"));
    if (test_hooks::scenario != "native-process")
      process = []() { return test_hooks::running(); };
#endif
    astro::auth::TrustSet module_trust;
    astro::pkg::TrustSet package_trust;
    load_trust(module_trust, package_trust, argument(args, "--fixture-anchor"));
    auto emit = [&](json::Value v) {
      const bool written = write_new_output(v, output, output_admitted);
      std::cout << json::serialize(v) << std::endl;
      return written ? 0 : 2;
    };
    auto admit_outputs = [&](const engine::Manager &manager) {
      output_admitted = false;
      auto roots = manager.protected_paths();
      roots.insert(roots.end(), root_inputs.begin(), root_inputs.end());
      check_output_location(output, roots);
      if (args.count("--smoke-test")) check_output_location(argument(args, "--report"), roots);
      output_admitted = true;
    };
    remember_root(game);
    remember_root(payload);
    remember_root(backup);
    for (const auto &save : saves)
      remember_root(save);
    auto forward_to_manager = [&](const std::string &root) -> std::optional<int> {
      if (root.empty() || persistent_manager || args.count("--manager")) return std::nullopt;
#ifdef INSTALLER_TEST_DRIVER
      if (args.count("--no-manager-handoff")) return std::nullopt;
#endif
      std::vector<manager_launch::Candidate> installed;
      int64_t offered_revision = 0;
      {
        engine::Manager offered(root, payload, backup, process, saves, module_trust, package_trust);
        if (auto current = offered.installed_manager()) installed.push_back(*current);
        offered_revision = offered.offered_manager_revision();
#ifdef ASTRO_UNIVERSAL_ENTRANCE
        // Revision 40 also preserves pre-receipt directory-creation cleanup.
        // Do not hand new requests to an older adapter with destructor-only
        // reporting; run locally until this compatible manager is installed.
        offered_revision = std::max<int64_t>(offered_revision, 40);
#elif defined(ASTRO_LIGHTWEIGHT_ENTRANCE)
        // Older managers predate the lightweight common-retention contract.
        // Run this normal entrance locally until a compatible manager is installed.
        offered_revision = std::max<int64_t>(offered_revision, 36);
#endif
      } // Release read/transaction leases before the child mutates the authority.
      auto decision = manager_launch::select(installed, offered_revision);
      if (!decision.handoff) return std::nullopt;
      std::vector<std::wstring> forwarded;
      for (const auto &[key, value] : args) {
        if (key == "--game-path" || key == "--backup-root" || key == "--payload-dir" ||
            key == "--library-root" || key == "--no-manager-handoff") continue;
        forwarded.push_back(io::wide(key));
        if (valued.count(key)) forwarded.push_back(io::wide(value));
      }
      forwarded.insert(forwarded.end(), {L"--manager", L"--game-path", io::wide(root),
                                        L"--backup-root", io::wide(backup),
                                        L"--package-dir", io::wide(payload)});
#ifdef ASTRO_UNIVERSAL_ENTRANCE
      // The bootstrap forwards the ORIGINAL distribution root beside this EXE,
      // never an already aggregated stage; the manager builds its own snapshot.
      forwarded.insert(forwarded.end(), {L"--library-root", io::wide(library_root)});
#endif
      std::string error;
      const int code = manager_launch::handoff(root, decision.chosen, forwarded, error);
      if (code < 0) throw std::runtime_error("manager_handoff_failed: " + error);
      return code;
    };
    if (actions) {
      if (auto code = forward_to_manager(game)) return *code;
      // Handoff already decided on the base common offer; the library is
      // collected once here, in the actual owner of the command.
      staging_retirement.replace(active_offer, stage::collect(payload, library_root, module_trust, catalog::VERSION));
      json::Value result;
      {
        engine::Manager manager(game, active_offer.payload, backup, process, saves, module_trust,
                                package_trust);
        manager.set_save_backup(args.count("--backup-saves") != 0);
        admit_outputs(manager);
        if (args.count("--status"))
          result = status_json(manager.status(), active_offer.diagnostics);
        else if (args.count("--install"))
          result = result_json(manager.install(selected, args.count("--adopt-legacy") != 0,
                                                args.count("--menu-only") != 0));
        else if (toggle_present) {
          // Parsing validated the operand before transaction admission.
          result = result_json(manager.toggle(toggle_id, toggle_on));
        } else
          result = result_json(manager.uninstall(selected, args.count("--accept-save-risk") != 0,
                                                  args.count("--menu-only") != 0));
      } // Release Manager source leases before retiring the command snapshot.
      auto response = result.as_object();
      if (!args.count("--status") && !active_offer.diagnostics.empty()) {
        json::Array diagnostics;
        for (const auto &line : active_offer.diagnostics) diagnostics.push_back(line);
        response["library_diagnostics"] = std::move(diagnostics);
      }
      staging_retirement.replace(active_offer, {});
      response["staging_cleanup"] = staging_cleanup_result();
      if (embedded) {
        const auto &detail = embedded->cleanup();
        // Cleanup warnings do not relabel an already committed installation as failed.
        response["extraction_cleanup"] = json::Object{{"complete", detail.empty()}, {"detail", detail}};
      }
      return emit(response);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ui::AppState initial;
    initial.start_uninstall = args.count("--start-uninstall") != 0;
    initial.game_path = game;
    initial.backup_path = backup;
    initial.backup_saves = args.count("--backup-saves") != 0;
    initial.message = "请选择 ASTRONEER 游戏目录。";
    if (game.empty()) {
      try {
        initial.game_path = discovery::find_game();
      } catch (const std::exception &e) {
        initial.message = e.what();
      }
    }
    remember_root(initial.game_path);
    if (auto code = forward_to_manager(initial.game_path)) {
      CoUninitialize();
      return *code;
    }
    ui::Callbacks cb;
    std::map<std::string, std::pair<bool, bool>> offered_map; // id -> {installed, offered}
    cb.validate_report = [&](const ui::AppState &state) {
      remember_root(state.game_path);
      engine::Manager manager(state.game_path, active_offer.payload, backup, process, saves,
                              module_trust, package_trust);
      admit_outputs(manager);
    };
    auto update_modules = [&](ui::AppState &state, const engine::StatusResult &status) {
      state.modules.clear();
      offered_map.clear();
      const auto core = status.common.find(registry::common_core);
      state.menu_installed = core != status.common.end() && core->second.installed;
      state.menu_offered = core != status.common.end() && core->second.offered;
      state.game_version = status.game_version;
      for (const auto &[id, m] : status.modules) {
        offered_map[id] = {m.installed, m.offered};
        const std::string label = m.installed ? (m.update_available ? "可更新 " : "已安装 ") : "未安装 ";
        const auto version = m.installed ? m.installed_version : m.available_version;
        auto text = label + version;
        if (m.update_available) text += " -> " + m.available_version;
        if (!m.load_error.empty()) text += " [" + m.load_error + "]";
        state.modules.push_back({id, m.name_zh, text, m.installed, m.offered,
                                 m.description_zh, m.enabled, m.offered, m.name_en, m.description_en});
      }
    };
    auto refresh = [&] {
      auto &state = ui::state();
      remember_root(state.game_path);
      try {
        // One collection per startup/Refresh; the shown rows and the later
        // confirm/toggle all use this same immutable snapshot.
        staging_retirement.replace(active_offer, stage::collect(payload, library_root, module_trust, catalog::VERSION));
        engine::Manager mgr(state.game_path, active_offer.payload, backup, process, saves,
                            module_trust, package_trust);
        auto status = mgr.status();
        state.valid = true;
        state.running = status.running;
        state.legacy = status.legacy;
        state.has_error = !status.issues.empty();
        state.message = status.running          ? "请保存并退出游戏后重试。"
                        : status.legacy && status.issues.empty()
                            ? "已识别旧版安装；确认安装 / 更新后先备份，再迁移。"
                        : status.issues.empty() ? "已核对游戏目录。安装器无需常驻运行。"
                                                : "检测到文件异常，操作会重新核对；请先检查备份。";
        for (const auto &line : active_offer.diagnostics)
          state.message += "\r\n" + line;
        if (!staging_retirement.complete()) state.message += "\r\n" + staging_retirement.detail();
        update_modules(state, status);
      } catch (const std::exception &e) {
        state.valid = false;
        state.has_error = true;
        state.message = e.what();
        if (!staging_retirement.complete()) state.message += "\r\n" + staging_retirement.detail();
      }
    };
    cb.refresh = refresh;
    cb.set_game_path = [&](const std::string &p) {
      ui::state().game_path = p;
      refresh();
    };
    cb.choose_game = [&] {
      IFileDialog *dialog = nullptr;
      if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog))))
        return;
      DWORD opts = 0;
      dialog->GetOptions(&opts);
      dialog->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
      if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
          PWSTR p = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            ui::state().game_path = io::utf8(p);
            CoTaskMemFree(p);
            refresh();
          }
          item->Release();
        }
      }
      dialog->Release();
    };
    auto prepare = [&](const std::string &requested_action) {
      auto &st = ui::state();
      auto action = requested_action;
      const bool framework_package = st.menu_offered &&
          std::none_of(st.modules.begin(), st.modules.end(), [](const auto &row) { return row.offered; });
      // Availability of a new offer is not ownership of an installed module.
      // A missing/bad download must not turn selected-module removal into
      // framework removal; only a truly empty installation may take that route.
      if (framework_package && (action == "install" ||
          (action == "uninstall" && std::none_of(st.modules.begin(), st.modules.end(),
                                               [](const auto &row) { return row.installed; }))))
        action += "-menu";
      st.pending_text.clear();
      selected.clear();
      // Selection is not a feature switch: it chooses what this operation acts
      // on. Install may only select modules this package actually offers;
      // uninstall may only select installed modules. Installed-only entries
      // still have real status/uninstall/toggle routes.
      for (auto &row : st.modules) {
        if (!row.selected)
          continue;
        auto it = offered_map.find(row.id);
        const bool offered = it != offered_map.end() && it->second.second;
        if (action == "install" && !offered)
          continue;
        if (action == "uninstall" && !row.installed)
          continue;
        selected.push_back(row.id);
      }
      const bool menu_only = action == "install-menu" || action == "uninstall-menu";
      if (action == "uninstall-menu" && !st.menu_installed) {
        st.message = "尚未安装共享菜单。";
        return;
      }
      if (action == "uninstall-menu" &&
          std::any_of(st.modules.begin(), st.modules.end(), [](const auto &row) { return row.installed; })) {
        st.message = "仍有已安装 Mod，不能单独卸载共享菜单。";
        return;
      }
      if (menu_only) selected.clear();
      if (selected.empty() && !menu_only) {
        st.message = action == "install"
                         ? "本安装包没有可安装的已选模块。"
                         : "请先勾选已安装模块。";
        return;
      }
      st.pending_action = action;
      st.requires_ack = action == "uninstall" || action == "uninstall-menu" || st.legacy;
      // Keep canonical pending state for the engine guard. The dialog renders
      // this same shared formatter in its currently selected display language.
      st.pending_text = installer_i18n::confirmation("zh-CN", action, backup, st.legacy, st.backup_saves);
    };
    cb.install = [&] { prepare("install"); };
    cb.uninstall = [&] { prepare("uninstall"); };
    cb.install_menu = [&] { prepare("install-menu"); };
    cb.uninstall_menu = [&] { prepare("uninstall-menu"); };
    cb.toggle = [&](const std::string &id, bool enabled) {
      // Use the already displayed snapshot, never a new scan behind the click.
      auto held = active_offer;
      const auto cleanup = staging_cleanup_result();
      ui::run_work([&, id, enabled, held, cleanup](ui::AppState st) {
        st.operation_result = json::Object{{"ok", false}, {"committed", false}};
        try {
          engine::Manager mgr(st.game_path, held.payload, backup, process, saves, module_trust,
                              package_trust);
          mgr.set_save_backup(st.backup_saves);
          auto result = mgr.toggle(id, enabled);
          auto operation = result_json(result).as_object();
          operation["staging_cleanup"] = cleanup;
          st.operation_result = std::move(operation);
          const auto status = mgr.status();
          if (!status.issues.empty()) throw std::runtime_error("操作后读回发现文件异常");
          update_modules(st, status);
          st.has_error = false;
          st.message = "操作完成。备份：" + result.backup;
          for (const auto &line : held.diagnostics) st.message += "\r\n" + line;
          if (!cleanup["complete"].as_bool()) st.message += "\r\n" + cleanup["detail"].as_string();
        } catch (const std::exception &e) {
          st.has_error = true;
          st.message = e.what();
        }
        return st;
      });
    };
    cb.cancel = [] { ui::state().pending_text.clear(); };
    cb.confirm = [&](bool ack) {
      if (ui::state().pending_text.empty() || (ui::state().requires_ack && !ack))
        return;
      auto selection = selected;
      // Consent covers the rows already displayed: install from that same
      // immutable snapshot, and keep its bytes held until the work completes.
      auto held = active_offer;
      const auto cleanup = staging_cleanup_result();
      ui::run_work([&, selection, held, cleanup](ui::AppState st) {
        st.operation_result = json::Object{{"ok", false}, {"committed", false}};
        try {
          engine::Manager mgr(st.game_path, held.payload, backup, process, saves, module_trust,
                              package_trust);
          mgr.set_save_backup(st.backup_saves);
          admit_outputs(mgr);
          const bool installing = st.pending_action == "install" || st.pending_action == "install-menu";
          const bool menu_only = st.pending_action == "install-menu" || st.pending_action == "uninstall-menu";
          auto r = installing ? mgr.install(selection, st.legacy, menu_only)
                              : mgr.uninstall(selection, true, menu_only);
          auto operation = result_json(r).as_object();
          operation["staging_cleanup"] = cleanup;
          st.operation_result = std::move(operation);
          auto status = mgr.status();
          if (!status.issues.empty())
            throw std::runtime_error("操作后读回发现文件异常");
          update_modules(st, status);
          st.valid = true;
          st.running = status.running;
          st.legacy = status.legacy;
          st.has_error = false;
          st.message = "操作完成。备份：" + r.backup;
          for (const auto &line : held.diagnostics) st.message += "\r\n" + line;
          if (!cleanup["complete"].as_bool()) st.message += "\r\n" + cleanup["detail"].as_string();
          for (auto &t : r.retained)
            st.message += "\r\n" + t;
        } catch (const std::exception &e) {
          st.message = e.what();
          st.has_error = true;
        }
        return st;
      });
    };
    cb.open_backups = [&] {
      try {
        io::mkdir(io::absolute(backup));
        shell(backup);
      } catch (const std::exception &e) {
        ui::state().message = e.what();
      }
    };
    cb.open_community = [&] {
      try {
        shell(catalog::COMMUNITY_URL);
      } catch (const std::exception &e) {
        ui::state().message = e.what();
      }
    };
    // Error results need the same resolved-root admission before UI option parsing.
    if (!output.empty()) cb.validate_report(initial);
    if (args.count("--smoke-dpi")) {
      if (!args.count("--smoke-test"))
        throw std::runtime_error("Smoke DPI requires smoke mode");
      ui::set_smoke_dpi(std::stoi(argument(args, "--smoke-dpi")));
    }
#ifdef INSTALLER_TEST_DRIVER
    if (args.count("--smoke-action")) {
      if (!args.count("--smoke-test"))
        throw std::runtime_error("Smoke action requires smoke mode");
      ui::set_smoke_action(argument(args, "--smoke-action"));
    }
#endif
    auto rc = ui::run_ui(initial, cb, args.count("--smoke-test") ? argument(args, "--report") : "");
    // run_ui joins its worker, so no operation still borrows this snapshot.
    staging_retirement.replace(active_offer, {});
    auto cleanup_detail = staging_retirement.detail();
    if (embedded) {
      const auto &detail = embedded->cleanup();
      if (!detail.empty()) cleanup_detail += (cleanup_detail.empty() ? "" : "\r\n") + detail;
    }
    if (!cleanup_detail.empty())
      MessageBoxW(nullptr, io::wide(installer_i18n::text(ui::language(), cleanup_detail)).c_str(),
                  io::wide(installer_i18n::text(ui::language(), "临时文件清理提醒")).c_str(), MB_OK | MB_ICONWARNING);
    CoUninitialize();
    return rc;
  } catch (const std::exception &e) {
    auto error = std::string(e.what());
    staging_retirement.replace(active_offer, {});
    if (!staging_retirement.complete()) error += "\r\n" + staging_retirement.detail();
    if (embedded) {
      const auto &detail = embedded->cleanup();
      if (!detail.empty()) error += "\r\n" + detail;
    }
    // Truthful final publication: an operation failure still publishes its
    // result document. Only an output LOCATION or create-new failure refuses
    // the write, and those checks run in publish_error too - a parse error never
    // skips them.
    publish_error(json::Object{{"ok", false}, {"error", error}, {"staging_cleanup", staging_cleanup_result()}});
    // CLI actions publish their failure and return; only an interactive run
    // opens a modal dialog. Intent is syntactic presence and is read from the
    // raw wide arguments, so a malformed `--toggle` is still a CLI command and
    // never reaches this dialog.
    if (!cli_action)
      MessageBoxW(nullptr, io::wide(installer_i18n::text(installer_i18n::load_language(), error)).c_str(),
                  io::wide(installer_i18n::text(installer_i18n::load_language(), "安装器错误")).c_str(), MB_OK | MB_ICONERROR);
    return 1;
  }
}
#ifdef INSTALLER_TEST_DRIVER
int wmain(int argc, wchar_t **argv) { return run(argc, argv); }
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv)
    return 1;
  auto rc = run(argc, argv);
  LocalFree(argv);
  return rc;
}
#endif
