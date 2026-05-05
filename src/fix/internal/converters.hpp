#pragma once
//
// src/fix/internal/converters.hpp — translation helpers between bist::fix
// POD types and QuickFIX types. Compiled exclusively from .cpp files in
// src/fix/, all of which run at -std=gnu++14 so they can include the
// QuickFIX headers (auto_ptr, dynamic-exception-spec, etc).
//
// Only this directory is allowed to include QuickFIX. Treat any include of
// `<quickfix/...>` outside src/fix/internal/ as a build-rule violation.

#include <string>

#include "bist/core/types.hpp"
#include "bist/fix/facade.hpp"
#include "bist/fix/fields.hpp"

namespace bist::fix::detail {

inline char fix_side(Side s) {
  switch (s) {
    case Side::Buy:       return side::Buy;
    case Side::Sell:      return side::Sell;
    case Side::ShortSell: return side::SellShort;
  }
  return side::Buy;
}

inline Side bist_side(char c) {
  switch (c) {
    case side::Buy:       return Side::Buy;
    case side::Sell:      return Side::Sell;
    case side::SellShort: return Side::ShortSell;
  }
  return Side::Buy;
}

inline char fix_ord_type(OrdType t) {
  switch (t) {
    case OrdType::Limit:           return ord_type::Limit;
    case OrdType::Market:          return ord_type::Market;
    case OrdType::MarketToLimit:   return ord_type::MarketWithLeftover;
    case OrdType::Imbalance:       return ord_type::Imbalance;
    case OrdType::MidpointLimit:   return ord_type::MidpointLimit;
    case OrdType::MidpointMarket:  return ord_type::MidpointMarket;
  }
  return ord_type::Limit;
}

inline char fix_tif(TimeInForce t) {
  switch (t) {
    case TimeInForce::Day:                return tif::Day;
    case TimeInForce::ImmediateOrCancel:  return tif::ImmediateOrCancel;
    case TimeInForce::FillOrKill:         return tif::FillOrKill;
  }
  return tif::Day;
}

inline double to_fix_price(PriceInt v, std::int8_t decimals) {
  double scale = 1;
  for (std::int8_t i = 0; i < decimals; ++i) scale *= 10.0;
  return static_cast<double>(v) / scale;
}

inline PriceInt from_fix_price(double v, std::int8_t decimals) {
  double scale = 1;
  for (std::int8_t i = 0; i < decimals; ++i) scale *= 10.0;
  return static_cast<PriceInt>(v * scale + (v >= 0 ? 0.5 : -0.5));
}

}  // namespace bist::fix::detail
