// Authenticated module record verifier (auth_record v1). Exact-byte envelope,
// strict bounded JSON, installation-relative namespace and Windows CNG ECDSA
// P-256 verification over SHA-256 of the exact descriptor bytes. No engine
// Value and no private ABI: every consumer compiles this unit with its own CRT.
//
// The signature always covers base64_decode(payload) exactly. The verifier
// never re-serializes a descriptor before checking a signature.
#include "auth_record.hpp"
#include "sha256.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <utility>
#pragma comment(lib, "bcrypt.lib")

namespace astro::auth {
namespace {
// ---------------------------------------------------------------- UTF-8 -----
bool valid_utf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = s[i];
    if (c == 0)
      return false;
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t extra = 0;
    uint32_t point = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      extra = 1;
      point = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
      extra = 2;
      point = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
      extra = 3;
      point = c & 0x07u;
    } else {
      return false;
    }
    if (i + extra >= s.size())
      return false;
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = s[i + k];
      if ((cc & 0xC0u) != 0x80u)
        return false;
      point = (point << 6) | (cc & 0x3Fu);
    }
    if ((extra == 2 && point < 0x800) || (extra == 3 && point < 0x10000))
      return false;
    if (point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF))
      return false;
    const unsigned char first_continuation = s[i + 1];
    if (extra == 2 && c == 0xE0 && first_continuation < 0xA0)
      return false;
    if (extra == 2 && c == 0xED && first_continuation >= 0xA0)
      return false;
    if (extra == 3 && c == 0xF0 && first_continuation < 0x90)
      return false;
    if (extra == 3 && c == 0xF4 && first_continuation >= 0x90)
      return false;
    i += extra + 1;
  }
  return true;
}
// Raw control characters are never valid in a document; JSON whitespace is
// restricted to the four documented bytes.
bool structural_text(const std::string &s) {
  for (unsigned char c : s) {
    if (c == 0x7F)
      return false;
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
      return false;
  }
  return true;
}
// ------------------------------------------------------------ strict JSON ---
struct JsonValue {
  enum class Kind { null, boolean, number, string, object, array };
  Kind kind = Kind::null;
  size_t source_bytes = 0; // exact object span, excluding aggregate separators
  bool boolean_value = false;
  int64_t number = 0;
  std::string text;
  std::vector<std::pair<std::string, JsonValue>> members;
  std::vector<JsonValue> items;
  const JsonValue *find(const std::string &key) const {
    for (const auto &entry : members)
      if (entry.first == key)
        return &entry.second;
    return nullptr;
  }
};
// One overflow identity per document role; the parser never invents text.
enum class Overflow { string, keys, items, depth };
enum class FailureKind { structural, duplicate, overflow };
struct JsonFailure {
  FailureKind kind;
};
struct JsonParser {
  const std::string &s;
  size_t pos = 0, depth = 0;
  const size_t max_string;
  explicit JsonParser(const std::string &text, size_t string_limit)
      : s(text), max_string(string_limit) {}
  [[noreturn]] void fail(FailureKind kind) { throw JsonFailure{kind}; }
  [[noreturn]] void overflow(Overflow) { throw JsonFailure{FailureKind::overflow}; }
  void ws() {
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
      ++pos;
  }
  bool take(char c) {
    ws();
    if (pos < s.size() && s[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }
  void expect(char c) {
    if (!take(c))
      fail(FailureKind::structural);
  }
  JsonValue parse_value(size_t string_limit = json_string_limit) {
    if (depth >= json_max_depth)
      overflow(Overflow::depth);
    ws();
    if (pos >= s.size())
      fail(FailureKind::structural);
    switch (s[pos]) {
    case '{': return parse_object();
    case '[': return parse_array();
    case '"': {
      JsonValue v;
      v.kind = JsonValue::Kind::string;
      v.text = parse_string(string_limit);
      return v;
    }
    case 't': {
      literal("true");
      JsonValue v;
      v.kind = JsonValue::Kind::boolean;
      v.boolean_value = true;
      return v;
    }
    case 'f': {
      literal("false");
      JsonValue v;
      v.kind = JsonValue::Kind::boolean;
      v.boolean_value = false;
      return v;
    }
    case 'n':
      literal("null");
      return {};
    default: return parse_number();
    }
  }
  void literal(const char *text) {
    const size_t n = std::strlen(text);
    if (s.compare(pos, n, text) != 0)
      fail(FailureKind::structural);
    pos += n;
  }
  JsonValue parse_number() {
    JsonValue v;
    v.kind = JsonValue::Kind::number;
    const size_t start = pos;
    const bool negative = pos < s.size() && s[pos] == '-';
    if (negative)
      ++pos;
    if (pos >= s.size() || !isdigit(static_cast<unsigned char>(s[pos])))
      fail(FailureKind::structural);
    if (s[pos] == '0') {
      ++pos; // no leading zeros
    } else {
      while (pos < s.size() && isdigit(static_cast<unsigned char>(s[pos])))
        ++pos;
    }
    if (pos < s.size() &&
        (s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' || s[pos] == '+' ||
         s[pos] == '-' || isdigit(static_cast<unsigned char>(s[pos]))))
      fail(FailureKind::structural); // integers only, no leading zeros, no fraction/exponent
    uint64_t magnitude = 0;
    const uint64_t limit = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
    for (size_t i = start + (negative ? 1 : 0); i < pos; ++i) {
      const uint64_t digit = uint64_t(s[i] - '0');
      // Reject before arithmetic: checking a wrapped result accepts huge values.
      if (magnitude > (limit - digit) / 10)
        fail(FailureKind::structural);
      magnitude = magnitude * 10 + digit;
    }
    if (negative && magnitude == uint64_t(INT64_MAX) + 1)
      v.number = INT64_MIN;
    else
      v.number = negative ? -int64_t(magnitude) : int64_t(magnitude);
    return v;
  }
  std::string parse_string(size_t string_limit = json_string_limit) {
    if (pos >= s.size() || s[pos] != '"')
      fail(FailureKind::structural);
    ++pos;
    std::string out;
    for (;;) {
      if (pos >= s.size())
        fail(FailureKind::structural);
      const unsigned char c = s[pos];
      if (c == '"') {
        ++pos;
        break;
      }
      if (c < 0x20 || c == 0x7F)
        fail(FailureKind::structural);
      if (c == '\\') {
        ++pos;
        if (pos >= s.size())
          fail(FailureKind::structural);
        const char e = s[pos++];
        switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: fail(FailureKind::structural); // \uXXXX and every other escape is rejected
        }
      } else {
        out.push_back(char(c));
        ++pos;
      }
      if (out.size() > string_limit)
        overflow(Overflow::string);
    }
    if (out.size() > string_limit)
      overflow(Overflow::string);
    return out;
  }
  JsonValue parse_object() {
    const size_t start = pos;
    expect('{');
    JsonValue v;
    v.kind = JsonValue::Kind::object;
    if (depth + 1 >= json_max_depth)
      overflow(Overflow::depth);
    ++depth;
    if (take('}')) {
      --depth;
      v.source_bytes = pos - start;
      return v;
    }
    for (;;) {
      ws();
      if (pos >= s.size() || s[pos] != '"')
        fail(FailureKind::structural);
      std::string key = parse_string();
      for (const auto &entry : v.members)
        if (entry.first == key)
          fail(FailureKind::duplicate);
      expect(':');
      // Only payload values use the caller's larger envelope/registry limit.
      const size_t limit = key == "payload" ? max_string : json_string_limit;
      v.members.emplace_back(std::move(key), parse_value(limit));
      if (v.members.size() > json_max_object_keys)
        overflow(Overflow::keys);
      if (take(','))
        continue;
      expect('}');
      break;
    }
    --depth;
    v.source_bytes = pos - start;
    return v;
  }
  JsonValue parse_array() {
    expect('[');
    JsonValue v;
    v.kind = JsonValue::Kind::array;
    if (depth + 1 >= json_max_depth)
      overflow(Overflow::depth);
    ++depth;
    if (take(']')) {
      --depth;
      return v;
    }
    for (;;) {
      v.items.push_back(parse_value());
      if (v.items.size() > json_max_array_items)
        overflow(Overflow::items);
      if (take(','))
        continue;
      expect(']');
      break;
    }
    --depth;
    return v;
  }
};
// Duplicate keys are a distinct bounded identity; other structural failures
// share the role's malformed identity. Overflow maps to the role's bounds
// identity so a huge string cannot masquerade as a generic parse error.
Reject parse_document(const std::string &text, size_t byte_limit,
                      size_t string_limit, JsonValue &out, Reject too_large,
                      Reject malformed, Reject bounds, Reject duplicate) {
  if (text.size() > byte_limit)
    return too_large;
  if (text.empty() || !valid_utf8(text) || !structural_text(text))
    return malformed;
  JsonParser parser(text, string_limit);
  try {
    out = parser.parse_value();
    parser.ws();
    if (parser.pos != text.size())
      return malformed;
  } catch (const JsonFailure &failure) {
    switch (failure.kind) {
    case FailureKind::duplicate: return duplicate;
    case FailureKind::overflow: return bounds;
    case FailureKind::structural: return malformed;
    }
    return malformed;
  }
  return Reject::none;
}
// ------------------------------------------------------------- key shapes ---
const JsonValue *required(const JsonValue &object, const char *key,
                          JsonValue::Kind kind) {
  if (object.kind != JsonValue::Kind::object)
    return nullptr;
  const JsonValue *value = object.find(key);
  if (!value || value->kind != kind)
    return nullptr;
  return value;
}
bool only_keys(const JsonValue &object,
               std::initializer_list<const char *> allowed) {
  if (object.members.size() != allowed.size())
    return false;
  for (const auto &entry : object.members) {
    bool known = false;
    for (const char *key : allowed)
      if (entry.first == key) {
        known = true;
        break;
      }
    if (!known)
      return false;
  }
  return true;
}
bool ascii_lower_hex(const std::string &s, size_t length) {
  if (s.size() != length)
    return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  return true;
}
uint8_t nibble(char c) {
  return c <= '9' ? uint8_t(c - '0') : uint8_t(c - 'a' + 10);
}
bool charset(const std::string &s, const char *allowed) {
  return !s.empty() && s.find_first_not_of(allowed) == std::string::npos;
}
size_t utf8_chars(const std::string &s) {
  size_t count = 0;
  for (unsigned char c : s)
    if ((c & 0xC0u) != 0x80u)
      ++count;
  return count;
}
bool free_text(const std::string &s, size_t char_limit, size_t byte_limit) {
  if (s.empty() || s.size() > byte_limit || utf8_chars(s) > char_limit)
    return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7F)
      return false;
  return true;
}
bool dotted_version(const std::string &s) {
  if (s.empty() || s.size() > core_limit)
    return false;
  size_t segments = 1, digits = 0;
  for (char c : s) {
    if (c == '.') {
      if (!digits || digits > 6)
        return false;
      digits = 0;
      if (++segments > 4)
        return false;
    } else if (c >= '0' && c <= '9') {
      ++digits;
    } else {
      return false;
    }
  }
  return digits > 0 && digits <= 6;
}
std::array<long, 4> version_parts(const std::string &s) {
  std::array<long, 4> out{0, 0, 0, 0};
  size_t index = 0, value = 0;
  for (char c : s) {
    if (c == '.') {
      out[index++] = long(value);
      value = 0;
    } else {
      value = value * 10 + size_t(c - '0');
    }
  }
  out[index] = long(value);
  return out;
}
bool core_satisfied(const std::string &compiled, const std::string &required_core) {
  const auto have = version_parts(compiled);
  const auto need = version_parts(required_core);
  for (size_t i = 0; i < 4; ++i) {
    if (have[i] > need[i])
      return true;
    if (have[i] < need[i])
      return false;
  }
  return true;
}
bool parse_utc(const std::string &stamp, int64_t &unix_seconds) {
  if (stamp.size() != expires_len)
    return false;
  for (size_t i = 0; i < expires_len; ++i) {
    const char c = stamp[i];
    if (i == 4 || i == 7) {
      if (c != '-')
        return false;
    } else if (i == 10) {
      if (c != 'T')
        return false;
    } else if (i == 13 || i == 16) {
      if (c != ':')
        return false;
    } else if (i == 19) {
      if (c != 'Z')
        return false;
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
  std::tm tm{};
  tm.tm_year = std::atoi(stamp.substr(0, 4).c_str()) - 1900;
  tm.tm_mon = std::atoi(stamp.substr(5, 2).c_str()) - 1;
  tm.tm_mday = std::atoi(stamp.substr(8, 2).c_str());
  tm.tm_hour = std::atoi(stamp.substr(11, 2).c_str());
  tm.tm_min = std::atoi(stamp.substr(14, 2).c_str());
  tm.tm_sec = std::atoi(stamp.substr(17, 2).c_str());
  if (tm.tm_year < 70 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 ||
      tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59)
    return false;
  const std::tm requested = tm;
  unix_seconds = int64_t(_mkgmtime(&tm));
  // The CRT normalizes impossible dates; do not authenticate the normalized day.
  return unix_seconds >= 0 && tm.tm_year == requested.tm_year &&
         tm.tm_mon == requested.tm_mon && tm.tm_mday == requested.tm_mday &&
         tm.tm_hour == requested.tm_hour && tm.tm_min == requested.tm_min &&
         tm.tm_sec == requested.tm_sec;
}
// -------------------------------------------------------------- namespace ---
const char *const reserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
bool reserved_segment(const std::string &segment) {
  const std::string stem = segment.substr(0, segment.find('.'));
  std::string upper;
  for (char c : stem)
    upper.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  for (const char *name : reserved)
    if (upper == name)
      return true;
  return upper.size() == 4 &&
         (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) &&
         upper[3] >= '0' && upper[3] <= '9';
}
// ------------------------------------------------------------------- CNG ----
struct Algorithm {
  BCRYPT_ALG_HANDLE handle = nullptr;
  Algorithm() {
    if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) < 0)
      handle = nullptr;
  }
  ~Algorithm() {
    if (handle)
      BCryptCloseAlgorithmProvider(handle, 0);
  }
  Algorithm(const Algorithm &) = delete;
  Algorithm &operator=(const Algorithm &) = delete;
};
bool verify_signature(const std::vector<uint8_t> &public_blob,
                      const std::vector<uint8_t> &digest,
                      const std::vector<uint8_t> &signature) {
  if (digest.size() != 32 || signature.size() != signature_bytes)
    return false;
  if (public_blob.size() != 72)
    return false;
  Algorithm algorithm;
  if (!algorithm.handle)
    return false;
  BCRYPT_KEY_HANDLE key = nullptr;
  std::vector<uint8_t> blob = public_blob;
  if (BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                          blob.data(), ULONG(blob.size()), 0) < 0)
    return false;
  std::vector<uint8_t> mutable_digest = digest;
  std::vector<uint8_t> mutable_signature = signature;
  const NTSTATUS status = BCryptVerifySignature(
      key, nullptr, mutable_digest.data(), ULONG(mutable_digest.size()),
      mutable_signature.data(), ULONG(mutable_signature.size()), 0);
  BCryptDestroyKey(key);
  return status == 0;
}
// --------------------------------------------------------------- envelope ---
struct EnvelopeFields {
  std::string key_id, payload, signature_hex;
};
Reject envelope_fields(const JsonValue &root, EnvelopeFields &out) {
  if (root.kind != JsonValue::Kind::object)
    return Reject::envelope_malformed;
  if (!only_keys(root, {"schema", "product", "kind", "key_id", "payload",
                        "signature"}))
    return Reject::envelope_key_unknown;
  const JsonValue *schema = required(root, "schema", JsonValue::Kind::number);
  const JsonValue *product = required(root, "product", JsonValue::Kind::string);
  const JsonValue *kind = required(root, "kind", JsonValue::Kind::string);
  const JsonValue *key_id = required(root, "key_id", JsonValue::Kind::string);
  const JsonValue *payload = required(root, "payload", JsonValue::Kind::string);
  const JsonValue *signature =
      required(root, "signature", JsonValue::Kind::string);
  if (!schema || !product || !kind || !key_id || !payload || !signature)
    return Reject::envelope_field_invalid;
  if (schema->number != record_schema)
    return Reject::envelope_schema_unsupported;
  if (product->text != record_product)
    return Reject::envelope_product_mismatch;
  if (kind->text != envelope_kind)
    return Reject::envelope_kind_mismatch;
  if (key_id->text.size() < key_id_min || key_id->text.size() > key_id_limit ||
      !charset(key_id->text, "abcdefghijklmnopqrstuvwxyz0123456789_-"))
    return Reject::envelope_field_invalid;
  if (payload->text.size() < 4 || payload->text.size() > payload_string_limit)
    return Reject::envelope_field_invalid;
  if (!ascii_lower_hex(signature->text, signature_hex_len))
    return Reject::signature_encoding_invalid;
  out.key_id = key_id->text;
  out.payload = payload->text;
  out.signature_hex = signature->text;
  return Reject::none;
}
// ------------------------------------------------------------- descriptor ---
Reject read_descriptor(const std::string &bytes, ModuleDescriptor &out) {
  if (bytes.size() > descriptor_byte_limit)
    return Reject::descriptor_too_large;
  if (bytes.size() < descriptor_byte_min)
    return Reject::descriptor_malformed;
  JsonValue root;
  const Reject parse = parse_document(
      bytes, descriptor_byte_limit, json_string_limit, root,
      Reject::descriptor_too_large, Reject::descriptor_malformed,
      Reject::descriptor_malformed, Reject::descriptor_malformed);
  if (parse != Reject::none)
    return parse;
  if (root.kind != JsonValue::Kind::object)
    return Reject::descriptor_malformed;
  static const char *const keys[15] = {
      "schema",       "product",        "kind",
      "module_id",    "version",        "revision",
      "abi",          "core",           "name_zh",
      "name_en",      "description_zh", "description_en",
      "hotkey",       "validity",       "dll"};
  if (root.members.size() != 15)
    return Reject::descriptor_malformed;
  for (const char *key : keys)
    if (!root.find(key))
      return Reject::descriptor_malformed;
  const JsonValue *schema = required(root, "schema", JsonValue::Kind::number);
  const JsonValue *product = required(root, "product", JsonValue::Kind::string);
  const JsonValue *kind = required(root, "kind", JsonValue::Kind::string);
  if (!schema || !product || !kind)
    return Reject::descriptor_malformed;
  if (schema->number != record_schema)
    return Reject::descriptor_schema_unsupported;
  if (product->text != record_product)
    return Reject::descriptor_product_mismatch;
  if (kind->text != descriptor_kind)
    return Reject::descriptor_kind_mismatch;
  // A valid identity is required before a row may contribute a name at all.
  const JsonValue *id = required(root, "module_id", JsonValue::Kind::string);
  if (!id || id->text.empty() || id->text.size() > module_id_limit ||
      id->text[0] < 'a' || id->text[0] > 'z' ||
      id->text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") !=
          std::string::npos)
    return Reject::descriptor_identity_invalid;
  out.module_id = id->text;
  const JsonValue *version = required(root, "version", JsonValue::Kind::string);
  if (!version || version->text.empty() || version->text.size() > version_limit ||
      version->text.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                      "abcdefghijklmnopqrstuvwxyz._+-") !=
          std::string::npos)
    return Reject::descriptor_version_invalid;
  out.version = version->text;
  const JsonValue *revision =
      required(root, "revision", JsonValue::Kind::number);
  if (!revision || revision->number < 0 || revision->number > revision_limit)
    return Reject::descriptor_revision_invalid;
  out.revision = revision->number;
  const JsonValue *abi = required(root, "abi", JsonValue::Kind::string);
  if (!abi || abi->text.empty() || abi->text.size() > abi_limit)
    return Reject::descriptor_text_invalid;
  out.abi = abi->text;
  const JsonValue *core = required(root, "core", JsonValue::Kind::string);
  if (!core || !dotted_version(core->text))
    return Reject::descriptor_version_invalid;
  out.core = core->text;
  const JsonValue *name_zh = required(root, "name_zh", JsonValue::Kind::string);
  const JsonValue *name_en = required(root, "name_en", JsonValue::Kind::string);
  const JsonValue *desc_zh =
      required(root, "description_zh", JsonValue::Kind::string);
  const JsonValue *desc_en =
      required(root, "description_en", JsonValue::Kind::string);
  if (!name_zh || !name_en || !desc_zh || !desc_en ||
      !free_text(name_zh->text, name_char_limit, name_byte_limit) ||
      !free_text(name_en->text, name_char_limit, name_byte_limit) ||
      !free_text(desc_zh->text, description_char_limit, description_byte_limit) ||
      !free_text(desc_en->text, description_char_limit, description_byte_limit))
    return Reject::descriptor_text_invalid;
  out.name_zh = name_zh->text;
  out.name_en = name_en->text;
  out.description_zh = desc_zh->text;
  out.description_en = desc_en->text;
  const JsonValue *hotkey = root.find("hotkey");
  if (!hotkey)
    return Reject::descriptor_malformed;
  if (hotkey->kind == JsonValue::Kind::string) {
    if (hotkey->text.empty() || hotkey->text.size() > hotkey_limit ||
        hotkey->text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijk"
                                       "lmnopqrstuvwxyz0123456789+_-") !=
            std::string::npos)
      return Reject::descriptor_hotkey_invalid;
    out.hotkey_present = true;
    out.hotkey = hotkey->text;
  } else if (hotkey->kind != JsonValue::Kind::null) {
    return Reject::descriptor_hotkey_invalid;
  }
  const JsonValue *validity =
      required(root, "validity", JsonValue::Kind::object);
  if (!validity || validity->members.size() != 1)
    return Reject::descriptor_validity_invalid;
  const JsonValue *expires = validity->find("expires");
  if (!expires)
    return Reject::descriptor_validity_invalid;
  if (expires->kind == JsonValue::Kind::string) {
    int64_t stamp = 0;
    if (!parse_utc(expires->text, stamp))
      return Reject::descriptor_validity_invalid;
    out.expires_present = true;
    out.expires = expires->text;
  } else if (expires->kind != JsonValue::Kind::null) {
    return Reject::descriptor_validity_invalid;
  }
  const JsonValue *dll = required(root, "dll", JsonValue::Kind::object);
  if (!dll || dll->members.size() != 3)
    return Reject::descriptor_payload_invalid;
  const JsonValue *path = required(*dll, "path", JsonValue::Kind::string);
  const JsonValue *size = required(*dll, "size", JsonValue::Kind::number);
  const JsonValue *hash = required(*dll, "sha256", JsonValue::Kind::string);
  if (!path || !size || !hash)
    return Reject::descriptor_payload_invalid;
  if (size->number <= 0 || uint64_t(size->number) > dll_size_limit)
    return Reject::descriptor_payload_invalid;
  if (!ascii_lower_hex(hash->text, sha256_hex_len))
    return Reject::descriptor_payload_invalid;
  out.dll_path = path->text;
  out.dll_size = uint64_t(size->number);
  out.dll_sha256 = hash->text;
  return Reject::none;
}
// ------------------------------------------------------- row verification ---
RecordOutcome verify_fields(const EnvelopeFields &envelope,
                            const TrustSet &trusted, const RecordPolicy &policy) {
  RecordOutcome outcome;
  if (trusted.empty()) {
    outcome.error = Reject::trust_root_not_provisioned;
    return outcome;
  }
  std::string descriptor_bytes;
  if (!base64_decode(envelope.payload, descriptor_bytes)) {
    outcome.error = Reject::payload_encoding_invalid;
    return outcome;
  }
  std::vector<uint8_t> signature(signature_bytes);
  for (size_t i = 0; i < signature_bytes; ++i)
    signature[i] = uint8_t((nibble(envelope.signature_hex[i * 2]) << 4) |
                           nibble(envelope.signature_hex[i * 2 + 1]));
  const TrustAnchor *anchor = nullptr;
  for (const auto &candidate : trusted)
    if (candidate.key_id == envelope.key_id) {
      anchor = &candidate;
      break;
    }
  if (!anchor) {
    outcome.error = Reject::key_id_unknown;
    return outcome;
  }
  const std::vector<uint8_t> digest = sha256_raw(descriptor_bytes);
  if (!verify_signature(anchor->public_key_blob, digest, signature)) {
    outcome.error = Reject::signature_invalid;
    return outcome;
  }
  ModuleDescriptor descriptor;
  const Reject descriptor_error = read_descriptor(descriptor_bytes, descriptor);
  if (descriptor_error != Reject::none) {
    outcome.error = descriptor_error;
    return outcome;
  }
  // The exact signed bytes are authenticated and the identity is usable. A
  // remaining failure keeps the row visible with a fixed identity.
  outcome.verified = true;
  outcome.key_id = envelope.key_id;
  outcome.descriptor = descriptor;
  if (!policy.abi_fingerprint.empty() && descriptor.abi != policy.abi_fingerprint)
    outcome.error = Reject::abi_fingerprint_mismatch;
  else if (!core_satisfied(policy.core_version, descriptor.core))
    outcome.error = Reject::core_requirement_unmet;
  else if (const Reject path_error =
               validate_namespace_path(descriptor.dll_path, descriptor.module_id);
           path_error != Reject::none)
    outcome.error = path_error;
  else if (descriptor.expires_present) {
    int64_t stamp = 0;
    if (!parse_utc(descriptor.expires, stamp))
      outcome.error = Reject::descriptor_validity_invalid;
    else if (stamp < policy.now_unix)
      outcome.error = Reject::record_expired;
  }
  return outcome;
}
// Global-structure isolation (docs §3/§4). Verified rows that claim the same
// module_id, or the same payload path case-insensitively, form a conflict group
// and the whole group is rejected. Ownership ambiguity is never resolved by
// picking a winner. Unauthenticated rows never participate, so a signature
// failure can neither hide nor impersonate a healthy module.
void apply_ownership(RegistryOutcome &outcome) {
  for (size_t i = 0; i < outcome.rows.size(); ++i) {
    for (size_t j = i + 1; j < outcome.rows.size(); ++j) {
      const auto &a = outcome.rows[i].descriptor;
      const auto &b = outcome.rows[j].descriptor;
      const bool same_id = a.module_id == b.module_id;
      std::string left = a.dll_path, right = b.dll_path;
      for (char &c : left)
        c = char(std::tolower(static_cast<unsigned char>(c)));
      for (char &c : right)
        c = char(std::tolower(static_cast<unsigned char>(c)));
      const bool same_path = left == right;
      if (!same_id && !same_path)
        continue;
      const Reject conflict = same_id ? Reject::duplicate_module_identity
                                      : Reject::payload_ownership_conflict;
      outcome.rows[i].error = conflict;
      outcome.rows[j].error = conflict;
    }
  }
}
} // namespace

const char *reject_text(Reject code) {
  static const char *const table[] = {
      "none",
      "envelope_too_large", "envelope_malformed", "envelope_schema_unsupported",
      "envelope_product_mismatch", "envelope_kind_mismatch",
      "envelope_key_unknown", "envelope_key_duplicate", "envelope_field_invalid",
      "payload_encoding_invalid", "signature_encoding_invalid",
      "signature_invalid", "key_id_unknown", "trust_root_not_provisioned",
      "descriptor_too_large", "descriptor_malformed",
      "descriptor_schema_unsupported", "descriptor_product_mismatch",
      "descriptor_kind_mismatch", "descriptor_identity_invalid",
      "descriptor_version_invalid", "descriptor_revision_invalid",
      "descriptor_text_invalid", "descriptor_hotkey_invalid",
      "descriptor_validity_invalid", "descriptor_payload_invalid",
      "record_expired", "abi_fingerprint_mismatch", "core_requirement_unmet",
      "namespace_invalid", "namespace_common_reserved",
      "duplicate_module_identity", "payload_ownership_conflict",
      "registry_too_large", "registry_malformed",
      "registry_schema_unsupported", "registry_product_mismatch",
      "registry_kind_mismatch", "registry_bounds_exceeded",
      "module_payload_missing", "module_payload_oversized",
      "module_size_mismatch", "module_hash_mismatch", "module_object_invalid",
      "module_dependency_unresolved", "module_map_failed",
      "module_mapped_identity_mismatch", "module_export_missing",
      "module_export_mismatch", "module_abi_rejected",
      "module_record_not_installed",
  };
  static_assert(sizeof(table) / sizeof(table[0]) ==
                size_t(Reject::module_record_not_installed) + 1);
  return table[size_t(code)];
}

std::string base64_encode(const std::string &raw) {
  static const char *const alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 3 <= raw.size(); i += 3) {
    const uint32_t n = (uint32_t(uint8_t(raw[i])) << 16) |
                       (uint32_t(uint8_t(raw[i + 1])) << 8) |
                       uint32_t(uint8_t(raw[i + 2]));
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    out.push_back(alphabet[(n >> 6) & 63]);
    out.push_back(alphabet[n & 63]);
  }
  const size_t rest = raw.size() - i;
  if (rest == 1) {
    const uint32_t n = uint32_t(uint8_t(raw[i])) << 16;
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (rest == 2) {
    const uint32_t n =
        (uint32_t(uint8_t(raw[i])) << 16) | (uint32_t(uint8_t(raw[i + 1])) << 8);
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    out.push_back(alphabet[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

bool base64_decode(const std::string &text, std::string &raw) {
  static const char *const alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  raw.clear();
  if (text.empty() || text.size() % 4 != 0 || text.size() > payload_string_limit)
    return false;
  auto value = [&](char c) -> int {
    const char *found = std::strchr(alphabet, c);
    return found ? int(found - alphabet) : -1;
  };
  size_t padding = 0;
  if (text.back() == '=')
    ++padding;
  if (text.size() >= 2 && text[text.size() - 2] == '=')
    ++padding;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '=') {
      if (i < text.size() - padding)
        return false;
      continue;
    }
    if (value(c) < 0)
      return false;
  }
  for (size_t i = 0; i < text.size(); i += 4) {
    const int a = value(text[i]), b = value(text[i + 1]);
    const int c = text[i + 2] == '=' ? 0 : value(text[i + 2]);
    const int d = text[i + 3] == '=' ? 0 : value(text[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0)
      return false;
    raw.push_back(char((a << 2) | (b >> 4)));
    if (text[i + 2] != '=')
      raw.push_back(char(((b & 15) << 4) | (c >> 2)));
    if (text[i + 3] != '=')
      raw.push_back(char(((c & 3) << 6) | d));
  }
  // Canonical encoding: re-encoding the decoded bytes must reproduce the input.
  return base64_encode(raw) == text;
}

std::string sha256_hex(const std::string &raw) {
  return crypto::sha256_hex(std::vector<uint8_t>(raw.begin(), raw.end()));
}
std::vector<uint8_t> sha256_raw(const std::string &raw) {
  return crypto::sha256_raw(reinterpret_cast<const uint8_t *>(raw.data()),
                            raw.size());
}

int64_t current_unix() { return int64_t(std::time(nullptr)); }

Reject validate_namespace_path(const std::string &relative,
                               const std::string &module_id) {
  if (relative.empty() || relative.size() > path_byte_limit ||
      !valid_utf8(relative))
    return Reject::namespace_invalid;
  for (unsigned char c : relative) {
    if (c < 0x20 || c == 0x7F)
      return Reject::namespace_invalid;
    if (std::string("<>:\"|?*").find(char(c)) != std::string::npos)
      return Reject::namespace_invalid;
    if (c == '\\')
      return Reject::namespace_invalid;
  }
  if (relative.front() == '/' || relative.back() == '/')
    return Reject::namespace_invalid;
  std::vector<std::string> segments;
  size_t start = 0;
  for (;;) {
    const size_t slash = relative.find('/', start);
    const std::string segment = relative.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start);
    if (segment.empty() || segment.size() > path_segment_limit)
      return Reject::namespace_invalid;
    if (segment == "." || segment == "..")
      return Reject::namespace_invalid;
    if (segment.back() == '.' || segment.back() == ' ')
      return Reject::namespace_invalid;
    if (reserved_segment(segment))
      return Reject::namespace_invalid;
    segments.push_back(segment);
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  if (segments.size() < path_segment_min || segments.size() > path_segment_max)
    return Reject::namespace_invalid;
  std::string first;
  for (char c : segments[0])
    first.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  if (first == "COMMON")
    return Reject::namespace_common_reserved;
  // Deterministic v1 layout: modules/<module_id>/<module_id>.dll
  if (segments.size() != 3 || segments[0] != "modules" ||
      segments[1] != module_id || segments[2] != module_id + ".dll")
    return Reject::namespace_invalid;
  return Reject::none;
}

RecordOutcome verify_record(const std::string &envelope_utf8,
                            const TrustSet &trusted, const RecordPolicy &policy) {
  RecordOutcome outcome;
  if (trusted.empty()) {
    outcome.error = Reject::trust_root_not_provisioned;
    return outcome;
  }
  JsonValue root;
  const Reject parse = parse_document(
      envelope_utf8, envelope_byte_limit, payload_string_limit, root,
      Reject::envelope_too_large, Reject::envelope_malformed,
      Reject::envelope_field_invalid, Reject::envelope_key_duplicate);
  if (parse != Reject::none) {
    outcome.error = parse;
    return outcome;
  }
  EnvelopeFields fields;
  const Reject fields_error = envelope_fields(root, fields);
  if (fields_error != Reject::none) {
    outcome.error = fields_error;
    return outcome;
  }
  return verify_fields(fields, trusted, policy);
}

RegistryOutcome parse_registry(const std::string &registry_utf8,
                               const TrustSet &trusted,
                               const RecordPolicy &policy) {
  RegistryOutcome outcome;
  if (trusted.empty()) {
    outcome.error = Reject::trust_root_not_provisioned;
    return outcome;
  }
  if (registry_utf8.size() > registry_byte_limit) {
    outcome.error = Reject::registry_too_large;
    return outcome;
  }
  JsonValue root;
  const Reject parse = parse_document(
      registry_utf8, registry_byte_limit, payload_string_limit, root,
      Reject::registry_too_large, Reject::registry_malformed,
      Reject::registry_bounds_exceeded, Reject::registry_malformed);
  if (parse != Reject::none) {
    outcome.error = parse;
    return outcome;
  }
  if (root.kind != JsonValue::Kind::object || root.members.size() != 4 ||
      !only_keys(root, {"schema", "product", "kind", "records"})) {
    outcome.error = Reject::registry_malformed;
    return outcome;
  }
  const JsonValue *schema = required(root, "schema", JsonValue::Kind::number);
  const JsonValue *product = required(root, "product", JsonValue::Kind::string);
  const JsonValue *kind = required(root, "kind", JsonValue::Kind::string);
  const JsonValue *records = required(root, "records", JsonValue::Kind::array);
  if (!schema || !product || !kind || !records) {
    outcome.error = Reject::registry_malformed;
    return outcome;
  }
  if (schema->number != record_schema) {
    outcome.error = Reject::registry_schema_unsupported;
    return outcome;
  }
  if (product->text != record_product) {
    outcome.error = Reject::registry_product_mismatch;
    return outcome;
  }
  if (kind->text != registry_kind) {
    outcome.error = Reject::registry_kind_mismatch;
    return outcome;
  }
  if (records->items.size() > registry_record_limit) {
    outcome.error = Reject::registry_bounds_exceeded;
    return outcome;
  }
  std::vector<EnvelopeFields> envelopes;
  for (const auto &item : records->items) {
    if (item.source_bytes > envelope_byte_limit) {
      outcome.error = Reject::registry_bounds_exceeded;
      return outcome;
    }
    EnvelopeFields fields;
    if (item.kind != JsonValue::Kind::object ||
        envelope_fields(item, fields) != Reject::none) {
      outcome.error = Reject::registry_malformed;
      return outcome;
    }
    envelopes.push_back(std::move(fields));
  }
  // Validate every row's shape before authenticating any individual identity.
  outcome.index_ok = true;
  for (const auto &fields : envelopes) {
    RecordOutcome row = verify_fields(fields, trusted, policy);
    if (!row.verified) {
      ++outcome.dropped_rows;
      continue;
    }
    outcome.rows.push_back(std::move(row));
  }
  apply_ownership(outcome);
  return outcome;
}
} // namespace astro::auth
