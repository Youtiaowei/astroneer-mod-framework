#pragma once
// Public trust-root build seam. The anchor set comes from a compiled/build
// input owned by us (trust/production_anchors.inc); it is never taken from an
// installed record and it is never a fixture key.
#include "auth_record.hpp"
#include <string>
namespace astro::auth {
// "not_provisioned" | "provisioned"
const char *compiled_trust_status();
const TrustSet &compiled_trust_anchors();
// Exact bytes of the embedded build-input text (the anchor lines).
std::string compiled_trust_source();
} // namespace astro::auth
