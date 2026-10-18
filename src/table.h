#pragma once

// Sorted String Table (SSTable): an immutable, sorted file of internal keys.
//
//   [data block 0 | crc32] ... [data block N | crc32]
//   [filter (bloom over user keys) | crc32]
//   [index block | crc32]        index: last_key_of_block -> (offset, size)
//   [footer: fixed64 filter_off, filter_size, index_off, index_size, magic]
//
// Readers keep the index and filter in memory; data blocks go through the
// shared LRU BlockCache. Every block is checksummed and verified on read.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "block.h"
#include "bloom.h"
#include "cache.h"
#include "env.h"
#include "lsmkv/iterator.h"

namespace lsmkv {

class TableBuilder {
 public:
  TableBuilder(std::unique_ptr<WritableFile> file, size_t block_size, int bloom_bits_per_key);
  // REQUIRES: ikey sorts after every previously added key.
  Status Add(std::string_view ikey, std::string_view value);
  Status Finish();  // writes filter, index, footer and fsyncs
  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return offset_; }
  uint64_t EstimatedSize() const { return offset_ + data_block_.CurrentSizeEstimate(); }
  const std::string& smallest() const { return smallest_; }
  const std::string& largest() const { return largest_; }

 private:
  Status FlushDataBlock();
  Status WriteRawBlock(std::string_view contents, uint64_t* offset, uint64_t* size);

  std::unique_ptr<WritableFile> file_;
  size_t block_size_;
  int bloom_bits_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::unique_ptr<BloomFilterBuilder> bloom_;
  uint64_t offset_ = 0;
  uint64_t num_entries_ = 0;
  std::string smallest_, largest_;
  std::string last_key_;
};

class TableReader {
 public:
  static Status Open(const std::string& path, uint64_t file_number, BlockCache* cache,
                     std::shared_ptr<TableReader>* out);

  // Point lookup. Returns OK + *found=false when the table has no visible
  // version. When found, *ikey_out/*value_out hold the newest version with
  // sequence <= the one encoded in `lookup_key`.
  Status Get(std::string_view lookup_key, bool* found, std::string* ikey_out,
             std::string* value_out, bool* bloom_negative) const;

  std::unique_ptr<Iterator> NewIterator(std::shared_ptr<const TableReader> self) const;

  uint64_t file_number() const { return file_number_; }

 private:
  friend class TableIterator;
  TableReader() = default;
  Status ReadBlock(uint64_t offset, uint64_t size, std::shared_ptr<const std::string>* out) const;

  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_number_ = 0;
  BlockCache* cache_ = nullptr;
  std::shared_ptr<const std::string> index_;
  std::string filter_;
};

}  // namespace lsmkv
