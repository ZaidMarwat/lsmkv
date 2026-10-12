#pragma once

// A skiplist that supports one writer and any number of concurrent,
// lock-free readers.
//
// Thread-safety contract (same as LevelDB's):
//   * Insert() must be externally synchronized (the DB mutex).
//   * Readers need no locks. Correctness relies on:
//       - nodes are never removed while the list is alive;
//       - a node is fully initialized before it is published with a
//         release-store into its predecessor's next pointer, and readers
//         follow pointers with acquire-loads.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#include "dbformat.h"

namespace lsmkv {

class SkipList {
 public:
  static constexpr int kMaxHeight = 12;

  struct Node {
    Node(std::string k, std::string v, int h)
        : key(std::move(k)), value(std::move(v)), height(h), next(new std::atomic<Node*>[h]) {
      for (int i = 0; i < h; i++) next[i].store(nullptr, std::memory_order_relaxed);
    }
    ~Node() { delete[] next; }
    Node* Next(int level) const { return next[level].load(std::memory_order_acquire); }
    void SetNext(int level, Node* n) { next[level].store(n, std::memory_order_release); }
    Node* NoBarrierNext(int level) const { return next[level].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int level, Node* n) { next[level].store(n, std::memory_order_relaxed); }

    const std::string key;  // internal key
    const std::string value;
    const int height;
    std::atomic<Node*>* next;
  };

  SkipList() : head_(new Node("", "", kMaxHeight)), max_height_(1), rng_(0xdeadbeef) {}

  ~SkipList() {
    Node* n = head_;
    while (n) {
      Node* next = n->NoBarrierNext(0);
      delete n;
      n = next;
    }
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: no entry with an equal internal key is present.
  void Insert(std::string key, std::string value) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || CompareInternalKeys(x->key, key) != 0);
    (void)x;
    int h = RandomHeight();
    int cur = max_height_.load(std::memory_order_relaxed);
    if (h > cur) {
      for (int i = cur; i < h; i++) prev[i] = head_;
      // Readers that observe the new height before the node is linked just
      // see head_->next == nullptr at those levels and drop down: harmless.
      max_height_.store(h, std::memory_order_relaxed);
    }
    Node* n = new Node(std::move(key), std::move(value), h);
    for (int i = 0; i < h; i++) {
      n->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
      prev[i]->SetNext(i, n);  // publish
    }
  }

  // First node with key >= target, or nullptr.
  Node* FindGreaterOrEqual(std::string_view target, Node** prev = nullptr) const {
    Node* x = head_;
    int level = max_height_.load(std::memory_order_relaxed) - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next != nullptr && CompareInternalKeys(next->key, target) < 0) {
        x = next;
      } else {
        if (prev) prev[level] = x;
        if (level == 0) return next;
        level--;
      }
    }
  }

  Node* First() const { return head_->Next(0); }

 private:
  int RandomHeight() {
    // Branching factor 4: P(height >= h) = 4^-(h-1).
    int h = 1;
    while (h < kMaxHeight && (rng_() & 3) == 0) h++;
    return h;
  }

  Node* const head_;
  std::atomic<int> max_height_;
  std::minstd_rand rng_;  // only touched by the (single) writer
};

}  // namespace lsmkv
