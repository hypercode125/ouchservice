#pragma once
//
// bist/runner/scenario.hpp — in-memory representation of a YAML cert scenario.
//
// The structure is intentionally shallow so that the loader and the executor
// stay easy to read. We use std::variant for the args field so that every
// supported step keeps its parameters strongly typed.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "bist/core/types.hpp"

namespace bist::runner {

// --- Per-action argument structs --------------------------------------------

struct LoginArgs {
  std::string session;
  std::uint64_t sequence{0};
  std::string password;
};

struct WaitHeartbeatArgs {
  std::string from;            // "server" or "client"
  int         count{1};
  int         timeout_ms{1500};
};

struct PlaceArgs {
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     quantity{0};
  std::string  price_str;          // unparsed; resolved against InstrumentCache
  TimeInForce  tif{TimeInForce::Day};
  ClientCategory category{ClientCategory::Client};
  std::string  client_account;
  std::string  customer_info;
  std::string  exchange_info;
  std::string  token;              // numeric or alphanumeric, untouched
  Quantity     display_quantity{0};
};

struct CancelByTokenArgs {
  std::string token;
};

struct CancelByOrderIdArgs {
  std::string token_ref;            // refers to a prior token; runner resolves
};

struct ReplaceArgs {
  std::string  existing_token;
  std::string  new_token;
  Quantity     quantity{0};
  std::string  price_str;
  ClientCategory category{ClientCategory::Client};
  std::string  client_account;
  Quantity     display_quantity{0};
};

struct QuoteEntrySpec {
  std::string symbol;
  std::string bid_px;
  Quantity    bid_size{0};
  std::string offer_px;
  Quantity    offer_size{0};
};

struct MassQuoteArgs {
  std::string                  token;
  ClientCategory               category{ClientCategory::Client};
  std::string                  afk;             // PYM / PYP
  std::string                  exchange_info;
  std::vector<QuoteEntrySpec>  entries;
};

// Empty marker types for argument-less actions.
struct LogoutArgs {};
struct TriggerOpeningMatchArgs {};
struct InactivateAllArgs {};

// Live-only failover drill: tag picks "primary" (default) or "secondary".
// Sequence number is preserved per cert directive; the runner passes the
// current session_id + next_inbound_seq through the transport so the new
// gateway resumes mid-stream.
struct SwitchGatewayArgs {
  std::string tag;            // "primary" or "secondary"
};

// Bölüm 2 throttling drill: burst N orders cycling through a symbol pattern
// at `rate_per_sec` (cert default 100/s for 1000 orders → 10 s wall time).
// Tokens are derived from `<token_prefix><sequence>` so each send is unique.
struct BurstPlacePatternEntry {
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     quantity{0};
  std::string  price_str;
  std::string  token_prefix;        // e.g. "10000" → 10000, 10001, 10002, …
  TimeInForce  tif{TimeInForce::Day};
  ClientCategory category{ClientCategory::Client};
};

struct BurstPlaceArgs {
  std::uint32_t                       count{0};
  std::uint32_t                       rate_per_sec{100};
  std::vector<BurstPlacePatternEntry> pattern;
};

// --- FIX action arguments ---------------------------------------------------

struct FixLogonArgs {
  std::string  password;          // 554
  std::string  new_password;      // 925 (optional, drives 554/925 rotation)
  bool         sequence_reset{false};
  std::uint32_t heartbeat_secs{30};
};

struct FixLogoutArgs {};

struct FixAmrSubscribeArgs {
  std::string appl_req_id;        // ApplReqID for AMR (BW)
};

struct FixWaitHeartbeatArgs {
  std::string from;              // "server" or "client"
  int         count{1};
  int         timeout_ms{32000}; // RD HB default 30s + jitter
};

struct FixPlaceArgs {
  std::string  cl_ord_id;
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     quantity{0};
  std::string  price_str;
  TimeInForce  tif{TimeInForce::Day};
  std::string  ord_type;          // "LIMIT","MARKET","MTL","IOC","FOK","ICEBERG","MIDPOINT_LIMIT","MIDPOINT_MARKET","IMBALANCE","AOF"
  ClientCategory category{ClientCategory::Client};
  std::string  account;
  std::string  afk;
  Quantity     display_quantity{0};
};

struct FixCancelArgs {
  std::string  cl_ord_id;
  std::string  orig_cl_ord_id;
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     quantity{0};
};

struct FixReplaceArgs {
  std::string  cl_ord_id;
  std::string  orig_cl_ord_id;
  std::string  symbol;
  Side         side{Side::Buy};
  Quantity     new_quantity{0};
  std::string  price_str;
  std::string  ord_type;
};

struct FixTriggerMatchArgs {};

// --- Expectations ------------------------------------------------------------

struct Expect {
  std::string                                          msg;        // e.g. "order_accepted"
  std::map<std::string, std::string>                   fields;     // string-typed for now
  int                                                  occurrences{1};
  int                                                  timeout_ms{0};   // 0 → use scenario default
};

// --- Step --------------------------------------------------------------------

using StepArgs = std::variant<
    std::monostate,
    LoginArgs,
    WaitHeartbeatArgs,
    PlaceArgs,
    CancelByTokenArgs,
    CancelByOrderIdArgs,
    ReplaceArgs,
    MassQuoteArgs,
    LogoutArgs,
    TriggerOpeningMatchArgs,
    InactivateAllArgs,
    SwitchGatewayArgs,
    BurstPlaceArgs,
    FixLogonArgs,
    FixLogoutArgs,
    FixAmrSubscribeArgs,
    FixWaitHeartbeatArgs,
    FixPlaceArgs,
    FixCancelArgs,
    FixReplaceArgs,
    FixTriggerMatchArgs>;

struct Step {
  int                  id{0};
  std::string          action;
  StepArgs             args{std::monostate{}};
  std::optional<Expect> expect;
};

// --- Scenario ----------------------------------------------------------------

struct Scenario {
  std::string                              name;
  std::string                              description;
  std::unordered_map<std::string, std::string> preconditions;
  int                                      default_timeout_ms{5000};
  bool                                     mock_skip{false};   // top-level flag
  std::vector<Step>                        steps;
  std::string                              source_path;
};

// --- YAML loader -------------------------------------------------------------

// Loads a YAML file from disk and returns the parsed Scenario. Throws
// std::runtime_error on parse failure.
Scenario load_scenario(const std::string& path);

}  // namespace bist::runner
