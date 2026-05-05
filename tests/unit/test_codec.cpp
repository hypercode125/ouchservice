// tests/unit/test_codec.cpp — alpha pad, hex dump, MassQuote builders.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "bist/ouch/codec.hpp"

namespace ouch = bist::ouch;

TEST(AlphaCodec, RightPadsWithSpaces) {
  char buf[8];
  ouch::alpha_set(buf, "ABC");
  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[1], 'B');
  EXPECT_EQ(buf[2], 'C');
  EXPECT_EQ(buf[3], ' ');
  EXPECT_EQ(buf[7], ' ');
}

TEST(AlphaCodec, TrimsTrailingSpacesOnGet) {
  char buf[8];
  ouch::alpha_set(buf, "ABC");
  EXPECT_EQ(ouch::alpha_get(buf), std::string_view{"ABC"});
}

TEST(AlphaCodec, TruncatesOversizeInput) {
  char buf[4];
  ouch::alpha_set(buf, "ABCDEF");
  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[1], 'B');
  EXPECT_EQ(buf[2], 'C');
  EXPECT_EQ(buf[3], 'D');
  EXPECT_EQ(ouch::alpha_get(buf), std::string_view{"ABCD"});
}

TEST(TokenCodec, RoundTrip) {
  bist::OrderToken tok{"T-00010"};
  char raw[ouch::kTokenLen];
  ouch::token_set(raw, tok);
  const auto back = ouch::token_get(raw);
  EXPECT_EQ(back.view(), std::string_view{"T-00010"});
}

TEST(HexDump, LowercaseNoSeparators) {
  const std::array<std::uint8_t, 4> bytes = {0xDE, 0xAD, 0xBE, 0xEF};
  EXPECT_EQ(ouch::hex_dump(std::span<const std::uint8_t>(bytes)),
            "deadbeef");
}

TEST(QuoteMatrix, NewTwoSided) {
  ouch::QuoteEntry q{};
  ouch::quote_new_two_sided(q, /*book=*/70616, /*bid_px=*/5100, /*bid_size=*/500,
                            /*offer_px=*/5110, /*offer_size=*/500);
  EXPECT_EQ(q.bid_price.get(), 5100);
  EXPECT_EQ(q.offer_price.get(), 5110);
  EXPECT_EQ(q.bid_size.get(), 500u);
  EXPECT_EQ(q.offer_size.get(), 500u);
}

TEST(QuoteMatrix, CancelTwoSidedZerosEverything) {
  ouch::QuoteEntry q{};
  // pre-fill with non-zero to ensure cancel actually rewrites.
  q.bid_price.set(1);
  q.offer_price.set(1);
  q.bid_size.set(1);
  q.offer_size.set(1);
  ouch::quote_cancel_two_sided(q, /*book=*/70616);
  EXPECT_EQ(q.bid_price.get(), 0);
  EXPECT_EQ(q.offer_price.get(), 0);
  EXPECT_EQ(q.bid_size.get(), 0u);
  EXPECT_EQ(q.offer_size.get(), 0u);
}

TEST(QuoteMatrix, CancelBidOnly) {
  ouch::QuoteEntry q{};
  ouch::quote_cancel_bid(q, 70616, /*offer_px=*/5110, /*offer_size=*/500);
  EXPECT_EQ(q.bid_price.get(), 0);
  EXPECT_EQ(q.bid_size.get(), 0u);
  EXPECT_EQ(q.offer_price.get(), 5110);
  EXPECT_EQ(q.offer_size.get(), 500u);
}
