// src/risk/ptrm.cpp — placeholder PTRM REST/JWT client.
//
// Production wiring will use libcurl + a JWT library to talk to the BIST
// PTRM REST endpoint documented in spec_ptrm.md. Member credentials and
// the base URL are environment-specific and only available inside the
// BIST member network, so this TU ships as an "Unsupported" stub until a
// member host wires real network calls.
//
// The PtrmAlwaysAccept / PtrmAlwaysReject implementations in the header
// remain available for unit tests and the LogOnly / Disabled paths.

#include "bist/risk/ptrm.hpp"

namespace bist::risk {

struct PtrmRestClient::Impl {
  Config cfg;
};

PtrmRestClient::PtrmRestClient(std::unique_ptr<Impl> i) noexcept
    : impl_(std::move(i)) {}
PtrmRestClient::~PtrmRestClient() = default;

Result<std::unique_ptr<PtrmRestClient>>
PtrmRestClient::create(Config cfg) {
  // Until libcurl + JWT are wired we accept the construction so callers
  // can register the gate in dependency-injection wiring; check() returns
  // a permissive decision in Disabled mode and an Unsupported-shaped
  // reject otherwise so the operator notices the missing integration.
  auto impl = std::unique_ptr<Impl>(new Impl{std::move(cfg)});
  return std::unique_ptr<PtrmRestClient>(new PtrmRestClient(std::move(impl)));
}

PtrmDecision PtrmRestClient::check(const PreTradeOrder&) noexcept {
  if (impl_->cfg.mode == PtrmMode::Disabled) {
    return PtrmDecision{};
  }
  PtrmDecision d{};
  d.allow  = false;
  d.rx     = RxCategory::RxOther;
  d.detail = "PtrmRestClient stub: enable real REST integration before "
             "production deploy (see spec_ptrm.md)";
  return d;
}

}  // namespace bist::risk
