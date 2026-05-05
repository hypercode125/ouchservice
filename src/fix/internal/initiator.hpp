#pragma once
//
// src/fix/internal/initiator.hpp — owns one QuickFIX SocketInitiator paired
// with a session.cfg generated from a POD InitiatorConfig.
//
// QuickFIX itself is single-threaded inside the SocketInitiator; the
// constructor's start() spins up its worker. We don't own the Application
// here because each *Client owns its own subclass; this helper just wires
// the lifetime knobs.

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <quickfix/Application.h>
#include <quickfix/FileLog.h>
#include <quickfix/FileStore.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketInitiator.h>

#include "bist/core/result.hpp"
#include "bist/fix/facade.hpp"

namespace bist::fix::detail {

inline std::string render_session_settings(const InitiatorConfig& c) {
  std::ostringstream o;
  o << "[DEFAULT]\n"
    << "ConnectionType=initiator\n"
    << "ReconnectInterval=1\n"
    << "FileStorePath=" << c.store_path << "\n"
    << "FileLogPath="   << c.log_path   << "\n"
    << "StartTime=00:00:00\n"
    << "EndTime=23:59:59\n"
    << "HeartBtInt="    << c.heartbeat_secs << "\n"
    << "TransportDataDictionary=" << c.transport_data_dictionary << "\n"
    << "AppDataDictionary="       << c.app_data_dictionary       << "\n"
    << "UseDataDictionary=N\n"
    << "ResetOnLogon=Y\n"
    << "ValidateUserDefinedFields=N\n"
    << "ValidateIncomingMessage=N\n"
    << "ValidateFieldsOutOfOrder=N\n"
    << "ValidateFieldsHaveValues=N\n"
    << "RejectInvalidMessage=N\n"
    << "AllowUnknownMsgFields=Y\n"
    << "[SESSION]\n"
    << "BeginString=FIXT.1.1\n"
    << "DefaultApplVerID=FIX.5.0SP2\n"
    << "SenderCompID=" << c.sender_comp_id << "\n"
    << "TargetCompID=" << c.target_comp_id << "\n"
    << "SocketConnectHost=" << c.host << "\n"
    << "SocketConnectPort=" << c.port << "\n";
  return o.str();
}

class FixInitiator {
 public:
  Result<void> start(const InitiatorConfig& cfg, FIX::Application& app) {
    try {
      const auto rendered = render_session_settings(cfg);
      std::istringstream stream(rendered);
      settings_  = std::unique_ptr<FIX::SessionSettings>(
          new FIX::SessionSettings(stream));
      store_     = std::unique_ptr<FIX::FileStoreFactory>(
          new FIX::FileStoreFactory(*settings_));
      log_       = std::unique_ptr<FIX::FileLogFactory>(
          new FIX::FileLogFactory(*settings_));
      initiator_ = std::unique_ptr<FIX::SocketInitiator>(
          new FIX::SocketInitiator(app, *store_, *settings_, *log_));
      initiator_->start();
    } catch (const FIX::ConfigError& e) {
      return make_error(ErrorCategory::Validation,
                        std::string("FIX config: ") + e.what());
    } catch (const FIX::RuntimeError& e) {
      return make_error(ErrorCategory::Io,
                        std::string("FIX runtime: ") + e.what());
    } catch (const std::exception& e) {
      return make_error(ErrorCategory::Io, e.what());
    }
    return {};
  }

  void stop() {
    if (initiator_) initiator_->stop();
    initiator_.reset();
    log_.reset();
    store_.reset();
    settings_.reset();
  }

  ~FixInitiator() { stop(); }

 private:
  std::unique_ptr<FIX::SessionSettings>   settings_;
  std::unique_ptr<FIX::FileStoreFactory>  store_;
  std::unique_ptr<FIX::FileLogFactory>    log_;
  std::unique_ptr<FIX::SocketInitiator>   initiator_;
};

}  // namespace bist::fix::detail
