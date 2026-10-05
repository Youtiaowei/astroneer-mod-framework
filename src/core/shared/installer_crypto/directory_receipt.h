#pragma once
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace io {
// v0 is a readable historical triple, never a deletion authorization.
// v2 carries local NTFS identity; only a checked create-only return grants provenance.
struct DirectoryReceipt {
  uint32_t volume_serial = 0;
  uint32_t file_index_high = 0;
  uint32_t file_index_low = 0;
  uint32_t version = 0;
  uint64_t volume_id = 0;
  std::array<uint8_t, 16> file_id{};
  uint64_t creation_time = 0;
  bool valid() const {
    return version == 2 && volume_id != 0 && volume_serial != 0 &&
           (file_index_high != 0 || file_index_low != 0) && creation_time != 0 &&
           creation_time <= INT64_MAX &&
           std::any_of(file_id.begin(), file_id.end(), [](uint8_t b){return b != 0;});
  }
  bool matches(const DirectoryReceipt &other) const {
    return valid() && other.valid() && volume_id == other.volume_id && file_id == other.file_id &&
           creation_time == other.creation_time && volume_serial == other.volume_serial &&
           file_index_high == other.file_index_high && file_index_low == other.file_index_low;
  }
};
inline std::string receipt_hex(uint64_t value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (size_t i = text.size(); i != 0; --i) { text[i-1] = digits[value & 15]; value >>= 4; }
  return text;
}
inline uint64_t receipt_number(const std::string &text, size_t width = 16) {
  if (text.size() != width || text.find_first_not_of("0123456789abcdef") != text.npos)
    throw std::runtime_error("Invalid directory receipt hex string");
  uint64_t value = 0;
  for (auto c : text) value = (value << 4) | uint64_t(c <= '9' ? c - '0' : c - 'a' + 10);
  return value;
}
inline std::string receipt_file_id(const DirectoryReceipt &receipt) {
  std::string text;
  for (auto byte : receipt.file_id) text += receipt_hex(byte).substr(14);
  return text;
}
} // namespace io
