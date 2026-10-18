#pragma once

#include <memory>
#include <vector>

#include "lsmkv/iterator.h"
#include "version.h"

namespace lsmkv {

// K-way merge of sorted internal-key iterators, backed by a binary min-heap:
// O(log k) per Next(). Duplicate internal keys cannot occur (sequence
// numbers are unique), so no tie-breaking is needed.
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

// Concatenating iterator over the non-overlapping, sorted files of one
// level >= 1. Opens one table at a time, so a level with 1000 files costs
// one child in the merge instead of 1000.
std::unique_ptr<Iterator> NewLevelIterator(std::vector<FilePtr> files);

}  // namespace lsmkv
