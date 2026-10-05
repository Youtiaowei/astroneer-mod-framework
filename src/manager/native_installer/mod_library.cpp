#include "mod_library.hpp"
#include "safe_io.h"
#include <map>
#include <system_error>
namespace fs = std::filesystem;
namespace modlib {
namespace {
// The exact original envelope file name carried by every standalone Mod folder.
constexpr const char *record_name = "模组信息.json";
constexpr uint64_t max_module_bytes = 128ull * 1024 * 1024;

// Quiet attribute probe for enumeration. A folder that cannot be inspected is
// never a Mod candidate; the loud, leased path below handles real candidates.
struct Entry {
  bool present = false, directory = false, reparse = false;
};
Entry probe(const fs::path &p) {
  Entry e;
  const DWORD a = GetFileAttributesW(io::win32_path(p).c_str());
  if (a == INVALID_FILE_ATTRIBUTES)
    return e;
  e.present = true;
  e.directory = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
  e.reparse = (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  return e;
}
// Diagnostics are bounded valid UTF-8: whole sequences only, no control bytes.
std::string bounded_text(const std::string &s, size_t limit = 160) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    size_t width = 1;
    if (c >= 0x80) {
      if (c >= 0xC2 && c <= 0xDF) width = 2;
      else if (c >= 0xE0 && c <= 0xEF) width = 3;
      else if (c >= 0xF0 && c <= 0xF4) width = 4;
      else return out; // invalid UTF-8 never enters the diagnostic sink
    }
    if (i + width > s.size() || out.size() + width > limit)
      break;
    for (size_t k = 0; k < width; ++k) {
      const unsigned char cc = s[i + k];
      if (k && (cc & 0xC0u) != 0x80u)
        return out;
      out += (k == 0 && (cc < 0x20 || cc == 0x7F)) ? '?' : char(cc);
    }
    i += width;
  }
  return out;
}
void note(Report &report, const std::string &zh, const std::string &en,
          const std::string &detail = {}) {
  if (report.diagnostics.size() >= max_diagnostics)
    return;
  std::string line = "跳过 Mod 文件夹" + zh;
  if (!detail.empty())
    line += "：" + detail;
  line += " / Skipped Mod folder" + en;
  if (!detail.empty())
    line += ": " + detail;
  report.diagnostics.push_back(bounded_text(line, 240));
}
} // namespace

Report discover(const std::string &root, const astro::auth::TrustSet &trust,
                const std::string &core_version,
                const std::vector<std::string> &reserved) {
  Report report;
  const auto base = io::path(root);
  io::reject_links(base);
  if (!io::directory(base))
    throw std::runtime_error("Mod 库目录不可用：" + root);
  // Protected root-down leases for the whole scan window.
  io::DirectoryPins leases(true);
  leases.add(base);
  std::vector<Candidate> collected;
  std::error_code ec;
  size_t examined = 0;
  for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (++examined > max_folders) {
      report.diagnostics.push_back(
          "跳过 Mod 库枚举：同级目录超过固定上限 / Skipped Mod library enumeration: "
          "adjacent folder bound exceeded");
      break;
    }
    const auto folder = it->path();
    const auto name = io::str(folder.filename());
    Entry kind;
    try {
      kind = probe(folder);
    } catch (const std::exception &) {
      continue; // unreadable entries are not Mod folders and stay ignored
    }
    // Ignore non-Mod folders, caches, loose files and any link/junction.
    if (!kind.present || !kind.directory || kind.reparse)
      continue;
    const auto record = folder / io::path(record_name);
    Entry record_kind;
    try {
      record_kind = probe(record);
    } catch (const std::exception &) {
      continue;
    }
    if (!record_kind.present || record_kind.directory || record_kind.reparse)
      continue;
    try {
      io::reject_links(folder);
      io::reject_links(record);
      leases.add(folder);
      const auto envelope = io::text(io::read(record, max_record_bytes));
      astro::auth::RecordPolicy policy;
      policy.now_unix = astro::auth::current_unix();
      policy.core_version = core_version;
      const auto outcome = astro::auth::verify_record(envelope, trust, policy);
      // Untrusted folder names never claim authenticated display metadata; only
      // a verified exact envelope may contribute identity, version or hashes.
      if (!outcome.verified || outcome.error != astro::auth::Reject::none) {
        note(report, "“" + name + "”", " \"" + name + "\"",
             astro::auth::reject_text(outcome.error));
        continue;
      }
      const auto &d = outcome.descriptor;
      const std::string canonical =
          "modules/" + d.module_id + "/" + d.module_id + ".dll";
      if (d.dll_path != canonical) {
        note(report, "“" + name + "”", " \"" + name + "\"", "namespace_invalid");
        continue;
      }
      bool refused = false;
      for (const auto &id : reserved)
        if (id == d.module_id) {
          note(report, "“" + name + "”", " \"" + name + "\"",
               "模块 " + d.module_id + " 已由安装包提供 / module " + d.module_id +
                   " already offered by the opened package");
          refused = true;
        }
      if (refused)
        continue;
      // Source is always <folder>/<module_id>.dll, strictly inside this folder.
      const auto source = folder / io::path(d.module_id + ".dll");
      io::reject_links(source);
      if (!io::file(source)) {
        note(report, "“" + name + "”", " \"" + name + "\"",
             "缺少 " + d.module_id + ".dll / missing " + d.module_id + ".dll");
        continue;
      }
      const auto bytes = io::read(source, max_dll_bytes);
      if (bytes.size() != d.dll_size) {
        note(report, "“" + name + "”", " \"" + name + "\"", "module_size_mismatch");
        continue;
      }
      if (crypto::sha256_hex(bytes) != d.dll_sha256) {
        note(report, "“" + name + "”", " \"" + name + "\"", "module_hash_mismatch");
        continue;
      }
      Candidate candidate;
      candidate.id = d.module_id;
      candidate.version = d.version;
      candidate.revision = d.revision;
      candidate.envelope = envelope;
      candidate.dll_sha256 = d.dll_sha256;
      candidate.dll_size = d.dll_size;
      candidate.folder = io::str(folder);
      candidate.dll_path = io::str(source);
      collected.push_back(std::move(candidate));
    } catch (const std::exception &e) {
      // One invalid Mod folder must never disable its valid or installed siblings.
      note(report, "“" + name + "”", " \"" + name + "\"", bounded_text(e.what()));
    }
  }
  if (ec)
    throw std::runtime_error("无法枚举 Mod 库目录：" + root);
  auto push = [&report](const std::string &line) {
    if (report.diagnostics.size() < max_diagnostics)
      report.diagnostics.push_back(line);
  };
  // Duplicate authenticated identities are never resolved by picking a winner:
  // every candidate of the group is dropped and reported.
  std::map<std::string, int> counts;
  for (const auto &candidate : collected)
    ++counts[candidate.id];
  uint64_t total = 0;
  bool module_bound_reported = false, byte_bound_reported = false;
  for (auto &candidate : collected) {
    if (counts[candidate.id] > 1) {
      push(bounded_text("跳过重复的模块标识“" + candidate.id +
                        "”（同标识候选均已弃用） / Skipped duplicate module identity \"" +
                        candidate.id + "\" (all candidates dropped)"));
      continue;
    }
    if (report.modules.size() >= max_modules) {
      if (!module_bound_reported) {
        module_bound_reported = true;
        push("跳过 Mod 文件夹：可安装模块数达到固定上限 / Skipped Mod folder: module bound reached");
      }
      continue;
    }
    if (total + candidate.dll_size > max_module_bytes) {
      if (!byte_bound_reported) {
        byte_bound_reported = true;
        push("跳过 Mod 文件夹：模块字节总量达到固定上限 / Skipped Mod folder: module byte bound reached");
      }
      continue;
    }
    total += candidate.dll_size;
    report.modules.push_back(std::move(candidate));
  }
  return report;
}

} // namespace modlib
