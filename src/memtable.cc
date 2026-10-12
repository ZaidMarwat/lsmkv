#include "memtable.h"

namespace lsmkv {

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  std::string ikey = MakeInternalKey(key, seq, type);
  // Node overhead estimate: key + value + pointers + bookkeeping.
  size_t charge = ikey.size() + value.size() + 64;
  list_.Insert(std::move(ikey), std::string(value));
  mem_usage_.fetch_add(charge, std::memory_order_relaxed);
  entries_.fetch_add(1, std::memory_order_relaxed);
}

bool MemTable::Get(std::string_view user_key, SequenceNumber snapshot, std::string* value,
                   bool* deleted) const {
  std::string lookup = MakeLookupKey(user_key, snapshot);
  SkipList::Node* n = list_.FindGreaterOrEqual(lookup);
  if (n == nullptr) return false;
  ParsedInternalKey p;
  if (!ParseInternalKey(n->key, &p) || p.user_key != user_key) return false;
  if (p.type == kTypeValue) {
    value->assign(n->value);
    *deleted = false;
  } else {
    *deleted = true;
  }
  return true;
}

namespace {

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(std::shared_ptr<const MemTable> mem) : mem_(std::move(mem)) {}
  bool Valid() const override { return node_ != nullptr; }
  void SeekToFirst() override { node_ = mem_->list().First(); }
  void Seek(std::string_view target) override { node_ = mem_->list().FindGreaterOrEqual(target); }
  void Next() override { node_ = node_->Next(0); }
  std::string_view key() const override { return node_->key; }
  std::string_view value() const override { return node_->value; }

 private:
  std::shared_ptr<const MemTable> mem_;
  SkipList::Node* node_ = nullptr;
};

}  // namespace

std::unique_ptr<Iterator> NewMemTableIterator(std::shared_ptr<const MemTable> mem) {
  return std::make_unique<MemTableIterator>(std::move(mem));
}

}  // namespace lsmkv
