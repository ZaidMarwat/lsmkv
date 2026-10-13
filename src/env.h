#pragma once

// Thin POSIX file layer. Kept separate so the engine could be ported or
// fault-injected without touching higher layers.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/status.h"

namespace lsmkv {

class WritableFile {
 public:
  static Status Open(const std::string& path, bool truncate, std::unique_ptr<WritableFile>* out);
  ~WritableFile();
  Status Append(std::string_view data);
  Status Flush();
  Status Sync();  // Flush + fsync
  Status Close();
  uint64_t Size() const { return size_; }

 private:
  WritableFile(int fd, std::string path, uint64_t size) : fd_(fd), path_(std::move(path)), size_(size) {}
  int fd_;
  std::string path_;
  std::string buf_;
  uint64_t size_;
};

class RandomAccessFile {
 public:
  static Status Open(const std::string& path, std::unique_ptr<RandomAccessFile>* out);
  ~RandomAccessFile();
  // Thread-safe positional read (pread).
  Status Read(uint64_t offset, size_t n, std::string* out) const;
  uint64_t Size() const { return size_; }

 private:
  RandomAccessFile(int fd, std::string path, uint64_t size)
      : fd_(fd), path_(std::move(path)), size_(size) {}
  int fd_;
  std::string path_;
  uint64_t size_;
};

Status ReadFileToString(const std::string& path, std::string* out);
// Atomically replace `path` with `data`: write temp file, fsync, rename, fsync dir.
Status WriteFileAtomic(const std::string& path, std::string_view data);
Status ListDir(const std::string& dir, std::vector<std::string>* names);
Status CreateDirIfMissing(const std::string& dir);
Status RemoveFile(const std::string& path);
Status SyncDir(const std::string& dir);
bool FileExists(const std::string& path);

// Exclusive advisory lock so two processes cannot open the same DB.
class FileLock {
 public:
  static Status Acquire(const std::string& path, std::unique_ptr<FileLock>* out);
  ~FileLock();

 private:
  explicit FileLock(int fd) : fd_(fd) {}
  int fd_;
};

}  // namespace lsmkv
