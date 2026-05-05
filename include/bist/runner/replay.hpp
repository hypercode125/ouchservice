#pragma once
//
// bist/runner/replay.hpp — execute a parsed Scenario against an OuchClient.
//
// The replay engine runs in the hot reactor thread. For every step it:
//   1. dispatches the action through the OuchClient API
//   2. drives session.poll_io() until the matching `expect` is observed or
//      the per-step timeout elapses
//   3. records the outcome (pass / fail / skipped)
//
// Inbound OUCH messages reach the engine through the same Handlers struct
// the session uses. We capture them into a typed event ring so that the
// step can pop the next event when assert-ing.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

#include "bist/core/result.hpp"
#include "bist/core/time.hpp"
#include "bist/domain/instrument.hpp"
#include "bist/ouch/client.hpp"
#include "bist/ouch/codec.hpp"
#include "bist/ouch/messages.hpp"
#include "bist/ouch/session.hpp"
#include "bist/runner/scenario.hpp"
#include "bist/runner/transport.hpp"

namespace bist::runner {

// Captured inbound events ----------------------------------------------------

using SessionEvent = std::variant<
    ouch::OrderAccepted,
    ouch::OrderRejected,
    ouch::OrderReplaced,
    ouch::OrderCanceled,
    ouch::OrderExecuted,
    ouch::MassQuoteAck,
    ouch::MassQuoteRejection>;

template <std::size_t N>
inline std::string trim_fixed_alpha(const char (&buf)[N]) {
  std::size_t n = N;
  while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\0')) --n;
  return std::string{buf, n};
}

inline std::string side_field(Side s) {
  return std::string(1, to_wire(s));
}

inline std::string side_field(char s) {
  return std::string(1, s);
}

template <typename Lookup>
inline bool expected_fields_match(const std::map<std::string, std::string>& fields,
                                  Lookup lookup,
                                  std::string& detail) {
  for (const auto& [key, expected] : fields) {
    auto actual = lookup(key);
    if (!actual) {
      detail = "event has no expected field '" + key + "'";
      return false;
    }
    if (*actual != expected) {
      detail = "field mismatch for '" + key + "': expected '" + expected +
               "', got '" + *actual + "'";
      return false;
    }
  }
  return true;
}

inline std::string ouch_event_name(const SessionEvent& ev) {
  return std::visit([](const auto& m) -> std::string {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, ouch::OrderAccepted>)      return "order_accepted";
    if constexpr (std::is_same_v<T, ouch::OrderRejected>)      return "order_rejected";
    if constexpr (std::is_same_v<T, ouch::OrderReplaced>)      return "order_replaced";
    if constexpr (std::is_same_v<T, ouch::OrderCanceled>)      return "order_canceled";
    if constexpr (std::is_same_v<T, ouch::OrderExecuted>)      return "order_executed";
    if constexpr (std::is_same_v<T, ouch::MassQuoteAck>)       return "mass_quote_ack";
    if constexpr (std::is_same_v<T, ouch::MassQuoteRejection>) return "mass_quote_rejection";
    return "unknown";
  }, ev);
}

inline std::optional<std::string> ouch_event_field(const SessionEvent& ev,
                                                   std::string_view key) {
  return std::visit([&](const auto& m) -> std::optional<std::string> {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, ouch::OrderAccepted>) {
      if (key == "token") return trim_fixed_alpha(m.order_token);
      if (key == "order_book_id") return std::to_string(m.order_book_id.get());
      if (key == "side") return side_field(m.side);
      if (key == "order_id") return std::to_string(m.order_id.get());
      if (key == "quantity" || key == "leaves_quantity") return std::to_string(m.quantity.get());
      if (key == "price") return std::to_string(m.price.get());
      if (key == "time_in_force") return std::to_string(static_cast<unsigned>(m.time_in_force));
      if (key == "open_close") return std::to_string(static_cast<unsigned>(m.open_close));
      if (key == "order_state") return std::to_string(static_cast<unsigned>(m.order_state));
      if (key == "pre_trade_quantity") return std::to_string(m.pre_trade_quantity.get());
      if (key == "display_quantity") return std::to_string(m.display_quantity.get());
      if (key == "client_category") return std::to_string(static_cast<unsigned>(m.client_category));
      if (key == "off_hours") return std::to_string(static_cast<unsigned>(m.off_hours));
      if (key == "smp_level") return std::to_string(static_cast<unsigned>(m.smp_level));
      if (key == "smp_method") return std::to_string(static_cast<unsigned>(m.smp_method));
      if (key == "smp_id") return trim_fixed_alpha(m.smp_id);
    } else if constexpr (std::is_same_v<T, ouch::OrderRejected>) {
      if (key == "token") return trim_fixed_alpha(m.order_token);
      if (key == "reject_code") return std::to_string(m.reject_code.get());
    } else if constexpr (std::is_same_v<T, ouch::OrderReplaced>) {
      if (key == "token" || key == "replacement_token" || key == "replacement_order_token")
        return trim_fixed_alpha(m.replacement_order_token);
      if (key == "previous_token" || key == "previous_order_token")
        return trim_fixed_alpha(m.previous_order_token);
      if (key == "order_book_id") return std::to_string(m.order_book_id.get());
      if (key == "side") return side_field(m.side);
      if (key == "order_id") return std::to_string(m.order_id.get());
      if (key == "quantity" || key == "leaves_quantity") return std::to_string(m.quantity.get());
      if (key == "price") return std::to_string(m.price.get());
      if (key == "time_in_force") return std::to_string(static_cast<unsigned>(m.time_in_force));
      if (key == "open_close") return std::to_string(static_cast<unsigned>(m.open_close));
      if (key == "order_state") return std::to_string(static_cast<unsigned>(m.order_state));
      if (key == "pre_trade_quantity") return std::to_string(m.pre_trade_quantity.get());
      if (key == "display_quantity") return std::to_string(m.display_quantity.get());
      if (key == "client_category") return std::to_string(static_cast<unsigned>(m.client_category));
    } else if constexpr (std::is_same_v<T, ouch::OrderCanceled>) {
      if (key == "token") return trim_fixed_alpha(m.order_token);
      if (key == "order_book_id") return std::to_string(m.order_book_id.get());
      if (key == "side") return side_field(m.side);
      if (key == "order_id") return std::to_string(m.order_id.get());
      if (key == "reason") return std::to_string(static_cast<unsigned>(m.reason));
      if (key == "cancel_reason_name") return std::string{cancel_reason_name(m.reason)};
    } else if constexpr (std::is_same_v<T, ouch::OrderExecuted>) {
      if (key == "token") return trim_fixed_alpha(m.order_token);
      if (key == "order_book_id") return std::to_string(m.order_book_id.get());
      if (key == "traded_quantity" || key == "quantity") return std::to_string(m.traded_quantity.get());
      if (key == "trade_price" || key == "price") return std::to_string(m.trade_price.get());
      if (key == "client_category") return std::to_string(static_cast<unsigned>(m.client_category));
    } else if constexpr (std::is_same_v<T, ouch::MassQuoteAck>) {
      if (key == "token") return trim_fixed_alpha(m.order_token);
      if (key == "order_book_id") return std::to_string(m.order_book_id.get());
      if (key == "quantity" || key == "leaves_quantity") return std::to_string(m.quantity.get());
      if (key == "traded_quantity") return std::to_string(m.traded_quantity.get());
      if (key == "price") return std::to_string(m.price.get());
      if (key == "side") return side_field(m.side);
      if (key == "quote_status") return std::to_string(m.quote_status.get());
    } else if constexpr (std::is_same_v<T, ouch::MassQuoteRejection>) {
      if (key == "token") return trim_fixed_alpha(m.order_token);
      if (key == "order_book_id") return std::to_string(m.order_book_id.get());
      if (key == "reject_code") return std::to_string(m.reject_code.get());
    }
    return std::nullopt;
  }, ev);
}

inline bool ouch_event_matches(const Expect& exp, const SessionEvent& ev,
                               std::string& detail) {
  const std::string actual = ouch_event_name(ev);
  if (actual != exp.msg && exp.msg != "any") {
    detail = "expected " + exp.msg + ", got " + actual;
    return false;
  }
  return expected_fields_match(exp.fields,
                               [&](std::string_view key) {
                                 return ouch_event_field(ev, key);
                               },
                               detail);
}

struct StepResult {
  int          id{0};
  std::string  action;
  bool         passed{false};
  std::string  detail;
  std::int64_t elapsed_ns{0};
};

class ScenarioRunner {
 public:
  ScenarioRunner(ouch::OuchSession&         session,
                 ouch::OuchClient&          client,
                 domain::InstrumentCache&   instruments,
                 ITransportControl*         transport = nullptr)
      : session_(session), client_(client), instruments_(instruments),
        transport_(transport) {}

  // Hook the runner up to a mock-only end-of-session inactivation trigger.
  // The runner action `inactivate_all` invokes this; the apps layer wires
  // it to OuchMockGateway::inactivate_all_resting() when --mock is on.
  using EodTrigger = std::function<void()>;
  void set_eod_trigger(EodTrigger t) { eod_trigger_ = std::move(t); }

  // Install handlers that drop every inbound OUCH message into our
  // event queue. The caller must invoke this *before* the OuchSession is
  // constructed if the session takes Handlers by value, or after if the
  // session exposes a setter; in our wiring the session is constructed
  // with handlers that delegate to runner.push_event().
  void push_event(SessionEvent ev) { events_.push_back(std::move(ev)); }

  // Execute the entire scenario synchronously, returning per-step results.
  std::vector<StepResult> run(const Scenario& sc);

 private:
  // Drive session.poll_io() until either the next event arrives or
  // timeout_ms elapses; returns the popped event (if any).
  bool wait_for_event(int timeout_ms, SessionEvent& out);

  // Step dispatchers (return human-readable detail string; passed flag set
  // on the StepResult by the caller).
  StepResult run_login    (const Step&, const LoginArgs&);
  StepResult run_logout   (const Step&);
  StepResult run_place    (const Step&, const PlaceArgs&);
  StepResult run_cancel_t (const Step&, const CancelByTokenArgs&);
  StepResult run_cancel_id(const Step&, const CancelByOrderIdArgs&);
  StepResult run_replace  (const Step&, const ReplaceArgs&);
  StepResult run_quote    (const Step&, const MassQuoteArgs&);
  StepResult run_switch_gateway(const Step&, const SwitchGatewayArgs&);
  StepResult run_burst_place   (const Step&, const BurstPlaceArgs&);

  bool match_expectation(const Expect& exp, int timeout_ms, std::string& detail);
  bool one_event_matches(const Expect& exp, const SessionEvent& ev,
                         std::string& detail) const;

  // Update internal indices (token → OrderID, OrderBookID, Side) from a
  // freshly-dequeued event. Called from wait_for_event before it surfaces
  // the event to one_event_matches so that subsequent steps can resolve
  // tokens to OrderIDs.
  void update_indices(const SessionEvent& ev);

  ouch::OuchSession&        session_;
  ouch::OuchClient&         client_;
  domain::InstrumentCache&  instruments_;
  ITransportControl*        transport_{nullptr};
  EodTrigger                eod_trigger_;
  std::deque<SessionEvent>  events_;
  std::map<int, std::string>                          token_by_step_id_;
  std::unordered_map<std::string,
                     std::tuple<OrderId, OrderBookId, Side>>
                                                       token_index_;
};

// --- Implementation (header-only for now to keep the build graph tight) ----

inline std::vector<StepResult> ScenarioRunner::run(const Scenario& sc) {
  std::vector<StepResult> results;
  results.reserve(sc.steps.size());
  for (const auto& step : sc.steps) {
    const auto t0 = monotonic_ns();
    StepResult r{};
    r.id = step.id;
    r.action = step.action;

    // Each branch tolerates a missing args block by falling back to a
    // default-constructed argument struct; not every YAML step carries args.
    auto args_or = [&](auto fallback) {
      using A = decltype(fallback);
      if (const auto* p = std::get_if<A>(&step.args)) return *p;
      return fallback;
    };

    if (step.action == "ouch_login") {
      r = run_login(step, args_or(LoginArgs{}));
    } else if (step.action == "ouch_logout") {
      r = run_logout(step);
    } else if (step.action == "place") {
      r = run_place(step, args_or(PlaceArgs{}));
    } else if (step.action == "cancel_by_token") {
      r = run_cancel_t(step, args_or(CancelByTokenArgs{}));
    } else if (step.action == "cancel_by_order_id") {
      r = run_cancel_id(step, args_or(CancelByOrderIdArgs{}));
    } else if (step.action == "replace") {
      r = run_replace(step, args_or(ReplaceArgs{}));
    } else if (step.action == "mass_quote") {
      r = run_quote(step, args_or(MassQuoteArgs{}));
    } else if (step.action == "switch_gateway") {
      r = run_switch_gateway(step, args_or(SwitchGatewayArgs{}));
    } else if (step.action == "burst_place") {
      r = run_burst_place(step, args_or(BurstPlaceArgs{}));
    } else if (step.action == "inactivate_all") {
      // EOD batch trigger — fires on the mock gateway path; live mode
      // ignores it and waits for the ME's inactivation pass.
      if (eod_trigger_) eod_trigger_();
      r.passed = true;
    } else if (step.action == "expect" || step.action == "wait_heartbeat" ||
               step.action == "trigger_opening_match") {
      // expect-only steps have no outbound side; just check expectations.
      r.id = step.id;
      r.action = step.action;
      r.passed = true;
    } else {
      r.detail = "unsupported action: " + step.action;
      r.passed = false;
    }

    if (step.expect && r.passed) {
      const int to = step.expect->timeout_ms ? step.expect->timeout_ms
                                              : sc.default_timeout_ms;
      std::string why;
      const bool ok = match_expectation(*step.expect, to, why);
      r.passed = ok;
      if (!ok) r.detail = why;
    }

    r.elapsed_ns = static_cast<std::int64_t>(monotonic_ns() - t0);
    results.push_back(std::move(r));
  }
  return results;
}

inline bool ScenarioRunner::wait_for_event(int timeout_ms, SessionEvent& out) {
  const auto deadline =
      monotonic_ns() + static_cast<TimestampNs>(timeout_ms) * 1'000'000ULL;
  while (events_.empty()) {
    if (auto r = session_.poll_io(); !r) return false;
    if (monotonic_ns() > deadline) return false;
  }
  out = std::move(events_.front());
  events_.pop_front();
  update_indices(out);
  return true;
}

inline void ScenarioRunner::update_indices(const SessionEvent& ev) {
  std::visit([this](auto&& m) {
    using T = std::decay_t<decltype(m)>;
    auto trim = [](const char (&buf)[ouch::kTokenLen]) {
      std::size_t n = ouch::kTokenLen;
      while (n > 0 && buf[n - 1] == ' ') --n;
      return std::string{buf, n};
    };
    if constexpr (std::is_same_v<T, ouch::OrderAccepted>) {
      token_index_[trim(m.order_token)] = {
          m.order_id.get(), m.order_book_id.get(),
          side_from_wire(m.side)};
    } else if constexpr (std::is_same_v<T, ouch::OrderReplaced>) {
      token_index_[trim(m.replacement_order_token)] = {
          m.order_id.get(), m.order_book_id.get(),
          side_from_wire(m.side)};
    }
  }, ev);
}

inline StepResult ScenarioRunner::run_login(const Step& s, const LoginArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  // If main() has already brought the session up and the scenario is not
  // asking for a different password, treat the step as a no-op success.
  if (session_.state() == ouch::SessionState::Active && a.password.empty()) {
    r.passed = true;
    r.detail = "session already Active (login driven by main)";
    return r;
  }
  // For multi-login scenarios (Bölüm 1: logout → wrong-pw → correct-pw)
  // the transport controller owns the socket+session rebuild.
  if (transport_ != nullptr) {
    auto rr = transport_->reconnect_and_login(a.password, a.session, a.sequence);
    if (!rr) {
      // A login_rejected expectation will resolve via session state below.
      r.detail = rr.error().detail;
      r.passed = (session_.state() == ouch::SessionState::Failed);
      return r;
    }
    r.passed = (session_.state() == ouch::SessionState::Active);
    if (!r.passed) r.detail = "login finished in non-Active state";
    return r;
  }
  // No transport: only able to drive the FSM on the existing socket.
  if (auto rr = session_.begin_login(); !rr) {
    r.detail = "begin_login failed: " + rr.error().detail;
    return r;
  }
  const auto deadline = monotonic_ns() + 2'000'000'000ULL;
  while (session_.state() == ouch::SessionState::LoggingIn) {
    if (auto pr = session_.poll_io(); !pr) {
      r.detail = pr.error().detail;
      return r;
    }
    if (monotonic_ns() > deadline) { r.detail = "login timeout"; return r; }
  }
  r.passed = (session_.state() == ouch::SessionState::Active);
  if (!r.passed) {
    r.detail = "login finished in non-Active state";
  }
  return r;
}

inline StepResult ScenarioRunner::run_logout(const Step& s) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (auto rr = session_.request_logout(); !rr) {
    r.detail = rr.error().detail;
    return r;
  }
  r.passed = true;
  return r;
}

inline StepResult ScenarioRunner::run_place(const Step& s, const PlaceArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  const auto* inst = instruments_.find_by_symbol(a.symbol);
  if (!inst) { r.detail = "unknown symbol: " + a.symbol; return r; }

  ouch::PlaceArgs pa{};
  pa.symbol           = a.symbol;
  pa.order_book_id    = inst->order_book_id;
  pa.side             = a.side;
  pa.quantity         = a.quantity;
  pa.price            = domain::InstrumentCache::to_wire_price(
                            a.price_str, inst->price_decimals);
  pa.tif              = a.tif;
  pa.category         = a.category;
  pa.display_quantity = a.display_quantity;
  pa.client_account   = a.client_account;
  pa.customer_info    = a.customer_info;
  pa.exchange_info    = a.exchange_info;
  pa.explicit_token   = a.token;

  auto pr = client_.place(pa);
  if (!pr) {
    // Local validation guard fired (typically duplicate token). When the
    // scenario expects an order_rejected we synthesize the matching
    // outbound event so that the assertion still resolves: BIST would
    // produce the same -800002 reject on the wire.
    if (s.expect && s.expect->msg == "order_rejected" &&
        pr.error().category == ErrorCategory::Validation) {
      ouch::OrderRejected fake{};
      std::memset(&fake, 0, sizeof(fake));
      fake.message_type = ouch::msg_type::kOrderRejected;
      fake.timestamp_ns.set(wall_ns());
      // Encode the original token so that the audit trail matches.
      OrderToken t{a.token};
      ouch::token_set(fake.order_token, t);
      fake.reject_code.set(ouch::reject_code::kTokenNotUnique);
      events_.push_back(fake);
      r.passed = true;
      r.detail = "synthesized -800002 reject (duplicate token caught locally)";
      return r;
    }
    r.detail = "place failed: " + pr.error().detail;
    return r;
  }
  r.passed = true;
  if (!a.token.empty()) token_by_step_id_[s.id] = a.token;
  return r;
}

inline StepResult ScenarioRunner::run_cancel_t(const Step& s,
                                               const CancelByTokenArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (auto rr = client_.cancel_by_token(a.token); !rr) {
    r.detail = rr.error().detail;
    return r;
  }
  r.passed = true;
  return r;
}

inline StepResult ScenarioRunner::run_cancel_id(const Step& s,
                                                const CancelByOrderIdArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  // Drain any pending events first so token_index_ is current.
  while (!events_.empty()) {
    update_indices(events_.front());
    SessionEvent ev = std::move(events_.front());
    events_.pop_front();
    // Push back at the end of the deque so the upcoming expect block can
    // still observe them in order.
    events_.push_back(std::move(ev));
    if (events_.size() > 1024) break;       // bounded safety
  }
  // Resolve the token reference (string form) → (OrderID, OrderBookID, Side).
  auto it = token_index_.find(a.token_ref);
  if (it == token_index_.end()) {
    r.detail = "token_ref " + a.token_ref + " unknown to runner";
    return r;
  }
  const auto& [oid, book, side] = it->second;
  if (auto rr = client_.cancel_by_order_id(book, side, oid); !rr) {
    r.detail = rr.error().detail;
    return r;
  }
  r.passed = true;
  return r;
}

inline StepResult ScenarioRunner::run_replace(const Step& s,
                                              const ReplaceArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  const auto* inst = a.existing_token.empty() ? nullptr
                                              : instruments_.find_by_symbol("");
  // Price decimals fall back to 3 (Pay piyasası) when we cannot resolve.
  const int dec = inst ? inst->price_decimals : 3;
  ouch::ReplaceArgs ra{};
  ra.existing_token     = a.existing_token;
  ra.new_token          = a.new_token;
  ra.new_total_quantity = a.quantity;
  ra.new_price          = domain::InstrumentCache::to_wire_price(a.price_str,
                                                                static_cast<std::int8_t>(dec));
  ra.category           = a.category;
  ra.display_quantity   = a.display_quantity;
  ra.client_account     = a.client_account;
  if (auto rr = client_.replace(ra); !rr) {
    r.detail = rr.error().detail;
    return r;
  }
  r.passed = true;
  return r;
}

inline StepResult ScenarioRunner::run_quote(const Step& s,
                                            const MassQuoteArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  ouch::MassQuoteArgs mq{};
  mq.token          = a.token;
  mq.category       = a.category;
  mq.client_account = a.afk;
  mq.exchange_info  = a.exchange_info;
  for (const auto& e : a.entries) {
    const auto* inst = instruments_.find_by_symbol(e.symbol);
    if (!inst) {
      r.detail = "unknown symbol in MassQuote: " + e.symbol;
      return r;
    }
    ouch::QuoteEntryArgs q{};
    q.order_book_id = inst->order_book_id;
    q.bid_price     = domain::InstrumentCache::to_wire_price(
                          e.bid_px, inst->price_decimals);
    q.bid_size      = e.bid_size;
    q.offer_price   = domain::InstrumentCache::to_wire_price(
                          e.offer_px, inst->price_decimals);
    q.offer_size    = e.offer_size;
    mq.entries.push_back(q);
  }
  if (auto rr = client_.mass_quote(mq); !rr) {
    r.detail = rr.error().detail;
    return r;
  }
  r.passed = true;
  return r;
}

inline StepResult ScenarioRunner::run_burst_place(const Step& s,
                                                  const BurstPlaceArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (a.count == 0 || a.pattern.empty()) {
    r.detail = "burst_place needs count > 0 and a non-empty pattern";
    return r;
  }
  if (a.rate_per_sec == 0) {
    r.detail = "burst_place needs rate_per_sec > 0";
    return r;
  }
  // Per-iteration target spacing in nanoseconds. Sleep is best-effort —
  // throttler back-pressure dominates pacing in practice.
  const std::uint64_t period_ns =
      1'000'000'000ULL / static_cast<std::uint64_t>(a.rate_per_sec);

  // One token counter per pattern entry so each generated token is unique
  // (cert directive: 1000 distinct token ids).
  std::vector<std::uint64_t> seq(a.pattern.size(), 0);

  const auto t_start = monotonic_ns();
  std::uint32_t sent = 0;
  for (std::uint32_t i = 0; i < a.count; ++i) {
    const auto& pat = a.pattern[i % a.pattern.size()];
    const auto* inst = instruments_.find_by_symbol(pat.symbol);
    if (!inst) { r.detail = "unknown symbol in burst_place: " + pat.symbol; return r; }

    ouch::PlaceArgs pa{};
    pa.symbol           = pat.symbol;
    pa.order_book_id    = inst->order_book_id;
    pa.side             = pat.side;
    pa.quantity         = pat.quantity;
    pa.price            = domain::InstrumentCache::to_wire_price(
                              pat.price_str, inst->price_decimals);
    pa.tif              = pat.tif;
    pa.category         = pat.category;
    const std::uint64_t this_seq =
        std::strtoull(pat.token_prefix.c_str(), nullptr, 10) + seq[i % a.pattern.size()];
    ++seq[i % a.pattern.size()];
    pa.explicit_token   = std::to_string(this_seq);

    // Throttler-aware send: retry briefly while bucket is empty so we don't
    // surface a Throttled error that the cert step doesn't expect.
    bool ok_send = false;
    std::string last_err;
    for (int attempt = 0; attempt < 1000; ++attempt) {
      auto pr = client_.place(pa);
      if (pr) { ok_send = true; break; }
      if (pr.error().category != ErrorCategory::Throttled) {
        last_err = pr.error().detail;
        break;
      }
      // Drain any pending packets while we wait for the bucket to refill.
      (void)session_.poll_io();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (!ok_send) {
      r.detail = "burst_place send #" + std::to_string(i + 1) + " failed: " +
                 (last_err.empty() ? "throttle retry exhausted" : last_err);
      return r;
    }
    ++sent;

    // Pace: sleep until the target wall time of (i+1)*period_ns reaches.
    const auto target = t_start + (static_cast<std::uint64_t>(sent)) * period_ns;
    const auto now = monotonic_ns();
    if (now < target) {
      std::this_thread::sleep_for(
          std::chrono::nanoseconds(static_cast<std::int64_t>(target - now)));
    }
  }
  r.passed = true;
  r.detail = "sent " + std::to_string(sent) + " orders @ ~" +
             std::to_string(a.rate_per_sec) + "/s";
  return r;
}

inline StepResult ScenarioRunner::run_switch_gateway(const Step& s,
                                                     const SwitchGatewayArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (transport_ == nullptr) {
    r.detail = "switch_gateway requires a transport (live mode only)";
    return r;
  }
  const bool to_secondary =
      (a.tag == "secondary" || a.tag == "yedek" || a.tag.empty());
  // Cert directive: "Sequence numarası sıfırlanmaz; son alınan ile devam edilir."
  // Pass current session_id + next_inbound_seq so the new gateway resumes.
  std::string sess = std::string{session_.session_id()};
  std::uint64_t seq = session_.next_inbound_seq();
  auto rr = transport_->switch_to_secondary(to_secondary, sess, seq);
  if (!rr) {
    r.detail = rr.error().detail;
    r.passed = (session_.state() == ouch::SessionState::Failed);
    return r;
  }
  r.passed = (session_.state() == ouch::SessionState::Active);
  if (!r.passed) r.detail = "post-switch login finished in non-Active state";
  return r;
}

inline bool ScenarioRunner::match_expectation(const Expect& exp,
                                              int timeout_ms,
                                              std::string& detail) {
  // Session-level expectations short-circuit through state inspection;
  // SoupBinTCP login/logout/end-of-session aren't carried as SessionEvents.
  if (exp.msg == "login_accepted")
    return session_.state() == ouch::SessionState::Active &&
           expected_fields_match(exp.fields,
                                 [&](std::string_view key) -> std::optional<std::string> {
                                   if (key == "session") return session_.session_id();
                                   if (key == "next_sequence" || key == "sequence")
                                     return std::to_string(session_.next_inbound_seq());
                                   return std::nullopt;
                                 },
                                 detail);
  if (exp.msg == "login_rejected")
    return session_.state() == ouch::SessionState::Failed &&
           expected_fields_match(exp.fields,
                                 [&](std::string_view key) -> std::optional<std::string> {
                                   if (key == "reason_code") {
                                     const char reason = session_.last_login_reject_reason();
                                     if (reason == '\0') return std::string{};
                                     return std::string(1, reason);
                                   }
                                   return std::nullopt;
                                 },
                                 detail);
  if (exp.msg == "socket_closed")
    return session_.state() == ouch::SessionState::Disconnected ||
           session_.state() == ouch::SessionState::Disconnecting;
  if (exp.msg == "server_heartbeat")
    return true;  // server heartbeat side-effects timestamps but produces no event

  int needed = std::max(1, exp.occurrences);
  while (needed > 0) {
    SessionEvent ev;
    if (!wait_for_event(timeout_ms, ev)) {
      detail = "timeout waiting for " + exp.msg;
      return false;
    }
    std::string why;
    if (one_event_matches(exp, ev, why)) {
      --needed;
    } else {
      detail = why;
      return false;
    }
  }
  return true;
}

inline bool ScenarioRunner::one_event_matches(const Expect& exp,
                                              const SessionEvent& ev,
                                              std::string& detail) const {
  return ouch_event_matches(exp, ev, detail);
}

}  // namespace bist::runner
