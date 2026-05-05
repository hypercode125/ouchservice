// tests/unit/test_ring_buffer.cpp — basic SPSC ring buffer behavior tests.
//
// Concurrency stress lives in a TSan-only suite; this file checks the
// single-threaded contract: pushes are visible to pops, the queue refuses
// pushes when full, and pops return nullopt when empty.

#include <gtest/gtest.h>

#include <optional>

#include "bist/core/ring_buffer.hpp"

TEST(SpscRing, PushPopRoundTrip) {
  bist::SpscRing<int, 8> q;
  EXPECT_TRUE(q.empty());
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_TRUE(q.try_push(3));
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(q.approx_size(), 3u);
  auto a = q.try_pop();
  ASSERT_TRUE(a);
  EXPECT_EQ(*a, 1);
  auto b = q.try_pop();
  ASSERT_TRUE(b);
  EXPECT_EQ(*b, 2);
  auto c = q.try_pop();
  ASSERT_TRUE(c);
  EXPECT_EQ(*c, 3);
  EXPECT_FALSE(q.try_pop().has_value());
}

TEST(SpscRing, RefusesWhenFull) {
  bist::SpscRing<int, 4> q;
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_TRUE(q.try_push(3));
  EXPECT_TRUE(q.try_push(4));
  EXPECT_FALSE(q.try_push(5));   // full
  EXPECT_TRUE(q.try_pop().has_value());
  EXPECT_TRUE(q.try_push(5));    // freed one slot
}

TEST(SpscRing, EmptyPopsAreNullopt) {
  bist::SpscRing<int, 2> q;
  EXPECT_FALSE(q.try_pop().has_value());
}
