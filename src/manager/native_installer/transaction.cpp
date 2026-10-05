#include "catalog.h"
#include "engine.h"
#include "payload_catalog.h"
#include "runtime_sidecars.h"
#include "safe_io.h"
#include "test_hooks.h"
namespace engine {
namespace fs = std::filesystem;
void Manager::capture_before(const std::vector<std::string> &module_ids) {
  planned_before_.clear();
  auto paths = allowed_files();
  // A module payload lives in its own deterministic namespace and is NOT part
  // of the closed common allowlist. Without these rows the transaction plan
  // would fail closed on `planned_before_.at()` for every real module write.
  for (auto &o : manifest_.modules)
    paths.insert(catalog::module_target(o.id));
  for (auto &id : module_ids)
    paths.insert(catalog::module_target(id));
  for (auto &rel : paths)
    planned_before_[rel] = io::hash_or_absent(io::path(target_path(rel)));
  // The closed-tree recheck during the transaction must accept exactly the
  // paths this operation planned - including the module namespaces it creates.
  planned_paths_ = std::move(paths);
}
void Manager::plan_installed_modules(const Ledger &l) {
  // Installed module namespaces are NOT part of the closed common allowlist and
  // are unknown until the existing authority has been read and validated. They
  // still have to be part of the same plan: the transaction recheck walks the
  // whole owned tree and would otherwise refuse a retained module directory as
  // unknown. Identities are taken from the validated authority only - never
  // from arbitrary paths or a recursive scan - and are added before any of them
  // is removed from the in-memory authority, so removed and retained modules are
  // both covered.
  for (auto &[id, m] : l.modules) {
    auto rel = catalog::module_target(id);
    planned_paths_.insert(rel);
    // Additive: the pre-operation baseline captured before load_ledger() is the
    // authoritative one for shared files and must not be refreshed here.
    planned_before_.emplace(rel, io::hash_or_absent(io::path(target_path(rel))));
  }
}
OperationResult Manager::transaction(const std::map<std::string, io::Bytes> &changes,
                                     const std::set<std::string> &deletes,
                                     const std::string &action,
                                     std::set<fs::path> *created_directories,
                                     DirPlan *dir_plan) {
  auto game = io::path(game_), backup = io::path(backup_root_) / io::wide(io::random());
  // Long-lived pins prevent directory rename. Each IO operation additionally
  // acquires strict reparse protection before establishing its protected child.
  io::DirectoryPins pins(false);
  pins.add(game);
  // Freeze the complete required game-directory chain before backup. Existing
  // shared objects remain pinned; missing names must later be created by us.
  // Descendants of a planned-missing parent cannot become shared mid-plan.
  std::map<fs::path, bool> planned_missing;
  if (dir_plan) {
    std::set<fs::path> required;
    auto need_parents = [&](fs::path p) {
      for (auto q = p.parent_path(); q != game && io::within(q, game); q = q.parent_path())
        required.insert(q);
    };
    for (auto &[rel, bytes] : changes) {
      (void)bytes;
      need_parents(io::path(target_path(rel)));
    }
    if (dir_plan->has_pending_ledger && !dir_plan->erase_ledger)
      need_parents(io::path(target_path(ledger_rel_path())));
    for (const auto &p : required) {
      auto parent = planned_missing.find(p.parent_path());
      if ((parent != planned_missing.end() && parent->second) ||
          io::attrs(p) == INVALID_FILE_ATTRIBUTES) {
        planned_missing[p] = true;
      } else {
        if (!io::directory(p))
          throw InstallError("计划目录被文件占用：" + io::str(p));
        const auto expected = io::identity(p);
        pins.add(p);
        if (io::identity(p) != expected)
          throw InstallError("计划目录在保护交接时变化：" + io::str(p));
        planned_missing[p] = false;
      }
    }
  }
  auto pin_created_directory = [&](const fs::path &p, const io::DirectoryReceipt &receipt) {
    // Acquire continuous protection before comparing the original creation
    // identity. Pinning a same-name replacement is not ownership evidence.
    pins.add(p);
    io::Handle directory(CreateFileW(io::win32_path(p).c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!receipt.valid() || directory.h == INVALID_HANDLE_VALUE ||
        !receipt.matches(io::directory_receipt(directory.h)))
      throw InstallError("无法确认新建目录的归属，已取消操作。目录：" + io::str(p));
  };
  pins.add(backup.parent_path(), true);
  // A private transaction root must be created by this call, even if an
  // existing occupant is empty. Never infer ownership from directory contents.
  io::DirectoryReceipt backup_receipt;
  if (!io::atomic_create_directory(backup, backup_receipt))
    throw InstallError("备份目录已被占用。请使用新的备份位置。");
  pin_created_directory(backup, backup_receipt);
  // Only this transaction's admitted private containers may be reused.
  // A same-name directory observed on disk is never an ownership claim.
  std::map<fs::path, io::DirectoryReceipt> backup_directories{{backup, backup_receipt}};
  auto prepare_backup_parent = [&](const fs::path &parent) {
    std::vector<fs::path> chain;
    for (auto p = parent; p != backup; p = p.parent_path()) {
      if (!io::within(p, backup))
        throw InstallError("备份路径不在本次事务目录内：" + io::str(p));
      chain.push_back(p);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (backup_directories.count(*it)) continue;
      io::DirectoryReceipt receipt;
      if (!io::atomic_create_directory(*it, receipt))
        throw InstallError("备份内部目录已被占用，取消操作：" + io::str(*it));
      pin_created_directory(*it, receipt);
      backup_directories.emplace(*it, receipt);
    }
  };
  auto game_id = io::identity(game);
  // Mutable copies for deferred ledger support.
  auto effective_changes = changes;
  auto effective_deletes = deletes;
  std::set<std::string> all = effective_deletes;
  for (auto &[r, b] : effective_changes) {
    if (effective_deletes.count(r))
      throw InstallError("Conflicting transaction");
    all.insert(r);
  }
  // Track deferred ledger for backup/recheck even when not yet in changes.
  if (dir_plan)
    all.insert(ledger_rel_path());
  const auto write_targets = all;
  if (dir_plan)
    all.insert(dir_plan->backup_rels.begin(), dir_plan->backup_rels.end());
  std::map<std::string, std::string> before, after;
  std::vector<std::string> touched;
  std::map<std::string, io::MutationState> recovery;
  std::set<fs::path> created_dirs;
  json::Object journal{{"schema", 1},
                       {"action", action},
                       {"game_root", game_},
                       {"phase", "preparing"},
                       {"touched", json::Array{}}};
  json::Array phases;
  // First journal publication expects absence; subsequent writes require the
  // hash of the prior successfully published journal content.
  std::string journal_hash;
  std::vector<std::string> journal_warnings;
  auto record = [&](const std::string &phase) {
    TEST_EVENT(phase, game, backup);
    journal["phase"] = phase;
    phases.push_back(phase);
    journal["phases"] = phases;
    json::Array t;
    for (auto &r : touched)
      t.push_back(r);
    journal["touched"] = t;
    auto data = io::bytes(json::serialize(journal));
    io::MutationState journal_mutation;
    try {
      io::atomic(backup / L"transaction.json", data, nullptr, journal_hash, &journal_mutation);
    } catch (const std::exception &error) {
      // A completed publication remains ours even if its later cleanup failed.
      // Use intended bytes, never recapture a concurrent occupant's content.
      if (journal_mutation.published) journal_hash = crypto::sha256_hex(data);
      if (phase == "committed" && journal_mutation.published) {
        // Publication of the complete, flushed commit record is irreversible.
        // Do not roll the game back while leaving a durable committed journal.
        journal_warnings.push_back(std::string("文件已提交，事务日志后处理未完成：") + error.what());
        return;
      }
      throw;
    }
    journal_hash = crypto::sha256_hex(data);
  };
  auto current = [&](const std::string &r) { return io::hash_or_absent(io::path(target_path(r))); };
  try {
    TEST_EVENT("before-backup", game, backup);
    record("preparing");
    for (auto &r : all) {
      auto p = io::path(target_path(r));
      pins.add(p.parent_path());
      before[r] = planned_before_.at(r);
      if (current(r) != before[r])
        throw InstallError("Target changed after plan, before backup: " + r);
      if (!before[r].empty()) {
        TEST_EVENT("backup", game, backup);
        auto b = io::read(p);
        if (crypto::sha256_hex(b) != before[r])
          throw InstallError("Changed during backup");
        auto dest = backup / L"files" / io::path(r);
        prepare_backup_parent(dest.parent_path());
        io::atomic(dest, b, nullptr, "");
        if (crypto::sha256_hex(io::read(dest)) != before[r])
          throw InstallError("Backup mismatch");
      }
    }
    for (size_t i = 0; backup_saves_ && i < save_dirs_.size(); ++i) {
      auto dir = io::absolute(save_dirs_[i]);
      io::reject_links(dir);
      if (!io::directory(dir))
        continue;
      size_t count = 0;
      for (auto &e : fs::directory_iterator(dir)) {
        io::reject_links(e.path());
        if (++count > 10000)
          throw InstallError("Save directory bound");
        if (e.path().extension() != L".savegame")
          continue;
        auto b = io::read(e.path());
        TEST_EVENT("backup", game, backup);
        auto dest = backup / L"saves" / std::to_wstring(i) / e.path().filename();
        prepare_backup_parent(dest.parent_path());
        io::atomic(dest, b, nullptr, "");
        if (io::read(e.path()) != b)
          throw InstallError("Save changed during backup");
      }
    }
    json::Object b;
    for (auto &[r, h] : before)
      b[r] = h.empty() ? json::Value(nullptr) : json::Value(h);
    journal["before"] = b;
    record("prepared");
  } catch (const std::exception &e) {
    journal["error"] = e.what();
    try {
      record("backup_failed");
    } catch (const std::exception &je) {
      throw InstallError("备份或日志失败；未修改游戏：" + io::str(backup) + "; 原因：" +
                         e.what() + "; 失败日志：" + je.what());
    }
    throw InstallError("备份失败；未修改游戏：" + io::str(backup) + "; " + e.what());
  }
  const bool uninstall_cleanup = action == "uninstall" && dir_plan;
  auto cleanup_directories = [&] {
    std::sort(dir_plan->cleanup_entries.begin(), dir_plan->cleanup_entries.end(),
              [](const auto &a, const auto &b) { return a.rel.size() > b.rel.size(); });
    for (auto &entry : dir_plan->cleanup_entries) {
      try {
        auto p = io::path(target_path(entry.rel));
        io::DirectoryPins pin(false);
        pin.add(p.parent_path());
        const auto &io_rec = entry.receipt;
        if (!io::safe_remove_empty_directory(p, io_rec.valid() ? &io_rec : nullptr))
          dir_plan->retained.push_back("目录保留（无法证明所有权或非空）：" + entry.rel);
      } catch (const std::exception &e) {
        dir_plan->retained.push_back("文件卸载已提交，目录清理失败：" + entry.rel + "; " + e.what());
      }
    }
    // A retained ledger keeps conservative creation history, not an existence
    // inventory. Never rewrite it after commit to remove cleaned-up receipts.
  };
  try {
    TEST_EVENT("after-backup", game, backup);
    auto recheck = [&] {
      if (process_check_())
        throw InstallError("游戏已启动，取消并回滚。");
      if (io::identity(game) != game_id)
        throw InstallError("Game identity changed");
      for (auto &r : all) {
        auto expected = after.count(r) ? after[r] : before[r];
        if (current(r) != expected)
          throw InstallError("目标文件在备份后变化：" + r);
      }
      // Planned paths, not just the closed common allowlist: this transaction
      // legitimately creates deterministic module namespaces.
      io::known_tree(game, catalog::CORE, planned_paths_);
      check_state_tree(game, planned_paths_);
    };
    recheck();
    // One parent-first creation path covers managed, ancestor and implicit
    // Content/Paks directories. Never rescan a planned-missing name as shared.
    if (dir_plan) {
      for (const auto &[p, missing] : planned_missing) {
        if (!missing) continue;
        io::DirectoryReceipt rec;
        if (!io::atomic_create_directory(p, rec))
          throw InstallError("计划新建目录已被占用，取消安装：" + io::str(p));
        auto rel = io::str(p.lexically_relative(game));
        const auto &drec = rec;
        dir_plan->created_with_receipts.push_back({rel, drec});
        created_dirs.insert(p);
        pin_created_directory(p, rec);
        if (std::find(dir_plan->create_rels.begin(), dir_plan->create_rels.end(), rel) !=
            dir_plan->create_rels.end()) {
          auto &ledger = dir_plan->pending_ledger;
          if (std::find(ledger.owned_directories.begin(), ledger.owned_directories.end(), rel) ==
              ledger.owned_directories.end())
            ledger.owned_directories.push_back(rel);
          ledger.directory_receipts[rel] = drec;
        }
      }
      // Install/update always serialize after receipt capture and before any
      // file publication, even when no directory enters persistent ownership.
      if ((action == "install" || action == "update") &&
          dir_plan->has_pending_ledger && !dir_plan->erase_ledger)
        effective_changes[ledger_rel_path()] = ledger_to_bytes(dir_plan->pending_ledger);
    }
    for (auto &r : write_targets) {
      // Skip deferred entries (uninstall ledger, processed after the loop).
      if (dir_plan && r == ledger_rel_path() &&
          !effective_changes.count(r) && !effective_deletes.count(r))
        continue;
      recheck();
      auto p = io::path(target_path(r));
      if (effective_changes.count(r)) {
        if (dir_plan) {
          if (!planned_missing.count(p.parent_path()) || !io::directory(p.parent_path()))
            throw InstallError("目标父目录不在已保护计划中：" + r);
        } else {
          for (auto q = p.parent_path(); io::within(q, game) && q != game; q = q.parent_path())
            if (!io::directory(q)) created_dirs.insert(q);
          io::mkdir(p.parent_path());
        }
        pins.add(p.parent_path());
      }
      journal["next"] = r;
      record("applying");
      bool mutated = false;
      io::MutationState mutation;
      auto intended = effective_changes.count(r) ? TEST_INTENDED_BYTES(effective_changes.at(r), p) : io::Bytes{};
      try {
        TEST_BEFORE_MUTATION(game, backup, p);
        if (effective_changes.count(r)) {
          std::string expected = before.count(r) ? before[r] : std::string{};
          io::atomic(p, intended, &mutated, expected, &mutation);
        } else {
          io::erase(p, before.at(r), &mutated);
        }
      } catch (...) {
        if (mutation.pending()) recovery[r] = mutation;
        if (mutated) {
          touched.push_back(r);
          after[r] = effective_changes.count(r) && mutation.published ? crypto::sha256_hex(intended) : std::string{};
        }
        throw;
      }
      if (mutated)
        touched.push_back(r);
      // Record the hash of bytes this transaction actually wrote — NOT current(r),
      // which could reflect a later external overwrite.  This ensures rollback
      // detects external modifications instead of falsely claiming them.
      after[r] = effective_changes.count(r) ? crypto::sha256_hex(intended) : std::string{};
      TEST_AFTER_WRITE(game, backup, p);
      record("applying");
    }
    // Finalize the ledger once while file rollback and all pins are intact.
    // All uninstall directory cleanup is deferred until committed persists;
    // deleting a directory first would make its creation identity unrestorable.
    auto lr = ledger_rel_path();
    if (dir_plan && (!dir_plan->cleanup_entries.empty() || dir_plan->erase_ledger ||
                     (dir_plan->has_pending_ledger && !effective_changes.count(lr)))) {
      // Write or erase the deferred ledger.
      auto ledger_path = io::path(target_path(lr));
      if (dir_plan->erase_ledger || (dir_plan->has_pending_ledger &&
          dir_plan->pending_ledger.files.empty() && dir_plan->pending_ledger.modules.empty())) {
        // Full uninstall: erase existing ledger.
        bool erased = false;
        io::erase(ledger_path, before.at(lr), &erased);
        if (erased) {
          touched.push_back(lr);
          after[lr] = std::string{};
        }
      } else if (dir_plan->has_pending_ledger) {
        // Partial uninstall or updated ledger: write final state.
        auto ledger_bytes = ledger_to_bytes(dir_plan->pending_ledger);
        bool mutated = false;
        io::MutationState ledger_mutation;
        try {
          io::atomic(ledger_path, ledger_bytes, &mutated, before.at(lr), &ledger_mutation);
        } catch (...) {
          // Deferred ledger publication has the same recovery obligations as
          // ordinary file writes, including an error after a real publication.
          if (ledger_mutation.pending()) recovery[lr] = ledger_mutation;
          if (mutated) {
            touched.push_back(lr);
            after[lr] = ledger_mutation.published ? crypto::sha256_hex(ledger_bytes) : std::string{};
          }
          throw;
        }
        if (mutated) {
          touched.push_back(lr);
          after[lr] = crypto::sha256_hex(ledger_bytes);
        }
      }
    }
    recheck();
    record("committed");
  } catch (const std::exception &e) {
    json::Array errors;
    json::Array pending;
    auto note_recovery = [&](const std::string &rel, const io::MutationState &state) {
      pending.push_back(json::Object{{"target", rel}, {"parked", io::str(state.parked)},
          {"stage", io::str(state.stage)}, {"published", state.published},
          {"error", state.recovery_error}});
      errors.push_back(rel + ": held object recovery required; parked=" + io::str(state.parked) +
                       "; stage=" + io::str(state.stage) + "; " + state.recovery_error);
    };
    for (auto &[rel, state] : recovery) note_recovery(rel, state);
    for (auto it = touched.rbegin(); it != touched.rend(); ++it) {
      if (recovery.count(*it)) continue;
      io::MutationState restoration;
      try {
        TEST_EVENT("rollback", game, backup);
        auto p = io::path(target_path(*it));
        if (after.count(*it) && current(*it) != after[*it])
          throw InstallError("Rollback target changed externally");
        TEST_BEFORE_RESTORE(game, backup, p);
        if (before[*it].empty())
          io::erase(p, after.at(*it));
        else {
          auto b = io::read(backup / L"files" / io::path(*it));
          if (crypto::sha256_hex(b) != before[*it])
            throw InstallError("Rollback backup changed");
          io::atomic(p, b, nullptr, after.at(*it), &restoration);
        }
        if (current(*it) != before[*it])
          throw InstallError("Rollback readback mismatch");
      } catch (const std::exception &restore) {
        if (restoration.pending()) note_recovery(*it, restoration);
        errors.push_back(*it + ": " + restore.what());
      }
    }
    // Roll back created directories (install DirPlan) child-first.
    // Uses identity-validated safe_remove_empty_directory so that a
    // concurrent replacement at the same path is preserved.
    if (dir_plan && !dir_plan->created_with_receipts.empty()) {
      try {
        // File recovery is finished. Overlap game/backup-root protection before
        // retiring descendant pins that would block our own directory deletion.
        io::DirectoryPins roots(false);
        roots.add(game);
        roots.add(backup);
        pins = std::move(roots);
        std::sort(dir_plan->created_with_receipts.begin(),
                  dir_plan->created_with_receipts.end(),
                  [](const auto &a, const auto &b) { return a.rel.size() > b.rel.size(); });
        for (auto &cd : dir_plan->created_with_receipts) {
          try {
            auto p = game / io::path(cd.rel);
            io::DirectoryPins parent(false);
            parent.add(p.parent_path());
            auto &rec = cd.receipt;
            const auto &io_rec = rec;
            if (!io::safe_remove_empty_directory(p, &io_rec))
              throw InstallError("未能安全清理，未执行强制删除。");
          } catch (const std::exception &cleanup) {
            auto detail = "目录清理未完成：" + cd.rel + "; " + cleanup.what();
            errors.push_back(detail);
          }
        }
      } catch (const std::exception &cleanup) {
        std::string detail = std::string("目录回滚保护或准备失败：") + cleanup.what();
        for (const auto &cd : dir_plan->created_with_receipts)
          detail += "\n" + cd.rel;
        errors.push_back(detail);
      }
    }
    journal["error"] = e.what();
    journal["rollback_errors"] = errors;
    journal["recovery"] = pending;
    std::string recovery_details;
    for (const auto &detail : errors) recovery_details += "\n" + detail.as_string();
    try {
      record(errors.empty() ? "rolled_back" : "rollback_failed");
    } catch (const std::exception &je) {
      throw InstallError("回滚日志写入失败。请勿启动游戏。备份：" + io::str(backup) + "; 原因：" +
                         e.what() + "; 失败日志：" + je.what() + recovery_details);
    }
    // Only receipt-backed creations are eligible for cleanup; unresolved
    // directories are reported with the file-recovery outcome, never forced.
    throw InstallError(std::string(errors.empty() ? "操作失败，已回滚文件。备份："
                                                  : "操作失败且回滚未完成。请勿启动游戏。备份：") +
                       io::str(backup) + "; " + e.what() + recovery_details);
  }
  if (uninstall_cleanup) {
    try {
      // Acquire overlapping root protection BEFORE retiring descendant pins.
      // Each child-first cleanup then pins only its own ancestors, and the
      // unchanged helper binds deletion to the original creation receipt.
      io::DirectoryPins roots(false);
      roots.add(game);
      roots.add(backup);
      pins = std::move(roots);
      cleanup_directories();
    } catch (const std::exception &e) {
      // Files and ledger have committed: report retained cleanup, never enter
      // file rollback after an irreversible directory deletion.
      dir_plan->retained.push_back(std::string("文件卸载已提交，目录清理未完成：") + e.what());
    }
  }
  OperationResult r;
  r.ok = true;
  r.action = action;
  r.version = catalog::VERSION;
  r.backup = io::str(backup);
  r.retained = std::move(journal_warnings);
  r.written = int(all.size());
  if (created_directories)
    *created_directories = created_dirs;
  return r;
}
} // namespace engine
