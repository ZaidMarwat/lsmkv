#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/iterator.h"
#include "lsmkv/status.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {

struct Options {
  // Memtable size before it is frozen and flushed to an L0 SSTable.
  size_t write_buffer_size = 4 << 20;
  // Uncompressed size of an SSTable data block.
  size_t block_size = 4096;
  // Bits per key in the per-table bloom filter (0 disables). 10 bits -> ~1% FPR.
  int bloom_bits_per_key = 10;
  // Capacity of the shared LRU block cache.
  size_t block_cache_bytes = 8 << 20;
  // Target size of SSTables produced by compaction.
  size_t target_file_size = 2 << 20;
  // Max bytes in L1; each deeper level is 10x larger.
  uint64_t level1_max_bytes = 10 << 20;
  // Number of L0 files that triggers an L0->L1 compaction.
  int l0_compaction_trigger = 4;
  // Number of L0 files at which writers stall until compaction catches up.
  int l0_stop_writes_trigger = 12;
  bool create_if_missing = true;
};

struct WriteOptions {
  // fsync the WAL before acknowledging the write.
  bool sync = false;
};

class Snapshot;

struct ReadOptions {
  // Read as of this snapshot; nullptr means "latest".
  const Snapshot* snapshot = nullptr;
};

struct DBStats {
  uint64_t user_bytes_written = 0;   // key+value bytes passed to Put/Delete
  uint64_t wal_bytes_written = 0;
  uint64_t sst_bytes_written = 0;    // flushes + compactions
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t trivial_moves = 0;
  uint64_t bloom_checks = 0;
  uint64_t bloom_negatives = 0;      // table reads avoided by the bloom filter
  uint64_t block_cache_hits = 0;
  uint64_t block_cache_misses = 0;
  uint64_t write_stalls = 0;
  std::vector<int> files_per_level;
  std::vector<uint64_t> bytes_per_level;

  double WriteAmplification() const {
    return user_bytes_written == 0
               ? 0.0
               : double(wal_bytes_written + sst_bytes_written) / double(user_bytes_written);
  }
};

// An embedded, persistent, ordered key-value store built on a
// log-structured merge tree. Thread-safe: any number of threads may call
// any method concurrently.
class DB {
 public:
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);
  virtual ~DB() = default;

  virtual Status Put(const WriteOptions& o, std::string_view key, std::string_view value) = 0;
  virtual Status Delete(const WriteOptions& o, std::string_view key) = 0;
  virtual Status Write(const WriteOptions& o, WriteBatch* batch) = 0;
  virtual Status Get(const ReadOptions& o, std::string_view key, std::string* value) = 0;

  // Iterator over user keys in ascending order, consistent as of the
  // snapshot (or the moment of creation).
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& o) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* s) = 0;

  // Force the active memtable to an SSTable and wait for it.
  virtual Status Flush() = 0;
  // Merge every file into a single sorted run at the bottom level.
  virtual Status CompactAll() = 0;
  // Block until no flush or compaction work is pending.
  virtual void WaitForIdle() = 0;

  virtual DBStats Stats() = 0;
  virtual std::string DebugString() = 0;
};

}  // namespace lsmkv
