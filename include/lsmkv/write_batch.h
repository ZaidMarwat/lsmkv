#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

class MemTable;

// A WriteBatch groups updates that are applied atomically: after a crash,
// recovery replays either all of them or none of them (the batch is a single
// checksummed WAL record).
//
// Encoding (also the WAL record payload):
//   fixed64 sequence | fixed32 count | records...
//   record := kTypeValue    varstring(key) varstring(value)
//           | kTypeDeletion varstring(key)
class WriteBatch {
 public:
  WriteBatch();
  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();
  uint32_t Count() const;
  size_t ApproximateSize() const { return rep_.size(); }

 private:
  friend class WriteBatchInternal;
  std::string rep_;
};

class WriteBatchInternal {
 public:
  static void SetSequence(WriteBatch* b, uint64_t seq);
  static uint64_t Sequence(const WriteBatch* b);
  static const std::string& Contents(const WriteBatch* b) { return b->rep_; }
  static Status SetContents(WriteBatch* b, std::string_view contents);
  static Status InsertInto(const WriteBatch* b, MemTable* mem);
};

}  // namespace lsmkv
