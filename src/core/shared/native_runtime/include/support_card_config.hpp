#pragma once
#include <string>
#include <vector>

// Frozen card geometry; browser actions are configured independently.
// Card revision ff66722d2bc8a825245a and its QR artwork remain immutable.

namespace astro {

struct LinkEntry {
  const char *id;
  const char *label;
  const char *url;
};

struct QREntryConfig {
  const char *id;
  double rect[4]; // x, y, w, h
};

struct CardGeometry {
  double size[2]; // 1024, 1536
  std::vector<QREntryConfig> entries;
};

namespace card_config {

constexpr const char *MATERIAL = "/Game/Mods/AstroSupportCard/M_Card.M_Card";
constexpr const char *REVISION = "ff66722d2bc8a825245a";

inline const std::vector<LinkEntry> &links() {
  static const std::vector<LinkEntry> L = {
      {"qq", "更多mod", "https://mod.cab"},
      {"afdian", "打开爱发电", "https://afdian.com/a/Youtiaowei"},
  };
  return L;
}

inline const CardGeometry &card_geometry() {
  static const CardGeometry G = {
      {1024, 1536},
      {{"qq", {93, 522, 333, 333}}, {"afdian", {93, 962, 333, 333}}}};
  return G;
}

} // namespace card_config
} // namespace astro
