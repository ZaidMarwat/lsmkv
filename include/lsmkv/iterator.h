#pragma once

#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

// Forward iterator over a sorted sequence of key/value pairs.
// The same interface is used internally (over internal keys) and
// externally (over user keys, via DB::NewIterator).
class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Position at the first entry with key >= target.
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  // REQUIRES: Valid(). Returned views are valid until the next mutation of the iterator.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual Status status() const { return Status::OK(); }
};

}  // namespace lsmkv
