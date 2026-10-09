#pragma once

// Little-endian fixed-width and varint encodings, CRC32C, and a 64-bit hash.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lsmkv {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char b[4];
  for (int i = 0; i < 4; i++) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(b, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char b[8];
  for (int i = 0; i < 8; i++) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(b, 8);
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= uint32_t(uint8_t(p[i])) << (8 * i);
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= uint64_t(uint8_t(p[i])) << (8 * i);
  return v;
}

inline void PutVarint32(std::string* dst, uint32_t v) {
  while (v >= 128) {
    dst->push_back(static_cast<char>(v | 128));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline bool GetVarint32(std::string_view* in, uint32_t* v) {
  uint32_t result = 0;
  for (int shift = 0; shift <= 28 && !in->empty(); shift += 7) {
    uint8_t byte = static_cast<uint8_t>(in->front());
    in->remove_prefix(1);
    result |= uint32_t(byte & 127) << shift;
    if (!(byte & 128)) {
      *v = result;
      return true;
    }
  }
  return false;
}

inline void PutVarint64(std::string* dst, uint64_t v) {
  while (v >= 128) {
    dst->push_back(static_cast<char>(v | 128));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline bool GetVarint64(std::string_view* in, uint64_t* v) {
  uint64_t result = 0;
  for (int shift = 0; shift <= 63 && !in->empty(); shift += 7) {
    uint8_t byte = static_cast<uint8_t>(in->front());
    in->remove_prefix(1);
    result |= uint64_t(byte & 127) << shift;
    if (!(byte & 128)) {
      *v = result;
      return true;
    }
  }
  return false;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

inline bool GetLengthPrefixed(std::string_view* in, std::string_view* out) {
  uint32_t n;
  if (!GetVarint32(in, &n) || in->size() < n) return false;
  *out = in->substr(0, n);
  in->remove_prefix(n);
  return true;
}

inline bool GetFixed64(std::string_view* in, uint64_t* v) {
  if (in->size() < 8) return false;
  *v = DecodeFixed64(in->data());
  in->remove_prefix(8);
  return true;
}

// CRC-32C (Castagnoli), table driven.
uint32_t Crc32c(const char* data, size_t n);
inline uint32_t Crc32c(std::string_view s) { return Crc32c(s.data(), s.size()); }

// 64-bit FNV-1a followed by a murmur-style finalizer for good bit mixing.
uint64_t Hash64(std::string_view s);

}  // namespace lsmkv
