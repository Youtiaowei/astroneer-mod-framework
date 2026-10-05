// CNG SHA-256 wrapper for the Astroneer Mod Installer.
// Uses Windows BCrypt API; no external crypto dependencies.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace crypto {

// Returns lowercase hex SHA-256 of a byte buffer.
std::string sha256_hex(const uint8_t *data, size_t len);
std::string sha256_hex(const std::vector<uint8_t> &data);
std::string sha256_hex(const std::string &s);

// Returns raw 32-byte SHA-256 digest.
std::vector<uint8_t> sha256_raw(const uint8_t *data, size_t len);

} // namespace crypto
