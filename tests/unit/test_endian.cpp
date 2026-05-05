// tests/unit/test_endian.cpp — round-trip and wire-byte verification for
// BigEndian<T>.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "bist/core/endian.hpp"

namespace b = bist;

TEST(BigEndian, U16RoundTripAndBytes) {
  b::be_u16 v;
  v.set(0x1234u);
  EXPECT_EQ(v.get(), 0x1234u);
  EXPECT_EQ(v.data()[0], 0x12);
  EXPECT_EQ(v.data()[1], 0x34);
}

TEST(BigEndian, U32WireBytes) {
  b::be_u32 v;
  v.set(0xDEADBEEFu);
  EXPECT_EQ(v.get(), 0xDEADBEEFu);
  EXPECT_EQ(v.data()[0], 0xDE);
  EXPECT_EQ(v.data()[1], 0xAD);
  EXPECT_EQ(v.data()[2], 0xBE);
  EXPECT_EQ(v.data()[3], 0xEF);
}

TEST(BigEndian, U64RoundTrip) {
  b::be_u64 v;
  v.set(0x0123456789ABCDEFull);
  EXPECT_EQ(v.get(), 0x0123456789ABCDEFull);
  static constexpr std::array<std::uint8_t, 8> kExpected = {
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    EXPECT_EQ(v.data()[i], kExpected[i]) << "byte " << i;
  }
}

TEST(BigEndian, I32SignedRoundTrip) {
  b::be_i32 v;
  v.set(-420131);
  EXPECT_EQ(v.get(), -420131);
  // 420131 == 0x00066923 → two's complement → 0xFFF996DD.
  EXPECT_EQ(v.data()[0], 0xFF);
  EXPECT_EQ(v.data()[1], 0xF9);
  EXPECT_EQ(v.data()[2], 0x96);
  EXPECT_EQ(v.data()[3], 0xDD);
}

TEST(BigEndian, U8IsByteIdentity) {
  b::be_u8 v;
  v.set(0xAB);
  EXPECT_EQ(v.get(), 0xAB);
  EXPECT_EQ(v.data()[0], 0xAB);
}
