#pragma once

// Internal key format shared by the memtable, SSTables and compaction.
//
//   internal_key := user_key | fixed64( sequence << 8 | type )
//
// Ordering: user_key ascending, then sequence DESCENDING, so that for a
// given user key the newest version is encountered first. This is what
// makes MVCC snapshots and "newest version wins" merges cheap.

#include <cstdint>
#include <string>
#include <string_view>

#include "coding.h"

namespace lsmkv {

enum ValueType : uint8_t { kTypeDeletion = 0, kTypeValue = 1 };

using SequenceNumber = uint64_t;
constexpr SequenceNumber kMaxSequenceNumber = (1ull << 56) - 1;
constexpr int kNumLevels = 7;

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  return (seq << 8) | t;
}

inline void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                              ValueType t) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackSequenceAndType(seq, t));
}

inline std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq, ValueType t) {
  std::string s;
  AppendInternalKey(&s, user_key, seq, t);
  return s;
}

inline std::string_view ExtractUserKey(std::string_view ikey) {
  return ikey.substr(0, ikey.size() - 8);
}

inline uint64_t ExtractTag(std::string_view ikey) {
  return DecodeFixed64(ikey.data() + ikey.size() - 8);
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence;
  ValueType type;
};

inline bool ParseInternalKey(std::string_view ikey, ParsedInternalKey* out) {
  if (ikey.size() < 8) return false;
  uint64_t tag = ExtractTag(ikey);
  uint8_t t = tag & 0xff;
  if (t > kTypeValue) return false;
  out->user_key = ExtractUserKey(ikey);
  out->sequence = tag >> 8;
  out->type = static_cast<ValueType>(t);
  return true;
}

inline int CompareUserKeys(std::string_view a, std::string_view b) {
  int r = a.compare(b);
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

inline int CompareInternalKeys(std::string_view a, std::string_view b) {
  int r = CompareUserKeys(ExtractUserKey(a), ExtractUserKey(b));
  if (r != 0) return r;
  uint64_t ta = ExtractTag(a), tb = ExtractTag(b);
  if (ta > tb) return -1;  // higher sequence sorts first
  if (ta < tb) return 1;
  return 0;
}

// Key used to look up `user_key` as of `snapshot`: it sorts before every
// version of user_key with sequence <= snapshot.
inline std::string MakeLookupKey(std::string_view user_key, SequenceNumber snapshot) {
  return MakeInternalKey(user_key, snapshot, kTypeValue);
}

}  // namespace lsmkv
