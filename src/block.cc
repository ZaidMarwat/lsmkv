#include "block.h"

#include <algorithm>
#include <cassert>

#include "coding.h"

namespace lsmkv {

void BlockBuilder::Reset() {
  buf_.clear();
  restarts_.assign(1, 0);
  counter_ = 0;
  last_key_.clear();
  finished_ = false;
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  size_t shared = 0;
  if (counter_ < kRestartInterval) {
    size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) shared++;
  } else {
    restarts_.push_back(static_cast<uint32_t>(buf_.size()));
    counter_ = 0;
  }
  size_t non_shared = key.size() - shared;
  PutVarint32(&buf_, static_cast<uint32_t>(shared));
  PutVarint32(&buf_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buf_, static_cast<uint32_t>(value.size()));
  buf_.append(key.data() + shared, non_shared);
  buf_.append(value.data(), value.size());
  last_key_.assign(key.data(), key.size());
  counter_++;
}

std::string_view BlockBuilder::Finish() {
  for (uint32_t r : restarts_) PutFixed32(&buf_, r);
  PutFixed32(&buf_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buf_;
}

namespace {

class BlockIterator : public Iterator {
 public:
  BlockIterator(std::shared_ptr<const std::string> contents, KeyComparator cmp)
      : contents_(std::move(contents)), cmp_(cmp) {
    const std::string& d = *contents_;
    if (d.size() < 4) {
      corrupt_ = true;
      return;
    }
    num_restarts_ = DecodeFixed32(d.data() + d.size() - 4);
    size_t trailer = 4 + size_t(num_restarts_) * 4;
    if (trailer > d.size() || num_restarts_ == 0) {
      corrupt_ = true;
      return;
    }
    restarts_offset_ = static_cast<uint32_t>(d.size() - trailer);
  }

  bool Valid() const override { return valid_; }
  Status status() const override {
    return corrupt_ ? Status::Corruption("bad block") : Status::OK();
  }

  void SeekToFirst() override {
    if (corrupt_) return;
    next_ = 0;
    key_.clear();
    ParseNext();
  }

  void Seek(std::string_view target) override {
    if (corrupt_) return;
    // Binary search for the last restart point whose key < target.
    uint32_t left = 0, right = num_restarts_ - 1;
    while (left < right) {
      uint32_t mid = (left + right + 1) / 2;
      std::string_view mid_key;
      if (!KeyAtRestart(mid, &mid_key)) {
        corrupt_ = true;
        valid_ = false;
        return;
      }
      if (cmp_(mid_key, target) < 0) left = mid;
      else right = mid - 1;
    }
    next_ = RestartPoint(left);
    key_.clear();
    ParseNext();
    while (valid_ && cmp_(key_, target) < 0) ParseNext();
  }

  void Next() override { ParseNext(); }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }

 private:
  uint32_t RestartPoint(uint32_t i) const {
    return DecodeFixed32(contents_->data() + restarts_offset_ + i * 4);
  }

  bool KeyAtRestart(uint32_t i, std::string_view* key) const {
    std::string_view in(contents_->data() + RestartPoint(i), restarts_offset_ - RestartPoint(i));
    uint32_t shared, non_shared, vlen;
    if (!GetVarint32(&in, &shared) || !GetVarint32(&in, &non_shared) ||
        !GetVarint32(&in, &vlen) || shared != 0 || in.size() < non_shared)
      return false;
    *key = in.substr(0, non_shared);
    return true;
  }

  void ParseNext() {
    if (next_ >= restarts_offset_) {
      valid_ = false;
      return;
    }
    std::string_view in(contents_->data() + next_, restarts_offset_ - next_);
    uint32_t shared, non_shared, vlen;
    if (!GetVarint32(&in, &shared) || !GetVarint32(&in, &non_shared) ||
        !GetVarint32(&in, &vlen) || in.size() < size_t(non_shared) + vlen ||
        shared > key_.size()) {
      corrupt_ = true;
      valid_ = false;
      return;
    }
    key_.resize(shared);
    key_.append(in.data(), non_shared);
    value_ = in.substr(non_shared, vlen);
    next_ = static_cast<uint32_t>((in.data() + non_shared + vlen) - contents_->data());
    valid_ = true;
  }

  std::shared_ptr<const std::string> contents_;
  KeyComparator cmp_;
  uint32_t num_restarts_ = 0;
  uint32_t restarts_offset_ = 0;
  uint32_t next_ = 0;
  std::string key_;
  std::string_view value_;
  bool valid_ = false;
  bool corrupt_ = false;
};

}  // namespace

std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const std::string> contents,
                                           KeyComparator cmp) {
  return std::make_unique<BlockIterator>(std::move(contents), cmp);
}

}  // namespace lsmkv
