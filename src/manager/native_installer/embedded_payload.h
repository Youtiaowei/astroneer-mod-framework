#pragma once
#include "safe_io.h"
#include <map>
#include <memory>
class EmbeddedPayload {
  io::fs::path root_;
  struct File { io::fs::path path; std::string hash; bool published = false; };
  std::vector<File> files_;
  std::map<std::wstring, io::DirectoryReceipt> dir_receipts_;
  std::unique_ptr<io::DirectoryPins> pins_;
  bool cleanup_attempted_ = false;
  std::string cleanup_report_;

public:
  EmbeddedPayload();
  ~EmbeddedPayload();
  // One attempt; subsequent calls return the same outcome without retrying deletion.
  [[nodiscard]] const std::string &cleanup();
  std::string path() const { return io::str(root_); }
};
