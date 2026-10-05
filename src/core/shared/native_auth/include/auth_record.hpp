#pragma once
// Authenticated module record contract (auth_record v1). See
// docs/auth_record_contract.md. Self-contained: no engine Value, no private ABI
// objects cross binaries. Every target compiles auth_record.cpp with its own CRT.
//
// BOUNDS ARE NAMED AND SHARED BY PRODUCER, VERIFIER AND TESTS (correction C3).
// A blanket 1024-character string cap cannot hold a 4096-byte descriptor: the
// canonical base64 of 4096 bytes is exactly 5464 characters. Ordinary strings
// keep the 1024-byte cap; only the envelope `payload` field may reach
// payload_string_limit. Every field then has a stricter semantic bound below.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace astro::auth {
// --- document bounds ---
inline constexpr size_t envelope_byte_limit = 8192;
inline constexpr size_t descriptor_byte_min = 128;
inline constexpr size_t descriptor_byte_limit = 4096;
inline constexpr size_t registry_byte_limit = 262144;
inline constexpr size_t registry_record_limit = 32;
// --- strict JSON bounds (kept, not relaxed) ---
inline constexpr size_t json_max_depth = 8;
inline constexpr size_t json_max_object_keys = 24;
inline constexpr size_t json_max_array_items = 64;
inline constexpr size_t json_string_limit = 1024;    // ordinary JSON strings
inline constexpr size_t payload_string_limit = 5464; // base64 chars of 4096 bytes
// --- envelope/descriptor field bounds ---
inline constexpr size_t key_id_min = 1, key_id_limit = 32;
inline constexpr size_t signature_bytes = 64, signature_hex_len = 128;
inline constexpr size_t module_id_limit = 64;
inline constexpr size_t version_limit = 32;
inline constexpr int64_t revision_limit = 1000000000;
inline constexpr size_t abi_limit = 512;
inline constexpr size_t core_limit = 16;
inline constexpr size_t name_char_limit = 64, name_byte_limit = 192;
inline constexpr size_t description_char_limit = 256, description_byte_limit = 768;
inline constexpr size_t hotkey_limit = 16;
inline constexpr size_t expires_len = 20;
inline constexpr size_t path_byte_limit = 240;
inline constexpr size_t path_segment_limit = 64;
inline constexpr size_t path_segment_min = 2, path_segment_max = 8;
inline constexpr size_t dll_size_limit = 67108864; // 64 MiB
inline constexpr size_t sha256_hex_len = 64;
inline constexpr const char *record_product = "AstroModRecord";
inline constexpr const char *envelope_kind = "module_record";
inline constexpr const char *descriptor_kind = "module";
inline constexpr const char *registry_kind = "module_registry";
inline constexpr uint32_t record_schema = 1;
// The aggregate 4096-byte limit is authoritative: a descriptor may satisfy
// every per-field bound and still exceed descriptor_byte_limit, in which case
// it is rejected (descriptor_too_large) and the producer must shrink it.

// Bounded failure identities. Never derived from record content.
enum class Reject : uint32_t {
  none = 0,
  envelope_too_large, envelope_malformed, envelope_schema_unsupported,
  envelope_product_mismatch, envelope_kind_mismatch, envelope_key_unknown,
  envelope_key_duplicate, envelope_field_invalid,
  payload_encoding_invalid, signature_encoding_invalid, signature_invalid,
  key_id_unknown, trust_root_not_provisioned,
  descriptor_too_large, descriptor_malformed, descriptor_schema_unsupported,
  descriptor_product_mismatch, descriptor_kind_mismatch,
  descriptor_identity_invalid, descriptor_version_invalid,
  descriptor_revision_invalid, descriptor_text_invalid,
  descriptor_hotkey_invalid, descriptor_validity_invalid,
  descriptor_payload_invalid, record_expired,
  abi_fingerprint_mismatch, core_requirement_unmet,
  namespace_invalid, namespace_common_reserved, duplicate_module_identity,
  payload_ownership_conflict,
  registry_too_large, registry_malformed, registry_schema_unsupported,
  registry_product_mismatch, registry_kind_mismatch, registry_bounds_exceeded,
  module_payload_missing, module_payload_oversized, module_size_mismatch,
  module_hash_mismatch, module_object_invalid, module_dependency_unresolved,
  module_map_failed, module_mapped_identity_mismatch, module_export_missing,
  module_export_mismatch, module_abi_rejected, module_record_not_installed,
};
const char *reject_text(Reject);

// One compiled/public trust anchor. public_key_blob is a BCRYPT_ECCPUBLIC_BLOB
// (ECDSA P-256, 72 bytes). Private keys never appear here or in any record.
struct TrustAnchor {
  std::string key_id;
  std::vector<uint8_t> public_key_blob;
};
using TrustSet = std::vector<TrustAnchor>;

struct ModuleDescriptor {
  std::string module_id, version, abi, core;
  int64_t revision = 0;
  std::string name_zh, name_en, description_zh, description_en;
  bool hotkey_present = false;
  std::string hotkey;
  bool expires_present = false; // false = explicit "no expiry"
  std::string expires;          // "YYYY-MM-DDTHH:MM:SSZ" when expires_present
  std::string dll_path;         // validated installation-relative path, '/' separated
  uint64_t dll_size = 0;
  std::string dll_sha256;       // 64 lowercase hex
};

// Compiled inputs supplied by the trusted caller, never by the record. Keeping
// them here leaves the reusable verifier free of the private module ABI.
struct RecordPolicy {
  int64_t now_unix = 0;
  std::string abi_fingerprint; // byte-exact required fingerprint
  std::string core_version;    // compiled core version, compared per segment
};

// Outcome of one envelope row.
// verified == true means the authentic identity (valid signature over the exact
// descriptor bytes, plus a valid module_id) is available; `error` may still
// reject that row. Rows with verified == false never contribute ids or display
// metadata and are dropped from the index: their claimed identity is
// unauthenticated and must not become an execution path or hide a healthy row.
struct RecordOutcome {
  Reject error = Reject::none;
  bool verified = false;
  std::string key_id;
  ModuleDescriptor descriptor;
};

struct RegistryOutcome {
  bool index_ok = false; // aggregate document itself accepted
  Reject error = Reject::none;
  std::vector<RecordOutcome> rows; // verified rows only
  size_t dropped_rows = 0;         // bounded count of unauthenticated rows
};

RecordOutcome verify_record(const std::string &envelope_utf8,
                            const TrustSet &trusted, const RecordPolicy &policy);
RegistryOutcome parse_registry(const std::string &registry_utf8,
                               const TrustSet &trusted, const RecordPolicy &policy);
// Namespace rule for the record-relative payload path (docs §3). The
// deterministic v1 layout is checked against the authenticated module_id.
Reject validate_namespace_path(const std::string &relative,
                               const std::string &module_id);
int64_t current_unix();

// Public trust-root build seam (compiled/build input owned by us).
const TrustSet &compiled_trust_anchors();
const char *compiled_trust_status(); // "not_provisioned" | "provisioned"
std::string compiled_trust_source(); // exact embedded build-input text

// Test/producer helpers sharing the same primitives (no private key material).
std::string base64_encode(const std::string &raw);
bool base64_decode(const std::string &text, std::string &raw);
std::string sha256_hex(const std::string &raw);
std::vector<uint8_t> sha256_raw(const std::string &raw);
} // namespace astro::auth
