#pragma once
//
// bist/core/types.hpp — fundamental scalar and tag types used across modules.
//
// Numeric and Price are protocol-level distinctions: Price is a signed int32
// scaled by an instrument-specific decimal count (delivered out-of-band via
// ITCH OrderbookDirectory or FIX RD SecurityDefinition).
//
// Order tokens are 14-byte ASCII (left-justified, right-padded with spaces)
// per BISTECH OUCH Protocol Specification (06 February 2025) §3.

#include <array>
#include <bit>
#include <cstdint>
#include <string_view>

namespace bist {

// --- Scalar aliases ----------------------------------------------------------

using OrderBookId   = std::uint32_t;          // ITCH/OUCH order book identifier
using OrderId       = std::uint64_t;          // System-assigned per (book, side)
using Quantity      = std::int64_t;           // Open/traded/display quantities
using PriceInt      = std::int32_t;           // Wire-level signed price
using TimestampNs   = std::uint64_t;          // Nanoseconds since UNIX epoch
using SeqNum        = std::uint64_t;          // SoupBinTCP and FIX sequence

// --- Side --------------------------------------------------------------------
//
// OUCH Side is an Alpha(1) field with values B/S/T. We keep it as a strong
// enum to prevent silent confusion with FIX Side(54) which uses '1'/'2'.

enum class Side : char {
  Buy       = 'B',
  Sell      = 'S',
  ShortSell = 'T',
};

constexpr char to_wire(Side s) noexcept { return static_cast<char>(s); }
constexpr Side side_from_wire(char c) noexcept { return static_cast<Side>(c); }

// --- TimeInForce -------------------------------------------------------------

enum class TimeInForce : std::uint8_t {
  Day = 0,
  ImmediateOrCancel = 3,   // FaK in BIST terminology
  FillOrKill        = 4,
};

// --- Open/Close --------------------------------------------------------------
//
// BISTECH OUCH Spec (Feb 2025) §4.1.1 (Enter Order) values:
//   0 = Default per account
//   1 = Open  (open position; derivs only)
//   2 = Close / Net (close position; derivs only)
//
// §4.1.2.2 (Replace Order) reuses the field with a different meaning:
//   0 = Replace keeps the existing open/close flag from the original order
//   4 = Replace explicitly resets to the account default
//
// Callers MUST use the correct subset for the message being built. To make
// the distinction visible at the call site we keep a single enum but provide
// `is_valid_for_*` predicates.

enum class OpenClose : std::uint8_t {
  // Enter Order semantics
  DefaultForAccount = 0,
  Open              = 1,
  CloseNet          = 2,
  // Replace Order semantics
  ReplaceKeep       = 0,   // alias for the Replace field's "no change"
  ReplaceDefault    = 4,
};

constexpr bool is_valid_open_close_for_enter(std::uint8_t v) noexcept {
  return v == 0 || v == 1 || v == 2;
}

constexpr bool is_valid_open_close_for_replace(std::uint8_t v) noexcept {
  return v == 0 || v == 4;
}

// --- Client Category (OUCH "Type of client") ---------------------------------
//
// Note: OUCH spec says Client Category is not used by the derivatives market.

enum class ClientCategory : std::uint8_t {
  Client                = 1,
  House                 = 2,
  Fund                  = 7,
  InvestmentTrust       = 9,
  PrimaryDealerGovt     = 10,
  PrimaryDealerCorp     = 11,
  PortfolioMgmt         = 12,
};

// --- Order State (Accepted / Replaced reply) --------------------------------

enum class OrderState : std::uint8_t {
  OnBook              = 1,
  NotOnBook           = 2,
  Paused              = 98,
  OuchOwnershipLost   = 99,
};

// --- Cancel Reason (OrderCanceled.reason) -----------------------------------
//
// BISTECH OUCH Spec (Feb 2025) Table 10. Values not listed here are reserved
// and surface to the caller as the raw byte.

enum class CancelReason : std::uint8_t {
  CanceledByUser     = 1,
  Trade              = 3,
  Inactivate         = 4,
  ReplacedByUser     = 5,
  New                = 6,
  ConvertedBySystem  = 8,
  CanceledBySystem   = 9,
  CodInactivate      = 10,
  MarketHalt         = 11,
};

constexpr const char* cancel_reason_name(std::uint8_t v) noexcept {
  switch (static_cast<CancelReason>(v)) {
    case CancelReason::CanceledByUser:    return "canceled_by_user";
    case CancelReason::Trade:             return "trade";
    case CancelReason::Inactivate:        return "inactivate";
    case CancelReason::ReplacedByUser:    return "replaced_by_user";
    case CancelReason::New:               return "new";
    case CancelReason::ConvertedBySystem: return "converted_by_system";
    case CancelReason::CanceledBySystem:  return "canceled_by_system";
    case CancelReason::CodInactivate:     return "cod_inactivate";
    case CancelReason::MarketHalt:        return "market_halt";
  }
  return "unknown";
}

// --- MassQuote acknowledge status -------------------------------------------

enum class MassQuoteStatus : std::uint32_t {
  Accept             = 0,
  Updated            = 1,
  Canceled           = 2,
  UnsolicitedUpdate  = 3,
  UnsolicitedCancel  = 4,
  Traded             = 5,
};

// --- AFK / Agency-Fund Codes (BIST-specific values) -------------------------
//
// These are 3-character codes that fill OUCH Client/Account field for
// fund and market-maker orders. Plain `const char*` keeps the header
// usable from C++14 translation units (bist::fix's QuickFIX bridge).

namespace afk {
inline constexpr const char* Fund      = "XRM";
inline constexpr const char* MmClient  = "PYM";  // piyasa yapıcı müşteri
inline constexpr const char* MmHouse   = "PYP";  // piyasa yapıcı portföy
}  // namespace afk

// --- OrderToken --------------------------------------------------------------
//
// 14 bytes ASCII, left-justified, right-padded with spaces. Strongly typed
// wrapper enforces the encoding invariant and provides a printable view.

class OrderToken {
 public:
  static constexpr std::size_t kSize = 14;
  using Storage = std::array<char, kSize>;

  OrderToken() noexcept { storage_.fill(' '); }

  // Construct from ASCII; truncates or pads to 14 bytes. Caller is responsible
  // for token-uniqueness (per OUCH Spec, duplicate tokens reject -800002).
  explicit OrderToken(std::string_view s) noexcept {
    storage_.fill(' ');
    const std::size_t n = std::min(s.size(), kSize);
    for (std::size_t i = 0; i < n; ++i) storage_[i] = s[i];
  }

  [[nodiscard]] const Storage& bytes() const noexcept { return storage_; }
  [[nodiscard]] Storage&       bytes()       noexcept { return storage_; }

  // Trimmed printable view (without trailing pad spaces).
  [[nodiscard]] std::string_view view() const noexcept {
    std::size_t n = kSize;
    while (n > 0 && storage_[n - 1] == ' ') --n;
    return {storage_.data(), n};
  }

  friend bool operator==(const OrderToken&, const OrderToken&) = default;

 private:
  Storage storage_{};
};

}  // namespace bist
