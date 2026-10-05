#pragma once
// Package/common provenance envelope (package_record v1) — installer/manager side.
//
// SEPARATE FROM MODULE PROVENANCE. Module records are auth_record v1 and are
// consumed unchanged (docs/auth_record_contract.md). This is a second, bounded
// envelope kind for shared framework/loader/manager bytes only. It is NOT a
// second module-record format: it never carries module identity, never grants a
// game path outside the closed `common/<role>/` namespace, and is never used to
// replace a module record.
//
// Signature is ECDSA P-256 over SHA-256(exact descriptor bytes), the same CNG
// primitives as auth_record v1 (BCryptSignHash/BCryptVerifySignature, 64-byte
// signature, BCRYPT_ECCPUBLIC_BLOB). Ledger hashes are NEVER signatures.
//
// Exact signed bytes: base64_decode(envelope.payload), byte for byte. Verification
// never re-serializes a descriptor.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace astro::pkg {

// --- document bounds (named, shared by producer/verifier/tests) ---
inline constexpr size_t envelope_byte_limit = 4096;
inline constexpr size_t descriptor_byte_min = 128;
inline constexpr size_t descriptor_byte_limit = 2048;
inline constexpr size_t payload_string_limit = 2732; // canonical base64 of 2048
inline constexpr size_t json_string_limit = 512;
inline constexpr size_t json_max_object_keys = 16;
inline constexpr size_t json_max_array_items = 16;
inline constexpr size_t key_id_min = 1, key_id_limit = 32;
inline constexpr size_t signature_bytes = 64, signature_hex_len = 128;
inline constexpr size_t package_id_limit = 64;
inline constexpr size_t version_limit = 32;
inline constexpr int64_t revision_limit = 1000000000;
inline constexpr size_t abi_limit = 512;
inline constexpr size_t resource_count_min = 1, resource_count_max = 16;
inline constexpr size_t path_byte_limit = 240;
inline constexpr size_t path_segment_limit = 64;
inline constexpr size_t resource_size_limit = 67108864; // 64 MiB per resource
inline constexpr uint64_t package_bytes_limit = 268435456; // 256 MiB closure
inline constexpr size_t sha256_hex_len = 64;

inline constexpr const char *record_product = "AstroModRecord";
inline constexpr const char *envelope_kind = "package_record";
inline constexpr const char *descriptor_kind = "package";
inline constexpr uint32_t record_schema = 1;

// Closed component roles. A package_record may only describe these; a module
// record may never describe them (namespace_common_reserved is enforced there).
inline constexpr const char *component_core = "core";
inline constexpr const char *component_loader = "loader";
inline constexpr const char *component_manager = "manager";
bool closed_component(const std::string &role);

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
  identity_invalid, version_invalid, revision_invalid, component_invalid,
  resource_invalid, namespace_invalid, namespace_role_mismatch,
  resource_conflict, resource_closure_incomplete, resource_order_invalid,
  total_bytes_exceeded,
  duplicate_package_identity,
};
const char *reject_text(Reject);

struct TrustAnchor {
  std::string key_id;
  std::vector<uint8_t> public_key_blob; // BCRYPT_ECCPUBLIC_BLOB (72 bytes)
};
using TrustSet = std::vector<TrustAnchor>;

struct Resource {
  std::string role;   // == descriptor component (closed role binding)
  std::string path;   // common/<component>/<name>
  uint64_t size = 0;
  std::string sha256; // 64 lowercase hex
};

// DETERMINISTIC ORDERING RULE (package_record v1). The `resources` array is
// sorted by `path` in ascending byte order and paths are strictly increasing.
// `validate_closure` rejects any other sequence with resource_order_invalid, so
// one resource set has exactly one authenticated descriptor encoding and one
// closure digest. See payload_catalog.h for the closed path<->target mapping.

struct PackageDescriptor {
  std::string package_id;
  std::string component; // core | loader | manager
  std::string version;
  int64_t revision = 0;
  std::string abi;
  std::vector<Resource> resources;
};

struct Policy {
  int64_t now_unix = 0;
  std::string abi_fingerprint; // byte-exact required fingerprint ("" = no gate)
};

struct Outcome {
  Reject error = Reject::none;
  bool verified = false;
  std::string key_id;
  PackageDescriptor descriptor;
};

// One envelope row. `envelope_utf8` is the exact on-disk envelope text.
Outcome verify_package_record(const std::string &envelope_utf8,
                              const TrustSet &trusted, const Policy &policy);

// Namespace rule for a package resource path (closed `common/<role>/...`).
Reject validate_resource_path(const std::string &relative, const std::string &role);

// Aggregate bound over one package's resource closure.
Reject validate_closure(const PackageDescriptor &d);

std::string base64_encode(const std::string &raw);
bool base64_decode(const std::string &text, std::string &raw);
std::string sha256_hex(const std::string &raw);
std::vector<uint8_t> sha256_raw(const std::string &raw);
int64_t current_unix();

// Public trust-root build seam for package/common records. Mirrors
// astro::auth::compiled_trust_anchors(): compiled/build input owned by us, never
// taken from an installed record. Unprovisioned => fail closed.
const TrustSet &compiled_package_anchors();
const char *compiled_package_status(); // "not_provisioned" | "provisioned"
std::string compiled_package_source(); // exact embedded build-input text

// Test/producer helpers. Only compiled into fixture builds
// (INSTALLER_TEST_SIGNING). The private key lives in memory for the process
// lifetime and is never exported, persisted, logged or printed.
namespace test {
// Create one ephemeral ECDSA P-256 key pair and keep it alive for the process.
// Returns the public anchor; the private half stays in memory only.
TrustAnchor make_anchor(const std::string &key_id);
// Sign one exact descriptor byte string into a complete envelope document.
std::string sign_package_record(const std::string &descriptor_utf8,
                                const std::string &key_id);
} // namespace test

} // namespace astro::pkg
