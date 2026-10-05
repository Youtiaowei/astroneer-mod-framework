#include <windows.h>
// CNG SHA-256 implementation using Windows BCrypt API.
#include "sha256.h"
#include <algorithm>
#include <bcrypt.h>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace crypto {

static std::string to_hex(const uint8_t *data, size_t len) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string r;
  r.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    r += hex[data[i] >> 4];
    r += hex[data[i] & 0x0F];
  }
  return r;
}

std::vector<uint8_t> sha256_raw(const uint8_t *data, size_t len) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<uint8_t> result(32);
  NTSTATUS status;

  status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status))
    throw std::runtime_error("BCryptOpenAlgorithmProvider failed");

  DWORD hash_len = 0, data_size = 0;
  status =
      BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PBYTE)&hash_len, sizeof(hash_len), &data_size, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(alg, 0);
    throw std::runtime_error("BCryptGetProperty failed");
  }

  status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(alg, 0);
    throw std::runtime_error("BCryptCreateHash failed");
  }

  // Process in chunks to handle large data
  constexpr size_t CHUNK = 1024 * 1024;
  size_t offset = 0;
  while (offset < len) {
    size_t chunk = (std::min)(CHUNK, len - offset);
    status = BCryptHashData(hash, (PBYTE)(data + offset), (ULONG)chunk, 0);
    if (!BCRYPT_SUCCESS(status)) {
      BCryptDestroyHash(hash);
      BCryptCloseAlgorithmProvider(alg, 0);
      throw std::runtime_error("BCryptHashData failed");
    }
    offset += chunk;
  }

  status = BCryptFinishHash(hash, result.data(), 32, 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  if (!BCRYPT_SUCCESS(status))
    throw std::runtime_error("BCryptFinishHash failed");

  return result;
}

std::string sha256_hex(const uint8_t *data, size_t len) {
  auto raw = sha256_raw(data, len);
  return to_hex(raw.data(), raw.size());
}

std::string sha256_hex(const std::vector<uint8_t> &data) {
  return sha256_hex(data.data(), data.size());
}

std::string sha256_hex(const std::string &s) {
  return sha256_hex(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

} // namespace crypto
