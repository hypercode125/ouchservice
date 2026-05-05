// tests/unit/test_order_book.cpp — local OrderBook state tracking and
// the OUCH Spec 4.1.2.1 Replace LeavesQty math.

#include <gtest/gtest.h>

#include "bist/domain/order_book.hpp"

namespace ob = bist::domain;

TEST(OrderBook, InsertAndAccept) {
  ob::OrderBook book;
  ob::OrderState s{};
  s.symbol       = "ADEL.E";
  s.side         = bist::Side::Buy;
  s.original_qty = 200;
  s.price        = 6200;
  book.insert("T-10", std::move(s));

  book.on_accepted("T-10", 12345, 200);
  const auto* st = book.find("T-10");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->order_id, 12345u);
  EXPECT_EQ(st->leaves, 200);
  EXPECT_EQ(st->status, ob::OrderStatus::OnBook);
}

TEST(OrderBook, ExecutedReducesLeaves) {
  ob::OrderBook book;
  ob::OrderState s{};
  s.original_qty = 1000;
  book.insert("T", std::move(s));
  book.on_accepted("T", 1, 1000);
  book.on_executed("T", 200);
  const auto* st = book.find("T");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->executed, 200);
  EXPECT_EQ(st->leaves, 800);
  EXPECT_EQ(st->status, ob::OrderStatus::OnBook);
}

TEST(OrderBook, FullFillFlipsToNotOnBook) {
  ob::OrderBook book;
  ob::OrderState s{};
  s.original_qty = 100;
  book.insert("T", std::move(s));
  book.on_accepted("T", 1, 100);
  book.on_executed("T", 100);
  const auto* st = book.find("T");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->leaves, 0);
  EXPECT_EQ(st->status, ob::OrderStatus::NotOnBook);
}

// Spec 4.1.2.1 Example 1: 1000 entered, 200 traded, replace to 750 by sending
// new total = 950 (open 750 + executed 200). Expected leaves = 750.
TEST(OrderBook, ReplaceMathSpecExample1) {
  EXPECT_EQ(ob::OrderBook::expected_leaves(/*new_total=*/950, /*executed=*/200),
            750);
}

// Spec 4.1.2.1 Example 2: 1000 entered, 600 traded, replace to 500 leaves
// nothing in the book (new total < executed) → leaves = 0, NotOnBook.
TEST(OrderBook, ReplaceMathSpecExample2) {
  EXPECT_EQ(ob::OrderBook::expected_leaves(/*new_total=*/500, /*executed=*/600),
            0);
}

// BIST cert OUCH Bölüm 2 ZOREN/KAREL walkthrough: 100 entered, 20 traded,
// replace request quantity=70 → 50 left in book, then quantity=90 → 70 left.
TEST(OrderBook, ReplaceMathBistCertWalkthrough) {
  EXPECT_EQ(ob::OrderBook::expected_leaves(70, 20), 50);
  EXPECT_EQ(ob::OrderBook::expected_leaves(90, 20), 70);
}

// ALCAR cert: 100 entered, 60 traded, replace quantity=50 → wipes order,
// LeavesQty 0 even though no cancel was requested.
TEST(OrderBook, ReplaceBelowExecutedWipesOrder) {
  EXPECT_EQ(ob::OrderBook::expected_leaves(50, 60), 0);
}
