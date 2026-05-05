#pragma once
//
// src/fix/internal/oe_app.hpp — FIX::Application subclass for the OE
// session, plus the send-side methods that map POD args onto QuickFIX
// FIX50SP2 messages. Internal only — never include from outside src/fix/.

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

#include <quickfix/Application.h>
#include <quickfix/Exceptions.h>
#include <quickfix/Group.h>
#include <quickfix/Message.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/fix50sp2/ExecutionReport.h>
#include <quickfix/fix50sp2/NewOrderSingle.h>
#include <quickfix/fix50sp2/OrderCancelReject.h>
#include <quickfix/fix50sp2/OrderCancelReplaceRequest.h>
#include <quickfix/fix50sp2/OrderCancelRequest.h>
#include <quickfix/fix50sp2/TradeCaptureReport.h>
#include <quickfix/fix50sp2/TradeCaptureReportAck.h>

#include "bist/core/result.hpp"
#include "bist/fix/facade.hpp"
#include "bist/fix/fields.hpp"
#include "src/fix/internal/converters.hpp"

namespace bist::fix::detail {

class OeApp : public FIX::Application, public FIX::MessageCracker {
 public:
  explicit OeApp(OeClient::Callbacks cbs) : cbs_(std::move(cbs)) {}

  // ---- FIX::Application ---------------------------------------------------

  void onCreate(const FIX::SessionID&) override {}

  void onLogon(const FIX::SessionID& s) override {
    {
      std::lock_guard<std::mutex> g(mu_);
      session_id_ = s;
      bound_ = true;
      state_.store(FixSessionState::Active);
    }
    fire_session(FixSessionState::Active, 0, "Active");
  }

  void onLogout(const FIX::SessionID&) override {
    state_.store(FixSessionState::LoggedOut);
    fire_session(FixSessionState::LoggedOut, -1, "LoggedOut");
  }

  void toAdmin(FIX::Message& m, const FIX::SessionID&) override {
    // Splice 554/925 into outbound Logon when the BIST gateway has signalled
    // SessionStatus=8 (PasswordExpired) on a previous attempt.
    bool is_logon = false;
    try {
      FIX::MsgType mt;
      m.getHeader().getField(mt);
      is_logon = (mt.getValue() == "A");
    } catch (...) {}
    if (is_logon) {
      std::lock_guard<std::mutex> g(mu_);
      if (!password_.empty())     m.setField(tag::Password,    password_);
      if (!new_password_.empty()) m.setField(tag::NewPassword, new_password_);
    }
  }

  void fromAdmin(const FIX::Message& m, const FIX::SessionID&)
      throw(FIX::FieldNotFound,
            FIX::IncorrectDataFormat,
            FIX::IncorrectTagValue,
            FIX::RejectLogon) override {
    // Inspect the inbound Logon to surface SessionStatus to the consumer.
    try {
      FIX::MsgType mt;
      m.getHeader().getField(mt);
      if (mt.getValue() == "A") {
        int status = -1;
        try {
          FIX::IntField f(tag::SessionStatus);
          m.getField(f);
          status = f.getValue();
        } catch (...) {}
        if (status == session_status::PasswordExpired) {
          state_.store(FixSessionState::PasswordExpired);
          fire_session(FixSessionState::PasswordExpired, status,
                       "PasswordExpired");
        } else if (status == session_status::SessionPasswordChanged) {
          state_.store(FixSessionState::PasswordChanged);
          fire_session(FixSessionState::PasswordChanged, status,
                       "Session password changed");
        }
      }
    } catch (...) {}
  }

  void toApp(FIX::Message&, const FIX::SessionID&)
      throw(FIX::DoNotSend) override {}

  void fromApp(const FIX::Message& m, const FIX::SessionID& s)
      throw(FIX::FieldNotFound,
            FIX::IncorrectDataFormat,
            FIX::IncorrectTagValue,
            FIX::UnsupportedMessageType) override {
    crack(m, s);
  }

  // ---- MessageCracker overloads ------------------------------------------

  void onMessage(const FIX50SP2::ExecutionReport& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_execution) return;
    ExecutionReportEvent ev{};
    try { FIX::ClOrdID f; m.getField(f); ev.cl_ord_id = f.getValue(); } catch (...) {}
    try { FIX::OrigClOrdID f; m.getField(f); ev.orig_cl_ord_id = f.getValue(); } catch (...) {}
    try { FIX::ExecID f; m.getField(f); ev.exec_id = f.getValue(); } catch (...) {}
    try { FIX::OrderID f; m.getField(f); ev.order_id = f.getValue(); } catch (...) {}
    try { FIX::Symbol f; m.getField(f); ev.symbol = f.getValue(); } catch (...) {}
    try { FIX::Side f; m.getField(f); ev.side = bist_side(f.getValue()); } catch (...) {}
    try { FIX::ExecType f; m.getField(f); ev.exec_type = f.getValue(); } catch (...) {}
    try { FIX::OrdStatus f; m.getField(f); ev.ord_status = f.getValue(); } catch (...) {}
    try { FIX::LeavesQty f; m.getField(f); ev.leaves_qty = static_cast<Quantity>(f.getValue()); } catch (...) {}
    try { FIX::CumQty f; m.getField(f); ev.cum_qty = static_cast<Quantity>(f.getValue()); } catch (...) {}
    try { FIX::LastQty f; m.getField(f); ev.last_qty = static_cast<Quantity>(f.getValue()); } catch (...) {}
    try { FIX::LastPx  f; m.getField(f); ev.last_price = from_fix_price(f.getValue(), 3); } catch (...) {}
    try { FIX::Text f; m.getField(f); ev.text = f.getValue(); } catch (...) {}
    cbs_.on_execution(ev);
  }

  void onMessage(const FIX50SP2::OrderCancelReject& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_cancel_reject) return;
    CancelRejectEvent ev{};
    try { FIX::ClOrdID f; m.getField(f); ev.cl_ord_id = f.getValue(); } catch (...) {}
    try { FIX::OrigClOrdID f; m.getField(f); ev.orig_cl_ord_id = f.getValue(); } catch (...) {}
    try { FIX::Text f; m.getField(f); ev.text = f.getValue(); } catch (...) {}
    cbs_.on_cancel_reject(ev);
  }

  void onMessage(const FIX50SP2::TradeCaptureReport& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_trade_report) return;
    TradeReportEvent ev{};
    try { FIX::TradeReportID f; m.getField(f); ev.trade_report_id = f.getValue(); } catch (...) {}
    try { FIX::TradeReportRefID f; m.getField(f); ev.trade_report_ref_id = f.getValue(); } catch (...) {}
    try { FIX::Symbol f; m.getField(f); ev.symbol = f.getValue(); } catch (...) {}
    try { FIX::Side f; m.getField(f); ev.side = bist_side(f.getValue()); } catch (...) {}
    try { FIX::LastQty f; m.getField(f); ev.quantity = static_cast<Quantity>(f.getValue()); } catch (...) {}
    try { FIX::LastPx f; m.getField(f); ev.price = from_fix_price(f.getValue(), 3); } catch (...) {}
    cbs_.on_trade_report(ev);
  }

  void onMessage(const FIX50SP2::TradeCaptureReportAck&,
                 const FIX::SessionID&) override {}

  // ---- Send side ----------------------------------------------------------

  void set_password(const std::string& pwd, const std::string& new_pwd) {
    std::lock_guard<std::mutex> g(mu_);
    password_     = pwd;
    new_password_ = new_pwd;
  }

  Result<std::string> place(const PlaceArgs& a) {
    if (!bound_) return make_error(ErrorCategory::StateMismatch,
                                   "FIX OE not bound");
    FIX50SP2::NewOrderSingle msg;
    msg.set(FIX::ClOrdID(a.cl_ord_id));
    msg.set(FIX::Symbol(a.symbol));
    msg.set(FIX::Side(fix_side(a.side)));
    msg.set(FIX::OrderQty(static_cast<double>(a.quantity)));
    msg.set(FIX::OrdType(fix_ord_type(a.ord_type)));
    if (a.ord_type != OrdType::Market &&
        a.ord_type != OrdType::MarketToLimit) {
      msg.set(FIX::Price(to_fix_price(a.price, a.price_decimals)));
    }
    msg.set(FIX::TimeInForce(fix_tif(a.tif)));
    msg.set(FIX::TransactTime{});
    if (!a.account.empty()) msg.set(FIX::Account(a.account));
    if (a.display_quantity > 0)
      msg.set(FIX::MaxFloor(static_cast<double>(a.display_quantity)));
    apply_afk(msg, a.afk);
    return send_or_error(msg, a.cl_ord_id);
  }

  Result<std::string> cancel(const CancelArgs& a) {
    if (!bound_) return make_error(ErrorCategory::StateMismatch,
                                   "FIX OE not bound");
    FIX50SP2::OrderCancelRequest msg;
    msg.set(FIX::ClOrdID(a.cl_ord_id));
    msg.set(FIX::OrigClOrdID(a.orig_cl_ord_id));
    msg.set(FIX::Symbol(a.symbol));
    msg.set(FIX::Side(fix_side(a.side)));
    msg.set(FIX::OrderQty(static_cast<double>(a.quantity)));
    msg.set(FIX::TransactTime{});
    return send_or_error(msg, a.cl_ord_id);
  }

  Result<std::string> replace(const ReplaceArgs& a) {
    if (!bound_) return make_error(ErrorCategory::StateMismatch,
                                   "FIX OE not bound");
    FIX50SP2::OrderCancelReplaceRequest msg;
    msg.set(FIX::ClOrdID(a.cl_ord_id));
    msg.set(FIX::OrigClOrdID(a.orig_cl_ord_id));
    msg.set(FIX::Symbol(a.symbol));
    msg.set(FIX::Side(fix_side(a.side)));
    msg.set(FIX::OrderQty(static_cast<double>(a.new_quantity)));
    msg.set(FIX::OrdType(fix_ord_type(a.ord_type)));
    if (a.ord_type != OrdType::Market) {
      msg.set(FIX::Price(to_fix_price(a.new_price, a.price_decimals)));
    }
    msg.set(FIX::TransactTime{});
    return send_or_error(msg, a.cl_ord_id);
  }

  Result<std::string> trade_report(const TradeReportArgs& a) {
    if (!bound_) return make_error(ErrorCategory::StateMismatch,
                                   "FIX OE not bound");
    FIX50SP2::TradeCaptureReport msg;
    msg.set(FIX::TradeReportID(a.trade_report_id));
    if (!a.trade_report_ref_id.empty())
      msg.set(FIX::TradeReportRefID(a.trade_report_ref_id));
    msg.set(FIX::Symbol(a.symbol));
    msg.set(FIX::LastQty(static_cast<double>(a.quantity)));
    msg.set(FIX::LastPx(to_fix_price(a.price, a.price_decimals)));
    // Side lives in the NoSides repeating group on TradeCaptureReport in
    // FIX 5.0 SP2. We populate it via the standard side group.
    {
      FIX50SP2::TradeCaptureReport::NoSides side_group;
      side_group.set(FIX::Side(fix_side(a.side)));
      msg.addGroup(side_group);
    }
    if (!a.counterparty_id.empty()) {
      FIX::Group g(tag::NoPartyIDs, tag::PartyID);
      g.setField(tag::PartyID,       a.counterparty_id);
      g.setField(tag::PartyIDSource, std::string(1, party_id_source::Proprietary));
      g.setField(tag::PartyRole,     std::to_string(party_role::ExecutingFirm));
      msg.addGroup(g);
    }
    return send_or_error(msg, a.trade_report_id);
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
                                     "OE session not bound");
      auto* sess = FIX::Session::lookupSession(session_id_);
      if (sess) sess->logon();
    } catch (...) {
      return make_error(ErrorCategory::Io, "OE login() exception");
    }
    state_.store(FixSessionState::LoggingIn);
    return {};
  }

  FixSessionState state() const noexcept { return state_.load(); }

 private:
  static void apply_afk(FIX::Message& m, const std::string& afk) {
    if (afk.empty()) return;
    FIX::Group g(tag::NoPartyIDs, tag::PartyID);
    g.setField(tag::PartyID,       afk);
    g.setField(tag::PartyIDSource, std::string(1, party_id_source::Proprietary));
    g.setField(tag::PartyRole,     std::to_string(party_role::DeskID));
    m.addGroup(g);
  }

  Result<std::string> send_or_error(FIX::Message& msg, const std::string& id) {
    try {
      FIX::Session::sendToTarget(msg, session_id_);
    } catch (const FIX::SessionNotFound&) {
      return make_error(ErrorCategory::StateMismatch, "FIX session not found");
    }
    return id;
  }

  void fire_session(FixSessionState s, int status, const std::string& detail) {
    if (cbs_.on_session) {
      LogonResult r{};
      r.state = s;
      r.session_status = status;
      r.detail = detail;
      cbs_.on_session(r);
    }
  }

  OeClient::Callbacks                cbs_;
  std::mutex                         mu_;
  FIX::SessionID                     session_id_;
  bool                               bound_{false};
  std::atomic<FixSessionState>       state_{FixSessionState::Disconnected};
  std::string                        password_;
  std::string                        new_password_;
};

}  // namespace bist::fix::detail
