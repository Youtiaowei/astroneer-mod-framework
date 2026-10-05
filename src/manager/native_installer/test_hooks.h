#pragma once
#ifdef INSTALLER_TEST_DRIVER
#include "safe_io.h"
namespace test_hooks {
// Raw Win32 write for simulating external writers.  Does NOT use the
// product's ownership-protected io::atomic so that test fixtures remain
// independent of the installer's safety contract.
inline void raw_write(const io::fs::path &p, const std::string &data, bool create_new = false) {
  io::Handle h(CreateFileW(p.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, create_new ? CREATE_NEW : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
  if (h.h == INVALID_HANDLE_VALUE) return;
  DWORD n = 0;
  WriteFile(h.h, data.data(), DWORD(data.size()), &n, nullptr);
  FlushFileBuffers(h.h);
}
inline std::string scenario;
inline int checks = 0, writes = 0;
inline bool late_writer_injected = false;
inline void (*observer)(const std::string &, const io::fs::path &, const io::fs::path &,
                        const io::fs::path &) = nullptr;
inline std::string owned_before, owned_intended;
inline io::Bytes intended_bytes(const io::Bytes &normal, const io::fs::path &target) {
  if ((scenario == "rollback" || scenario == "rollback-failure") && writes == 2) {
    auto changed = io::bytes("COMPLETE DIFFERENT TRANSACTION-OWNED FIXTURE CONTENT");
    owned_before = io::hash_or_absent(target);
    owned_intended = crypto::sha256_hex(changed);
    if (owned_before == owned_intended) throw std::runtime_error("Fixture must change bytes");
    return changed;
  }
  return normal;
}
inline bool running() {
  ++checks;
  return scenario == "running" || (scenario == "process-after-backup" && checks >= 2) ||
         (scenario == "process-mid-commit" && writes >= 2);
}
inline void event(const std::string &phase, const io::fs::path &game, const io::fs::path &backup,
                  const io::fs::path &target = {}) {
  if (observer) observer(phase, game, backup, target);
  const auto victim = game / L"Astro/Binaries/Win64/Mods/AstroConsoleCore/dlls/main.dll";
  const bool late_forward = phase == "before-mutation" &&
      (scenario == "late-forward-write" || scenario == "late-forward-delete");
  const bool late_restore = phase == "before-restore" && scenario == "late-restore";
  const bool late_post_write = phase == "after-write" && scenario == "late-post-write-throw";
  if (!late_writer_injected && target == victim &&
      (late_forward || late_restore || late_post_write)) {
    raw_write(target, "EXTERNAL-WRITER-CONTENT-MUST-SURVIVE");
    late_writer_injected = true;
    raw_write(backup / L"late-writer-evidence.json",
              "{\"external_writer_completed\":true,\"phase\":\"" + phase + "\"}", true);
    if (late_post_write)
      throw std::runtime_error("Injected failure after external writer completed");
  }
  if (phase == "committed" && scenario == "late-restore")
    throw std::runtime_error("Injected commit failure to exercise late rollback writer");
  if (phase == "before-backup" && scenario == "alter-before-backup")
    raw_write(game / L"Astro/Binaries/Win64/Mods/AstroConsoleCore/native_config.json",
              "externally changed before backup");
  if (phase == "after-backup" && scenario == "alter-after-backup")
    raw_write(game / L"Astro/Binaries/Win64/Mods/AstroConsoleCore/native_config.json",
              "externally changed after backup");
  if (phase == "backup" && scenario == "backup-failure")
    throw std::runtime_error("Injected backup failure");
  if (phase == "prepared" && scenario == "journal-failure")
    throw std::runtime_error("Injected journal write/flush failure");
  if (phase == "after-write") {
    ++writes;
    if ((scenario == "rollback" || scenario == "rollback-failure") && writes == 3) {
      const auto observed = io::hash_or_absent(target);
      if (observed != owned_intended || observed == owned_before)
        throw std::runtime_error("Owned fixture publication did not change bytes as intended");
      raw_write(backup / L"fixture-partial-write.json",
                "{\"partial_replacement_written_and_read_back\":true,"
                "\"complete_transaction_owned_publication\":true,\"before_sha256\":\"" +
                owned_before + "\",\"intent_sha256\":\"" + owned_intended +
                "\",\"observed_sha256\":\"" + observed + "\"}", true);
      throw std::runtime_error("Injected failure after third complete owned publication");
    }
  }
  if (phase == "rollback" && scenario == "rollback-failure")
    throw std::runtime_error("Injected restore failure");
  if (phase == "committed" && scenario == "commit-journal-failure")
    throw std::runtime_error("Injected committed journal failure");
  if (phase == "after-lock" && scenario == "hold-lock") {
    raw_write(backup.parent_path() / L"lock-ready", "ready", true);
    Sleep(2500);
  }
}
} // namespace test_hooks
#define TEST_EVENT(phase, game, backup) test_hooks::event(phase, game, backup)
#define TEST_INTENDED_BYTES(bytes, target) test_hooks::intended_bytes(bytes, target)
#define TEST_BEFORE_MUTATION(game, backup, target)                                                \
  test_hooks::event("before-mutation", game, backup, target)
#define TEST_BEFORE_RESTORE(game, backup, target)                                                 \
  test_hooks::event("before-restore", game, backup, target)
#define TEST_AFTER_WRITE(game, backup, target)                                                     \
  test_hooks::event("after-write", game, backup, target)
#else
#define TEST_INTENDED_BYTES(bytes, target) (bytes)
#define TEST_EVENT(phase, game, backup) ((void)0)
#define TEST_BEFORE_MUTATION(game, backup, target) ((void)0)
#define TEST_BEFORE_RESTORE(game, backup, target) ((void)0)
#define TEST_AFTER_WRITE(game, backup, target) ((void)0)
#endif
