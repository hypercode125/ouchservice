#pragma once
//
// bist/risk/ptrm.hpp — pre-trade risk gate against BIST PTRM REST/JWT.
//
// BIST Pre-Trade Risk Management evaluates 15 riskTypes per outbound
// order. The gateway returns RX_* statuses; OUCH cancel-reason 115-125
// surface PTRM rejections back over the OE wire.
//
// This header defines a transport-agnostic interface so a real HTTP
// client (libcurl + JWT) and an in-process mock can both satisfy it.
// Production wiring instantiates `PtrmRestClient`; bench/unit tests use
// `PtrmAlwaysAccept` or `PtrmAlwaysReject`.
//
// The gate is invoked synchronously from the hot thread before
// OuchClient::place forwards the EnterOrder. With `Mode::LogOnly` the
// reject becomes a structured warning instead of a refusal — useful for
// staging environments where PTRM is not yet provisioned.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "bist/core/result.hpp"
#include "bist/core/types.hpp"

namespace bist::risk {

enum class PtrmMode : std::uint8_t {
  Disabled,    // never call PTRM, always allow (dev/mock default)
  LogOnly,     // call PTRM, log rejections, allow the order
  Enforce,     // production: reject locally on PTRM denial
};

// Subset of BIST PTRM riskTypes documented in spec_ptrm.md. The numeric
// codes match BIST's OUCH cancel-reason mapping (115-125) so audit
// review can correlate without lookup tables.
enum class RxCategory : std::uint8_t {
  None                       = 0,
  RxPositionLimit            = 115,
  RxOrderRateLimit           = 116,
  RxOrderQuantityLimit       = 117,
  RxOrderValueLimit          = 118,
  RxLossLimit                = 119,
  RxMarginCallExceeded       = 120,
  RxRestrictedInstrument     = 121,
  RxAccountSuspended         = 122,
  RxPriceCollar              = 123,
  RxKillSwitch               = 124,
  RxOther                    = 125,
};

constexpr const char* rx_name(RxCategory c) noexcept {
  switch (c) {
    case RxCategory::None:                      return "ok";
    case RxCategory::RxPositionLimit:           return "position_limit";
    case RxCategory::RxOrderRateLimit:          return "order_rate_limit";
    case RxCategory::RxOrderQuantityLimit:      return "order_qty_limit";
    case RxCategory::RxOrderValueLimit:         return "order_value_limit";
    case RxCategory::RxLossLimit:               return "loss_limit";
    case RxCategory::RxMarginCallExceeded:      return "margin_call";
    case RxCategory::RxRestrictedInstrument:    return "restricted_instrument";
    case RxCategory::RxAccountSuspended:        return "account_suspended";
    case RxCategory::RxPriceCollar:             return "price_collar";
    case RxCategory::RxKillSwitch:              return "kill_switch";
    case RxCategory::RxOther:                   return "rx_other";
  }
  return "unknown";
}

struct PreTradeOrder {
  std::string  symbol;
  std::uint32_t order_book_id{0};
  Side         side{Side::Buy};
  Quantity     quantity{0};
  PriceInt     price{0};
  std::int8_t  price_decimals{3};
  std::string  account;
};

struct PtrmDecision {
  bool        allow{true};
  RxCategory  rx{RxCategory::None};
  std::string detail;
};

// Abstract gate; production wiring uses PtrmRestClient (libcurl), tests
// use PtrmAlwaysAccept / PtrmAlwaysReject.
class IPtrmGate {
 public:
  virtual ~IPtrmGate() = default;
  virtual PtrmDecision check(const PreTradeOrder& o) noexcept = 0;
};

class PtrmAlwaysAccept final : public IPtrmGate {
 public:
  PtrmDecision check(const PreTradeOrder&) noexcept override {
    return PtrmDecision{};
  }
};

class PtrmAlwaysReject final : public IPtrmGate {
 public:
  explicit PtrmAlwaysReject(RxCategory rx, std::string detail = {}) noexcept
      : rx_(rx), detail_(std::move(detail)) {}
  PtrmDecision check(const PreTradeOrder&) noexcept override {
    return PtrmDecision{false, rx_, detail_};
  }
 private:
  RxCategory  rx_;
  std::string detail_;
};

// REST/JWT-backed implementation. The header is forward-declared so
// libcurl is contained in the .cpp; consumers don't pay the include cost.
class PtrmRestClient final : public IPtrmGate {
 public:
  struct Config {
    std::string base_url;        // e.g. https://ptrm.bist.example
    std::string member_id;       // BIST member id for JWT subject
    std::string api_key;         // shared secret used to sign the JWT
    std::chrono::milliseconds timeout_ms{500};
    PtrmMode    mode{PtrmMode::Disabled};
  };

  static Result<std::unique_ptr<PtrmRestClient>> create(Config cfg);
  ~PtrmRestClient();

  PtrmDecision check(const PreTradeOrder& o) noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit PtrmRestClient(std::unique_ptr<Impl> i) noexcept;
};

// Helper that resolves Mode-aware behaviour: Disabled => allow,
// LogOnly => allow but log, Enforce => follow gate decision.
inline PtrmDecision evaluate(IPtrmGate& gate, PtrmMode mode,
                             const PreTradeOrder& o) noexcept {
  if (mode == PtrmMode::Disabled) return PtrmDecision{};
  PtrmDecision d = gate.check(o);
  if (mode == PtrmMode::LogOnly && !d.allow) {
    d.allow  = true;
    d.detail = std::string{"[log_only] "} + d.detail;
  }
  return d;
}

}  // namespace bist::risk
