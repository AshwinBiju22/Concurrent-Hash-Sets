#ifndef HASH_SET_REFINABLE_H
#define HASH_SET_REFINABLE_H

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "src/hash_set_base.h"

template <typename T>
class HashSetRefinable : public HashSetBase<T> {
 public:
  explicit HashSetRefinable(size_t initial_capacity)
      // Clamp to >= 1 bucket, since capacity 0 would make % table_.size()
      // divide by zero
      : table_(std::max<size_t>(initial_capacity, 1)),
        locks_(MakeLocks(table_.size())),
        set_size_(0),
        resizing_(false) {}

  bool Add(T elem) final {
    const size_t hash = std::hash<T>()(elem);
    // Variable used to check the resize policy whilst holding the bucket lock
    // to avoid racing with concurrent resize
    bool should_resize = false;

    {
      auto guard = Acquire(hash);
      const size_t my_bucket = hash % table_.size();
      auto& bucket = table_[my_bucket];

      if (std::find(bucket.begin(), bucket.end(), elem) != bucket.end()) {
        return false;
      }

      bucket.push_back(elem);
      set_size_++;

      should_resize = Policy();
    }

    // Resize must happen after releasing the bucket lock,
    // otherwise the resizing thread could wait on its own lock
    if (should_resize) {
      Resize();
    }

    return true;
  }

  bool Remove(T elem) final {
    const size_t hash = std::hash<T>()(elem);
    auto guard = Acquire(hash);

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
    auto guard = Acquire(hash);

    const size_t my_bucket = hash % table_.size();
    const auto& bucket = table_[my_bucket];

    return std::find(bucket.begin(), bucket.end(), elem) != bucket.end();
  }

  [[nodiscard]] size_t Size() const final { return set_size_.load(); }

 private:
  using MutexArray = std::vector<std::unique_ptr<std::mutex>>;
  // Book uses a volatile array and relies on Java's GC to free old ones but C++
  // has no GC so shared_ptr reference counting helps here
  using MutexArrayPtr = std::shared_ptr<MutexArray>;

  // Keeps the lock array alive until its bucket lock is released
  struct BucketGuard {
    MutexArrayPtr mutexes;
    std::unique_lock<std::mutex> lock;
  };

  std::vector<std::vector<T>> table_;

  // Refinable locking replaces the lock array whenever table grows
  // Shared ownership keeps old locks alive while operations that
  // already captured them finish
  MutexArrayPtr locks_;

  // Different bucket locks allow concurrent modifications so atomic counter
  // needed
  std::atomic<size_t> set_size_;

  // Only one thread can resize at a time
  // Other operations wait until new table and lock array is ready
  // In the book the owner records the resizing thread because its resize() re
  // adds items via a initializeFrom() -> add() -> acquire() chain but ours
  // never calls Acquire(), so a bool suffices
  std::atomic<bool> resizing_;

  static MutexArrayPtr MakeLocks(size_t count) {
    auto mutexes = std::make_shared<MutexArray>();
    mutexes->reserve(count);

    for (size_t i = 0; i < count; ++i) {
      mutexes->push_back(std::make_unique<std::mutex>());
    }

    return mutexes;
  }

  [[nodiscard]] bool Policy() const {
    return set_size_.load() / table_.size() > 4;
  }

  BucketGuard Acquire(size_t hash) {
    while (true) {
      // Wait if another thread is resizing the table
      while (resizing_.load()) {
        std::this_thread::yield();
      }

      // Atomic load required as normal shared_ptr read would race with its
      // replacement in Resize()
      auto mutexes = std::atomic_load(&locks_);
      const size_t index = hash % mutexes->size();

      std::unique_lock<std::mutex> lock(*(*mutexes)[index]);

      // A resize could have happened while acquiring the mutex
      // Only proceed if this is still the current lock array
      if (!resizing_.load() && std::atomic_load(&locks_) == mutexes) {
        return BucketGuard{std::move(mutexes), std::move(lock)};
      }

      // Otherwise RAII releases the lock and we retry
    }
  }

  // Once resizing_ is true, new operations cannot successfully pass Acquire()
  // Book spins on lock.isLocked() but std::mutex has no isLocked(), so locking
  // and releasing each old mutex ensures previously admitted operations have
  // finished
  void Quiesce(const MutexArrayPtr& mutexes) {
    for (auto& mutex : *mutexes) {
      std::scoped_lock lock(*mutex);
    }
  }

  void Resize() {
    bool expected = false;

    // Only one thread can claim ownership of resizing
    if (!resizing_.compare_exchange_strong(expected, true)) {
      return;
    }

    try {
      auto old_mutexes = std::atomic_load(&locks_);

      // Ensure no existing operation is accessing the old table
      Quiesce(old_mutexes);

      // Another thread may have already resized the table
      if (Policy()) {
        const size_t new_capacity = table_.size() * 2;

        std::vector<std::vector<T>> new_table(new_capacity);
        auto new_mutexes = MakeLocks(new_capacity);

        for (const auto& bucket : table_) {
          for (const auto& elem : bucket) {
            const size_t my_bucket = std::hash<T>()(elem) % new_capacity;
            new_table[my_bucket].push_back(elem);
          }
        }

        // Updates table and locks before allowing operations to resume
        table_.swap(new_table);
        std::atomic_store(&locks_, new_mutexes);
      }
    } catch (...) {
      // Don't leave the set permanently blocked errors during resizing
      // C++ has no finally block like in the book so clear resizing_ here and
      // rethrow
      resizing_.store(false);
      throw;
    }

    resizing_.store(false);
  }
};

#endif  // HASH_SET_REFINABLE_H
