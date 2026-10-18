#include "iterators.h"

#include <algorithm>

#include "dbformat.h"

namespace lsmkv {

namespace {

class MergingIterator : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return !heap_.empty(); }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    RebuildHeap();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    RebuildHeap();
  }

  void Next() override {
    Iterator* top = heap_.front();
    std::pop_heap(heap_.begin(), heap_.end(), Greater);
    heap_.pop_back();
    top->Next();
    if (top->Valid()) {
      heap_.push_back(top);
      std::push_heap(heap_.begin(), heap_.end(), Greater);
    }
  }

  std::string_view key() const override { return heap_.front()->key(); }
  std::string_view value() const override { return heap_.front()->value(); }

  Status status() const override {
    for (auto& c : children_)
      if (!c->status().ok()) return c->status();
    return Status::OK();
  }

 private:
  // std::*_heap builds a max-heap w.r.t. the comparator; "greater" => min-heap.
  static bool Greater(Iterator* a, Iterator* b) {
    return CompareInternalKeys(a->key(), b->key()) > 0;
  }

  void RebuildHeap() {
    heap_.clear();
    for (auto& c : children_)
      if (c->Valid()) heap_.push_back(c.get());
    std::make_heap(heap_.begin(), heap_.end(), Greater);
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<Iterator*> heap_;
};

class LevelIterator : public Iterator {
 public:
  explicit LevelIterator(std::vector<FilePtr> files) : files_(std::move(files)) {}

  bool Valid() const override { return cur_ && cur_->Valid(); }
  Status status() const override { return status_; }

  void SeekToFirst() override {
    Open(0);
    if (cur_) cur_->SeekToFirst();
    Skip();
  }

  void Seek(std::string_view target) override {
    // First file whose largest key >= target.
    auto it = std::lower_bound(files_.begin(), files_.end(), target,
                               [](const FilePtr& f, std::string_view t) {
                                 return CompareInternalKeys(f->largest, t) < 0;
                               });
    Open(it - files_.begin());
    if (cur_) cur_->Seek(target);
    Skip();
  }

  void Next() override {
    cur_->Next();
    Skip();
  }

  std::string_view key() const override { return cur_->key(); }
  std::string_view value() const override { return cur_->value(); }

 private:
  void Open(size_t idx) {
    idx_ = idx;
    cur_.reset();
    if (idx_ < files_.size()) cur_ = files_[idx_]->table->NewIterator(files_[idx_]->table);
  }

  void Skip() {
    while (cur_ && !cur_->Valid()) {
      if (!cur_->status().ok()) {
        status_ = cur_->status();
        cur_.reset();
        return;
      }
      Open(idx_ + 1);
      if (cur_) cur_->SeekToFirst();
    }
  }

  std::vector<FilePtr> files_;
  size_t idx_ = 0;
  std::unique_ptr<Iterator> cur_;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

std::unique_ptr<Iterator> NewLevelIterator(std::vector<FilePtr> files) {
  return std::make_unique<LevelIterator>(std::move(files));
}

}  // namespace lsmkv
