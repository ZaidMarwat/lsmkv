#include "table.h"

#include "bloom.h"
#include "coding.h"
#include "dbformat.h"

namespace lsmkv {

namespace {
constexpr uint64_t kTableMagic = 0x4c534d4b56544231ull;  // "LSMKVTB1"
constexpr size_t kFooterSize = 40;

int InternalCmp(std::string_view a, std::string_view b) { return CompareInternalKeys(a, b); }

void EncodeHandle(std::string* dst, uint64_t off, uint64_t size) {
  PutFixed64(dst, off);
  PutFixed64(dst, size);
}

bool DecodeHandle(std::string_view v, uint64_t* off, uint64_t* size) {
  if (v.size() != 16) return false;
  *off = DecodeFixed64(v.data());
  *size = DecodeFixed64(v.data() + 8);
  return true;
}
}  // namespace

// ---------------- TableBuilder ----------------

TableBuilder::TableBuilder(std::unique_ptr<WritableFile> file, size_t block_size,
                           int bloom_bits_per_key)
    : file_(std::move(file)),
      block_size_(block_size),
      bloom_bits_(bloom_bits_per_key),
      bloom_(new BloomFilterBuilder(bloom_bits_per_key > 0 ? bloom_bits_per_key : 1)) {}

Status TableBuilder::Add(std::string_view ikey, std::string_view value) {
  if (num_entries_ == 0) smallest_.assign(ikey.data(), ikey.size());
  largest_.assign(ikey.data(), ikey.size());
  last_key_ = largest_;
  data_block_.Add(ikey, value);
  if (bloom_bits_ > 0) bloom_->AddKey(ExtractUserKey(ikey));
  num_entries_++;
  if (data_block_.CurrentSizeEstimate() >= block_size_) return FlushDataBlock();
  return Status::OK();
}

Status TableBuilder::WriteRawBlock(std::string_view contents, uint64_t* offset, uint64_t* size) {
  *offset = offset_;
  *size = contents.size();
  std::string trailer;
  PutFixed32(&trailer, Crc32c(contents));
  Status s = file_->Append(contents);
  if (s.ok()) s = file_->Append(trailer);
  offset_ += contents.size() + 4;
  return s;
}

Status TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return Status::OK();
  uint64_t off, size;
  Status s = WriteRawBlock(data_block_.Finish(), &off, &size);
  data_block_.Reset();
  std::string handle;
  EncodeHandle(&handle, off, size);
  // Separator = last key in the block: the first block whose separator is
  // >= the lookup key is the only block that can contain it.
  index_block_.Add(last_key_, handle);
  return s;
}

Status TableBuilder::Finish() {
  Status s = FlushDataBlock();
  if (!s.ok()) return s;
  uint64_t filter_off = 0, filter_size = 0, index_off, index_size;
  std::string filter = bloom_bits_ > 0 ? bloom_->Finish() : std::string();
  s = WriteRawBlock(filter, &filter_off, &filter_size);
  if (!s.ok()) return s;
  s = WriteRawBlock(index_block_.Finish(), &index_off, &index_size);
  if (!s.ok()) return s;
  std::string footer;
  PutFixed64(&footer, filter_off);
  PutFixed64(&footer, filter_size);
  PutFixed64(&footer, index_off);
  PutFixed64(&footer, index_size);
  PutFixed64(&footer, kTableMagic);
  s = file_->Append(footer);
  offset_ += footer.size();
  if (s.ok()) s = file_->Sync();
  if (s.ok()) s = file_->Close();
  return s;
}

// ---------------- TableReader ----------------

Status TableReader::Open(const std::string& path, uint64_t file_number, BlockCache* cache,
                         std::shared_ptr<TableReader>* out) {
  std::shared_ptr<TableReader> t(new TableReader());
  Status s = RandomAccessFile::Open(path, &t->file_);
  if (!s.ok()) return s;
  t->file_number_ = file_number;
  t->cache_ = cache;
  if (t->file_->Size() < kFooterSize) return Status::Corruption(path + ": file too short");
  std::string footer;
  s = t->file_->Read(t->file_->Size() - kFooterSize, kFooterSize, &footer);
  if (!s.ok()) return s;
  if (DecodeFixed64(footer.data() + 32) != kTableMagic)
    return Status::Corruption(path + ": bad magic");
  uint64_t filter_off = DecodeFixed64(footer.data());
  uint64_t filter_size = DecodeFixed64(footer.data() + 8);
  uint64_t index_off = DecodeFixed64(footer.data() + 16);
  uint64_t index_size = DecodeFixed64(footer.data() + 24);

  std::shared_ptr<const std::string> filter;
  s = t->ReadBlock(filter_off, filter_size, &filter);
  if (!s.ok()) return s;
  t->filter_ = *filter;
  s = t->ReadBlock(index_off, index_size, &t->index_);
  if (!s.ok()) return s;
  *out = std::move(t);
  return Status::OK();
}

Status TableReader::ReadBlock(uint64_t offset, uint64_t size,
                              std::shared_ptr<const std::string>* out) const {
  if (cache_) {
    if (auto hit = cache_->Lookup(file_number_, offset)) {
      *out = std::move(hit);
      return Status::OK();
    }
  }
  std::string raw;
  Status s = file_->Read(offset, size + 4, &raw);
  if (!s.ok()) return s;
  uint32_t expected = DecodeFixed32(raw.data() + size);
  if (Crc32c(raw.data(), size) != expected)
    return Status::Corruption("block checksum mismatch in table " + std::to_string(file_number_));
  raw.resize(size);
  auto block = std::make_shared<const std::string>(std::move(raw));
  if (cache_) cache_->Insert(file_number_, offset, block);
  *out = std::move(block);
  return Status::OK();
}

Status TableReader::Get(std::string_view lookup_key, bool* found, std::string* ikey_out,
                        std::string* value_out, bool* bloom_negative) const {
  *found = false;
  *bloom_negative = false;
  std::string_view user_key = ExtractUserKey(lookup_key);
  if (!BloomMayContain(filter_, user_key)) {
    *bloom_negative = true;
    return Status::OK();
  }
  auto index_it = NewBlockIterator(index_, InternalCmp);
  index_it->Seek(lookup_key);
  if (!index_it->Valid()) return index_it->status();
  uint64_t off, size;
  if (!DecodeHandle(index_it->value(), &off, &size)) return Status::Corruption("bad block handle");
  std::shared_ptr<const std::string> block;
  Status s = ReadBlock(off, size, &block);
  if (!s.ok()) return s;
  auto it = NewBlockIterator(block, InternalCmp);
  it->Seek(lookup_key);
  if (it->Valid() && ExtractUserKey(it->key()) == user_key) {
    *found = true;
    ikey_out->assign(it->key());
    value_out->assign(it->value());
  }
  return it->status();
}

// Two-level iterator: walks the index block, opening data blocks lazily.
class TableIterator : public Iterator {
 public:
  explicit TableIterator(std::shared_ptr<const TableReader> t)
      : table_(std::move(t)), index_(NewBlockIterator(table_->index_, InternalCmp)) {}

  bool Valid() const override { return data_ && data_->Valid(); }
  Status status() const override { return status_; }

  void SeekToFirst() override {
    index_->SeekToFirst();
    InitDataBlock();
    if (data_) data_->SeekToFirst();
    SkipEmptyBlocks();
  }

  void Seek(std::string_view target) override {
    index_->Seek(target);
    InitDataBlock();
    if (data_) data_->Seek(target);
    SkipEmptyBlocks();
  }

  void Next() override {
    data_->Next();
    SkipEmptyBlocks();
  }

  std::string_view key() const override { return data_->key(); }
  std::string_view value() const override { return data_->value(); }

 private:
  void InitDataBlock() {
    data_.reset();
    if (!index_->Valid()) return;
    uint64_t off, size;
    if (!DecodeHandle(index_->value(), &off, &size)) {
      status_ = Status::Corruption("bad block handle");
      return;
    }
    std::shared_ptr<const std::string> block;
    Status s = table_->ReadBlock(off, size, &block);
    if (!s.ok()) {
      status_ = s;
      return;
    }
    data_ = NewBlockIterator(block, InternalCmp);
  }

  void SkipEmptyBlocks() {
    while (data_ && !data_->Valid()) {
      if (!data_->status().ok()) {
        status_ = data_->status();
        data_.reset();
        return;
      }
      index_->Next();
      InitDataBlock();
      if (data_) data_->SeekToFirst();
    }
  }

  std::shared_ptr<const TableReader> table_;
  std::unique_ptr<Iterator> index_;
  std::unique_ptr<Iterator> data_;
  Status status_;
};

std::unique_ptr<Iterator> TableReader::NewIterator(std::shared_ptr<const TableReader> self) const {
  return std::make_unique<TableIterator>(std::move(self));
}

}  // namespace lsmkv
