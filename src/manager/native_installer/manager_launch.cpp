#include "manager_launch.hpp"
#include "safe_io.h"
#include "sha256.h"
#include <windows.h>
#include <algorithm>

namespace manager_launch {
namespace {
bool valid_relative(const std::string &r) {
  if (r.empty() || r.size() > 240 || r.front() == '/' || r.back() == '/' ||
      r.find("//") != std::string::npos || r.find('\\') != std::string::npos ||
      r.find(':') != std::string::npos)
    return false;
  size_t start = 0;
  int segments = 0;
  while (start <= r.size()) {
    auto cut = r.find('/', start);
    auto seg = r.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
    if (seg.empty() || seg.size() > 64 || seg == "." || seg == ".." ||
        seg.back() == '.' || seg.back() == ' ')
      return false;
    ++segments;
    if (cut == std::string::npos)
      break;
    start = cut + 1;
  }
  return segments >= 2 && segments <= 8;
}
bool hash_ok(const std::string &h) {
  return h.size() == 64 && h.find_first_not_of("0123456789abcdef") == std::string::npos;
}
} // namespace

Decision select(const std::vector<Candidate> &installed, int64_t package_revision) {
  Decision d;
  const Candidate *best = nullptr;
  for (auto &c : installed) {
    if (!valid_relative(c.rel) || !hash_ok(c.sha256))
      continue;
    if (c.revision < package_revision)
      continue; // never run an older engine than this entrance already has
    if (!best || c.revision > best->revision ||
        (c.revision == best->revision && c.version > best->version))
      best = &c;
  }
  if (!best) {
    d.reason = "no_newer_compatible_manager";
    return d;
  }
  d.handoff = true;
  d.chosen = *best;
  d.reason = "newest_compatible_manager";
  return d;
}

std::wstring quote_argv(const std::wstring &arg) {
  // CommandLineToArgvW quoting: a bare word needs no quotes; otherwise wrap in
  // quotes and double every backslash that precedes a quote (or the closer).
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

int handoff(const std::string &game_root, const Candidate &manager,
            const std::vector<std::wstring> &args, std::string &error) {
  error.clear();
  namespace io = ::io;
  if (!valid_relative(manager.rel) || !hash_ok(manager.sha256)) {
    error = "manager_candidate_invalid";
    return -1;
  }
  try {
    io::reject_links(io::path(game_root));
    auto exe = io::path(game_root) / io::path(manager.rel);
    io::reject_links(exe);
    // Pin the ancestor chain root-down for the whole lookup/verify/launch window.
    io::DirectoryPins pins;
    pins.add(exe.parent_path());
    // Hold the manager object itself with no write and no delete sharing so its
    // identity cannot change between verification and process creation.
    io::Handle held(CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (held.h == INVALID_HANDLE_VALUE) {
      error = "manager_open_failed";
      return -1;
    }
    io::validate_object(held.h);
    auto bytes = io::read_via_handle(held.h, 64 * 1024 * 1024);
    if (crypto::sha256_hex(bytes) != manager.sha256) {
      error = "manager_hash_mismatch";
      return -1;
    }
    auto before = io::object_receipt(held.h, false);
    // The continuously held image keeps the leaf nonempty. Retain directory
    // identity pins, but allow the child to rename its ledger/config files.
    pins.allow_file_rename();

    std::wstring command = L"\"" + exe.wstring() + L"\"";
    for (auto &a : args)
      command += L" " + quote_argv(a);
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // The held read handle keeps the image identity fixed across creation.
    if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi)) {
      error = "manager_launch_failed";
      return -1;
    }
    CloseHandle(pi.hThread);
    // Re-verify the held object identity after creation; a swap is refused.
    auto after = io::object_receipt(held.h, false);
    if (before.volume_serial != after.volume_serial ||
        before.file_index_high != after.file_index_high ||
        before.file_index_low != after.file_index_low) {
      TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hProcess);
      error = "manager_identity_changed";
      return -1;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, INFINITE);
    if (wait != WAIT_OBJECT_0) {
      CloseHandle(pi.hProcess);
      error = "manager_wait_failed";
      return -1;
    }
    DWORD code = 1;
    if (!GetExitCodeProcess(pi.hProcess, &code)) {
      CloseHandle(pi.hProcess);
      error = "manager_exit_unavailable";
      return -1;
    }
    CloseHandle(pi.hProcess);
    return int(code);
  } catch (const std::exception &e) {
    error = e.what();
    return -1;
  }
}

} // namespace manager_launch
