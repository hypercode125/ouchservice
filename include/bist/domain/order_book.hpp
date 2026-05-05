#pragma once
//
// bist/domain/order_book.hpp — local view of the orders we have entered.
//
// Used to answer questions like "what is the executed quantity of
// token=30?" so that replace requests carry the correct total quantity per
// OUCH Spec 4.1.2.1.  The replace_total_qty helper computes the leaves
// the system will report:
//
//      leaves = max(0, new_total - executed)
//
// If leaves drops to zero the order is removed (Order State NotOnBook = 2).

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bist/core/types.hpp"

namespace bist::domain {

enum class OrderStatus : std::uint8_t {
  Pending,       // sent, awaiting Order Accepted
  OnBook,        // accepted, currently resting
  Replacing,     // replace request in flight
  Canceling,     // cancel request in flight
  NotOnBook,     // replaced or canceled or fully filled
  Rejected,      // peer rejected
};

struct OrderState {
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     original_qty{0};
  Quantity     leaves{0};
  Quantity     executed{0};
  PriceInt     price{0};
  TimeInForce  tif{TimeInForce::Day};
  OrderId      order_id{0};
  OrderStatus  status{OrderStatus::Pending};
};

class OrderBook {
 public:
  void insert(std::string_view token, OrderState state) {
    state.leaves = state.original_qty;
    states_[std::string{token}] = std::move(state);
  }

  [[nodiscard]] const OrderState* find(std::string_view token) const {
    const auto it = states_.find(std::string{token});
    return it == states_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] OrderState* find_mut(std::string_view token) {
    auto it = states_.find(std::string{token});
    return it == states_.end() ? nullptr : &it->second;
  }

  void on_accepted(std::string_view token, OrderId id, Quantity open_qty) {
    if (auto* s = find_mut(token)) {
      s->order_id = id;
      s->leaves   = open_qty;
      s->status   = OrderStatus::OnBook;
    }
  }

  void on_executed(std::string_view token, Quantity traded) {
    if (auto* s = find_mut(token)) {
      s->executed += traded;
      s->leaves = std::max<Quantity>(0, s->leaves - traded);
      if (s->leaves == 0) s->status = OrderStatus::NotOnBook;
    }
  }

  void on_canceled(std::string_view token) {
    if (auto* s = find_mut(token)) {
      s->leaves = 0;
      s->status = OrderStatus::NotOnBook;
    }
  }

  // Compute the leaves that the gateway will report after a Replace where
  // the client wants the order book quantity to become `desired_open`. Per
  // OUCH Spec 4.1.2.1 the wire-level quantity field is desired_open + already
  // executed; the gateway then computes leaves = max(0, total - executed).
  [[nodiscard]] static Quantity expected_leaves(Quantity new_total_qty,
                                                Quantity already_executed) noexcept {
    const Quantity diff = new_total_qty - already_executed;
    return diff < 0 ? 0 : diff;
  }

  [[nodiscard]] std::size_t size() const noexcept { return states_.size(); }

 private:
  std::unordered_map<std::string, OrderState> states_;
};

}  // namespace bist::domain
