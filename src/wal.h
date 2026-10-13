#pragma once

// Write-ahead log.
//
// Record format:  fixed32 crc32c(len|payload) | fixed32 len | payload
//
// Every WriteBatch becomes exactly one record, so a batch is atomic under
// crashes. On replay a truncated or checksum-failing record marks the end of
// the durable prefix (a torn write at the tail); everything before it is
// recovered and everything after it is discarded.

#include <memory>
#include <string>
#include <string_view>

#include "env.h"

namespace lsmkv {

class LogWriter {
 public:
  explicit LogWriter(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}
  Status AddRecord(std::string_view payload, bool sync);
  Status Close() { return file_->Close(); }
  uint64_t Size() const { return file_->Size(); }

 private:
  std::unique_ptr<WritableFile> file_;
};

class LogReader {
 public:
  explicit LogReader(std::string contents) : data_(std::move(contents)), in_(data_) {}
  // Returns false at the end of the valid prefix.
  bool ReadRecord(std::string_view* record);
  // True if reading stopped because of a torn/corrupt record rather than clean EOF.
  bool hit_corruption() const { return hit_corruption_; }
  size_t dropped_bytes() const { return in_.size(); }

 private:
  std::string data_;
  std::string_view in_;
  bool hit_corruption_ = false;
};

}  // namespace lsmkv
