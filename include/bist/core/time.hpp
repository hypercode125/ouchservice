#pragma once
//
// bist/core/time.hpp — monotonic + wall clocks at nanosecond resolution.
//
// OUCH outbound messages carry a wall-clock UNIX nanosecond timestamp. We
// expose two distinct functions to keep that distinction visible at call
// sites: monotonic_ns() is for measuring intervals (latency, heartbeat
// timers); wall_ns() for stamping outbound or audit-log records.

#include <chrono>

#include "bist/core/types.hpp"

namespace bist {

[[nodiscard]] inline TimestampNs monotonic_ns() noexcept {
  return static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

[[nodiscard]] inline TimestampNs wall_ns() noexcept {
  return static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace bist
