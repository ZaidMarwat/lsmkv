#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "dbformat.h"
#include "lsmkv/iterator.h"
#include "skiplist.h"

namespace lsmkv {

// In-memory write buffer. Entries are keyed by internal key so multiple
// versions of a user key coexist; snapshots pick the right one.
class MemTable {
 public:
  MemTable() = default;

  // REQUIRES: externally synchronized with other Add() calls.
  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // Returns true if the memtable holds a version of `user_key` visible at
  // `snapshot`. On a live value, *value is filled and *deleted=false; on a
  // tombstone, *deleted=true.
  bool Get(std::string_view user_key, SequenceNumber snapshot, std::string* value,
           bool* deleted) const;

  size_t ApproximateMemoryUsage() const { return mem_usage_.load(std::memory_order_relaxed); }
  size_t NumEntries() const { return entries_.load(std::memory_order_relaxed); }

  const SkipList& list() const { return list_; }

 private:
  SkipList list_;
  std::atomic<size_t> mem_usage_{0};
  std::atomic<size_t> entries_{0};
};

// Iterator over internal keys. Holds a reference so the memtable outlives it.
std::unique_ptr<Iterator> NewMemTableIterator(std::shared_ptr<const MemTable> mem);

}  // namespace lsmkv
