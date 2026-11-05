#ifndef HASH_SET_SEQUENTIAL_H
#define HASH_SET_SEQUENTIAL_H

#include <algorithm>
#include <functional>
#include <vector>

#include "src/hash_set_base.h"

template <typename T>
class HashSetSequential : public HashSetBase<T> {
 public:
  explicit HashSetSequential(size_t initial_capacity)
      // Ensuring that initial_capacity can't be 0
      : table_(std::max<size_t>(initial_capacity, 1)), set_size_(0) {}

  bool Add(T elem) final {
    const size_t my_bucket = GetBucketIndex(elem);
    auto& bucket = table_[my_bucket];

    if (std::find(bucket.begin(), bucket.end(), elem) != bucket.end()) {
      return false;
    }

    bucket.push_back(elem);
    set_size_++;

    if (Policy()) {
      Resize();
    }

    return true;
  }

  bool Remove(T elem) final {
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
    const size_t my_bucket = GetBucketIndex(elem);
    const auto& bucket = table_[my_bucket];

    return std::find(bucket.begin(), bucket.end(), elem) != bucket.end();
  }

  [[nodiscard]] size_t Size() const final { return set_size_; }

 private:
  // No synchronization is needed in a sequential set
  std::vector<std::vector<T>> table_;
  size_t set_size_;

  [[nodiscard]] size_t GetBucketIndex(const T& elem) const {
    return std::hash<T>()(elem) % table_.size();
  }

  // Resize when average bucket size exceeds 4
  [[nodiscard]] bool Policy() const { return set_size_ / table_.size() > 4; }

  void Resize() {
    const size_t new_capacity = table_.size() * 2;
    std::vector<std::vector<T>> new_table(new_capacity);

    for (const auto& bucket : table_) {
      for (const auto& elem : bucket) {
        // Bucket indices depend on capacity, so every elem
        // must be rehashed with new capacity
        const size_t my_bucket = std::hash<T>()(elem) % new_capacity;
        new_table[my_bucket].push_back(elem);
      }
    }

    table_.swap(new_table);
  }
};

#endif  // HASH_SET_SEQUENTIAL_H
