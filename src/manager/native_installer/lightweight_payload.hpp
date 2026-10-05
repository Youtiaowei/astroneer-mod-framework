#pragma once
#include "safe_io.h"
#include "embedded_manifest.h"
#include <map>
#include <set>

// Production external resources are fixed by this entrance's build contract.
// There is no user-supplied payload-dir switch or test-only trust path.
namespace lightweight {
class Payload {
  io::fs::path root_;
  io::DirectoryPins directories_;
  std::vector<io::Handle> files_;
public:
  explicit Payload(const io::fs::path &root) : root_(root), directories_(true) {
    if (!io::directory(root_))
      throw std::runtime_error("轻量包资源缺失，请先完整解压整个压缩包，再运行安装程序。");
    io::reject_links(root_);
    directories_.add(root_);
    for (const auto &relative : embedded_directories()) {
      auto directory = root_ / io::path(relative);
      if (!io::directory(directory))
        throw std::runtime_error("轻量包资源目录缺失，请重新完整解压。");
      directories_.add(directory);
    }
    std::map<std::string, std::string> expected{{"release.json", EMBEDDED_MANIFEST_SHA256}};
    for (const auto &entry : embedded_sources())
      expected.emplace(entry.source, entry.hash);
    if (expected.at("release.json").empty())
      throw std::runtime_error("轻量包缺少编译时资源清单。");
    std::set<std::string> names;
    files_.reserve(expected.size());
    for (const auto &[relative, digest] : expected) {
      io::relative(relative);
      auto path = root_ / io::path(relative);
      io::reject_links(path);
      io::Handle file(CreateFileW(io::win32_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (file.h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("轻量包资源文件缺失或正在被修改，请重新完整解压。 [" + relative + "]");
      BY_HANDLE_FILE_INFORMATION info{};
      if (!GetFileInformationByHandle(file.h, &info) ||
          (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("轻量包资源文件类型不受支持。");
      files_.push_back(std::move(file));
      if (crypto::sha256_hex(io::read(path, 64 * 1024 * 1024)) != digest)
        throw std::runtime_error("轻量包资源校验失败，请重新下载并完整解压。 [" + relative + "]");
      names.insert(relative);
    }
    io::known_tree(root_, "", names);
  }
};
} // namespace lightweight
