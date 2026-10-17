#pragma once

// Thread-safe LRU cache for decoded SSTable blocks, bounded by bytes.
// Values are shared_ptr, so an evicted block stays valid for any iterator
// still reading it.

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lsmkv {

class BlockCache {
 public:
  using Value = std::shared_ptr<const std::string>;

  explicit BlockCache(size_t capacity_bytes) : capacity_(capacity_bytes) {}

  Value Lookup(uint64_t file, uint64_t offset) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = map_.find(Key{file, offset});
    if (it == map_.end()) {
      misses_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);  // move to front
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second->value;
  }

  void Insert(uint64_t file, uint64_t offset, Value v) {
    if (capacity_ == 0) return;
    std::lock_guard<std::mutex> l(mu_);
    Key k{file, offset};
    auto it = map_.find(k);
    if (it != map_.end()) {
      usage_ -= it->second->value->size();
      lru_.erase(it->second);
      map_.erase(it);
    }
    usage_ += v->size();
    lru_.push_front(Entry{k, std::move(v)});
    map_[k] = lru_.begin();
    while (usage_ > capacity_ && !lru_.empty()) {
      auto& victim = lru_.back();
      usage_ -= victim.value->size();
      map_.erase(victim.key);
      lru_.pop_back();
    }
  }

  // Drop every block from a file that has been deleted.
  void EraseFile(uint64_t file) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = lru_.begin(); it != lru_.end();) {
      if (it->key.file == file) {
        usage_ -= it->value->size();
        map_.erase(it->key);
        it = lru_.erase(it);
      } else {
        ++it;
      }
    }
  }

  uint64_t hits() const { return hits_.load(); }
  uint64_t misses() const { return misses_.load(); }

 private:
  struct Key {
    uint64_t file, offset;
    bool operator==(const Key& o) const { return file == o.file && offset == o.offset; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<uint64_t>()(k.file * 0x9E3779B97F4A7C15ull ^ k.offset);
    }
  };
  struct Entry {
    Key key;
    Value value;
  };

  std::mutex mu_;
  size_t capacity_;
  size_t usage_ = 0;
  std::list<Entry> lru_;
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map_;
  std::atomic<uint64_t> hits_{0}, misses_{0};
};

}  // namespace lsmkv
