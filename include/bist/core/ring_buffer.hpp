#pragma once
//
// bist/core/ring_buffer.hpp — single-producer single-consumer (SPSC) lock-free
// queue. Used for cross-thread handoff: hot thread <-> CLI / DropCopy / Logger.
//
// Design notes:
//   - Capacity must be a power of two; index masking is one AND.
//   - Head and tail counters are 64-bit; wrap is benign because we mask before
//     indexing the storage array.
//   - The two atomics live on separate cache lines to avoid producer/consumer
//     contention (false sharing).
//   - acquire/release ordering matches the standard SPSC pattern: writer
//     publishes payload before bumping head; reader sees payload because of
//     the matching acquire on tail's load of head.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <optional>
#include <utility>

namespace bist {

#if defined(__cpp_lib_hardware_interference_size) && __cpp_lib_hardware_interference_size >= 201703L
inline constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity > 0 && std::has_single_bit(Capacity),
                "SpscRing Capacity must be a power of two");

 public:
  using value_type = T;
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t kMask     = Capacity - 1;

  SpscRing() noexcept = default;
  SpscRing(const SpscRing&)            = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer-side: returns false if the queue is full (drop policy is the
  // caller's responsibility).
  template <typename U>
  [[nodiscard]] bool try_push(U&& v) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = head + 1;
    if (next - tail_.load(std::memory_order_acquire) > kCapacity) {
      return false;
    }
    storage_[head & kMask] = std::forward<U>(v);
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer-side: returns nullopt if the queue is empty.
  [[nodiscard]] std::optional<T> try_pop() noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    T v = std::move(storage_[tail & kMask]);
    tail_.store(tail + 1, std::memory_order_release);
    return v;
  }

  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t approx_size() const noexcept {
    const auto h = head_.load(std::memory_order_acquire);
    const auto t = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(h - t);
  }

 private:
  alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};
  alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
  alignas(kCacheLineSize) std::array<T, Capacity>    storage_{};
};

}  // namespace bist
