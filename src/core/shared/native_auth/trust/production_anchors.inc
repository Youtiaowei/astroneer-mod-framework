// Production PUBLIC trust anchors for auth_record v1.
// This file is owned by us as a compiled/build input and NEVER contains private
// keys, and a key embedded in an installed record is never a trust anchor.
//
// Provisioned with the authorized dedicated local release PUBLIC key.
// Its private half stays DPAPI-encrypted outside source/payload distribution.
// Test-only ephemeral CNG key pairs never become production fallbacks.
//
// Entry shape (when provisioned): ASTRO_AUTH_ANCHOR(key_id, public_blob_hex)
// where public_blob_hex is exactly 144 lowercase hex characters = the 72-byte
// BCRYPT_ECCPUBLIC_BLOB (BCRYPT_ECCKEY_BLOB header + big-endian X + Y).
#ifdef ASTRO_AUTH_ANCHORS_TEXT
R"astroanchors(ASTRO_AUTH_ANCHOR("astro-r25-release-1", "4543533120000000f39bd40fa36470213df7cae209b213210646cb52ffba6def31425ab3ae6c2dece2323a4477ad227a5fbb8a90fc03d11e0fa816d1d85c298ed4ec5e9c8cd4ef63"))astroanchors"
#else
ASTRO_AUTH_ANCHOR("astro-r25-release-1", "4543533120000000f39bd40fa36470213df7cae209b213210646cb52ffba6def31425ab3ae6c2dece2323a4477ad227a5fbb8a90fc03d11e0fa816d1d85c298ed4ec5e9c8cd4ef63")
#endif
