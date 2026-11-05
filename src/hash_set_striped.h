#ifndef HASH_SET_STRIPED_H
#define HASH_SET_STRIPED_H

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "src/hash_set_base.h"

template <typename T>
class HashSetStriped : public HashSetBase<T> {
 public:
  explicit HashSetStriped(size_t initial_capacity)
      // Ensuring that initial_capacity can't be 0
      : table_(std::max<size_t>(initial_capacity, 1)), set_size_(0) {
    for (size_t i = 0; i < table_.size(); ++i) {
      locks_.push_back(std::make_unique<std::mutex>());
    }
  }

  bool Add(T elem) final {
    const size_t hash = std::hash<T>()(elem);
    const size_t stripe = hash % locks_.size();

    bool should_resize = false;

    // Introduce new scope so stripe can be locked
    // and prevent deadlock when calling Resize()
    {
      // Lock only the relevant stripe, allowing operations on
      // different stripes to happen concurrently.
      std::scoped_lock lock(*locks_[stripe]);

      const size_t my_bucket = hash % table_.size();
      auto& bucket = table_[my_bucket];

      if (std::find(bucket.begin(), bucket.end(), elem) != bucket.end()) {
        return false;
      }

      bucket.push_back(elem);
      set_size_++;

      should_resize = set_size_.load() / table_.size() > 4;
    }

    // Release the stripe lock before resizing since Resize()
    // must acquire all stripe locks including this one so this prevents
    // potential deadlock
    if (should_resize) {
      Resize();
    }

    return true;
  }

  bool Remove(T elem) final {
    const size_t hash = std::hash<T>()(elem);
    const size_t stripe = hash % locks_.size();

    std::scoped_lock lock(*locks_[stripe]);

    const size_t my_bucket = hash % table_.size();
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
    const size_t hash = std::hash<T>()(elem);
    const size_t stripe = hash % locks_.size();

    std::scoped_lock lock(*locks_[stripe]);

    const size_t my_bucket = hash % table_.size();
    const auto& bucket = table_[my_bucket];

    return std::find(bucket.begin(), bucket.end(), elem) != bucket.end();
  }

  [[nodiscard]] size_t Size() const final { return set_size_.load(); }

 private:
  std::vector<std::vector<T>> table_;
  std::vector<std::unique_ptr<std::mutex>> locks_;
  // Atomic counter needed as different stripe locks
  // allow multiple threads to update the set simultaneously
  std::atomic<size_t> set_size_;

  void Resize() {
    // Acquire all stripe locks in a consistent order to prevent
    // concurrent bucket operations while the table is rehashed,
    // and prevents deadlock between competing resize operations
    std::vector<std::unique_lock<std::mutex>> guards;

    for (auto& mutex : locks_) {
      guards.emplace_back(*mutex);
    }

    // Another thread may have resized before we acquired all locks,
    // so recheck the policy to avoid unnecessary repeated resizing
    if (set_size_.load() / table_.size() <= 4) {
      return;
    }

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

#endif  // HASH_SET_STRIPED_H
