#pragma once
//
// bist/ouch/client.hpp — high-level convenience API over OuchSession.
//
// Responsibilities:
//   - allocate unique 14-byte tokens via TokenRegistry
//   - apply the local Throttler before every send
//   - track local OrderBook state so that callers can ask for "what is the
//     current leaves of token=30?" without round-tripping the gateway
//   - assemble dynamic Mass Quote messages with an in-place buffer
//
// The client owns the *behaviour*; the session owns the *protocol*. Wiring
// the two together makes the call sites read like a plain trading API.

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bist/core/result.hpp"
#include "bist/domain/order_book.hpp"
#include "bist/domain/throttler.hpp"
#include "bist/domain/token_registry.hpp"
#include "bist/ouch/codec.hpp"
#include "bist/ouch/messages.hpp"
#include "bist/ouch/session.hpp"

namespace bist::ouch {

struct PlaceArgs {
  std::string    symbol;            // human-readable, for local order book
  OrderBookId    order_book_id{0};
  Side           side{Side::Buy};
  Quantity       quantity{0};
  PriceInt       price{0};
  TimeInForce    tif{TimeInForce::Day};
  OpenClose      open_close{OpenClose::DefaultForAccount};
  ClientCategory category{ClientCategory::Client};
  Quantity       display_quantity{0};      // 0 unless iceberg/reserve
  std::string    client_account;           // AFK code: XRM, PYM, PYP, …
  std::string    customer_info;
  std::string    exchange_info;
  // If set, the client will use this token verbatim and skip auto-allocation.
  // Useful for cert scenarios that pin tokens to specific integers.
  std::string    explicit_token;
  std::uint8_t   off_hours{0};
};

struct ReplaceArgs {
  std::string    existing_token;
  std::string    new_token;            // optional; auto-allocated if empty
  Quantity       new_total_quantity{0};
  PriceInt       new_price{0};         // 0 = "no change" per Spec 4.1.2.2
  // Replace-form Open/Close (Spec 4.1.2.2):
  //   ReplaceKeep    (0) — keep the original order's Open/Close flag
  //   ReplaceDefault (4) — explicitly reset to the account default
  // Open (1) and CloseNet (2) are illegal in Replace and are runtime-rejected
  // by OuchClient::replace.
  OpenClose      open_close{OpenClose::ReplaceKeep};
  ClientCategory category{ClientCategory::Client};
  Quantity       display_quantity{0};
  std::string    client_account;
  std::string    customer_info;
  std::string    exchange_info;
};

struct QuoteEntryArgs {
  OrderBookId order_book_id{0};
  PriceInt    bid_price{0};
  Quantity    bid_size{0};
  PriceInt    offer_price{0};
  Quantity    offer_size{0};
};

struct MassQuoteArgs {
  std::string                  token;          // optional
  ClientCategory               category{ClientCategory::Client};
  std::string                  client_account; // AFK PYM/PYP
  std::string                  exchange_info;  // narrowed to 16 bytes per Table 6
  std::vector<QuoteEntryArgs>  entries;
};

class OuchClient {
 public:
  OuchClient(OuchSession& session, domain::TokenRegistry& tokens,
             domain::Throttler& throttler, domain::OrderBook& book)
      : session_(session), tokens_(tokens),
        throttler_(throttler), book_(book) {}

  // --- Enter Order ----------------------------------------------------------

  Result<OrderToken> place(const PlaceArgs& args) {
    if (!throttler_.try_acquire()) {
      return make_error(ErrorCategory::Throttled,
                        "throttler bucket empty (>= rate)");
    }
    OrderToken tok;
    if (!args.explicit_token.empty()) {
      if (!tokens_.register_external(args.explicit_token)) {
        return make_error(ErrorCategory::Validation,
                          "token already used: " + args.explicit_token);
      }
      tok = OrderToken{args.explicit_token};
    } else {
      tok = tokens_.allocate();
    }

    EnterOrder eo{};
    std::memset(&eo, 0, sizeof(eo));
    eo.message_type = msg_type::kEnterOrder;
    token_set(eo.order_token, tok);
    eo.order_book_id.set(args.order_book_id);
    eo.side = to_wire(args.side);
    eo.quantity.set(static_cast<std::uint64_t>(args.quantity));
    eo.price.set(args.price);
    eo.time_in_force = static_cast<std::uint8_t>(args.tif);
    eo.open_close    = static_cast<std::uint8_t>(args.open_close);
    alpha_set(eo.client_account, args.client_account);
    alpha_set(eo.customer_info,  args.customer_info);
    alpha_set(eo.exchange_info,  args.exchange_info);
    eo.display_quantity.set(static_cast<std::uint64_t>(args.display_quantity));
    eo.client_category = static_cast<std::uint8_t>(args.category);
    eo.off_hours       = args.off_hours;

    domain::OrderState s{};
    s.symbol      = args.symbol;
    s.side        = args.side;
    s.original_qty = args.quantity;
    s.price       = args.price;
    s.tif         = args.tif;
    s.status      = domain::OrderStatus::Pending;
    book_.insert(tok.view(), std::move(s));

    if (auto r = session_.send_enter_order(eo); !r) return r.error();
    return tok;
  }

  // --- Replace Order --------------------------------------------------------

  Result<OrderToken> replace(const ReplaceArgs& args) {
    // Spec 4.1.2.2: Replace Order's Open/Close field accepts only
    //   0 (ReplaceKeep) — keep original Open/Close
    //   4 (ReplaceDefault) — reset to account default
    // Enter-Order values Open(1) and CloseNet(2) are illegal here; reject
    // before consuming a throttle slot or allocating a token so the caller
    // sees a clean Validation error instead of a wire reject from the ME.
    if (!is_valid_open_close_for_replace(
            static_cast<std::uint8_t>(args.open_close))) {
      return make_error(
          ErrorCategory::Validation,
          "OpenClose value illegal in Replace per Spec 4.1.2.2 "
          "(allowed: ReplaceKeep=0, ReplaceDefault=4)");
    }
    if (!throttler_.try_acquire()) {
      return make_error(ErrorCategory::Throttled, "throttler empty");
    }
    if (args.existing_token.empty()) {
      return make_error(ErrorCategory::Validation,
                        "existing_token required");
    }
    OrderToken next;
    if (!args.new_token.empty()) {
      if (!tokens_.register_external(args.new_token)) {
        return make_error(ErrorCategory::Validation,
                          "new_token already used: " + args.new_token);
      }
      next = OrderToken{args.new_token};
    } else {
      next = tokens_.allocate();
    }

    ReplaceOrder ro{};
    std::memset(&ro, 0, sizeof(ro));
    ro.message_type = msg_type::kReplaceOrder;
    OrderToken existing{args.existing_token};
    token_set(ro.existing_order_token,    existing);
    token_set(ro.replacement_order_token, next);
    ro.quantity.set(static_cast<std::uint64_t>(args.new_total_quantity));
    ro.price.set(args.new_price);
    ro.open_close = static_cast<std::uint8_t>(args.open_close);
    alpha_set(ro.client_account, args.client_account);
    alpha_set(ro.customer_info,  args.customer_info);
    alpha_set(ro.exchange_info,  args.exchange_info);
    ro.display_quantity.set(static_cast<std::uint64_t>(args.display_quantity));
    ro.client_category = static_cast<std::uint8_t>(args.category);

    if (auto* s = book_.find_mut(args.existing_token)) {
      s->status = domain::OrderStatus::Replacing;
    }
    if (auto r = session_.send_replace_order(ro); !r) return r.error();
    return next;
  }

  // --- Cancel ---------------------------------------------------------------

  Result<void> cancel_by_token(std::string_view token) {
    if (!throttler_.try_acquire()) {
      return make_error(ErrorCategory::Throttled, "throttler empty");
    }
    CancelOrder co{};
    co.message_type = msg_type::kCancelOrder;
    OrderToken t{token};
    token_set(co.order_token, t);
    if (auto* s = book_.find_mut(token)) s->status = domain::OrderStatus::Canceling;
    return session_.send_cancel(co);
  }

  Result<void> cancel_by_order_id(OrderBookId book, Side side, OrderId id) {
    if (!throttler_.try_acquire()) {
      return make_error(ErrorCategory::Throttled, "throttler empty");
    }
    CancelByOrderId msg{};
    msg.message_type = msg_type::kCancelByOrderId;
    msg.order_book_id.set(book);
    msg.side = to_wire(side);
    msg.order_id.set(id);
    return session_.send_cancel_by_id(msg);
  }

  // --- Mass Quote -----------------------------------------------------------
  //
  // Builds a dynamically-sized Mass Quote in a thread-local scratch buffer
  // and sends it in one shot. Caller is responsible for staying within the
  // 5-quote ceiling enforced by the spec.

  Result<OrderToken> mass_quote(const MassQuoteArgs& args) {
    if (args.entries.empty() || args.entries.size() > kMaxQuoteEntries) {
      return make_error(ErrorCategory::Validation,
                        "MassQuote requires 1..5 entries");
    }
    if (!throttler_.try_acquire()) {
      return make_error(ErrorCategory::Throttled, "throttler empty");
    }

    OrderToken tok = args.token.empty()
                         ? tokens_.allocate()
                         : OrderToken{args.token};
    if (!args.token.empty() && !tokens_.register_external(args.token)) {
      return make_error(ErrorCategory::Validation,
                        "MassQuote token already used: " + args.token);
    }

    const std::size_t total =
        kMassQuoteHdrLen + kQuoteEntryLen * args.entries.size();
    std::vector<std::uint8_t> blob(total);

    auto* hdr = reinterpret_cast<MassQuoteHeader*>(blob.data());
    std::memset(hdr, 0, sizeof(MassQuoteHeader));
    hdr->message_type = msg_type::kMassQuote;
    token_set(hdr->order_token, tok);
    hdr->client_category = static_cast<std::uint8_t>(args.category);
    alpha_set(hdr->client_account, args.client_account);
    alpha_set(hdr->exchange_info,  args.exchange_info);
    hdr->no_quote_entries.set(
        static_cast<std::uint16_t>(args.entries.size()));

    auto* entries =
        reinterpret_cast<QuoteEntry*>(blob.data() + kMassQuoteHdrLen);
    for (std::size_t i = 0; i < args.entries.size(); ++i) {
      const auto& a = args.entries[i];
      QuoteEntry& e = entries[i];
      e.order_book_id.set(a.order_book_id);
      e.bid_price.set(a.bid_price);
      e.bid_size.set(static_cast<std::uint64_t>(a.bid_size));
      e.offer_price.set(a.offer_price);
      e.offer_size.set(static_cast<std::uint64_t>(a.offer_size));
    }

    if (auto r = session_.send_mass_quote(
            std::span<const std::uint8_t>{blob.data(), blob.size()});
        !r) {
      return r.error();
    }
    return tok;
  }

 private:
  OuchSession&            session_;
  domain::TokenRegistry&  tokens_;
  domain::Throttler&      throttler_;
  domain::OrderBook&      book_;
};

}  // namespace bist::ouch
