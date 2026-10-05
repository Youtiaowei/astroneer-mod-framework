// Compiled public trust-root build seam for auth_record v1. The anchor set is
// a build input owned by us; there is no runtime configuration, no installed
// record and no test fixture that can add an anchor here.
#include "auth_trust_root.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>
namespace astro::auth {
namespace {
struct RawAnchor {
  const char *key_id;
  const char *public_blob_hex;
};
#define ASTRO_AUTH_ANCHOR(key_id, public_blob_hex) {key_id, public_blob_hex},
constexpr RawAnchor raw_anchors[] = {
#include "trust/production_anchors.inc"
#undef ASTRO_AUTH_ANCHOR
    {nullptr, nullptr}};
std::vector<uint8_t> decode_blob(const char *hex) {
  std::vector<uint8_t> out;
  if (!hex)
    return out;
  const size_t length = std::strlen(hex);
  if (length != 144)
    return out;
  for (size_t i = 0; i < length; i += 2) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      return -1;
    };
    const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
    if (high < 0 || low < 0)
      return {};
    out.push_back(uint8_t((high << 4) | low));
  }
  return out;
}
} // namespace

const TrustSet &compiled_trust_anchors() {
  static const TrustSet anchors = [] {
    TrustSet set;
    for (const RawAnchor *entry = raw_anchors; entry->key_id; ++entry) {
      TrustAnchor anchor;
      anchor.key_id = entry->key_id;
      anchor.public_key_blob = decode_blob(entry->public_blob_hex);
      if (anchor.key_id.empty() || anchor.public_key_blob.size() != 72)
        continue; // malformed build input is never an anchor
      set.push_back(std::move(anchor));
    }
    return set;
  }();
  return anchors;
}

const char *compiled_trust_status() {
  return compiled_trust_anchors().empty() ? "not_provisioned" : "provisioned";
}

std::string compiled_trust_source() {
  static const std::string source = [] {
#define ASTRO_AUTH_ANCHORS_TEXT
    const char text[] =
#include "trust/production_anchors.inc"
        ;
#undef ASTRO_AUTH_ANCHORS_TEXT
    return std::string(text);
  }();
  return source;
}
} // namespace astro::auth
