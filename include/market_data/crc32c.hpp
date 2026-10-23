#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mokuren::mdf {

// CRC-32C (Castagnoli), reflected form.
//
// One implementation, shared by the writer and the reader. A checksum that
// exists twice can disagree with itself, and the failure it produces -- a block
// that was written correctly reported as corrupt, or a damaged block accepted --
// looks like a storage bug rather than the code bug it is.
inline uint32_t Crc32c(const uint8_t* data, size_t n) {
  static const std::array<uint32_t, 256> kTable = [] {
    std::array<uint32_t, 256> table{};
    constexpr uint32_t kPoly = 0x82F63B78u;
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1u) ? (c >> 1) ^ kPoly : (c >> 1);
      }
      table[i] = c;
    }
    return table;
  }();

  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c = kTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace mokuren::mdf
