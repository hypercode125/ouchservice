// tests/unit/test_reconnect_policy.cpp — CoD-aware reconnect deadline.
//
// Drive `reconnect_with_deadline` with a synthetic try_connect that lets
// us assert: success short-circuits, deadline expiry returns Timeout +
// flips cod_risk, the deadline applies across multiple backoff attempts.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

#include "bist/app/reconnect_policy.hpp"
#include "bist/core/result.hpp"

using bist::app::ReconnectPolicy;
using bist::app::ReconnectState;
using bist::app::reconnect_with_deadline;

TEST(ReconnectPolicy, SuccessShortCircuits) {
  ReconnectPolicy policy{1'000, /*initial_backoff_ms=*/10};
  ReconnectState  state{};
  std::atomic<int> calls{0};

  auto try_connect = [&](std::uint32_t /*attempt*/) -> bist::Result<void> {
    ++calls;
    return {};  // success on first try
  };
  auto r = reconnect_with_deadline(try_connect, policy, state);
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(state.attempts, 1u);
  EXPECT_FALSE(state.cod_risk);
}

TEST(ReconnectPolicy, DeadlineExpirySetsCodRiskAndReturnsTimeout) {
  ReconnectPolicy policy{/*deadline_ms=*/50, /*initial_backoff_ms=*/5,
                         /*max_backoff_ms=*/5};
  ReconnectState  state{};

  auto try_connect = [&](std::uint32_t /*attempt*/) -> bist::Result<void> {
    return bist::make_error(bist::ErrorCategory::Io, "refused");
  };
  auto r = reconnect_with_deadline(try_connect, policy, state);
  ASSERT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.error().category, bist::ErrorCategory::Timeout);
  EXPECT_TRUE(state.cod_risk);
  EXPECT_GE(state.attempts, 2u);  // at least 2 tries inside 50 ms
}

TEST(ReconnectPolicy, EventualSuccessClearsRisk) {
  ReconnectPolicy policy{/*deadline_ms=*/200, /*initial_backoff_ms=*/5,
                         /*max_backoff_ms=*/5};
  ReconnectState  state{};
  std::atomic<int> calls{0};

  auto try_connect = [&](std::uint32_t /*attempt*/) -> bist::Result<void> {
    if (++calls < 3) {
      return bist::make_error(bist::ErrorCategory::Io, "refused");
    }
    return {};
  };
  auto r = reconnect_with_deadline(try_connect, policy, state);
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(state.attempts, 3u);
  EXPECT_FALSE(state.cod_risk);
}

TEST(ReconnectPolicy, DeadlineRespectedAcrossMultipleBackoffSteps) {
  ReconnectPolicy policy{/*deadline_ms=*/30, /*initial_backoff_ms=*/2,
                         /*max_backoff_ms=*/8};
  ReconnectState  state{};
  auto try_connect = [&](std::uint32_t /*attempt*/) -> bist::Result<void> {
    return bist::make_error(bist::ErrorCategory::Io, "refused");
  };
  const auto t0 = std::chrono::steady_clock::now();
  auto r = reconnect_with_deadline(try_connect, policy, state);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.error().category, bist::ErrorCategory::Timeout);
  // Deadline 30 ms — actual loop never spends more than ~50 ms incl. one
  // backoff overshoot. If we crossed the BIST CoD window we'd be in trouble.
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 200);
}
