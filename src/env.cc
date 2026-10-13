#include "env.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace lsmkv {

namespace {
Status PosixError(const std::string& ctx, int err) {
  return Status::IOError(ctx + ": " + std::strerror(err));
}
constexpr size_t kWriteBuffer = 64 * 1024;

std::string DirName(const std::string& path) {
  auto pos = path.rfind('/');
  return pos == std::string::npos ? "." : path.substr(0, pos);
}
}  // namespace

// ---------------- WritableFile ----------------

Status WritableFile::Open(const std::string& path, bool truncate, std::unique_ptr<WritableFile>* out) {
  int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND);
  int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) return PosixError(path, errno);
  struct stat st;
  uint64_t size = 0;
  if (!truncate && ::fstat(fd, &st) == 0) size = st.st_size;
  out->reset(new WritableFile(fd, path, size));
  return Status::OK();
}

WritableFile::~WritableFile() {
  if (fd_ >= 0) Close();
}

Status WritableFile::Append(std::string_view data) {
  size_ += data.size();
  if (buf_.size() + data.size() <= kWriteBuffer) {
    buf_.append(data.data(), data.size());
    return Status::OK();
  }
  Status s = Flush();
  if (!s.ok()) return s;
  if (data.size() <= kWriteBuffer) {
    buf_.append(data.data(), data.size());
    return Status::OK();
  }
  // Large write: bypass the buffer.
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_, errno);
    }
    p += n;
    left -= n;
  }
  return Status::OK();
}

Status WritableFile::Flush() {
  const char* p = buf_.data();
  size_t left = buf_.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_, errno);
    }
    p += n;
    left -= n;
  }
  buf_.clear();
  return Status::OK();
}

Status WritableFile::Sync() {
  Status s = Flush();
  if (!s.ok()) return s;
  if (::fsync(fd_) != 0) return PosixError(path_, errno);
  return Status::OK();
}

Status WritableFile::Close() {
  Status s = Flush();
  if (fd_ >= 0 && ::close(fd_) != 0 && s.ok()) s = PosixError(path_, errno);
  fd_ = -1;
  return s;
}

// ---------------- RandomAccessFile ----------------

Status RandomAccessFile::Open(const std::string& path, std::unique_ptr<RandomAccessFile>* out) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return PosixError(path, errno);
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    int e = errno;
    ::close(fd);
    return PosixError(path, e);
  }
  out->reset(new RandomAccessFile(fd, path, st.st_size));
  return Status::OK();
}

RandomAccessFile::~RandomAccessFile() {
  if (fd_ >= 0) ::close(fd_);
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, std::string* out) const {
  out->resize(n);
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd_, &(*out)[done], n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_, errno);
    }
    if (r == 0) return Status::Corruption(path_ + ": short read");
    done += r;
  }
  return Status::OK();
}

// ---------------- helpers ----------------

Status ReadFileToString(const std::string& path, std::string* out) {
  std::unique_ptr<RandomAccessFile> f;
  Status s = RandomAccessFile::Open(path, &f);
  if (!s.ok()) return s;
  if (f->Size() == 0) {
    out->clear();
    return Status::OK();
  }
  return f->Read(0, f->Size(), out);
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) return PosixError(dir, errno);
  ::fsync(fd);  // best effort: some filesystems reject fsync on directories
  ::close(fd);
  return Status::OK();
}

Status WriteFileAtomic(const std::string& path, std::string_view data) {
  std::string tmp = path + ".tmp";
  {
    std::unique_ptr<WritableFile> f;
    Status s = WritableFile::Open(tmp, /*truncate=*/true, &f);
    if (!s.ok()) return s;
    s = f->Append(data);
    if (s.ok()) s = f->Sync();
    if (s.ok()) s = f->Close();
    if (!s.ok()) return s;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) return PosixError(path, errno);
  return SyncDir(DirName(path));
}

Status ListDir(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (!d) return PosixError(dir, errno);
  while (struct dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n != "." && n != "..") names->push_back(n);
  }
  ::closedir(d);
  return Status::OK();
}

Status CreateDirIfMissing(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return PosixError(dir, errno);
  return Status::OK();
}

Status RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0) return PosixError(path, errno);
  return Status::OK();
}

bool FileExists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

Status FileLock::Acquire(const std::string& path, std::unique_ptr<FileLock>* out) {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return PosixError(path, errno);
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return Status::Busy("database is locked by another process: " + path);
  }
  out->reset(new FileLock(fd));
  return Status::OK();
}

FileLock::~FileLock() {
  ::flock(fd_, LOCK_UN);
  ::close(fd_);
}

}  // namespace lsmkv
