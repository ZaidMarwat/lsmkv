#pragma once

// Bloom filter over user keys, one per SSTable.
//
// Uses double hashing (Kirsch & Mitzenmacher): g_i(x) = h1(x) + i*h2(x),
// which gives the same asymptotic false-positive rate as k independent
// hashes while computing only one 64-bit hash per key.
//
// With b bits/key the optimal k is b*ln2; at b=10, k=7 and FPR ~= 0.8%.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsmkv {

class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}
  void AddKey(std::string_view user_key);
  std::string Finish();  // filter bytes followed by one byte holding k
  size_t NumKeys() const { return hashes_.size(); }

 private:
  int bits_per_key_;
  std::vector<uint64_t> hashes_;
  std::string last_key_;
};

bool BloomMayContain(std::string_view filter, std::string_view user_key);

}  // namespace lsmkv
