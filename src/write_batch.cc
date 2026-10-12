#include "lsmkv/write_batch.h"

#include "coding.h"
#include "dbformat.h"
#include "memtable.h"

namespace lsmkv {

static constexpr size_t kHeader = 12;  // fixed64 seq + fixed32 count

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data() + 8); }

static void SetCount(std::string* rep, uint32_t n) {
  std::string enc;
  PutFixed32(&enc, n);
  rep->replace(8, 4, enc);
}

void WriteBatch::Put(std::string_view key, std::string_view value) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixed(&rep_, key);
}

void WriteBatchInternal::SetSequence(WriteBatch* b, uint64_t seq) {
  std::string enc;
  PutFixed64(&enc, seq);
  b->rep_.replace(0, 8, enc);
}

uint64_t WriteBatchInternal::Sequence(const WriteBatch* b) { return DecodeFixed64(b->rep_.data()); }

Status WriteBatchInternal::SetContents(WriteBatch* b, std::string_view contents) {
  if (contents.size() < kHeader) return Status::Corruption("write batch too small");
  b->rep_.assign(contents.data(), contents.size());
  return Status::OK();
}

Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* mem) {
  std::string_view in(b->rep_);
  SequenceNumber seq = Sequence(b);
  uint32_t expected = b->Count();
  in.remove_prefix(kHeader);
  uint32_t found = 0;
  while (!in.empty()) {
    auto tag = static_cast<uint8_t>(in.front());
    in.remove_prefix(1);
    std::string_view key, value;
    if (!GetLengthPrefixed(&in, &key)) return Status::Corruption("bad batch key");
    if (tag == kTypeValue) {
      if (!GetLengthPrefixed(&in, &value)) return Status::Corruption("bad batch value");
      mem->Add(seq, kTypeValue, key, value);
    } else if (tag == kTypeDeletion) {
      mem->Add(seq, kTypeDeletion, key, {});
    } else {
      return Status::Corruption("unknown batch tag");
    }
    seq++;
    found++;
  }
  if (found != expected) return Status::Corruption("batch count mismatch");
  return Status::OK();
}

}  // namespace lsmkv
