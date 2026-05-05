#pragma once
//
// bist/fix/facade.hpp — POD-only public facade for the FIX layer.
//
// Phase-2.2 isolation contract:
//   - This header is the ONLY surface the rest of the project sees of FIX.
//   - It exposes plain-old-data argument and event structs plus opaque
//     `OeClient` / `RdClient` / `DcClient` types backed by Pimpl pointers.
//   - QuickFIX/C++ headers are forbidden in any consumer TU because the
//     vendor still uses `std::auto_ptr` and dynamic exception specs that
//     don't parse under C++17+. The implementation lives behind .cpp
//     files compiled at -std=gnu++14 (see src/fix/CMakeLists.txt).
//
// All numeric prices are wire-level signed integers; conversion to FIX's
// floating-point Price field happens inside the Impl using the supplied
// `price_decimals` per instrument (the same TL→wire scale the OUCH layer
// uses, so price wire integers stay consistent across protocols).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bist/core/result.hpp"
#include "bist/core/types.hpp"

namespace bist::fix {

// --- Initiator config (POD) --------------------------------------------------
//
// Mirrors the BIST cert geometry but stays free of any QuickFIX type so
// callers can construct it from plain TOML without including FIX headers.

struct InitiatorConfig {
  std::string  sender_comp_id;
  std::string  target_comp_id;
  std::string  host;
  std::uint16_t port{0};
  std::string  username;          // 553 Username
  std::string  password;          // 554 Password (LLL on first cert logon)
  std::string  new_password;      // 925 NewPassword (MMM after rotation)
  int          heartbeat_secs{30};
  std::string  store_path  = "state/fix";
  std::string  log_path    = "log/fix";
  std::string  app_data_dictionary;        // path to FIX 5.0 SP2 XML
  std::string  transport_data_dictionary;  // path to FIXT 1.1 XML
};

// --- Order Entry argument structs (POD) -------------------------------------

enum class OrdType : std::uint8_t {
  Limit            = 0,
  Market           = 1,
  MarketToLimit    = 2,
  Imbalance        = 3,
  MidpointLimit    = 4,
  MidpointMarket   = 5,
};

enum class FixCategory : std::uint8_t {
  Client    = 0,
  House     = 1,
  Fund      = 2,
  Portfolio = 3,
};

struct PlaceArgs {
  std::string   cl_ord_id;
  std::string   symbol;
  Side          side{Side::Buy};
  Quantity      quantity{0};
  PriceInt      price{0};
  std::int8_t   price_decimals{3};
  OrdType       ord_type{OrdType::Limit};
  TimeInForce   tif{TimeInForce::Day};
  FixCategory   category{FixCategory::Client};
  std::string   account;            // 1
  std::string   afk;                // PartyID under PartyRole=76
  Quantity      display_quantity{0};
};

struct CancelArgs {
  std::string  cl_ord_id;
  std::string  orig_cl_ord_id;
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     quantity{0};
};

struct ReplaceArgs {
  std::string  cl_ord_id;
  std::string  orig_cl_ord_id;
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     new_quantity{0};
  PriceInt     new_price{0};
  std::int8_t  price_decimals{3};
  OrdType      ord_type{OrdType::Limit};
};

struct TradeReportArgs {
  std::string  trade_report_id;
  std::string  trade_report_ref_id;
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     quantity{0};
  PriceInt     price{0};
  std::int8_t  price_decimals{3};
  std::string  counterparty_id;
  bool         two_party_internal{false};
};

// --- Event structs (POD) ----------------------------------------------------
//
// These cover the cert program's assertion surface; field-level comparisons
// are kept stringly so the runner stays uniform with the OUCH events.

struct ExecutionReportEvent {
  std::string   cl_ord_id;
  std::string   orig_cl_ord_id;
  std::string   exec_id;
  std::string   order_id;
  std::string   symbol;
  Side          side{Side::Buy};
  char          exec_type{'\0'};      // 150
  char          ord_status{'\0'};     // 39
  Quantity      leaves_qty{0};        // 151
  Quantity      cum_qty{0};
  Quantity      last_qty{0};
  PriceInt      last_price{0};
  std::int8_t   price_decimals{3};
  std::string   text;                 // 58 — rejection detail
};

struct CancelRejectEvent {
  std::string  cl_ord_id;
  std::string  orig_cl_ord_id;
  std::string  text;
};

struct TradeReportEvent {
  std::string  trade_report_id;
  std::string  trade_report_ref_id;
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     quantity{0};
  PriceInt     price{0};
  std::int8_t  price_decimals{3};
  std::string  counterparty_id;
};

struct QuoteStatusEvent {
  std::string  quote_id;
  std::string  symbol;
  Side         side{Side::Buy};
  std::uint32_t quote_status{0};
  Quantity     leaves_qty{0};
  PriceInt     price{0};
  std::int8_t  price_decimals{3};
};

enum class FixSessionState : std::uint8_t {
  Disconnected,
  LoggingIn,
  Active,
  PasswordExpired,
  PasswordChanged,
  LoggedOut,
  Failed,
};

struct LogonResult {
  FixSessionState state{FixSessionState::Disconnected};
  int             session_status{-1};   // 1409
  std::string     detail;
};

// --- Client API (opaque pimpl) ----------------------------------------------
//
// Each *Client owns one QuickFIX SocketInitiator + Application + Session.
// Construction starts the underlying initiator; destruction stops it.
// All callbacks fire on QuickFIX's internal thread; consumers must marshal
// to their own threads (the runner uses a side queue).

class OeClient {
 public:
  using OnExecution    = std::function<void(const ExecutionReportEvent&)>;
  using OnCancelReject = std::function<void(const CancelRejectEvent&)>;
  using OnTradeReport  = std::function<void(const TradeReportEvent&)>;
  using OnSession      = std::function<void(const LogonResult&)>;

  struct Callbacks {
    OnExecution    on_execution;
    OnCancelReject on_cancel_reject;
    OnTradeReport  on_trade_report;
    OnSession      on_session;
  };

  static Result<std::unique_ptr<OeClient>> create(InitiatorConfig cfg,
                                                   Callbacks cbs);
  ~OeClient();

  Result<std::string> place(const PlaceArgs& a);
  Result<std::string> cancel(const CancelArgs& a);
  Result<std::string> replace(const ReplaceArgs& a);
  Result<std::string> trade_report(const TradeReportArgs& a);
  Result<void>        logout();
  // Re-drive a fresh Logon over the existing SocketInitiator. Use after an
  // explicit logout() to satisfy cert flows that exercise multiple logons
  // within a single run (e.g. fix_oe_temel cert steps 6-8 logout/relogon).
  Result<void>        login();

  [[nodiscard]] FixSessionState state() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit OeClient(std::unique_ptr<Impl> i) noexcept;
};

class RdClient {
 public:
  struct Callbacks {
    std::function<void(const std::string& symbol)>     on_security_def;
    std::function<void(const std::string& symbol)>     on_security_status;
    std::function<void(const LogonResult&)>            on_session;
    std::function<void(bool ok, std::string detail)>   on_amr_ack;
  };

  static Result<std::unique_ptr<RdClient>> create(InitiatorConfig cfg,
                                                   Callbacks cbs);
  ~RdClient();

  Result<std::string> subscribe_all(const std::string& appl_req_id);
  Result<void>        logout();
  Result<void>        login();

  [[nodiscard]] FixSessionState state() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit RdClient(std::unique_ptr<Impl> i) noexcept;
};

class DcClient {
 public:
  struct Callbacks {
    std::function<void(const ExecutionReportEvent&)>  on_execution;
    std::function<void(const TradeReportEvent&)>      on_trade_report;
    std::function<void(const QuoteStatusEvent&)>      on_quote_status;
    std::function<void(const LogonResult&)>           on_session;
  };

  static Result<std::unique_ptr<DcClient>> create(InitiatorConfig cfg,
                                                   Callbacks cbs);
  ~DcClient();

  Result<void> logout();
  Result<void> login();

  [[nodiscard]] FixSessionState state() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit DcClient(std::unique_ptr<Impl> i) noexcept;
};

}  // namespace bist::fix
