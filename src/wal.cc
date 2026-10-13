#include "wal.h"

#include "coding.h"

namespace lsmkv {

Status LogWriter::AddRecord(std::string_view payload, bool sync) {
  std::string header;
  std::string len;
  PutFixed32(&len, static_cast<uint32_t>(payload.size()));
  uint32_t crc = Crc32c(len + std::string(payload));
  PutFixed32(&header, crc);
  header += len;
  Status s = file_->Append(header);
  if (s.ok()) s = file_->Append(payload);
  if (s.ok()) s = sync ? file_->Sync() : file_->Flush();
  return s;
}

bool LogReader::ReadRecord(std::string_view* record) {
  if (in_.empty()) return false;
  if (in_.size() < 8) {
    hit_corruption_ = true;
    return false;
  }
  uint32_t crc = DecodeFixed32(in_.data());
  uint32_t len = DecodeFixed32(in_.data() + 4);
  if (in_.size() - 8 < len) {
    hit_corruption_ = true;  // torn write
    return false;
  }
  if (Crc32c(in_.data() + 4, 4 + static_cast<size_t>(len)) != crc) {
    hit_corruption_ = true;
    return false;
  }
  *record = in_.substr(8, len);
  in_.remove_prefix(8 + len);
  return true;
}

}  // namespace lsmkv
