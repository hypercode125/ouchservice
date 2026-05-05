#pragma once
//
// bist/ouch/codec.hpp — encode/decode helpers around the wire structs.
//
// The structs in messages.hpp are byte-exact representations of the protocol.
// This header provides:
//   - alpha_set / alpha_get : ISO-8859-9 left-justified, space-padded fields
//   - hex_dump for audit and debugging
//   - cancel_one_side / cancel_both etc. for the MassQuote Quote Matrix
//
// The functions here are deliberately allocation-free in the hot path; they
// operate on user-provided buffers.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "bist/core/types.hpp"
#include "bist/ouch/messages.hpp"

namespace bist::ouch {

// --- Alpha field helpers -----------------------------------------------------
//
// OUCH "Alpha" fields are fixed-length, left-justified, right-padded with
// ASCII space (0x20). alpha_set copies up to N bytes and pads the rest;
// alpha_get returns a string_view that omits trailing pad characters.
//
// We deliberately do NOT validate against the ISO-8859-9 character set here:
// callers feeding raw ASCII (which is a strict subset) are correct by
// construction. If non-ASCII content ever needs to flow through these
// fields, a separate transcoder belongs at the call site.

template <std::size_t N>
inline void alpha_set(char (&dst)[N], std::string_view src) noexcept {
  const std::size_t n = std::min(src.size(), N);
  for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
  for (std::size_t i = n; i < N; ++i) dst[i] = ' ';
}

template <std::size_t N>
[[nodiscard]] inline std::string_view alpha_get(const char (&src)[N]) noexcept {
  std::size_t n = N;
  while (n > 0 && src[n - 1] == ' ') --n;
  return {src, n};
}

inline void token_set(char (&dst)[kTokenLen], const OrderToken& tok) noexcept {
  for (std::size_t i = 0; i < kTokenLen; ++i) dst[i] = tok.bytes()[i];
}

[[nodiscard]] inline OrderToken token_get(const char (&src)[kTokenLen]) noexcept {
  OrderToken t;
  for (std::size_t i = 0; i < kTokenLen; ++i) t.bytes()[i] = src[i];
  return t;
}

// --- Hex dump helper ---------------------------------------------------------
//
// Used by the audit log to record every sent/received message. Output format:
//   "4f30303031..." (lowercase hex, no separators, no prefix).

[[nodiscard]] inline std::string hex_dump(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i]     = kDigits[(bytes[i] >> 4) & 0x0F];
    out[2 * i + 1] = kDigits[ bytes[i]       & 0x0F];
  }
  return out;
}

// --- MassQuote builders ------------------------------------------------------
//
// The Quote Matrix (Spec Appendix B) is encoded in the values of bid/offer
// price and size: zero on a side means "cancel" or "leave unchanged"
// depending on context. These helpers express the intent at the call site.

inline void quote_new_two_sided(QuoteEntry& e, OrderBookId book,
                                PriceInt bid_px, Quantity bid_size,
                                PriceInt offer_px, Quantity offer_size) noexcept {
  e.order_book_id.set(book);
  e.bid_price.set(bid_px);
  e.bid_size.set(static_cast<std::uint64_t>(bid_size));
  e.offer_price.set(offer_px);
  e.offer_size.set(static_cast<std::uint64_t>(offer_size));
}

inline void quote_cancel_two_sided(QuoteEntry& e, OrderBookId book) noexcept {
  e.order_book_id.set(book);
  e.bid_price.set(0);
  e.bid_size.set(0);
  e.offer_price.set(0);
  e.offer_size.set(0);
}

inline void quote_cancel_bid(QuoteEntry& e, OrderBookId book,
                             PriceInt offer_px, Quantity offer_size) noexcept {
  e.order_book_id.set(book);
  e.bid_price.set(0);
  e.bid_size.set(0);
  e.offer_price.set(offer_px);
  e.offer_size.set(static_cast<std::uint64_t>(offer_size));
}

inline void quote_cancel_offer(QuoteEntry& e, OrderBookId book,
                               PriceInt bid_px, Quantity bid_size) noexcept {
  e.order_book_id.set(book);
  e.bid_price.set(bid_px);
  e.bid_size.set(static_cast<std::uint64_t>(bid_size));
  e.offer_price.set(0);
  e.offer_size.set(0);
}

}  // namespace bist::ouch
