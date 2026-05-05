#pragma once
//
// src/fix/internal/acceptor_mock.hpp — minimal in-process QuickFIX
// SocketAcceptor that simulates the BIST PSP gateway just enough to drive
// the FIX OE/RD cert YAML scenarios under `--mock`.
//
// Internal only. Compiled at -std=gnu++14 (see src/fix/CMakeLists.txt).
//
// Behaviour:
//   * Logon (A) with 554 Password + 925 NewPassword → reply Logon with
//     1409=1 (Session Password Changed). Logon with only 554 → 1409=0
//     (Active).
//   * NewOrderSingle (D) → ExecutionReport (8) ExecType=0 (New),
//     OrdStatus=0 (New), LeavesQty=OrderQty.
//   * OrderCancelRequest (F) → ExecutionReport ExecType=4 (Canceled),
//     OrdStatus=4.
//   * OrderCancelReplaceRequest (G) → ExecutionReport ExecType=5
//     (Replaced), OrdStatus=0, LeavesQty=new total - 0.
//   * TradeCaptureReport (AE) → TradeCaptureReportAck (AR) with
//     TrdRptStatus=0.
//   * ApplicationMessageRequest (BW) → ApplicationMessageRequestAck (BX)
//     with ApplResponseType=0 (Request Successfully Processed). Second
//     BW with the same ApplReqID → BX with ApplResponseType=2 (Reject /
//     "Duplicate Application ID").
//   * Logout (5) → echo Logout, session goes Disconnected.
//
// The acceptor binds 127.0.0.1:0 and exposes the chosen port via
// `port()`. The InitiatorConfig used by OeClient/RdClient/DcClient must
// point to that port and use SenderCompID/TargetCompID such that the
// Acceptor's SessionID is the swap.

#if defined(BIST_HAS_QUICKFIX)

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <quickfix/Application.h>
#include <quickfix/FileLog.h>
#include <quickfix/FileStore.h>
#include <quickfix/Message.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/fix50sp2/ApplicationMessageRequest.h>
#include <quickfix/fix50sp2/ApplicationMessageRequestAck.h>
#include <quickfix/fix50sp2/ExecutionReport.h>
#include <quickfix/fix50sp2/NewOrderSingle.h>
#include <quickfix/fix50sp2/OrderCancelRequest.h>
#include <quickfix/fix50sp2/OrderCancelReplaceRequest.h>
#include <quickfix/fix50sp2/TradeCaptureReport.h>
#include <quickfix/fix50sp2/TradeCaptureReportAck.h>

#include "bist/core/result.hpp"
#include "bist/fix/fields.hpp"

namespace bist::fix::detail {

// AcceptorEndpoints describes the QuickFIX session geometry the mock
// exposes. The initiator's SenderCompID becomes the acceptor's
// TargetCompID and vice-versa.
struct AcceptorEndpoints {
  std::string oe_target_comp_id;   // initiator side (e.g. "CLIENT_OE")
  std::string oe_sender_comp_id;   // mock's SenderCompID (e.g. "BIST_OE")
  std::string rd_target_comp_id;
  std::string rd_sender_comp_id;
  std::string app_data_dictionary;
  std::string transport_data_dictionary;
  std::string store_path = "state/fix_mock";
  std::string log_path   = "log/fix_mock";
  int         port       = 0;       // 0 → bind ephemeral
  int         heartbeat  = 30;
};

class AcceptorMockApp : public FIX::Application, public FIX::MessageCracker {
 public:
  // ---- FIX::Application -----------------------------------------------------
  void onCreate(const FIX::SessionID&) override {}
  void onLogon(const FIX::SessionID& s) override {
    std::lock_guard<std::mutex> g(mu_);
    sessions_.insert(std::string(s.toString()));
  }
  void onLogout(const FIX::SessionID& s) override {
    std::lock_guard<std::mutex> g(mu_);
    sessions_.erase(std::string(s.toString()));
  }
  void toAdmin(FIX::Message& m, const FIX::SessionID&) override {
    // When we are about to send an outbound Logon, splice 1409 SessionStatus
    // based on the inbound Logon's password fields. Stored from
    // fromAdmin().
    try {
      FIX::MsgType mt;
      m.getHeader().getField(mt);
      if (mt.getValue() == "A") {
        std::lock_guard<std::mutex> g(mu_);
        m.setField(tag::SessionStatus, std::to_string(pending_session_status_));
        // Reset between logons.
        pending_session_status_ = session_status::Active;
      }
    } catch (...) {}
  }
  void fromAdmin(const FIX::Message& m, const FIX::SessionID&)
      throw(FIX::FieldNotFound,
            FIX::IncorrectDataFormat,
            FIX::IncorrectTagValue,
            FIX::RejectLogon) override {
    try {
      FIX::MsgType mt;
      m.getHeader().getField(mt);
      if (mt.getValue() != "A") return;
      bool has_pwd = false, has_new = false;
      try { FIX::Password f; m.getField(f); has_pwd = !f.getValue().empty(); }
      catch (...) {}
      try { FIX::StringField f(tag::NewPassword); m.getField(f); has_new = !f.getValue().empty(); }
      catch (...) {}
      std::lock_guard<std::mutex> g(mu_);
      if (has_pwd && has_new) {
        pending_session_status_ = session_status::SessionPasswordChanged;
      } else {
        pending_session_status_ = session_status::Active;
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

  // ---- App message handlers -------------------------------------------------
  void onMessage(const FIX50SP2::NewOrderSingle& m,
                 const FIX::SessionID& s) override {
    FIX50SP2::ExecutionReport er;
    copy_basic(m, er);
    er.set(FIX::ExecID(next_id("E")));
    er.set(FIX::OrderID(next_id("O")));
    er.set(FIX::ExecType(FIX::ExecType_NEW));
    er.set(FIX::OrdStatus(FIX::OrdStatus_NEW));
    double qty = 0;
    try { FIX::OrderQty f; m.getField(f); qty = f.getValue(); } catch (...) {}
    er.set(FIX::LeavesQty(qty));
    er.set(FIX::CumQty(0));
    er.set(FIX::AvgPx(0));
    // BIST short-sell flag: when Side(54)=5 ("Sell short"), echo
    // TrdType(828)=2 in the ER and stamp Text(58)=short_sell so the cert
    // scenario can assert trd_type: short_sell without needing a custom
    // FIX dictionary hop.
    try {
      FIX::Side sf; m.getField(sf);
      if (sf.getValue() == FIX::Side_SELL_SHORT) {
        er.setField(828, "2");                 // TrdType (BIST custom 828=2)
        er.set(FIX::Text("short_sell"));
      }
    } catch (...) {}
    send(er, s);
  }
  void onMessage(const FIX50SP2::OrderCancelRequest& m,
                 const FIX::SessionID& s) override {
    FIX50SP2::ExecutionReport er;
    copy_basic(m, er);
    er.set(FIX::ExecID(next_id("E")));
    er.set(FIX::OrderID(next_id("O")));
    er.set(FIX::ExecType(FIX::ExecType_CANCELED));
    er.set(FIX::OrdStatus(FIX::OrdStatus_CANCELED));
    er.set(FIX::LeavesQty(0));
    er.set(FIX::CumQty(0));
    er.set(FIX::AvgPx(0));
    send(er, s);
  }
  void onMessage(const FIX50SP2::OrderCancelReplaceRequest& m,
                 const FIX::SessionID& s) override {
    FIX50SP2::ExecutionReport er;
    copy_basic(m, er);
    er.set(FIX::ExecID(next_id("E")));
    er.set(FIX::OrderID(next_id("O")));
    er.set(FIX::ExecType(FIX::ExecType_REPLACE));
    er.set(FIX::OrdStatus(FIX::OrdStatus_NEW));
    double qty = 0;
    try { FIX::OrderQty f; m.getField(f); qty = f.getValue(); } catch (...) {}
    er.set(FIX::LeavesQty(qty));
    er.set(FIX::CumQty(0));
    er.set(FIX::AvgPx(0));
    send(er, s);
  }
  void onMessage(const FIX50SP2::TradeCaptureReport& m,
                 const FIX::SessionID& s) override {
    FIX50SP2::TradeCaptureReportAck ack;
    try { FIX::TradeReportID f; m.getField(f); ack.set(f); } catch (...) {}
    try { FIX::Symbol f; m.getField(f); ack.set(f); } catch (...) {}
    ack.set(FIX::TrdRptStatus(0));
    send(ack, s);
  }
  void onMessage(const FIX50SP2::ApplicationMessageRequest& m,
                 const FIX::SessionID& s) override {
    std::string id;
    try { FIX::ApplReqID f; m.getField(f); id = f.getValue(); } catch (...) {}
    // BIST cert "Duplicate Application ID" semantics: a *second* AMR within
    // the same logon is treated as a duplicate even if the ApplReqID
    // differs, because the subscription state is per-session not per-ID.
    // We track both axes: the explicit ID set (caught when reused with the
    // same string) AND a per-session "has subscribed" flag.
    bool dup = false;
    {
      std::lock_guard<std::mutex> g(mu_);
      const std::string key = s.toString();
      dup = !subscribed_sessions_.insert(key).second;
      if (!dup && !id.empty()) {
        dup = !appl_req_ids_.insert(id).second;
      }
    }
    FIX50SP2::ApplicationMessageRequestAck ack;
    ack.set(FIX::ApplResponseID(next_id("AR")));
    if (!id.empty()) ack.set(FIX::ApplReqID(id));
    if (dup) {
      ack.set(FIX::ApplResponseType(2));
      ack.set(FIX::Text("Duplicate Application ID"));
    } else {
      ack.set(FIX::ApplResponseType(0));
      ack.set(FIX::Text("Request successfully processed"));
    }
    send(ack, s);
  }

 private:
  template <class M, class N>
  static void copy_basic(const M& src, N& dst) {
    try { FIX::ClOrdID f; src.getField(f); dst.set(f); } catch (...) {}
    try { FIX::OrigClOrdID f; src.getField(f); dst.set(f); } catch (...) {}
    try { FIX::Symbol f; src.getField(f); dst.set(f); } catch (...) {}
    try { FIX::Side f; src.getField(f); dst.set(f); } catch (...) {}
  }
  std::string next_id(const char* prefix) {
    std::lock_guard<std::mutex> g(mu_);
    return std::string(prefix) + std::to_string(++id_counter_);
  }
  static void send(FIX::Message& m, const FIX::SessionID& s) {
    try { FIX::Session::sendToTarget(m, s); } catch (...) {}
  }

  std::mutex                  mu_;
  std::set<std::string>       sessions_;
  std::set<std::string>       appl_req_ids_;
  std::set<std::string>       subscribed_sessions_;
  std::uint64_t               id_counter_{0};
  int                         pending_session_status_{
      session_status::Active};
};

// Pretty-prints the SessionSettings for both the OE and RD acceptor sessions.
inline std::string render_acceptor_settings(const AcceptorEndpoints& e) {
  std::ostringstream o;
  o << "[DEFAULT]\n"
    << "ConnectionType=acceptor\n"
    << "FileStorePath=" << e.store_path << "\n"
    << "FileLogPath="   << e.log_path   << "\n"
    << "StartTime=00:00:00\n"
    << "EndTime=23:59:59\n"
    << "HeartBtInt="    << e.heartbeat  << "\n"
    << "TransportDataDictionary=" << e.transport_data_dictionary << "\n"
    << "AppDataDictionary="       << e.app_data_dictionary       << "\n"
    << "UseDataDictionary=N\n"
    << "ResetOnLogon=Y\n"
    << "SocketAcceptPort=" << e.port << "\n"
    << "ValidateUserDefinedFields=N\n"
    << "ValidateIncomingMessage=N\n"
    << "ValidateFieldsOutOfOrder=N\n"
    << "ValidateFieldsHaveValues=N\n"
    << "RejectInvalidMessage=N\n"
    << "AllowUnknownMsgFields=Y\n";

  if (!e.oe_sender_comp_id.empty()) {
    o << "[SESSION]\n"
      << "BeginString=FIXT.1.1\n"
      << "DefaultApplVerID=FIX.5.0SP2\n"
      << "SenderCompID=" << e.oe_sender_comp_id << "\n"
      << "TargetCompID=" << e.oe_target_comp_id << "\n";
  }
  if (!e.rd_sender_comp_id.empty()) {
    o << "[SESSION]\n"
      << "BeginString=FIXT.1.1\n"
      << "DefaultApplVerID=FIX.5.0SP2\n"
      << "SenderCompID=" << e.rd_sender_comp_id << "\n"
      << "TargetCompID=" << e.rd_target_comp_id << "\n";
  }
  return o.str();
}

class AcceptorMock {
 public:
  Result<int> start(AcceptorEndpoints e) {
    endpoints_ = std::move(e);
    if (endpoints_.port == 0) {
      // Pick an ephemeral port; QuickFIX SocketAcceptor binds at start().
      // We resolve "0" to a real port by binding a probe socket first.
      int probed = pick_ephemeral_port();
      if (probed <= 0) {
        return make_error(ErrorCategory::Io, "could not pick ephemeral port");
      }
      endpoints_.port = probed;
    }
    try {
      const auto rendered = render_acceptor_settings(endpoints_);
      std::istringstream stream(rendered);
      settings_ = std::unique_ptr<FIX::SessionSettings>(
          new FIX::SessionSettings(stream));
      store_    = std::unique_ptr<FIX::FileStoreFactory>(
          new FIX::FileStoreFactory(*settings_));
      log_      = std::unique_ptr<FIX::FileLogFactory>(
          new FIX::FileLogFactory(*settings_));
      acceptor_ = std::unique_ptr<FIX::SocketAcceptor>(
          new FIX::SocketAcceptor(app_, *store_, *settings_, *log_));
      acceptor_->start();
    } catch (const FIX::ConfigError& e) {
      return make_error(ErrorCategory::Validation,
                        std::string("acceptor config: ") + e.what());
    } catch (const FIX::RuntimeError& e) {
      return make_error(ErrorCategory::Io,
                        std::string("acceptor runtime: ") + e.what());
    } catch (const std::exception& e) {
      return make_error(ErrorCategory::Io, e.what());
    }
    return endpoints_.port;
  }

  void stop() {
    if (acceptor_) acceptor_->stop();
    acceptor_.reset();
    log_.reset();
    store_.reset();
    settings_.reset();
  }

  ~AcceptorMock() { stop(); }

  int port() const noexcept { return endpoints_.port; }

 private:
  // pick_ephemeral_port forward-declared; defined in acceptor_mock.cpp to
  // keep <sys/socket.h> out of every TU including this header.
  static int pick_ephemeral_port();

  AcceptorEndpoints                       endpoints_;
  AcceptorMockApp                         app_;
  std::unique_ptr<FIX::SessionSettings>   settings_;
  std::unique_ptr<FIX::FileStoreFactory>  store_;
  std::unique_ptr<FIX::FileLogFactory>    log_;
  std::unique_ptr<FIX::SocketAcceptor>    acceptor_;
};

}  // namespace bist::fix::detail

#endif  // BIST_HAS_QUICKFIX
