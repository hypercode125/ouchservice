// tests/unit/test_instrument.cpp — symbol/book id lookup and price parsing.

#include <gtest/gtest.h>

#include "bist/domain/instrument.hpp"

namespace d = bist::domain;

TEST(InstrumentCache, PutAndLookupBidirectional) {
  d::InstrumentCache c;
  d::Instrument adel{"ADEL.E", 70676, 3, 1, 6000};
  c.put(adel);
  ASSERT_NE(c.find_by_symbol("ADEL.E"), nullptr);
  EXPECT_EQ(c.find_by_symbol("ADEL.E")->order_book_id, 70676u);
  ASSERT_NE(c.find_by_book_id(70676u), nullptr);
  EXPECT_EQ(c.find_by_book_id(70676u)->symbol, "ADEL.E");
  auto rid = c.resolve("ADEL.E");
  ASSERT_TRUE(rid.has_value());
  EXPECT_EQ(*rid, 70676u);
}

TEST(InstrumentCache, ToWirePriceCommonValues) {
  EXPECT_EQ(d::InstrumentCache::to_wire_price("6.200", 3), 6200);
  EXPECT_EQ(d::InstrumentCache::to_wire_price("5",     3), 5000);
  EXPECT_EQ(d::InstrumentCache::to_wire_price("0.000", 3), 0);
  EXPECT_EQ(d::InstrumentCache::to_wire_price("0.010", 3), 10);
  EXPECT_EQ(d::InstrumentCache::to_wire_price("-0.010", 3), -10);
  EXPECT_EQ(d::InstrumentCache::to_wire_price("0.01",  3), 10);
  EXPECT_EQ(d::InstrumentCache::to_wire_price("10.000", 3), 10000);
}

TEST(InstrumentCache, ToWirePriceRespectsDecimals) {
  EXPECT_EQ(d::InstrumentCache::to_wire_price("1.5", 2), 150);
  EXPECT_EQ(d::InstrumentCache::to_wire_price("1.5", 4), 15000);
}
