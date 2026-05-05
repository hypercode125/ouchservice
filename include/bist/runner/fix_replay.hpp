#pragma once
//
// bist/runner/fix_replay.hpp — execute a parsed Scenario against the FIX
// facade (OeClient / RdClient / DcClient).
//
// Mirrors the shape of bist/runner/replay.hpp but talks the POD facade in
// bist/fix/facade.hpp instead of OuchClient. FIX facade callbacks fire on
// QuickFIX's internal thread, so the event queue is mutex-guarded.
//
// Per-step dispatch:
//   fix_logon              -> drives the underlying initiator handshake
//                             (the password rotation flow is performed by
//                             the QuickFIX Application via 554/925, see
//                             facade Impl); blocks until LogonResult arrives.
//   fix_logout             -> OeClient::logout() / RdClient::logout()
//   fix_amr_subscribe      -> RdClient::subscribe_all()
//   fix_place / fix_cancel / fix_replace
//                          -> OeClient::place / cancel / replace
//   fix_wait_heartbeat     -> server_heartbeat is a passive expect; we just
//                             sleep until timeout to observe one cycle.
//   trigger_match          -> live-only no-op (mock injects synthetic ER).
//
// `expect` strings recognised here:
//   fix_logon_response, fix_logout_complete, fix_execution_report,
//   fix_amr_ack, server_heartbeat.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "bist/core/result.hpp"
#include "bist/core/time.hpp"
#include "bist/fix/facade.hpp"
#include "bist/runner/replay.hpp"      // re-uses StepResult
#include "bist/runner/scenario.hpp"

namespace bist::runner {

// FIX-side captured events --------------------------------------------------

struct AmrAck {
  bool        ok{false};
  std::string detail;
};

using FixEvent = std::variant<
    fix::LogonResult,
    fix::ExecutionReportEvent,
    fix::CancelRejectEvent,
    fix::TradeReportEvent,
    fix::QuoteStatusEvent,
    AmrAck>;

inline std::string fix_session_state_field(fix::FixSessionState s) {
  return std::to_string(static_cast<unsigned>(s));
}

inline std::string fix_event_name(const FixEvent& ev) {
  return std::visit([](const auto& m) -> std::string {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, fix::LogonResult>)          return "fix_logon_response";
    if constexpr (std::is_same_v<T, fix::ExecutionReportEvent>) return "fix_execution_report";
    if constexpr (std::is_same_v<T, fix::CancelRejectEvent>)    return "fix_cancel_reject";
    if constexpr (std::is_same_v<T, fix::TradeReportEvent>)     return "fix_trade_report";
    if constexpr (std::is_same_v<T, fix::QuoteStatusEvent>)     return "fix_quote_status";
    if constexpr (std::is_same_v<T, AmrAck>)                    return "fix_amr_ack";
    return "unknown";
  }, ev);
}

inline std::optional<std::string> fix_event_field(const FixEvent& ev,
                                                  std::string_view key) {
  return std::visit([&](const auto& m) -> std::optional<std::string> {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, fix::LogonResult>) {
      if (key == "SessionStatus" || key == "session_status") return std::to_string(m.session_status);
      if (key == "state") return fix_session_state_field(m.state);
      if (key == "Text" || key == "detail") return m.detail;
    } else if constexpr (std::is_same_v<T, fix::ExecutionReportEvent>) {
      if (key == "ClOrdID" || key == "cl_ord_id") return m.cl_ord_id;
      if (key == "OrigClOrdID" || key == "orig_cl_ord_id") return m.orig_cl_ord_id;
      if (key == "ExecID" || key == "exec_id") return m.exec_id;
      if (key == "OrderID" || key == "order_id") return m.order_id;
      if (key == "Symbol" || key == "symbol") return m.symbol;
      if (key == "Side" || key == "side") return side_field(m.side);
      if (key == "ExecType" || key == "exec_type") return std::string(1, m.exec_type);
      if (key == "OrdStatus" || key == "ord_status") return std::string(1, m.ord_status);
      if (key == "LeavesQty" || key == "leaves_qty") return std::to_string(m.leaves_qty);
      if (key == "CumQty" || key == "cum_qty") return std::to_string(m.cum_qty);
      if (key == "LastQty" || key == "last_qty") return std::to_string(m.last_qty);
      if (key == "LastPx" || key == "last_price" || key == "Price" || key == "price")
        return std::to_string(m.last_price);
      if (key == "Text" || key == "text") return m.text;
      // BIST cert: short-sell ER carries TrdType(828)=2 + Text=short_sell.
      // Expose `trd_type` as a stringly-typed alias so YAMLs can assert it
      // without depending on FIX numeric tag values directly.
      if (key == "trd_type" || key == "TrdType" || key == "trade_type") {
        if (m.text.find("short_sell") != std::string::npos) return std::string{"short_sell"};
        return std::string{};
      }
    } else if constexpr (std::is_same_v<T, fix::CancelRejectEvent>) {
      if (key == "ClOrdID" || key == "cl_ord_id") return m.cl_ord_id;
      if (key == "OrigClOrdID" || key == "orig_cl_ord_id") return m.orig_cl_ord_id;
      if (key == "Text" || key == "text") return m.text;
    } else if constexpr (std::is_same_v<T, fix::TradeReportEvent>) {
      if (key == "TradeReportID" || key == "trade_report_id") return m.trade_report_id;
      if (key == "TradeReportRefID" || key == "trade_report_ref_id") return m.trade_report_ref_id;
      if (key == "Symbol" || key == "symbol") return m.symbol;
      if (key == "Side" || key == "side") return side_field(m.side);
      if (key == "LastQty" || key == "quantity" || key == "last_qty") return std::to_string(m.quantity);
      if (key == "LastPx" || key == "price" || key == "last_price") return std::to_string(m.price);
      if (key == "CounterpartyID" || key == "counterparty_id") return m.counterparty_id;
    } else if constexpr (std::is_same_v<T, fix::QuoteStatusEvent>) {
      if (key == "QuoteID" || key == "quote_id") return m.quote_id;
      if (key == "Symbol" || key == "symbol") return m.symbol;
      if (key == "Side" || key == "side") return side_field(m.side);
      if (key == "QuoteStatus" || key == "quote_status") return std::to_string(m.quote_status);
      if (key == "LeavesQty" || key == "leaves_qty") return std::to_string(m.leaves_qty);
      if (key == "Price" || key == "price") return std::to_string(m.price);
    } else if constexpr (std::is_same_v<T, AmrAck>) {
      if (key == "ok") return m.ok ? "true" : "false";
      if (key == "detail" || key == "Text") return m.detail;
    }
    return std::nullopt;
  }, ev);
}

inline bool fix_event_matches(const Expect& exp, const FixEvent& ev,
                              std::string& detail) {
  const std::string actual = fix_event_name(ev);
  if (actual != exp.msg && exp.msg != "any") {
    detail = "expected " + exp.msg + ", got " + actual;
    return false;
  }
  return expected_fields_match(exp.fields,
                               [&](std::string_view key) {
                                 return fix_event_field(ev, key);
                               },
                               detail);
}

class FixScenarioRunner {
 public:
  // The runner does not own the clients; main() owns them so that lifetime
  // matches the scenario-block. Either client may be null when the scenario
  // doesn't need that side (e.g. RD-only or OE-only YAMLs).
  FixScenarioRunner(fix::OeClient* oe,
                    fix::RdClient* rd,
                    fix::DcClient* dc) noexcept
      : oe_(oe), rd_(rd), dc_(dc) {}

  void set_clients(fix::OeClient* oe,
                   fix::RdClient* rd,
                   fix::DcClient* dc) noexcept {
    oe_ = oe; rd_ = rd; dc_ = dc;
  }

  // ---- callbacks (thread-safe; invoked from QuickFIX internal thread) ----
  void on_oe_session(const fix::LogonResult& r)             { push(r); }
  void on_oe_execution(const fix::ExecutionReportEvent& e)  { push(e); }
  void on_oe_cancel_reject(const fix::CancelRejectEvent& e) { push(e); }
  void on_oe_trade_report(const fix::TradeReportEvent& e)   { push(e); }
  void on_rd_session(const fix::LogonResult& r)             { push(r); }
  void on_rd_amr_ack(bool ok, std::string d)                { push(AmrAck{ok, std::move(d)}); }
  void on_dc_session(const fix::LogonResult& r)             { push(r); }
  void on_dc_execution(const fix::ExecutionReportEvent& e)  { push(e); }
  void on_dc_quote_status(const fix::QuoteStatusEvent& e)   { push(e); }

  std::vector<StepResult> run(const Scenario& sc);

 private:
  void push(FixEvent ev) {
    {
      std::lock_guard<std::mutex> g(mu_);
      events_.push_back(std::move(ev));
    }
    cv_.notify_one();
  }

  bool wait_for_event(int timeout_ms, FixEvent& out) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [this] { return !events_.empty(); })) {
      return false;
    }
    out = std::move(events_.front());
    events_.pop_front();
    return true;
  }

  StepResult run_logon (const Step&, const FixLogonArgs&);
  StepResult run_logout(const Step&);
  StepResult run_amr   (const Step&, const FixAmrSubscribeArgs&);
  StepResult run_wait_hb(const Step&, const FixWaitHeartbeatArgs&);
  StepResult run_place (const Step&, const FixPlaceArgs&);
  StepResult run_cancel(const Step&, const FixCancelArgs&);
  StepResult run_replace(const Step&, const FixReplaceArgs&);

  bool match_expectation(const Expect& exp, int timeout_ms, std::string& detail);
  bool one_event_matches(const Expect& exp, const FixEvent& ev,
                         std::string& detail) const;

  static fix::OrdType ord_type_from(const std::string& s) {
    if (s == "MARKET")           return fix::OrdType::Market;
    if (s == "MTL")              return fix::OrdType::MarketToLimit;
    if (s == "IMBALANCE")        return fix::OrdType::Imbalance;
    if (s == "MIDPOINT_LIMIT")   return fix::OrdType::MidpointLimit;
    if (s == "MIDPOINT_MARKET")  return fix::OrdType::MidpointMarket;
    return fix::OrdType::Limit;
  }
  static PriceInt parse_price(const std::string& s, std::int8_t decimals) {
    if (s.empty()) return 0;
    bool neg = false;
    std::size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; }
    long long whole = 0, frac = 0;
    int frac_digits = 0;
    bool seen_dot = false;
    for (; i < s.size(); ++i) {
      char c = s[i];
      if (c == '.') { seen_dot = true; continue; }
      if (c < '0' || c > '9') return 0;
      if (!seen_dot) whole = whole * 10 + (c - '0');
      else { frac = frac * 10 + (c - '0'); ++frac_digits; }
    }
    long long pow10 = 1;
    for (std::int8_t d = 0; d < decimals; ++d) pow10 *= 10;
    while (frac_digits < decimals) { frac *= 10; ++frac_digits; }
    while (frac_digits > decimals) { frac /= 10; --frac_digits; }
    long long v = whole * pow10 + frac;
    return static_cast<PriceInt>(neg ? -v : v);
  }

  // Per-token snapshot captured from fix_place / fix_replace calls so that
  // downstream cancel_by_token / replace steps can fill in the FIX Symbol
  // and Side fields when the cert YAML omits them (cert authors often write
  // OUCH-style `cancel_by_token { token: 20 }` without the symbol).
  struct TokenInfo {
    std::string symbol;
    Side        side{Side::Buy};
    Quantity    quantity{0};
  };
  std::unordered_map<std::string, TokenInfo> token_index_;

  fix::OeClient*           oe_;
  fix::RdClient*           rd_;
  fix::DcClient*           dc_;
  std::mutex               mu_;
  std::condition_variable  cv_;
  std::deque<FixEvent>     events_;
};

// --- Implementation --------------------------------------------------------

inline std::vector<StepResult> FixScenarioRunner::run(const Scenario& sc) {
  std::vector<StepResult> results;
  results.reserve(sc.steps.size());
  for (const auto& step : sc.steps) {
    const auto t0 = monotonic_ns();
    StepResult r{};
    r.id = step.id;
    r.action = step.action;

    auto args_or = [&](auto fallback) {
      using A = decltype(fallback);
      if (const auto* p = std::get_if<A>(&step.args)) return *p;
      return fallback;
    };

    if (step.action == "fix_logon") {
      r = run_logon(step, args_or(FixLogonArgs{}));
    } else if (step.action == "fix_logout") {
      r = run_logout(step);
    } else if (step.action == "fix_amr_subscribe" || step.action == "fix_amr") {
      r = run_amr(step, args_or(FixAmrSubscribeArgs{}));
    } else if (step.action == "fix_wait_heartbeat" ||
               step.action == "wait_heartbeat") {
      r = run_wait_hb(step, args_or(FixWaitHeartbeatArgs{}));
    } else if (step.action == "fix_place" || step.action == "place") {
      r = run_place(step, args_or(FixPlaceArgs{}));
    } else if (step.action == "fix_cancel" ||
               step.action == "cancel_by_token" ||
               step.action == "cancel_by_order_id") {
      r = run_cancel(step, args_or(FixCancelArgs{}));
    } else if (step.action == "fix_replace" || step.action == "replace") {
      r = run_replace(step, args_or(FixReplaceArgs{}));
    } else if (step.action == "expect" || step.action == "trigger_match") {
      r.id = step.id;
      r.action = step.action;
      r.passed = true;
    } else {
      r.detail = "unsupported FIX action: " + step.action;
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

inline StepResult FixScenarioRunner::run_logon(const Step& s,
                                               const FixLogonArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  (void)a;
  // First-shot logon: the initiator was started at create() time; the
  // QuickFIX worker drives the actual logon. Just wait for LogonResult.
  if (oe_ && oe_->state() == fix::FixSessionState::Active) {
    r.passed = true; r.detail = "OE already Active";
    return r;
  }
  if (rd_ && rd_->state() == fix::FixSessionState::Active) {
    r.passed = true; r.detail = "RD already Active";
    return r;
  }

  // Multi-logon path: cert flows logout then re-logon (fix_oe_temel cert
  // steps 6-8). QuickFIX's SocketInitiator does not auto-relogon after an
  // explicit Session::logout(); we have to ask for it explicitly.
  if (oe_ && (oe_->state() == fix::FixSessionState::LoggedOut ||
              oe_->state() == fix::FixSessionState::Disconnected ||
              oe_->state() == fix::FixSessionState::Failed)) {
    if (auto rr = oe_->login(); !rr) {
      r.detail = "OE login() failed: " + rr.error().detail;
      return r;
    }
  }
  if (rd_ && (rd_->state() == fix::FixSessionState::LoggedOut ||
              rd_->state() == fix::FixSessionState::Disconnected ||
              rd_->state() == fix::FixSessionState::Failed)) {
    if (auto rr = rd_->login(); !rr) {
      r.detail = "RD login() failed: " + rr.error().detail;
      return r;
    }
  }
  // The expect block resolves the outcome. If neither client ever reaches
  // Active within the timeout we'll see fix_logon_response timeout.
  r.passed = true;
  return r;
}

inline StepResult FixScenarioRunner::run_logout(const Step& s) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (oe_) (void)oe_->logout();
  if (rd_) (void)rd_->logout();
  if (dc_) (void)dc_->logout();
  r.passed = true;
  return r;
}

inline StepResult FixScenarioRunner::run_amr(const Step& s,
                                             const FixAmrSubscribeArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (!rd_) { r.detail = "no RdClient"; return r; }
  auto rr = rd_->subscribe_all(a.appl_req_id);
  if (!rr) { r.detail = rr.error().detail; return r; }
  r.passed = true;
  return r;
}

inline StepResult FixScenarioRunner::run_wait_hb(const Step& s,
                                                 const FixWaitHeartbeatArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  // QuickFIX handles heartbeats internally. We just spin briefly so the
  // session has time to exchange a HB pair before subsequent steps fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(
      std::min(a.timeout_ms, 200)));
  r.passed = true;
  return r;
}

inline StepResult FixScenarioRunner::run_place(const Step& s,
                                               const FixPlaceArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (!oe_) { r.detail = "no OeClient"; return r; }
  fix::PlaceArgs pa{};
  pa.cl_ord_id        = a.cl_ord_id;
  pa.symbol           = a.symbol;
  pa.side             = a.side;
  pa.quantity         = a.quantity;
  pa.price_decimals   = 3;
  pa.price            = parse_price(a.price_str, pa.price_decimals);
  pa.ord_type         = ord_type_from(a.ord_type);
  pa.tif              = a.tif;
  pa.account          = a.account;
  pa.afk              = a.afk;
  pa.display_quantity = a.display_quantity;
  auto rr = oe_->place(pa);
  if (!rr) { r.detail = rr.error().detail; return r; }
  // Index the token so subsequent cancel_by_token / replace steps can
  // fill in Symbol/Side when the YAML omits them.
  if (!a.cl_ord_id.empty()) {
    token_index_[a.cl_ord_id] = TokenInfo{a.symbol, a.side, a.quantity};
  }
  r.passed = true;
  return r;
}

inline StepResult FixScenarioRunner::run_cancel(const Step& s,
                                                const FixCancelArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (!oe_) { r.detail = "no OeClient"; return r; }
  fix::CancelArgs ca{};
  ca.cl_ord_id      = a.cl_ord_id;
  ca.orig_cl_ord_id = a.orig_cl_ord_id;
  ca.symbol         = a.symbol;
  ca.side           = a.side;
  ca.quantity       = a.quantity;
  // Cert YAMLs commonly omit Symbol on cancel-by-token. Resolve from the
  // index populated in run_place so the FIX message carries a non-empty
  // Symbol (acceptors that validate inbound require it).
  if (ca.symbol.empty() && !ca.orig_cl_ord_id.empty()) {
    auto it = token_index_.find(ca.orig_cl_ord_id);
    if (it != token_index_.end()) {
      ca.symbol   = it->second.symbol;
      ca.side     = it->second.side;
      if (ca.quantity == 0) ca.quantity = it->second.quantity;
    }
  }
  auto rr = oe_->cancel(ca);
  if (!rr) { r.detail = rr.error().detail; return r; }
  // Track the cancel-request id so a follow-up replace doesn't repeat-emit.
  if (!ca.cl_ord_id.empty()) {
    token_index_[ca.cl_ord_id] = TokenInfo{ca.symbol, ca.side, ca.quantity};
  }
  r.passed = true;
  return r;
}

inline StepResult FixScenarioRunner::run_replace(const Step& s,
                                                 const FixReplaceArgs& a) {
  StepResult r{}; r.id = s.id; r.action = s.action;
  if (!oe_) { r.detail = "no OeClient"; return r; }
  fix::ReplaceArgs ra{};
  ra.cl_ord_id       = a.cl_ord_id;
  ra.orig_cl_ord_id  = a.orig_cl_ord_id;
  ra.symbol          = a.symbol;
  ra.side            = a.side;
  ra.new_quantity    = a.new_quantity;
  ra.price_decimals  = 3;
  ra.new_price       = parse_price(a.price_str, ra.price_decimals);
  ra.ord_type        = ord_type_from(a.ord_type);
  if (ra.symbol.empty() && !ra.orig_cl_ord_id.empty()) {
    auto it = token_index_.find(ra.orig_cl_ord_id);
    if (it != token_index_.end()) {
      ra.symbol = it->second.symbol;
      ra.side   = it->second.side;
    }
  }
  auto rr = oe_->replace(ra);
  if (!rr) { r.detail = rr.error().detail; return r; }
  if (!ra.cl_ord_id.empty()) {
    token_index_[ra.cl_ord_id] =
        TokenInfo{ra.symbol, ra.side, ra.new_quantity};
  }
  r.passed = true;
  return r;
}

inline bool FixScenarioRunner::match_expectation(const Expect& exp,
                                                 int timeout_ms,
                                                 std::string& detail) {
  if (exp.msg == "server_heartbeat") return true;
  if (exp.msg == "fix_logout_complete") {
    // Drain *all* queued events — once the session is disconnected, every
    // breadcrumb in flight is from the prior session and must not satisfy
    // a downstream expect.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      bool ok = true;
      if (oe_ && oe_->state() == fix::FixSessionState::Active) ok = false;
      if (rd_ && rd_->state() == fix::FixSessionState::Active) ok = false;
      if (dc_ && dc_->state() == fix::FixSessionState::Active) ok = false;
      if (ok) {
        std::lock_guard<std::mutex> g(mu_);
        events_.clear();
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    detail = "logout did not complete within timeout";
    return false;
  }

  int needed = std::max(1, exp.occurrences);
  while (needed > 0) {
    FixEvent ev;
    if (!wait_for_event(timeout_ms, ev)) {
      detail = "timeout waiting for " + exp.msg;
      return false;
    }
    // For fix_logon_response only count LogonResults that reflect a
    // successful handshake (Active or PasswordChanged); ignore the
    // LoggedOut/Disconnected breadcrumbs that fromAdmin / onLogout fire.
    if (exp.msg == "fix_logon_response") {
      if (auto* lr = std::get_if<fix::LogonResult>(&ev)) {
        if (lr->state != fix::FixSessionState::Active &&
            lr->state != fix::FixSessionState::PasswordChanged) {
          continue;
        }
      }
    }
    std::string why;
    if (one_event_matches(exp, ev, why)) {
      --needed;
    } else if (exp.msg == "any") {
      --needed;
    } else {
      // Don't fail on cross-talk events (e.g. heartbeat-driven session
      // notifications). Drop and keep waiting until needed satisfied or
      // timeout fires on the next iteration.
      continue;
    }
  }
  return true;
}

inline bool FixScenarioRunner::one_event_matches(const Expect& exp,
                                                 const FixEvent& ev,
                                                 std::string& detail) const {
  return fix_event_matches(exp, ev, detail);
}

}  // namespace bist::runner
