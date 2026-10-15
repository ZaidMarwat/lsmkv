#include "bloom.h"

#include <algorithm>
#include <cmath>

#include "coding.h"

namespace lsmkv {

void BloomFilterBuilder::AddKey(std::string_view user_key) {
  // Tables contain many versions of a key back-to-back; add each once.
  if (!hashes_.empty() && user_key == last_key_) return;
  last_key_.assign(user_key.data(), user_key.size());
  hashes_.push_back(Hash64(user_key));
}

std::string BloomFilterBuilder::Finish() {
  int k = static_cast<int>(std::round(bits_per_key_ * 0.69314718));
  k = std::clamp(k, 1, 30);
  size_t bits = std::max<size_t>(64, hashes_.size() * size_t(bits_per_key_));
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  std::string out(bytes, '\0');
  for (uint64_t h : hashes_) {
    uint32_t h1 = static_cast<uint32_t>(h);
    uint32_t h2 = static_cast<uint32_t>(h >> 32) | 1;  // odd => full period
    for (int i = 0; i < k; i++) {
      uint64_t pos = (uint64_t(h1) + uint64_t(i) * h2) % bits;
      out[pos / 8] |= static_cast<char>(1 << (pos % 8));
    }
  }
  out.push_back(static_cast<char>(k));
  return out;
}

bool BloomMayContain(std::string_view filter, std::string_view user_key) {
  if (filter.size() < 2) return true;  // no/empty filter: must check the table
  int k = static_cast<uint8_t>(filter.back());
  if (k < 1 || k > 30) return true;  // unknown encoding: be conservative
  size_t bits = (filter.size() - 1) * 8;
  uint64_t h = Hash64(user_key);
  uint32_t h1 = static_cast<uint32_t>(h);
  uint32_t h2 = static_cast<uint32_t>(h >> 32) | 1;
  for (int i = 0; i < k; i++) {
    uint64_t pos = (uint64_t(h1) + uint64_t(i) * h2) % bits;
    if ((filter[pos / 8] & (1 << (pos % 8))) == 0) return false;
  }
  return true;
}

}  // namespace lsmkv
