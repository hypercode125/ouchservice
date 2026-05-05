#pragma once
//
// bist/app/reconnect_policy.hpp — CoD-aware reconnect deadline.
//
// BIST Cancel-on-Disconnect (CoD) annotation, April 2026: when the OUCH
// session is lost the matching engine starts a 55-62 s inactivation timer.
// All resting orders that were sent under the lost session are inactivated
// when the timer fires unless the same client logs back in first.
//
// This policy lets the operator bound the reconnect budget at a value below
// that window (default 30 s). Once the budget is exhausted the caller must
// either escalate to the secondary gateway, surface a "CoD risk" audit
// record, or both.

#include <cstdint>
#include <string>

#include "bist/core/result.hpp"
#include "bist/core/time.hpp"

namespace bist::app {

struct ReconnectPolicy {
  // Total reconnect budget, monotonic, hard-bounded.
  std::uint32_t deadline_ms{30'000};
  // Initial backoff between attempts. Doubled per failed attempt up to max.
  std::uint32_t initial_backoff_ms{500};
  std::uint32_t max_backoff_ms{15'000};

  // Default constructor matches the cert-day recommendation; callers may
  // shorten the deadline in production-like profiles.
  constexpr ReconnectPolicy() noexcept = default;
  constexpr ReconnectPolicy(std::uint32_t deadline_ms_,
                            std::uint32_t initial_backoff_ms_ = 500,
                            std::uint32_t max_backoff_ms_     = 15'000) noexcept
      : deadline_ms(deadline_ms_),
        initial_backoff_ms(initial_backoff_ms_),
        max_backoff_ms(max_backoff_ms_) {}
};

// State carried alongside an active reconnect attempt. Exposed so the
// caller can stamp audit records and decide whether the order book has
// entered the BIST CoD inactivation window.
struct ReconnectState {
  TimestampNs    started_ns{0};       // monotonic_ns at first attempt
  std::uint32_t  attempts{0};
  bool           cod_risk{false};     // true when deadline expired
};

// Thin helper used by tests and run_live to drive a deadline-bounded
// reconnect loop. The actual TCP work is supplied as a callable so that
// network code stays in apps/.
//
// `try_connect` should return a successful Result on a clean connection
// and a Result with an Error otherwise. The function returns:
//   - Success on first successful connect
//   - Timeout error if the deadline elapses
//   - The last seen connect error otherwise
template <typename Connect>
Result<void> reconnect_with_deadline(Connect&& try_connect,
                                     const ReconnectPolicy& policy,
                                     ReconnectState& state) {
  state.started_ns = monotonic_ns();
  state.attempts   = 0;
  state.cod_risk   = false;

  std::uint32_t backoff = policy.initial_backoff_ms;
  Error         last_err{};
  const auto    deadline_ns = state.started_ns +
                              static_cast<TimestampNs>(policy.deadline_ms) * 1'000'000ULL;

  while (true) {
    ++state.attempts;
    auto r = try_connect(state.attempts);
    if (r) return {};
    last_err = r.error();

    const auto now_ns = monotonic_ns();
    if (now_ns >= deadline_ns) {
      state.cod_risk = true;
      return make_error(ErrorCategory::Timeout,
                        "CoD reconnect deadline elapsed after " +
                        std::to_string(state.attempts) + " attempt(s); last: " +
                        last_err.detail);
    }

    // Sleep up to the next attempt, but never past the deadline.
    const auto remaining_ns = deadline_ns - now_ns;
    const auto step_ns = static_cast<TimestampNs>(backoff) * 1'000'000ULL;
    const auto sleep_ns = step_ns < remaining_ns ? step_ns : remaining_ns;

    // Inline sleep_for to keep this header free of <thread>; callers that
    // run unit tests can avoid the wall-clock cost by using a tiny
    // deadline.
    if (sleep_ns > 0) {
      // Spin until monotonic clock advances. This is intentionally a busy
      // wait — production callers use the synchronous TCP loop in
      // apps/bist_colo.cpp which provides its own sleep.
      const auto wake_at = now_ns + sleep_ns;
      while (monotonic_ns() < wake_at) {
        // Yield to scheduler so the test harness stays responsive.
#if defined(__linux__) || defined(__APPLE__)
        __asm__ __volatile__("");
#endif
      }
    }
    backoff = backoff * 2u;
    if (backoff > policy.max_backoff_ms) backoff = policy.max_backoff_ms;
  }
}

}  // namespace bist::app
