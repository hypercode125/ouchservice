#pragma once
//
// bist/domain/throttler.hpp — BIST rolling-window rate limiter for outbound orders.
//
// BIST enforces the order budget with 10 consecutive 100 ms sliding windows.
// A request is accepted only when the sum of the current and previous nine
// windows is still below the configured one-second limit.
//
// The throttler is single-threaded by construction (called only from the
// hot thread) and therefore uses plain integers rather than atomics.

#include <array>
#include <cstdint>
#include <limits>

#include "bist/core/time.hpp"

namespace bist::domain {

class Throttler {
 public:
  // capacity       : maximum local one-second budget
  // tokens_per_sec : configured BIST order-per-second budget
  Throttler(std::uint64_t capacity, std::uint64_t tokens_per_sec) noexcept
      : limit_(tokens_per_sec == 0 ? 0 : (capacity < tokens_per_sec ? capacity : tokens_per_sec)) {}

  // Returns true if the request fits inside the BIST rolling second.
  [[nodiscard]] bool try_acquire(std::uint64_t now_ns = 0) noexcept {
    if (limit_ == 0) return false;
    const std::uint64_t current = window_id(now_ns ? now_ns : monotonic_ns());
    Slot& slot = slots_[current % kWindowCount];
    if (slot.window_id != current) {
      slot.window_id = current;
      slot.count = 0;
    }
    if (used_in_rolling_second(current) >= limit_) return false;
    ++slot.count;
    return true;
  }

  // Reset the rolling counters; useful in tests and when recovering from a
  // session reset.
  void reset() noexcept {
    for (auto& slot : slots_) slot = Slot{};
  }

  [[nodiscard]] std::uint64_t available(std::uint64_t now_ns = 0) const noexcept {
    if (limit_ == 0) return 0;
    const std::uint64_t current = window_id(now_ns ? now_ns : monotonic_ns());
    const auto used = used_in_rolling_second(current);
    return used >= limit_ ? 0 : limit_ - used;
  }

  [[nodiscard]] std::uint64_t capacity() const noexcept { return limit_; }

 private:
  static constexpr std::uint64_t kWindowNs = 100'000'000ULL;
  static constexpr std::uint64_t kWindowCount = 10;
  static constexpr std::uint64_t kNoWindow =
      std::numeric_limits<std::uint64_t>::max();

  struct Slot {
    std::uint64_t window_id{kNoWindow};
    std::uint64_t count{0};
  };

  static std::uint64_t window_id(std::uint64_t now_ns) noexcept {
    return now_ns / kWindowNs;
  }

  [[nodiscard]] std::uint64_t used_in_rolling_second(std::uint64_t current) const noexcept {
    std::uint64_t total = 0;
    for (const auto& slot : slots_) {
      if (slot.window_id == kNoWindow) continue;
      if (slot.window_id > current) continue;
      if (current - slot.window_id >= kWindowCount) continue;
      total += slot.count;
    }
    return total;
  }

  std::uint64_t limit_;
  std::array<Slot, kWindowCount> slots_{};
};

}  // namespace bist::domain
