#pragma once
//
// src/fix/internal/rd_app.hpp — FIX::Application subclass for the
// Reference Data session. Responsible for the AMR subscription handshake
// per the BIST cert, and surfacing each Security Definition / Security
// Status update as a POD event.

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
#include <quickfix/fix50sp2/SecurityDefinition.h>
#include <quickfix/fix50sp2/SecurityStatus.h>
#include <quickfix/fix50sp2/TradingSessionList.h>

#include "bist/core/result.hpp"
#include "bist/fix/facade.hpp"
#include "bist/fix/fields.hpp"

namespace bist::fix::detail {

class RdApp : public FIX::Application, public FIX::MessageCracker {
 public:
  explicit RdApp(RdClient::Callbacks cbs) : cbs_(std::move(cbs)) {}

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
    bool is_logon = false;
    try {
      FIX::MsgType mt;
      m.getHeader().getField(mt);
      is_logon = (mt.getValue() == "A");
    } catch (...) {}
    if (is_logon) {
      std::lock_guard<std::mutex> g(mu_);
      if (!password_.empty()) m.setField(tag::Password, password_);
    }
  }

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
    // ApplicationMessageRequestAck (BX) and other reference-data app
    // messages route through the cracker for the typed overloads we
    // support; otherwise we surface ApplReqID/Text via the AMR-ack hook.
    std::string mtype;
    try {
      FIX::MsgType mt;
      m.getHeader().getField(mt);
      mtype = mt.getValue();
    } catch (...) {}
    if (mtype == "BX") {
      handle_amr_ack(m);
      return;
    }
    crack(m, s);
  }

  void onMessage(const FIX50SP2::SecurityDefinition& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_security_def) return;
    std::string sym;
    try { FIX::Symbol f; m.getField(f); sym = f.getValue(); } catch (...) {}
    cbs_.on_security_def(sym);
  }

  void onMessage(const FIX50SP2::SecurityStatus& m,
                 const FIX::SessionID&) override {
    if (!cbs_.on_security_status) return;
    std::string sym;
    try { FIX::Symbol f; m.getField(f); sym = f.getValue(); } catch (...) {}
    cbs_.on_security_status(sym);
  }

  void onMessage(const FIX50SP2::TradingSessionList&,
                 const FIX::SessionID&) override {}

  void set_password(const std::string& pwd) {
    std::lock_guard<std::mutex> g(mu_);
    password_ = pwd;
  }

  Result<std::string> subscribe_all(const std::string& appl_req_id) {
    if (!bound_) return make_error(ErrorCategory::StateMismatch,
                                   "FIX RD not bound");
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("BW"));
    msg.setField(tag::ApplReqID,   appl_req_id);
    msg.setField(tag::ApplReqType, std::to_string(appl_req_type::RequestAndSubscribe));
    try {
      FIX::Session::sendToTarget(msg, session_id_);
    } catch (const FIX::SessionNotFound&) {
      return make_error(ErrorCategory::StateMismatch, "FIX session not found");
    }
    return appl_req_id;
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
                                     "RD session not bound");
      auto* sess = FIX::Session::lookupSession(session_id_);
      if (sess) {
        sess->logon();
      }
    } catch (...) {
      return make_error(ErrorCategory::Io, "RD login() exception");
    }
    state_.store(FixSessionState::LoggingIn);
    return {};
  }

  FixSessionState state() const noexcept { return state_.load(); }

 private:
  void handle_amr_ack(const FIX::Message& m) {
    if (!cbs_.on_amr_ack) return;
    std::string text;
    try { FIX::Text f; m.getField(f); text = f.getValue(); } catch (...) {}
    const bool ok = text.find("Duplicate") == std::string::npos;
    cbs_.on_amr_ack(ok, std::move(text));
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

  RdClient::Callbacks            cbs_;
  std::mutex                     mu_;
  FIX::SessionID                 session_id_;
  bool                           bound_{false};
  std::atomic<FixSessionState>   state_{FixSessionState::Disconnected};
  std::string                    password_;
};

}  // namespace bist::fix::detail
