#pragma once
#include <filesystem>
#include <optional>
#include <string>
namespace astro {
class Files {
public:
  virtual ~Files() = default;
  virtual std::optional<std::string> read(const std::string &name,
                                          size_t limit) = 0;
  virtual bool write_atomic(const std::string &name,
                            const std::string &content) = 0;
  // Complete synchronous telemetry commit, not a crash-durable journal write.
  virtual bool publish_runtime_state(const std::string &content) {
    return write_atomic("runtime-state.json", content);
  }
};
class NativeFiles final : public Files {
  std::filesystem::path root;
  std::filesystem::path path(const std::string &) const;
  bool write_file(const std::string &, const std::string &, bool durable);

public:
  explicit NativeFiles(std::filesystem::path root);
  std::optional<std::string> read(const std::string &, size_t) override;
  bool write_atomic(const std::string &, const std::string &) override;
  bool publish_runtime_state(const std::string &) override;
};
struct RuntimePaths {
  std::filesystem::path game, mod, data;
};
RuntimePaths resolve_paths(const std::filesystem::path &dll);
std::filesystem::path this_module_path();
} // namespace astro
