// tests/unit/test_token_registry.cpp — uniqueness and OrderID lookup.

#include <gtest/gtest.h>

#include "bist/domain/token_registry.hpp"

TEST(TokenRegistry, AllocateProducesDistinctTokens) {
  bist::domain::TokenRegistry r{"C-"};
  auto a = r.allocate();
  auto b = r.allocate();
  auto c = r.allocate();
  EXPECT_NE(a.view(), b.view());
  EXPECT_NE(b.view(), c.view());
  EXPECT_EQ(r.size(), 3u);
}

TEST(TokenRegistry, RegisterExternalReturnsFalseOnDuplicate) {
  bist::domain::TokenRegistry r;
  EXPECT_TRUE(r.register_external("MQ-001"));
  EXPECT_FALSE(r.register_external("MQ-001"));
  EXPECT_TRUE(r.register_external("MQ-002"));
}

TEST(TokenRegistry, OrderIdLookup) {
  bist::domain::TokenRegistry r;
  EXPECT_TRUE(r.register_external("T1"));
  r.map_to_order_id("T1", 42);
  EXPECT_EQ(r.order_id_of("T1"), 42u);
  EXPECT_EQ(r.order_id_of("UNKNOWN"), 0u);
}
