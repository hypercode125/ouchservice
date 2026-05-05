// tests/unit/test_throttler.cpp — BIST 10 x 100 ms rolling-window behaviour.

#include <gtest/gtest.h>

#include "bist/domain/throttler.hpp"

TEST(Throttler, BurstUpToCapacity) {
  bist::domain::Throttler t(/*capacity=*/100, /*tokens_per_sec=*/100);
  const std::uint64_t base = 10'000'000'000ULL;
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(t.try_acquire(base));
  }
  EXPECT_FALSE(t.try_acquire(base));
}

TEST(Throttler, DoesNotRefillInsideRollingSecond) {
  bist::domain::Throttler t(/*capacity=*/10, /*tokens_per_sec=*/10);
  const std::uint64_t base = 10'000'000'000ULL;
  for (int i = 0; i < 10; ++i) EXPECT_TRUE(t.try_acquire(base));
  EXPECT_FALSE(t.try_acquire(base));

  // At +500 ms the original 10 messages are still inside the last 10
  // 100 ms windows, so BIST would still reject another order.
  EXPECT_FALSE(t.try_acquire(base + 500'000'000ULL));
}

TEST(Throttler, OneSecondBoundaryDropsExpiredWindow) {
  bist::domain::Throttler t(100, 100);
  const std::uint64_t base = 10'000'000'000ULL;
  for (int i = 0; i < 100; ++i) EXPECT_TRUE(t.try_acquire(base));

  // Exactly 1000 ms later the original 100 ms slot has rolled out.
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(t.try_acquire(base + 1'000'000'000ULL));
  }
  EXPECT_FALSE(t.try_acquire(base + 1'000'000'000ULL));
}

TEST(Throttler, BistDocumentExampleRejectsResidualSecondBurst) {
  bist::domain::Throttler t(100, 100);
  const std::uint64_t base = 10'000'000'000ULL;

  for (int i = 0; i < 30; ++i) EXPECT_TRUE(t.try_acquire(base));
  for (int i = 0; i < 56; ++i) EXPECT_TRUE(t.try_acquire(base + 100'000'000ULL));
  for (int i = 0; i < 14; ++i) EXPECT_TRUE(t.try_acquire(base + 200'000'000ULL));
  EXPECT_FALSE(t.try_acquire(base + 200'000'000ULL));

  // At +1000 ms only the first 30-message window has expired. The 56 + 14
  // messages in the next two windows still consume 70/100 capacity, so only
  // 30 of the next 100 requests may pass.
  for (int i = 0; i < 30; ++i) EXPECT_TRUE(t.try_acquire(base + 1'000'000'000ULL));
  for (int i = 0; i < 70; ++i) EXPECT_FALSE(t.try_acquire(base + 1'000'000'000ULL));
}

TEST(Throttler, ZeroRateRefusesEverythingAfterDrain) {
  bist::domain::Throttler t(5, 0);  // 0 tokens/s
  EXPECT_FALSE(t.try_acquire(10'000'000'000ULL));
  EXPECT_FALSE(t.try_acquire(20'000'000'000ULL));
}
