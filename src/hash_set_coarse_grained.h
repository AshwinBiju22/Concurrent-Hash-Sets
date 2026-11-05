#ifndef HASH_SET_COARSE_GRAINED_H
#define HASH_SET_COARSE_GRAINED_H

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "src/hash_set_base.h"

template <typename T>
class HashSetCoarseGrained : public HashSetBase<T> {
 public:
  explicit HashSetCoarseGrained(size_t initial_capacity)
      // Clamp to >= 1 bucket, since capacity 0 would make % table_.size()
      // divide by zero
      : table_(std::max<size_t>(initial_capacity, 1)), set_size_(0) {}

  bool Add(T elem) final {
    // Holds the global lock throughout insertion and any resizing
    // so no other thread can access the table while it is rehashed.
    // scoped_lock follows RAII which replaces the try/finally and
    // acquire/release blocks in the book
    std::scoped_lock lock(m_);

    const size_t my_bucket = GetBucketIndex(elem);
    auto& bucket = table_[my_bucket];

    if (std::find(bucket.begin(), bucket.end(), elem) != bucket.end()) {
      return false;
    }

    bucket.push_back(elem);
    set_size_++;

    // Unlike the textbook, this resizes while holding the global lock
    // Reentrancy not required so Resize() does not need to lock again
    if (Policy()) {
      Resize();
    }

    return true;
  }

  bool Remove(T elem) final {
    std::scoped_lock lock(m_);

    const size_t my_bucket = GetBucketIndex(elem);
    auto& bucket = table_[my_bucket];

    auto e = std::find(bucket.begin(), bucket.end(), elem);

    if (e == bucket.end()) {
      return false;
    }

    bucket.erase(e);
    set_size_--;
    return true;
  }

  [[nodiscard]] bool Contains(T elem) final {
    std::scoped_lock lock(m_);

    const size_t my_bucket = GetBucketIndex(elem);
    const auto& bucket = table_[my_bucket];

    return std::find(bucket.begin(), bucket.end(), elem) != bucket.end();
  }
  // Lock free atomic read avoids a mutable mutex in a const method
  [[nodiscard]] size_t Size() const final { return set_size_; }

 private:
  std::vector<std::vector<T>> table_;
  // Atomic only because Size() reads it without m_,  all writes happen under m_
  std::atomic<size_t> set_size_;
  // Single global mutex ensures that only one operation can access the set at a
  // time.
  std::mutex m_;

  [[nodiscard]] size_t GetBucketIndex(const T& elem) const {
    return std::hash<T>()(elem) % table_.size();
  }

  [[nodiscard]] bool Policy() const { return set_size_ / table_.size() > 4; }

  // This is called only while holding m_, ensuring exclusive access to table
  // whilst rehashing
  void Resize() {
    const size_t new_capacity = table_.size() * 2;
    std::vector<std::vector<T>> new_table(new_capacity);

    for (const auto& bucket : table_) {
      for (const auto& elem : bucket) {
        const size_t my_bucket = std::hash<T>()(elem) % new_capacity;
        new_table[my_bucket].push_back(elem);
      }
    }

    table_.swap(new_table);
  }
};

#endif  // HASH_SET_COARSE_GRAINED_H
