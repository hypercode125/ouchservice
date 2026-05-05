#pragma once
//
// src/fix/internal/dc_app.hpp — FIX::Application subclass for the Drop
// Copy session. Receives Execution Report, Trade Capture Report and
// Quote Status Report mirrors of the OE flow and surfaces each as a POD
// event suitable for the audit log.

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

#include <quickfix/Application.h>
#include <quickfix/Exceptions.h>
#include <quickfix/Message.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/fix50sp2/ExecutionReport.h>
#include <quickfix/fix50sp2/QuoteStatusReport.h>
#include <quickfix/fix50sp2/TradeCaptureReport.h>

#include "bist/core/result.hpp"
#include "bist/fix/facade.hpp"
#include "src/fix/internal/converters.hpp"

namespace bist::fix::detail {

class DcApp : public FIX::Application, public FIX::MessageCracker {
 public:
  explicit DcApp(DcClient::Callbacks cbs) : cbs_(std::move(cbs)) {}

  void onCreate(const FIX::SessionID&) override {}

  void onLogon(const FIX::SessionID& s) override {
    {
      std::lock_guard<std::mutex> g(mu_);
      session_id_ = s;
      bound_ = true;
      state_.store(FixSessionState::Active);
    }
    if (cbs_.on_session) {
      LogonResult r{};
      r.state = FixSessionState::Active;
      r.session_status = 0;
      r.detail = "DC Active";
      cbs_.on_session(r);
    }
  }

  void onLogout(const FIX::SessionID&) override {
    state_.store(FixSessionState::LoggedOut);
    if (cbs_.on_session) {
      LogonResult r{};
      r.state = FixSessionState::LoggedOut;
      r.session_status = -1;
      r.detail = "DC LoggedOut";
      cbs_.on_session(r);
    }
  }

  void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
  void fromAdmin(const FIX::Message&, const FIX::SessionID&)
      throw(FIX::FieldNotFound,
            FIX::IncorrectDataFormat,
            FIX::IncorrectTagValue,
            FIX::RejectLogon) override {}
  void toApp(FIX::Message&, const FIX::SessionID&)
      throw(FIX::DoNotSend) override {}
  void fromApp(const FIX::Message& m, const FIX::SessionID& s)
      throw(FIX::FieldNotFound,
            FIX::IncorrectDataFormat,
            FIX::IncorrectTagValue,
            FIX::UnsupportedMessageType) override {
    crack(m, s);
  }

  void onMessage(const FIX50SP2::ExecutionReport& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_execution) return;
    ExecutionReportEvent ev{};
    try { FIX::ClOrdID f; m.getField(f); ev.cl_ord_id = f.getValue(); } catch (...) {}
    try { FIX::OrderID f; m.getField(f); ev.order_id = f.getValue(); } catch (...) {}
    try { FIX::Symbol f; m.getField(f); ev.symbol = f.getValue(); } catch (...) {}
    try { FIX::Side f; m.getField(f); ev.side = bist_side(f.getValue()); } catch (...) {}
    try { FIX::ExecType f; m.getField(f); ev.exec_type = f.getValue(); } catch (...) {}
    try { FIX::OrdStatus f; m.getField(f); ev.ord_status = f.getValue(); } catch (...) {}
    try { FIX::LeavesQty f; m.getField(f); ev.leaves_qty = static_cast<Quantity>(f.getValue()); } catch (...) {}
    try { FIX::LastQty f; m.getField(f); ev.last_qty = static_cast<Quantity>(f.getValue()); } catch (...) {}
    try { FIX::LastPx  f; m.getField(f); ev.last_price = from_fix_price(f.getValue(), 3); } catch (...) {}
    cbs_.on_execution(ev);
  }

  void onMessage(const FIX50SP2::TradeCaptureReport& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_trade_report) return;
    TradeReportEvent ev{};
    try { FIX::TradeReportID f; m.getField(f); ev.trade_report_id = f.getValue(); } catch (...) {}
    try { FIX::Symbol f; m.getField(f); ev.symbol = f.getValue(); } catch (...) {}
    try { FIX::Side f; m.getField(f); ev.side = bist_side(f.getValue()); } catch (...) {}
    try { FIX::LastQty f; m.getField(f); ev.quantity = static_cast<Quantity>(f.getValue()); } catch (...) {}
    try { FIX::LastPx f; m.getField(f); ev.price = from_fix_price(f.getValue(), 3); } catch (...) {}
    cbs_.on_trade_report(ev);
  }

  void onMessage(const FIX50SP2::QuoteStatusReport& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_quote_status) return;
    QuoteStatusEvent ev{};
    try { FIX::QuoteID f; m.getField(f); ev.quote_id = f.getValue(); } catch (...) {}
    try { FIX::Symbol f; m.getField(f); ev.symbol = f.getValue(); } catch (...) {}
    try { FIX::Side f; m.getField(f); ev.side = bist_side(f.getValue()); } catch (...) {}
    cbs_.on_quote_status(ev);
  }

  Result<void> logout() {
    try {
      std::lock_guard<std::mutex> g(mu_);
      if (bound_) FIX::Session::lookupSession(session_id_)->logout();
    } catch (...) {}
    return {};
  }

  Result<void> login() {
    try {
      std::lock_guard<std::mutex> g(mu_);
      if (!bound_) return make_error(ErrorCategory::StateMismatch,
                                     "DC session not bound");
      auto* sess = FIX::Session::lookupSession(session_id_);
      if (sess) {
        sess->logon();
      }
    } catch (...) {
      return make_error(ErrorCategory::Io, "DC login() exception");
    }
    state_.store(FixSessionState::LoggingIn);
    return {};
  }

  FixSessionState state() const noexcept { return state_.load(); }

 private:
  DcClient::Callbacks            cbs_;
  std::mutex                     mu_;
  FIX::SessionID                 session_id_;
  bool                           bound_{false};
  std::atomic<FixSessionState>   state_{FixSessionState::Disconnected};
};

}  // namespace bist::fix::detail
