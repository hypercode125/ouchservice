// tests/unit/test_soupbintcp.cpp — header sizing and length encoding.

#include <gtest/gtest.h>

#include <cstdint>

#include "bist/net/soupbintcp.hpp"

namespace soup = bist::soup;

TEST(SoupBinTcp, HeaderIsThreeBytes) {
  EXPECT_EQ(sizeof(soup::Header), 3u);
}

TEST(SoupBinTcp, LoginRequestSize) {
  EXPECT_EQ(sizeof(soup::LoginRequest), 49u);
}

TEST(SoupBinTcp, MakeHeaderEncodesLengthPlusOne) {
  soup::Header h;
  // wrapping a 14-byte CancelOrder => length = 14 + 1 = 15.
  soup::make_header(h, soup::pkt::kUnsequencedData, /*inner_len=*/14);
  EXPECT_EQ(h.length.get(), 15u);
  EXPECT_EQ(h.packet_type, 'U');
  EXPECT_EQ(soup::total_packet_size(h), 17u);  // length(2) + length value(15)
}
