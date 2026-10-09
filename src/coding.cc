#include "coding.h"

#include <array>

namespace lsmkv {

namespace {

std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> t{};
  const uint32_t kPoly = 0x82F63B78u;  // reflected Castagnoli polynomial
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ kPoly : (c >> 1);
    t[i] = c;
  }
  return t;
}

const std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

}  // namespace

uint32_t Crc32c(const char* data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) c = kCrcTable[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint64_t Hash64(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char ch : s) {
    h ^= ch;
    h *= 1099511628211ull;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

}  // namespace lsmkv
