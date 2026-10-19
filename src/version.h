#pragma once

// A Version is an immutable snapshot of which SSTables make up the tree.
// Readers grab a shared_ptr to the current Version and can then read
// without holding the DB mutex; flushes/compactions build a new Version and
// swap it in. A table file is unlinked only when (a) a compaction has made
// it obsolete AND (b) the last Version/iterator referencing it is gone,
// which is exactly when its FileMetaData destructor runs.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "cache.h"
#include "dbformat.h"
#include "env.h"
#include "table.h"

namespace lsmkv {

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // internal keys
  std::string largest;
  std::string path;
  std::shared_ptr<TableReader> table;
  BlockCache* cache = nullptr;
  std::atomic<bool> obsolete{false};

  std::string_view smallest_user() const { return ExtractUserKey(smallest); }
  std::string_view largest_user() const { return ExtractUserKey(largest); }

  ~FileMetaData() {
    table.reset();
    if (obsolete.load()) {
      RemoveFile(path);
      if (cache) cache->EraseFile(number);
    }
  }
};

using FilePtr = std::shared_ptr<FileMetaData>;

struct Version {
  // Level 0: newest file first; files may overlap.
  // Level >= 1: sorted by smallest key; files never overlap (by user key).
  std::vector<FilePtr> files[kNumLevels];

  uint64_t LevelBytes(int level) const {
    uint64_t b = 0;
    for (auto& f : files[level]) b += f->file_size;
    return b;
  }
};

}  // namespace lsmkv
