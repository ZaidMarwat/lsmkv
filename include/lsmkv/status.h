#pragma once

#include <string>
#include <utility>

namespace lsmkv {

// Status is the error type returned by every fallible operation.
// It is cheap to copy in the common (OK) case: no heap allocation.
class Status {
 public:
  enum Code { kOk = 0, kNotFound, kCorruption, kIOError, kInvalidArgument, kBusy };

  Status() : code_(kOk) {}
  static Status OK() { return Status(); }
  static Status NotFound(std::string m = "") { return Status(kNotFound, std::move(m)); }
  static Status Corruption(std::string m) { return Status(kCorruption, std::move(m)); }
  static Status IOError(std::string m) { return Status(kIOError, std::move(m)); }
  static Status InvalidArgument(std::string m) { return Status(kInvalidArgument, std::move(m)); }
  static Status Busy(std::string m) { return Status(kBusy, std::move(m)); }

  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsIOError() const { return code_ == kIOError; }
  Code code() const { return code_; }

  std::string ToString() const {
    static const char* kNames[] = {"OK", "NotFound", "Corruption", "IOError",
                                   "InvalidArgument", "Busy"};
    std::string s = kNames[code_];
    if (!msg_.empty()) s += ": " + msg_;
    return s;
  }

 private:
  Status(Code c, std::string m) : code_(c), msg_(std::move(m)) {}
  Code code_;
  std::string msg_;
};

}  // namespace lsmkv
