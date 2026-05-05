// tests/unit/test_messages.cpp — verifies OUCH wire structs match the spec.
//
// The static_asserts in messages.hpp already block the build if a struct
// drifts from the BIST OUCH R2.11 layout. This test adds a runtime-level
// sanity check covering field placement (e.g. that the message type byte is
// at offset 0 and that big-endian wrapping round-trips correctly) so that
// catastrophic damage to the header would fail loudly even if a future
// refactor were to relax the static_asserts.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

#include "bist/core/types.hpp"
#include "bist/ouch/messages.hpp"

namespace ouch = bist::ouch;

TEST(OuchMessages, EnterOrderLayout) {
  ouch::EnterOrder eo{};
  std::memset(&eo, 0, sizeof(eo));
  eo.message_type = ouch::msg_type::kEnterOrder;
  eo.order_book_id.set(70616u);
  eo.side          = 'B';
  eo.quantity.set(200u);
  eo.price.set(6200);
  eo.time_in_force = static_cast<std::uint8_t>(bist::TimeInForce::Day);
  eo.client_category =
      static_cast<std::uint8_t>(bist::ClientCategory::Client);

  EXPECT_EQ(sizeof(eo), 114u);
  EXPECT_EQ(eo.message_type, 'O');
  EXPECT_EQ(eo.order_book_id.get(), 70616u);
  EXPECT_EQ(eo.quantity.get(), 200u);
  EXPECT_EQ(eo.price.get(), 6200);
  EXPECT_EQ(eo.time_in_force, 0u);
  EXPECT_EQ(eo.client_category, 1u);
}

TEST(OuchMessages, EnterOrderExposesFeb2025SmpFields) {
  EXPECT_EQ(offsetof(ouch::EnterOrder, smp_level), 107u);
  EXPECT_EQ(offsetof(ouch::EnterOrder, smp_method), 108u);
  EXPECT_EQ(offsetof(ouch::EnterOrder, smp_id), 109u);
  EXPECT_EQ(offsetof(ouch::EnterOrder, reserved), 112u);

  ouch::EnterOrder eo{};
  std::memset(&eo, 0, sizeof(eo));
  eo.smp_level = 1;
  eo.smp_method = 2;
  std::memcpy(eo.smp_id, "ABC", 3);
  eo.reserved[0] = 0;
  eo.reserved[1] = 0;

  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&eo);
  EXPECT_EQ(bytes[107], 1u);
  EXPECT_EQ(bytes[108], 2u);
  EXPECT_EQ(bytes[109], static_cast<std::uint8_t>('A'));
  EXPECT_EQ(bytes[110], static_cast<std::uint8_t>('B'));
  EXPECT_EQ(bytes[111], static_cast<std::uint8_t>('C'));
  EXPECT_EQ(bytes[112], 0u);
  EXPECT_EQ(bytes[113], 0u);
}

TEST(OuchMessages, ReplaceOrderLayout) {
  ouch::ReplaceOrder ro{};
  std::memset(&ro, 0, sizeof(ro));
  ro.message_type = ouch::msg_type::kReplaceOrder;
  ro.quantity.set(950u);
  ro.price.set(0);  // 0 = "no change" per Spec 4.1.2.2.

  EXPECT_EQ(sizeof(ro), 122u);
  EXPECT_EQ(ro.quantity.get(), 950u);
  EXPECT_EQ(ro.price.get(), 0);
}

TEST(OuchMessages, MassQuoteHeaderAndEntry) {
  EXPECT_EQ(sizeof(ouch::MassQuoteHeader), 50u);
  EXPECT_EQ(sizeof(ouch::QuoteEntry), 28u);

  ouch::QuoteEntry q{};
  q.order_book_id.set(70616u);
  q.bid_price.set(5100);
  q.bid_size.set(500u);
  q.offer_price.set(5110);
  q.offer_size.set(500u);
  EXPECT_EQ(q.order_book_id.get(), 70616u);
  EXPECT_EQ(q.bid_price.get(), 5100);
  EXPECT_EQ(q.offer_price.get(), 5110);
  EXPECT_EQ(q.bid_size.get(), 500u);
  EXPECT_EQ(q.offer_size.get(), 500u);
}

TEST(OuchMessages, OutboundSizes) {
  EXPECT_EQ(sizeof(ouch::OrderAccepted),     137u);
  EXPECT_EQ(sizeof(ouch::OrderRejected),     27u);
  EXPECT_EQ(sizeof(ouch::OrderReplaced),     145u);
  EXPECT_EQ(sizeof(ouch::OrderCanceled),     37u);
  EXPECT_EQ(sizeof(ouch::OrderExecuted),     68u);
  EXPECT_EQ(sizeof(ouch::MassQuoteAck),      52u);
  EXPECT_EQ(sizeof(ouch::MassQuoteRejection), 31u);
}

TEST(OuchMessages, MassQuoteAckUsesFeb2025Offsets) {
  EXPECT_EQ(sizeof(ouch::MassQuoteAck), 52u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, message_type), 0u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, timestamp_ns), 1u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, order_token), 9u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, order_book_id), 23u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, quantity), 27u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, traded_quantity), 35u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, price), 43u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, side), 47u);
  EXPECT_EQ(offsetof(ouch::MassQuoteAck, quote_status), 48u);

  ouch::MassQuoteAck ack{};
  std::memset(&ack, 0, sizeof(ack));
  ack.message_type = ouch::msg_type::kMassQuoteAck;
  ack.timestamp_ns.set(0x0102030405060708ULL);
  std::memcpy(ack.order_token, "MQ1           ", ouch::kTokenLen);
  ack.order_book_id.set(70616u);
  ack.quantity.set(500u);
  ack.traded_quantity.set(12u);
  ack.price.set(5110);
  ack.side = 'B';
  ack.quote_status.set(0u);

  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&ack);
  EXPECT_EQ(bytes[0], static_cast<std::uint8_t>('K'));
  EXPECT_EQ(bytes[27], 0u);
  EXPECT_EQ(bytes[34], 0xF4u);
  EXPECT_EQ(bytes[42], 0x0Cu);
  EXPECT_EQ(bytes[43], 0u);
  EXPECT_EQ(bytes[45], 0x13u);
  EXPECT_EQ(bytes[46], 0xF6u);
  EXPECT_EQ(bytes[47], static_cast<std::uint8_t>('B'));
  EXPECT_EQ(bytes[48], 0u);
  EXPECT_EQ(bytes[51], 0u);

  ouch::MassQuoteAck decoded{};
  std::memcpy(&decoded, bytes, sizeof(decoded));
  EXPECT_EQ(decoded.quantity.get(), 500u);
  EXPECT_EQ(decoded.traded_quantity.get(), 12u);
  EXPECT_EQ(decoded.price.get(), 5110);
  EXPECT_EQ(decoded.side, 'B');
  EXPECT_EQ(decoded.quote_status.get(), 0u);
}

TEST(OuchMessages, RejectCodes) {
  EXPECT_EQ(ouch::reject_code::kPriceOutsideLimits, -420131);
  EXPECT_EQ(ouch::reject_code::kTokenNotUnique,     -800002);
}
