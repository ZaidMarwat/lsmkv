#pragma once

// SSTable data/index block.
//
// Keys are prefix-compressed against the previous key. Every
// kRestartInterval entries a "restart point" stores the full key, so Seek()
// can binary-search restart points and then scan at most 16 entries.
//
//   entry   := varint shared | varint non_shared | varint value_len
//              | key_delta[non_shared] | value[value_len]
//   trailer := fixed32 restart_offset[num_restarts] | fixed32 num_restarts

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/iterator.h"

namespace lsmkv {

class BlockBuilder {
 public:
  static constexpr int kRestartInterval = 16;
  BlockBuilder() { Reset(); }
  void Reset();
  // REQUIRES: key > every previously added key.
  void Add(std::string_view key, std::string_view value);
  std::string_view Finish();
  size_t CurrentSizeEstimate() const { return buf_.size() + restarts_.size() * 4 + 4; }
  bool empty() const { return buf_.empty(); }

 private:
  std::string buf_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  std::string last_key_;
  bool finished_ = false;
};

using KeyComparator = int (*)(std::string_view, std::string_view);

// Iterator over one block. Shares ownership of the block contents, so a
// block evicted from the cache stays alive while an iterator uses it.
std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const std::string> contents,
                                           KeyComparator cmp);

}  // namespace lsmkv
