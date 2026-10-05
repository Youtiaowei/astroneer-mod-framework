#pragma once
#include "safe_io.h"
#include "json.hpp"
#include <functional>

// Output files are create-only. Observing existing bytes never grants ownership.
// This read-only admission runs before any game operation; atomic publication
// still rechecks absence to reject later arrivals without replacement.
inline void check_new_output(const std::string &name) {
  if (name.empty()) return;
  auto path = io::absolute(name);
  io::reject_links(path);
  if (io::attrs(path) != INVALID_FILE_ATTRIBUTES)
    throw std::runtime_error("输出路径已被占用，请选择新文件：" + io::str(path));
}

inline void check_output_location(const std::string &name, const std::vector<std::string> &protected_paths) {
  if (name.empty()) return;
  const auto path = io::absolute(name);
  for (const auto &root : protected_paths)
    if (io::within(path, io::absolute(root)))
      throw std::runtime_error("结果/报告必须放在游戏、存档、载荷和备份目录外：" + name);
}

// Preserve the primary operation result even if its optional file sink fails.
// Never retry a failed publication or recapture an occupant's hash as ownership.
inline bool write_new_output(json::Value &document, const std::string &name, bool admitted = true,
                             const std::function<void()> &validate = {}) {
  if (name.empty()) return true;
  io::MutationState state;
  try {
    if (!admitted) throw std::runtime_error("输出路径未通过操作前检查，未尝试写入。");
    if (validate) validate();
    io::atomic(io::absolute(name), io::bytes(json::serialize(document)), nullptr, "", &state);
    return true;
  } catch (const std::exception &error) {
    auto failure = document.as_object();
    failure["result_output"] = json::Object{
        {"ok", false}, {"path", name}, {"error", error.what()},
        {"published", state.published}, {"stage", io::str(state.stage)},
        {"parked", io::str(state.parked)}, {"recovery_error", state.recovery_error}};
    document = std::move(failure);
    return false;
  }
}
