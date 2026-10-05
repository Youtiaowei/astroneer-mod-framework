#include "package_record.hpp"
#include "json.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#pragma comment(lib, "bcrypt.lib")

namespace astro::pkg {
namespace {
// Consume the SDK's own magic instead of a hand-written literal. The copied
// constant here was 0x31424345 ("ECB1"), which silently rejected every real
// fixture signature: CNG writes BCRYPT_ECDSA_PUBLIC_P256_MAGIC ("ECS1") for an
// ECDSA P-256 BCRYPT_ECCPUBLIC_BLOB. ECDH's "ECK1" magic is a different value
// and must not be substituted for it.
constexpr uint32_t kEcdaPublicMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;

bool lower_hex(const std::string &s, size_t n) {
  return s.size() == n && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool charset(const std::string &s, const std::string &allowed) {
  for (char c : s)
    if (allowed.find(c) == std::string::npos)
      return false;
  return true;
}
bool valid_utf8_text(const std::string &s, size_t min_chars, size_t max_chars,
                     size_t max_bytes) {
  if (s.empty() || s.size() > max_bytes)
    return false;
  size_t chars = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7F)
      return false;
    size_t need = 0;
    if (c < 0x80)
      need = 0;
    else if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else
      return false;
    if (i + need >= s.size() && need != 0)
      return false;
    for (size_t k = 1; k <= need; ++k)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80)
        return false;
    i += need + 1;
    ++chars;
  }
  return chars >= min_chars && chars <= max_chars;
}
bool reserved_segment(const std::string &seg) {
  static const char *names[] = {"CON",  "PRN",  "AUX",  "NUL",  "CONIN$",
                                "CONOUT$", "COM0", "COM1", "COM2", "COM3",
                                "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                "LPT0", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                "LPT6", "LPT7", "LPT8", "LPT9"};
  auto stem = seg.substr(0, seg.find('.'));
  std::string up;
  for (char c : stem)
    up.push_back(char(std::toupper((unsigned char)c)));
  for (auto *n : names)
    if (up == n)
      return true;
  return false;
}
bool verified(const std::vector<uint8_t> &digest, const std::vector<uint8_t> &sig,
              const std::vector<uint8_t> &blob) {
  if (blob.size() != 72 || sig.size() != 64)
    return false;
  uint32_t magic = 0, cb = 0;
  std::memcpy(&magic, blob.data(), 4);
  std::memcpy(&cb, blob.data() + 4, 4);
  if (magic != kEcdaPublicMagic || cb != 32)
    return false;
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0)
    return false;
  struct Guard {
    BCRYPT_ALG_HANDLE a;
    ~Guard() {
      if (a)
        BCryptCloseAlgorithmProvider(a, 0);
    }
  } guard{alg};
  BCRYPT_KEY_HANDLE key = nullptr;
  if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                          (PUCHAR)blob.data(), (ULONG)blob.size(), 0) < 0)
    return false;
  const NTSTATUS status =
      BCryptVerifySignature(key, nullptr, (PUCHAR)digest.data(), (ULONG)digest.size(),
                            (PUCHAR)sig.data(), (ULONG)sig.size(), 0);
  BCryptDestroyKey(key);
  return status >= 0;
}
} // namespace

bool closed_component(const std::string &role) {
  return role == component_core || role == component_loader || role == component_manager;
}
const char *reject_text(Reject r) {
  switch (r) {
  case Reject::none: return "none";
  case Reject::envelope_too_large: return "envelope_too_large";
  case Reject::envelope_malformed: return "envelope_malformed";
  case Reject::envelope_schema_unsupported: return "envelope_schema_unsupported";
  case Reject::envelope_product_mismatch: return "envelope_product_mismatch";
  case Reject::envelope_kind_mismatch: return "envelope_kind_mismatch";
  case Reject::envelope_key_unknown: return "envelope_key_unknown";
  case Reject::envelope_key_duplicate: return "envelope_key_duplicate";
  case Reject::envelope_field_invalid: return "envelope_field_invalid";
  case Reject::payload_encoding_invalid: return "payload_encoding_invalid";
  case Reject::signature_encoding_invalid: return "signature_encoding_invalid";
  case Reject::signature_invalid: return "signature_invalid";
  case Reject::key_id_unknown: return "key_id_unknown";
  case Reject::trust_root_not_provisioned: return "trust_root_not_provisioned";
  case Reject::descriptor_too_large: return "descriptor_too_large";
  case Reject::descriptor_malformed: return "descriptor_malformed";
  case Reject::descriptor_schema_unsupported: return "descriptor_schema_unsupported";
  case Reject::descriptor_product_mismatch: return "descriptor_product_mismatch";
  case Reject::descriptor_kind_mismatch: return "descriptor_kind_mismatch";
  case Reject::identity_invalid: return "identity_invalid";
  case Reject::version_invalid: return "version_invalid";
  case Reject::revision_invalid: return "revision_invalid";
  case Reject::component_invalid: return "component_invalid";
  case Reject::resource_invalid: return "resource_invalid";
  case Reject::namespace_invalid: return "namespace_invalid";
  case Reject::namespace_role_mismatch: return "namespace_role_mismatch";
  case Reject::resource_conflict: return "resource_conflict";
  case Reject::resource_closure_incomplete: return "resource_closure_incomplete";
  case Reject::resource_order_invalid: return "resource_order_invalid";
  case Reject::total_bytes_exceeded: return "total_bytes_exceeded";
  case Reject::duplicate_package_identity: return "duplicate_package_identity";
  }
  return "unknown";
}

std::string base64_encode(const std::string &raw) {
  static const char *tab =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < raw.size(); i += 3) {
    uint32_t n = ((uint8_t)raw[i] << 16) | ((uint8_t)raw[i + 1] << 8) | (uint8_t)raw[i + 2];
    out.push_back(tab[(n >> 18) & 63]);
    out.push_back(tab[(n >> 12) & 63]);
    out.push_back(tab[(n >> 6) & 63]);
    out.push_back(tab[n & 63]);
  }
  if (i + 1 == raw.size()) {
    uint32_t n = (uint8_t)raw[i] << 16;
    out.push_back(tab[(n >> 18) & 63]);
    out.push_back(tab[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == raw.size()) {
    uint32_t n = ((uint8_t)raw[i] << 16) | ((uint8_t)raw[i + 1] << 8);
    out.push_back(tab[(n >> 18) & 63]);
    out.push_back(tab[(n >> 12) & 63]);
    out.push_back(tab[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}
bool base64_decode(const std::string &text, std::string &raw) {
  if (text.size() % 4 != 0)
    return false;
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  raw.clear();
  for (size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    for (int k = 0; k < 4; ++k) {
      char c = text[i + k];
      if (c == '=') {
        if (i + 4 != text.size() || k < 2)
          return false;
        v[k] = -2;
      } else {
        v[k] = value(c);
        if (v[k] < 0)
          return false;
      }
    }
    if (v[0] < 0 || v[1] < 0)
      return false;
    raw.push_back(char((v[0] << 2) | (v[1] >> 4)));
    if (v[2] == -2) {
      if (v[3] != -2 || (v[1] & 15) != 0)
        return false;
      continue;
    }
    raw.push_back(char(((v[1] & 15) << 4) | (v[2] >> 2)));
    if (v[3] == -2) {
      if ((v[2] & 3) != 0)
        return false;
      continue;
    }
    raw.push_back(char(((v[2] & 3) << 6) | v[3]));
  }
  // Canonical encoding: re-encoding the decoded bytes must reproduce the input.
  return base64_encode(raw) == text;
}
std::string sha256_hex(const std::string &raw) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    return {};
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::array<uint8_t, 32> out{};
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return {};
  }
  if (!raw.empty())
    BCryptHashData(hash, (PUCHAR)raw.data(), (ULONG)raw.size(), 0);
  BCryptFinishHash(hash, out.data(), (ULONG)out.size(), 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  static const char *hex = "0123456789abcdef";
  std::string s;
  for (auto b : out) {
    s.push_back(hex[b >> 4]);
    s.push_back(hex[b & 15]);
  }
  return s;
}
std::vector<uint8_t> sha256_raw(const std::string &raw) {
  auto text = sha256_hex(raw);
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < text.size(); i += 2)
    out.push_back(uint8_t(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}
int64_t current_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Reject validate_resource_path(const std::string &relative, const std::string &role) {
  if (relative.empty() || relative.size() > path_byte_limit)
    return Reject::namespace_invalid;
  for (unsigned char c : relative)
    if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '<' || c == '>' ||
        c == '"' || c == '|' || c == '?' || c == '*')
      return Reject::namespace_invalid;
  if (relative.front() == '/' || relative.back() == '/' ||
      relative.find("//") != std::string::npos)
    return Reject::namespace_invalid;
  std::vector<std::string> segs;
  size_t start = 0;
  while (start <= relative.size()) {
    auto cut = relative.find('/', start);
    segs.push_back(relative.substr(start, cut == std::string::npos
                                              ? std::string::npos
                                              : cut - start));
    if (cut == std::string::npos)
      break;
    start = cut + 1;
  }
  if (segs.size() < 3 || segs.size() > 8)
    return Reject::namespace_invalid;
  // Closed layout: common/<component>/<name>[/<sub>...]
  if (segs[0] != "common")
    return Reject::namespace_invalid;
  if (segs[1] != role || !closed_component(role))
    return Reject::namespace_role_mismatch;
  for (auto &seg : segs) {
    if (seg.empty() || seg.size() > path_segment_limit || seg == "." || seg == ".." ||
        seg.back() == '.' || seg.back() == ' ' || reserved_segment(seg))
      return Reject::namespace_invalid;
  }
  return Reject::none;
}
Reject validate_closure(const PackageDescriptor &d) {
  if (d.resources.size() < resource_count_min || d.resources.size() > resource_count_max)
    return Reject::resource_closure_incomplete;
  std::set<std::string> paths;
  uint64_t total = 0;
  std::string previous;
  for (auto &r : d.resources) {
    if (r.role != d.component || !closed_component(r.role))
      return Reject::namespace_role_mismatch;
    auto ns = validate_resource_path(r.path, r.role);
    if (ns != Reject::none)
      return ns;
    if (r.size < 1 || r.size > resource_size_limit || !lower_hex(r.sha256, sha256_hex_len))
      return Reject::resource_invalid;
    // DETERMINISTIC ORDERING RULE: strictly ascending `path`. Without it the
    // same resource set would admit several authenticated encodings and several
    // closure digests, which payload.cpp and engine.cpp could not both consume.
    if (!previous.empty() && r.path <= previous)
      return Reject::resource_order_invalid;
    previous = r.path;
    std::string key;
    for (char c : r.path)
      key.push_back(char(std::tolower((unsigned char)c)));
    if (!paths.insert(key).second)
      return Reject::resource_conflict;
    total += r.size;
    if (total > package_bytes_limit)
      return Reject::total_bytes_exceeded;
  }
  return Reject::none;
}

Outcome verify_package_record(const std::string &envelope_utf8,
                              const TrustSet &trusted, const Policy &policy) {
  Outcome out;
  if (envelope_utf8.empty() || envelope_utf8.size() > envelope_byte_limit) {
    out.error = Reject::envelope_too_large;
    return out;
  }
  json::Value env;
  try {
    env = json::parse(envelope_utf8);
  } catch (const std::exception &) {
    out.error = Reject::envelope_malformed;
    return out;
  }
  if (!env.is_object()) {
    out.error = Reject::envelope_malformed;
    return out;
  }
  const auto &obj = env.as_object();
  static const char *keys[] = {"schema", "product", "kind", "key_id", "payload",
                               "signature"};
  if (obj.size() != 6) {
    out.error = obj.size() > 6 ? Reject::envelope_key_unknown : Reject::envelope_malformed;
    return out;
  }
  for (auto *k : keys)
    if (!obj.count(k)) {
      out.error = Reject::envelope_key_unknown;
      return out;
    }
  for (auto &[k, v] : obj) {
    (void)v;
    if (std::find(std::begin(keys), std::end(keys), k) == std::end(keys)) {
      out.error = Reject::envelope_key_unknown;
      return out;
    }
  }
  if (!env["schema"].is_integer() || env["schema"].as_int64() != record_schema) {
    out.error = Reject::envelope_schema_unsupported;
    return out;
  }
  if (!env["product"].is_string() || env["product"].as_string() != record_product) {
    out.error = Reject::envelope_product_mismatch;
    return out;
  }
  if (!env["kind"].is_string() || env["kind"].as_string() != envelope_kind) {
    out.error = Reject::envelope_kind_mismatch;
    return out;
  }
  if (!env["key_id"].is_string() || !charset(env["key_id"].as_string(), "abcdefghijklmnopqrstuvwxyz0123456789_-") ||
      env["key_id"].as_string().size() < key_id_min ||
      env["key_id"].as_string().size() > key_id_limit) {
    out.error = Reject::envelope_field_invalid;
    return out;
  }
  out.key_id = env["key_id"].as_string();
  if (!env["payload"].is_string() ||
      env["payload"].as_string().size() > payload_string_limit) {
    out.error = Reject::envelope_field_invalid;
    return out;
  }
  if (!env["signature"].is_string() ||
      !lower_hex(env["signature"].as_string(), signature_hex_len)) {
    out.error = Reject::signature_encoding_invalid;
    return out;
  }
  std::string descriptor;
  if (!base64_decode(env["payload"].as_string(), descriptor)) {
    out.error = Reject::payload_encoding_invalid;
    return out;
  }
  if (descriptor.size() < descriptor_byte_min) {
    out.error = Reject::descriptor_malformed;
    return out;
  }
  if (descriptor.size() > descriptor_byte_limit) {
    out.error = Reject::descriptor_too_large;
    return out;
  }
  for (unsigned char c : descriptor)
    if (c < 0x20 || c == 0x7F) {
      out.error = Reject::descriptor_malformed;
      return out;
    }
  std::vector<uint8_t> signature;
  for (size_t i = 0; i + 1 < env["signature"].as_string().size(); i += 2)
    signature.push_back(uint8_t(std::strtoul(
        env["signature"].as_string().substr(i, 2).c_str(), nullptr, 16)));

  const TrustAnchor *anchor = nullptr;
  for (auto &a : trusted)
    if (a.key_id == out.key_id)
      anchor = &a;
  if (!anchor) {
    out.error = trusted.empty() ? Reject::trust_root_not_provisioned
                                : Reject::key_id_unknown;
    return out;
  }
  if (!verified(sha256_raw(descriptor), signature, anchor->public_key_blob)) {
    out.error = Reject::signature_invalid;
    return out;
  }
  out.verified = true;

  json::Value d;
  try {
    d = json::parse(descriptor);
  } catch (const std::exception &) {
    out.error = Reject::descriptor_malformed;
    return out;
  }
  if (!d.is_object()) {
    out.error = Reject::descriptor_malformed;
    return out;
  }
  const auto &db = d.as_object();
  static const char *dkeys[] = {"schema",   "product", "component", "package_id",
                                "kind",     "version", "revision",  "abi",
                                "resources"};
  if (db.size() != 9) {
    out.error = Reject::descriptor_malformed;
    return out;
  }
  for (auto *k : dkeys)
    if (!db.count(k)) {
      out.error = Reject::descriptor_malformed;
      return out;
    }
  if (!d["schema"].is_integer() || d["schema"].as_int64() != record_schema) {
    out.error = Reject::descriptor_schema_unsupported;
    return out;
  }
  if (!d["product"].is_string() || d["product"].as_string() != record_product) {
    out.error = Reject::descriptor_product_mismatch;
    return out;
  }
  if (!d["kind"].is_string() || d["kind"].as_string() != descriptor_kind) {
    out.error = Reject::descriptor_kind_mismatch;
    return out;
  }
  auto &desc = out.descriptor;
  if (!d["package_id"].is_string() ||
      !valid_utf8_text(d["package_id"].as_string(), 1, package_id_limit,
                       package_id_limit) ||
      d["package_id"].as_string()[0] < 'a' || d["package_id"].as_string()[0] > 'z') {
    out.error = Reject::identity_invalid;
    return out;
  }
  desc.package_id = d["package_id"].as_string();
  for (char c : desc.package_id)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      out.error = Reject::identity_invalid;
      return out;
    }
  if (!d["component"].is_string() || !closed_component(d["component"].as_string())) {
    out.error = Reject::component_invalid;
    return out;
  }
  desc.component = d["component"].as_string();
  if (!d["version"].is_string() || d["version"].as_string().empty() ||
      d["version"].as_string().size() > version_limit ||
      !charset(d["version"].as_string(), "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._+-")) {
    out.error = Reject::version_invalid;
    return out;
  }
  desc.version = d["version"].as_string();
  if (!d["revision"].is_integer() || d["revision"].as_int64() < 0 ||
      d["revision"].as_int64() > revision_limit) {
    out.error = Reject::revision_invalid;
    return out;
  }
  desc.revision = d["revision"].as_int64();
  if (!d["abi"].is_string() || d["abi"].as_string().empty() ||
      d["abi"].as_string().size() > abi_limit ||
      !valid_utf8_text(d["abi"].as_string(), 1, abi_limit, abi_limit)) {
    out.error = Reject::resource_invalid;
    return out;
  }
  desc.abi = d["abi"].as_string();
  if (!policy.abi_fingerprint.empty() && desc.abi != policy.abi_fingerprint) {
    out.error = Reject::resource_invalid;
    return out;
  }
  if (!d["resources"].is_array()) {
    out.error = Reject::resource_invalid;
    return out;
  }
  for (auto &row : d["resources"].as_array()) {
    // A resource row is exactly role|path|size|sha256. The previous bound asked
    // for 3 members while requiring all 4 names, which no real row can satisfy.
    if (!row.is_object() || row.as_object().size() != 4 || !row.has("role") ||
        !row.has("path") || !row.has("size") || !row.has("sha256")) {
      out.error = Reject::resource_invalid;
      return out;
    }
    Resource r;
    if (!row["role"].is_string() || !row["path"].is_string() ||
        !row["size"].is_integer() || !row["sha256"].is_string()) {
      out.error = Reject::resource_invalid;
      return out;
    }
    r.role = row["role"].as_string();
    r.path = row["path"].as_string();
    r.size = uint64_t(row["size"].as_int64());
    r.sha256 = row["sha256"].as_string();
    desc.resources.push_back(std::move(r));
  }
  out.error = validate_closure(desc);
  return out;
}

const TrustSet &compiled_package_anchors() {
  // Same compiled PUBLIC input as the runtime module verifier. Keep this TU
  // self-contained for the existing standalone package/signing tools.
  static const TrustSet anchors = [] {
    struct Raw { const char *id; const char *hex; };
    constexpr Raw rows[] = {
#define ASTRO_AUTH_ANCHOR(id, hex) {id, hex},
#include "trust/production_anchors.inc"
#undef ASTRO_AUTH_ANCHOR
        {nullptr, nullptr}};
    TrustSet result;
    for (const auto *row = rows; row->id; ++row) {
      if (!row->hex || !lower_hex(row->hex, 144)) continue;
      TrustAnchor anchor{row->id, {}};
      for (size_t i = 0; i < 144; i += 2)
        anchor.public_key_blob.push_back(uint8_t(std::strtoul(std::string(row->hex + i, 2).c_str(), nullptr, 16)));
      result.push_back(std::move(anchor));
    }
    return result;
  }();
  return anchors;
}
const char *compiled_package_status() {
  return compiled_package_anchors().empty() ? "not_provisioned" : "provisioned";
}
std::string compiled_package_source() {
#define ASTRO_AUTH_ANCHORS_TEXT
  const char text[] =
#include "trust/production_anchors.inc"
      ;
#undef ASTRO_AUTH_ANCHORS_TEXT
  return text;
}

namespace test {
namespace {
struct Key {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE pair = nullptr;
  std::vector<uint8_t> pub;
  std::string key_id;
};
Key &key() {
  static Key k;
  return k;
}
} // namespace

TrustAnchor make_anchor(const std::string &key_id) {
  Key &k = key();
  if (!k.pair) {
    if (BCryptOpenAlgorithmProvider(&k.alg, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) < 0)
      return {};
    if (BCryptGenerateKeyPair(k.alg, &k.pair, 256, 0) < 0 ||
        BCryptFinalizeKeyPair(k.pair, 0) < 0)
      return {};
    ULONG size = 0;
    if (BCryptExportKey(k.pair, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &size,
                        0) < 0)
      return {};
    k.pub.resize(size);
    if (BCryptExportKey(k.pair, nullptr, BCRYPT_ECCPUBLIC_BLOB, k.pub.data(),
                        size, &size, 0) < 0)
      return {};
  }
  k.key_id = key_id;
  return TrustAnchor{key_id, k.pub};
}

std::string sign_package_record(const std::string &descriptor_utf8,
                                const std::string &key_id) {
  Key &k = key();
  if (!k.pair)
    return {};
  auto digest = sha256_raw(descriptor_utf8);
  std::vector<uint8_t> signature(signature_bytes);
  ULONG written = 0;
  if (BCryptSignHash(k.pair, nullptr, (PUCHAR)digest.data(), (ULONG)digest.size(),
                     signature.data(), (ULONG)signature.size(), &written, 0) < 0 ||
      written != signature_bytes)
    return {};
  static const char *hex = "0123456789abcdef";
  std::string sig;
  for (auto b : signature) {
    sig.push_back(hex[b >> 4]);
    sig.push_back(hex[b & 15]);
  }
  json::Value env = json::Object{
      {"schema", int64_t(record_schema)},
      {"product", record_product},
      {"kind", envelope_kind},
      {"key_id", key_id},
      {"payload", base64_encode(descriptor_utf8)},
      {"signature", sig}};
  return json::serialize(env);
}
} // namespace test

} // namespace astro::pkg
