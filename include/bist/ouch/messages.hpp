#pragma once
//
// bist/ouch/messages.hpp — wire-level OUCH message structs.
//
// Source of truth: BISTECH OUCH Protocol Specification
//                  (Publish 06 February 2025).
//
// Layout invariants:
//   - All numeric fields are big-endian. We wrap them in BigEndian<T> so that
//     no caller can accidentally read or write host-order bytes.
//   - All "Alpha" fields are ISO-8859-9 (Latin-9), left-justified, right-
//     padded with ASCII space (0x20). We model them as fixed-size char
//     arrays and provide pad helpers in codec.hpp.
//   - Every struct is __attribute__((packed)) AND wrapped in #pragma pack(1)
//     so that unintended padding cannot creep in even on toolchains that
//     interpret the attribute conservatively. Size and selected offsets are
//     verified by static_assert below.
//
// Note on outbound `Order Replaced`: BIST overloads message type 'U' for both
// the inbound Replace request and the outbound acknowledgement. We model them
// as separate types (`ReplaceOrder` and `OrderReplaced`) to avoid confusion.

#include <cstddef>
#include <cstdint>

#include "bist/core/endian.hpp"

namespace bist::ouch {

// --- Message type bytes ------------------------------------------------------

namespace msg_type {
// Inbound (client -> host)
inline constexpr char kEnterOrder      = 'O';
inline constexpr char kReplaceOrder    = 'U';
inline constexpr char kCancelOrder     = 'X';
inline constexpr char kCancelByOrderId = 'Y';
inline constexpr char kMassQuote       = 'Q';

// Outbound (host -> client)
inline constexpr char kOrderAccepted     = 'A';
inline constexpr char kOrderRejected     = 'J';
inline constexpr char kOrderReplaced     = 'U';
inline constexpr char kOrderCanceled     = 'C';
inline constexpr char kOrderExecuted     = 'E';
inline constexpr char kMassQuoteAck      = 'K';
inline constexpr char kMassQuoteRejection = 'R';
}  // namespace msg_type

// --- Common field sizes ------------------------------------------------------

inline constexpr std::size_t kTokenLen        = 14;
inline constexpr std::size_t kClientAccLen    = 16;
inline constexpr std::size_t kCustomerInfoLen = 15;
inline constexpr std::size_t kExchangeInfoLen = 32;
inline constexpr std::size_t kMassQuoteHdrLen = 50;
inline constexpr std::size_t kQuoteEntryLen   = 28;
inline constexpr std::size_t kMaxQuoteEntries = 5;

#pragma pack(push, 1)

// =====================================================================
// Inbound messages
// =====================================================================

// 4.1.1 Enter Order — total 114 bytes including 'O' type byte.
struct [[gnu::packed]] EnterOrder {
  char         message_type;        // offset 0,   value 'O'
  char         order_token[kTokenLen];  // offset 1,   length 14
  be_u32       order_book_id;       // offset 15,  length 4
  char         side;                // offset 19,  length 1
  be_u64       quantity;            // offset 20,  length 8
  be_i32       price;               // offset 28,  length 4
  std::uint8_t time_in_force;       // offset 32,  length 1
  std::uint8_t open_close;          // offset 33,  length 1
  char         client_account[kClientAccLen];      // offset 34, length 16
  char         customer_info[kCustomerInfoLen];    // offset 50, length 15
  char         exchange_info[kExchangeInfoLen];    // offset 65, length 32
  be_u64       display_quantity;    // offset 97,  length 8
  std::uint8_t client_category;     // offset 105, length 1
  std::uint8_t off_hours;           // offset 106, length 1
  std::uint8_t smp_level;           // offset 107, length 1
  std::uint8_t smp_method;          // offset 108, length 1
  char         smp_id[3];           // offset 109, length 3
  std::uint8_t reserved[2];         // offset 112, length 2
};
static_assert(sizeof(EnterOrder) == 114,
              "EnterOrder must be 114 bytes per BISTECH OUCH Spec (Feb 2025) Table 2");
static_assert(offsetof(EnterOrder, order_book_id)    == 15);
static_assert(offsetof(EnterOrder, quantity)         == 20);
static_assert(offsetof(EnterOrder, price)            == 28);
static_assert(offsetof(EnterOrder, time_in_force)    == 32);
static_assert(offsetof(EnterOrder, client_account)   == 34);
static_assert(offsetof(EnterOrder, customer_info)    == 50);
static_assert(offsetof(EnterOrder, exchange_info)    == 65);
static_assert(offsetof(EnterOrder, display_quantity) == 97);
static_assert(offsetof(EnterOrder, client_category)  == 105);
static_assert(offsetof(EnterOrder, off_hours)        == 106);
static_assert(offsetof(EnterOrder, smp_level)        == 107);
static_assert(offsetof(EnterOrder, smp_method)       == 108);
static_assert(offsetof(EnterOrder, smp_id)           == 109);
static_assert(offsetof(EnterOrder, reserved)         == 112);

// 4.1.2 Replace Order — total 122 bytes.
struct [[gnu::packed]] ReplaceOrder {
  char         message_type;                       // offset 0,   value 'U'
  char         existing_order_token[kTokenLen];    // offset 1,   length 14
  char         replacement_order_token[kTokenLen]; // offset 15,  length 14
  be_u64       quantity;            // offset 29,  length 8
  be_i32       price;               // offset 37,  length 4 (0 = no change)
  std::uint8_t open_close;          // offset 41,  length 1
  char         client_account[kClientAccLen];      // offset 42,  length 16
  char         customer_info[kCustomerInfoLen];    // offset 58,  length 15
  char         exchange_info[kExchangeInfoLen];    // offset 73,  length 32
  be_u64       display_quantity;    // offset 105, length 8
  std::uint8_t client_category;     // offset 113, length 1
  std::uint8_t reserved[8];         // offset 114, length 8
};
static_assert(sizeof(ReplaceOrder) == 122,
              "ReplaceOrder must be 122 bytes per OUCH Spec R2.11 Table 3");
static_assert(offsetof(ReplaceOrder, replacement_order_token) == 15);
static_assert(offsetof(ReplaceOrder, quantity)         == 29);
static_assert(offsetof(ReplaceOrder, price)            == 37);
static_assert(offsetof(ReplaceOrder, client_account)   == 42);
static_assert(offsetof(ReplaceOrder, exchange_info)    == 73);
static_assert(offsetof(ReplaceOrder, display_quantity) == 105);
static_assert(offsetof(ReplaceOrder, client_category)  == 113);

// 4.1.3 Cancel Order — total 15 bytes.
struct [[gnu::packed]] CancelOrder {
  char message_type;                // offset 0, value 'X'
  char order_token[kTokenLen];      // offset 1, length 14
};
static_assert(sizeof(CancelOrder) == 15,
              "CancelOrder must be 15 bytes per OUCH Spec R2.11 Table 4");

// 4.1.4 Cancel By Order ID — total 14 bytes.
struct [[gnu::packed]] CancelByOrderId {
  char   message_type;              // offset 0, value 'Y'
  be_u32 order_book_id;             // offset 1, length 4
  char   side;                      // offset 5, length 1
  be_u64 order_id;                  // offset 6, length 8
};
static_assert(sizeof(CancelByOrderId) == 14,
              "CancelByOrderId must be 14 bytes per OUCH Spec R2.11 Table 5");

// 4.1.5 Mass Quote — header is 50 bytes, plus 28 bytes per QuoteEntry, with
// 1..5 quote entries. The on-wire layout is dynamically sized; we model the
// header and the entry struct independently and let the codec assemble them.
struct [[gnu::packed]] MassQuoteHeader {
  char         message_type;        // offset 0,  value 'Q'
  char         order_token[kTokenLen]; // offset 1,  length 14
  std::uint8_t client_category;     // offset 15, length 1
  char         client_account[kClientAccLen];  // offset 16, length 16
  char         exchange_info[16];   // offset 32, length 16  (BIST narrows this
                                    //                       to 16 bytes per
                                    //                       Table 6)
  be_u16       no_quote_entries;    // offset 48, length 2
};
static_assert(sizeof(MassQuoteHeader) == kMassQuoteHdrLen,
              "MassQuoteHeader must be 50 bytes per OUCH Spec R2.11 Table 6");
static_assert(offsetof(MassQuoteHeader, no_quote_entries) == 48);

struct [[gnu::packed]] QuoteEntry {
  be_u32 order_book_id;             // length 4
  be_i32 bid_price;                 // length 4
  be_i32 offer_price;               // length 4
  be_u64 bid_size;                  // length 8
  be_u64 offer_size;                // length 8
};
static_assert(sizeof(QuoteEntry) == kQuoteEntryLen,
              "QuoteEntry must be 28 bytes per OUCH Spec R2.11 Table 6");

// =====================================================================
// Outbound messages
// =====================================================================

// 4.2.1 Order Accepted — 137 bytes (BISTECH OUCH Spec, Feb 2025 Table 7).
// Note: R2.11 (May 2020) had this struct at 135 bytes. The Feb 2025 spec
// appends SMP (Self-Match Prevention) fields after OffHours.
struct [[gnu::packed]] OrderAccepted {
  char         message_type;        // offset 0,   value 'A'
  be_u64       timestamp_ns;        // offset 1,   length 8
  char         order_token[kTokenLen];  // offset 9,   length 14
  be_u32       order_book_id;       // offset 23,  length 4
  char         side;                // offset 27,  length 1
  be_u64       order_id;            // offset 28,  length 8
  be_u64       quantity;            // offset 36,  length 8 (open quantity)
  be_i32       price;               // offset 44,  length 4
  std::uint8_t time_in_force;       // offset 48,  length 1
  std::uint8_t open_close;          // offset 49,  length 1
  char         client_account[kClientAccLen];      // offset 50, length 16
  std::uint8_t order_state;         // offset 66,  length 1
  char         customer_info[kCustomerInfoLen];    // offset 67, length 15
  char         exchange_info[kExchangeInfoLen];    // offset 82, length 32
  be_u64       pre_trade_quantity;  // offset 114, length 8
  be_u64       display_quantity;    // offset 122, length 8
  std::uint8_t client_category;     // offset 130, length 1
  std::uint8_t off_hours;           // offset 131, length 1
  std::uint8_t smp_level;           // offset 132, length 1
  std::uint8_t smp_method;          // offset 133, length 1
  char         smp_id[3];           // offset 134, length 3
};
static_assert(sizeof(OrderAccepted) == 137,
              "OrderAccepted must be 137 bytes per BISTECH OUCH Spec (Feb 2025) Table 7");
static_assert(offsetof(OrderAccepted, order_token)        == 9);
static_assert(offsetof(OrderAccepted, order_book_id)      == 23);
static_assert(offsetof(OrderAccepted, side)               == 27);
static_assert(offsetof(OrderAccepted, order_id)           == 28);
static_assert(offsetof(OrderAccepted, quantity)           == 36);
static_assert(offsetof(OrderAccepted, price)              == 44);
static_assert(offsetof(OrderAccepted, client_account)     == 50);
static_assert(offsetof(OrderAccepted, order_state)        == 66);
static_assert(offsetof(OrderAccepted, exchange_info)      == 82);
static_assert(offsetof(OrderAccepted, pre_trade_quantity) == 114);
static_assert(offsetof(OrderAccepted, display_quantity)   == 122);
static_assert(offsetof(OrderAccepted, client_category)    == 130);
static_assert(offsetof(OrderAccepted, off_hours)          == 131);
static_assert(offsetof(OrderAccepted, smp_level)          == 132);
static_assert(offsetof(OrderAccepted, smp_method)         == 133);
static_assert(offsetof(OrderAccepted, smp_id)             == 134);

// 4.2.2 Order Rejected — 27 bytes.
struct [[gnu::packed]] OrderRejected {
  char   message_type;              // offset 0, value 'J'
  be_u64 timestamp_ns;              // offset 1, length 8
  char   order_token[kTokenLen];    // offset 9, length 14
  be_i32 reject_code;               // offset 23, length 4 (signed)
};
static_assert(sizeof(OrderRejected) == 27,
              "OrderRejected must be 27 bytes per OUCH Spec R2.11 Table 8");

// 4.2.3 Order Replaced — 145 bytes (outbound 'U').
struct [[gnu::packed]] OrderReplaced {
  char         message_type;        // offset 0,   value 'U'
  be_u64       timestamp_ns;        // offset 1,   length 8
  char         replacement_order_token[kTokenLen]; // offset 9, length 14
  char         previous_order_token[kTokenLen];    // offset 23, length 14
  be_u32       order_book_id;       // offset 37,  length 4
  char         side;                // offset 41,  length 1
  be_u64       order_id;            // offset 42,  length 8
  be_u64       quantity;            // offset 50,  length 8 (open quantity)
  be_i32       price;               // offset 58,  length 4
  std::uint8_t time_in_force;       // offset 62,  length 1
  std::uint8_t open_close;          // offset 63,  length 1
  char         client_account[kClientAccLen];      // offset 64,  length 16
  std::uint8_t order_state;         // offset 80,  length 1
  char         customer_info[kCustomerInfoLen];    // offset 81,  length 15
  char         exchange_info[kExchangeInfoLen];    // offset 96,  length 32
  be_u64       pre_trade_quantity;  // offset 128, length 8
  be_u64       display_quantity;    // offset 136, length 8
  std::uint8_t client_category;     // offset 144, length 1
};
static_assert(sizeof(OrderReplaced) == 145,
              "OrderReplaced must be 145 bytes per OUCH Spec R2.11 Table 9");
static_assert(offsetof(OrderReplaced, previous_order_token) == 23);
static_assert(offsetof(OrderReplaced, order_book_id)        == 37);
static_assert(offsetof(OrderReplaced, order_id)             == 42);
static_assert(offsetof(OrderReplaced, quantity)             == 50);
static_assert(offsetof(OrderReplaced, price)                == 58);
static_assert(offsetof(OrderReplaced, order_state)          == 80);
static_assert(offsetof(OrderReplaced, exchange_info)        == 96);
static_assert(offsetof(OrderReplaced, pre_trade_quantity)   == 128);
static_assert(offsetof(OrderReplaced, display_quantity)     == 136);
static_assert(offsetof(OrderReplaced, client_category)      == 144);

// 4.2.4 Order Canceled — 37 bytes.
struct [[gnu::packed]] OrderCanceled {
  char         message_type;        // offset 0,  value 'C'
  be_u64       timestamp_ns;        // offset 1,  length 8
  char         order_token[kTokenLen];  // offset 9,  length 14
  be_u32       order_book_id;       // offset 23, length 4
  char         side;                // offset 27, length 1
  be_u64       order_id;            // offset 28, length 8
  std::uint8_t reason;              // offset 36, length 1
};
static_assert(sizeof(OrderCanceled) == 37,
              "OrderCanceled must be 37 bytes per OUCH Spec R2.11 Table 10");

// 4.2.5 Order Executed — 68 bytes.
struct [[gnu::packed]] OrderExecuted {
  char         message_type;        // offset 0,  value 'E'
  be_u64       timestamp_ns;        // offset 1,  length 8
  char         order_token[kTokenLen];  // offset 9,  length 14
  be_u32       order_book_id;       // offset 23, length 4
  be_u64       traded_quantity;     // offset 27, length 8
  be_i32       trade_price;         // offset 35, length 4
  std::uint8_t match_id[12];        // offset 39, length 12 (numeric, 12 bytes)
  std::uint8_t client_category;     // offset 51, length 1
  std::uint8_t reserved[16];        // offset 52, length 16
};
static_assert(sizeof(OrderExecuted) == 68,
              "OrderExecuted must be 68 bytes per OUCH Spec R2.11 Table 11");
static_assert(offsetof(OrderExecuted, traded_quantity) == 27);
static_assert(offsetof(OrderExecuted, trade_price)     == 35);
static_assert(offsetof(OrderExecuted, match_id)        == 39);
static_assert(offsetof(OrderExecuted, client_category) == 51);
static_assert(offsetof(OrderExecuted, reserved)        == 52);

// 4.2.6 Mass Quote Acknowledgement — 52 bytes per accepted side.
struct [[gnu::packed]] MassQuoteAck {
  char   message_type;              // offset 0,  value 'K'
  be_u64 timestamp_ns;              // offset 1,  length 8
  char   order_token[kTokenLen];    // offset 9,  length 14
  be_u32 order_book_id;             // offset 23, length 4
  be_u64 quantity;                  // offset 27, length 8 (leaves)
  be_u64 traded_quantity;           // offset 35, length 8
  be_i32 price;                     // offset 43, length 4
  char   side;                      // offset 47, length 1
  be_u32 quote_status;              // offset 48, length 4 (MassQuoteStatus)
};
static_assert(sizeof(MassQuoteAck) == 52,
              "MassQuoteAck must be 52 bytes per BISTECH OUCH Spec (Feb 2025) Table 12");
static_assert(offsetof(MassQuoteAck, timestamp_ns)     == 1);
static_assert(offsetof(MassQuoteAck, order_token)      == 9);
static_assert(offsetof(MassQuoteAck, order_book_id)    == 23);
static_assert(offsetof(MassQuoteAck, quantity)         == 27);
static_assert(offsetof(MassQuoteAck, traded_quantity)  == 35);
static_assert(offsetof(MassQuoteAck, price)            == 43);
static_assert(offsetof(MassQuoteAck, side)             == 47);
static_assert(offsetof(MassQuoteAck, quote_status)     == 48);

// 4.2.7 Mass Quote Rejection — 31 bytes.
struct [[gnu::packed]] MassQuoteRejection {
  char   message_type;              // offset 0,  value 'R'
  be_u64 timestamp_ns;              // offset 1,  length 8
  char   order_token[kTokenLen];    // offset 9,  length 14
  be_u32 order_book_id;             // offset 23, length 4
  be_i32 reject_code;               // offset 27, length 4
};
static_assert(sizeof(MassQuoteRejection) == 31,
              "MassQuoteRejection must be 31 bytes per OUCH Spec R2.11 Table 13");

#pragma pack(pop)

// --- Reject codes (signed int32 over the wire) -------------------------------
//
// The OUCH "Reject Code" is signed because BIST returns negative integers.
// We expose the well-known constants observed in the certification document;
// the full catalog lives in the System Error Messages Reference.

namespace reject_code {
inline constexpr std::int32_t kPriceOutsideLimits = -420131;  // "premium outside allowed price limits"
inline constexpr std::int32_t kTokenNotUnique     = -800002;  // "token is not unique"
}  // namespace reject_code

}  // namespace bist::ouch
