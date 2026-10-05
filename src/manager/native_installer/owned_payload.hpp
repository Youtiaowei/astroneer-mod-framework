#pragma once
// Owned staged payload adapter (universal Mod library).
//
// The engine keeps reading ONE bounded verified schema-3 manifest through its
// unchanged transaction logic. This unit aggregates the opened package's
// unchanged rows plus verified library modules into a private, create-only
// staging tree: exact bytes are copied and verified, held against replacement
// until the operation or manager handoff completes, then cleaned by exact
// published hash and receipt-bound empty directories. Existing folders are
// never reused or cleared; foreign occupants are retained with an explicit
// warning (EmbeddedPayload safety recipe).
#include "mod_library.hpp"
#include "safe_io.h"
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace stage {

// Keep cleanup evidence distinct from an optional preparation failure.
class PreparationFailure final : public std::runtime_error {
public:
  PreparationFailure(const std::string &cause, std::string cleanup)
      : std::runtime_error(cause), cleanup_(std::move(cleanup)) {}
  const std::string &cleanup_detail() const { return cleanup_; }
private:
  std::string cleanup_;
};

class Snapshot {
public:
  Snapshot(const std::string &parent, const std::string &base_payload,
           const modlib::Report &library);
  ~Snapshot();
  Snapshot(const Snapshot &) = delete;
  Snapshot &operator=(const Snapshot &) = delete;
  // One cleanup attempt; later calls return the same outcome.
  [[nodiscard]] const std::string &cleanup();
  std::string path() const { return io::str(root_); }
  const std::vector<std::string> &diagnostics() const { return diagnostics_; }

private:
  struct Published {
    io::fs::path path;
    std::string hash;
    bool published = false;
  };
  void publish(const io::fs::path &target, const io::Bytes &bytes, const std::string &hash);
  io::fs::path root_;
  std::vector<Published> files_;
  std::map<std::wstring, io::DirectoryReceipt> dir_receipts_;
  std::vector<io::Handle> held_; // published file identities, no write/no delete share
  std::unique_ptr<io::DirectoryPins> pins_;
  std::vector<std::string> diagnostics_;
  bool cleanup_attempted_ = false;
  std::string cleanup_report_;
};

// The active offer for one engine::Manager open: the base package payload when
// no library module is staged, otherwise the owned snapshot that exactly
// matches what the user is shown. `hold` keeps those bytes alive; diagnostics
// are the bounded bilingual rejected-folder report.
struct Active {
  std::string payload;
  std::shared_ptr<Snapshot> hold;
  std::vector<std::string> diagnostics;
  std::string preparation_cleanup;
};

// Retired snapshots remain alive until borrowed operation owners finish. Failed
// one-attempt cleanup is recorded for callers, never retried or silently erased.
class Retirement {
public:
  void replace(Active &current, Active next);
  void reap();
  bool complete() const { return retained_.empty() && pending_.empty(); }
  std::string detail() const;
private:
  std::vector<std::shared_ptr<Snapshot>> pending_;
  std::string retained_;
  void record(const std::string &detail);
};

// Collect the library once (startup / explicit Refresh / one CLI command).
// With no library root the base package is used unchanged and nothing is
// scanned. Requires the base package payload whenever the library is routed,
// so an already aggregated stage is never mistaken for the common-only base.
Active collect(const std::string &base_payload, const std::string &library_root,
               const astro::auth::TrustSet &trust, const std::string &core_version);

} // namespace stage
