#pragma once
//
// bist/domain/instrument.hpp — bidirectional Symbol ↔ OrderBookID map.
//
// The OUCH protocol takes orders against numeric OrderBookIDs whereas
// humans (and YAML scenarios) speak in symbols (ADEL.E, GARAN.E, …).
// We seed the cache from configuration at start-up and let FIX RD's
// Security Definition stream override / extend it at runtime.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bist/core/types.hpp"

namespace bist::domain {

struct Instrument {
  std::string  symbol;
  OrderBookId  order_book_id{0};
  std::int8_t  price_decimals{3};   // Pay piyasası default scale
  std::int32_t partition{1};        // 1..6 per BIST topology
  PriceInt     base_price{0};       // last known reference; nullable
};

class InstrumentCache {
 public:
  // Insert or replace by symbol; returns true if a new symbol was added,
  // false if an existing entry was overwritten.
  bool put(Instrument inst) {
    const auto symbol = inst.symbol;
    const auto book_id = inst.order_book_id;
    const bool inserted = by_symbol_.find(symbol) == by_symbol_.end();
    by_symbol_[symbol] = inst;
    by_book_id_[book_id] = std::move(inst);
    return inserted;
  }

  [[nodiscard]] const Instrument* find_by_symbol(std::string_view s) const {
    auto it = by_symbol_.find(std::string{s});
    return it == by_symbol_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] const Instrument* find_by_book_id(OrderBookId id) const {
    auto it = by_book_id_.find(id);
    return it == by_book_id_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::optional<OrderBookId>
  resolve(std::string_view symbol) const {
    if (const auto* i = find_by_symbol(symbol)) return i->order_book_id;
    return std::nullopt;
  }

  [[nodiscard]] std::size_t size() const noexcept { return by_symbol_.size(); }

  // Convert a TL price like "6.200" into the wire-level signed integer that
  // OUCH expects, applying the instrument's price_decimals scale. We use a
  // string parser rather than double precision so that prices remain exact
  // (a 6.200 TL price must reach the wire as exactly 6200 at decimals=3).
  [[nodiscard]] static PriceInt to_wire_price(std::string_view txt,
                                              std::int8_t decimals);

 private:
  std::unordered_map<std::string, Instrument>     by_symbol_;
  std::unordered_map<OrderBookId, Instrument>     by_book_id_;
};

inline PriceInt InstrumentCache::to_wire_price(std::string_view txt,
                                               std::int8_t decimals) {
  // Accepts "6.200", "5", "-0.010", "0.000". Rejects junk by best-effort.
  bool        negative = false;
  std::size_t i        = 0;
  if (!txt.empty() && (txt[0] == '+' || txt[0] == '-')) {
    negative = txt[0] == '-';
    ++i;
  }
  std::int64_t whole = 0;
  while (i < txt.size() && txt[i] != '.' && txt[i] != ',') {
    if (txt[i] < '0' || txt[i] > '9') return 0;
    whole = whole * 10 + (txt[i] - '0');
    ++i;
  }
  std::int64_t frac        = 0;
  std::int8_t  frac_digits = 0;
  if (i < txt.size() && (txt[i] == '.' || txt[i] == ',')) {
    ++i;
    while (i < txt.size() && frac_digits < decimals) {
      if (txt[i] < '0' || txt[i] > '9') break;
      frac = frac * 10 + (txt[i] - '0');
      ++frac_digits;
      ++i;
    }
  }
  while (frac_digits < decimals) {
    frac *= 10;
    ++frac_digits;
  }
  std::int64_t scale = 1;
  for (std::int8_t k = 0; k < decimals; ++k) scale *= 10;
  std::int64_t v = whole * scale + frac;
  if (negative) v = -v;
  return static_cast<PriceInt>(v);
}

}  // namespace bist::domain
