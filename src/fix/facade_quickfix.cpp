// src/fix/facade_quickfix.cpp — implements the public bist::fix::OeClient
// / RdClient / DcClient pimpl types declared in include/bist/fix/facade.hpp.
//
// This translation unit (and only this one) bridges the public POD facade
// to QuickFIX. Everything inside the bist::fix::detail namespace below is
// compiled at -std=gnu++14 (see src/fix/CMakeLists.txt) so the QuickFIX
// 1.15.1 headers (auto_ptr, dynamic-exception-spec) parse cleanly.

#if defined(BIST_HAS_QUICKFIX)

#include "bist/fix/facade.hpp"

#include <memory>
#include <utility>

#include "src/fix/internal/dc_app.hpp"
#include "src/fix/internal/initiator.hpp"
#include "src/fix/internal/oe_app.hpp"
#include "src/fix/internal/rd_app.hpp"

namespace bist::fix {

// ============================================================================
// OeClient
// ============================================================================

struct OeClient::Impl {
  detail::OeApp            app;
  detail::FixInitiator     initiator;

  explicit Impl(Callbacks cbs) : app(std::move(cbs)) {}
};

OeClient::OeClient(std::unique_ptr<Impl> i) noexcept : impl_(std::move(i)) {}

OeClient::~OeClient() = default;

Result<std::unique_ptr<OeClient>> OeClient::create(InitiatorConfig cfg,
                                                    Callbacks cbs) {
  auto impl = std::unique_ptr<Impl>(new Impl(std::move(cbs)));
  impl->app.set_password(cfg.password, cfg.new_password);
  if (auto r = impl->initiator.start(cfg, impl->app); !r) {
    return r.error();
  }
  return std::unique_ptr<OeClient>(new OeClient(std::move(impl)));
}

Result<std::string> OeClient::place(const PlaceArgs& a)   { return impl_->app.place(a); }
Result<std::string> OeClient::cancel(const CancelArgs& a) { return impl_->app.cancel(a); }
Result<std::string> OeClient::replace(const ReplaceArgs& a) { return impl_->app.replace(a); }
Result<std::string> OeClient::trade_report(const TradeReportArgs& a) {
  return impl_->app.trade_report(a);
}
Result<void> OeClient::logout() { return impl_->app.logout(); }
Result<void> OeClient::login()  { return impl_->app.login(); }
FixSessionState OeClient::state() const noexcept { return impl_->app.state(); }

// ============================================================================
// RdClient
// ============================================================================

struct RdClient::Impl {
  detail::RdApp           app;
  detail::FixInitiator    initiator;

  explicit Impl(Callbacks cbs) : app(std::move(cbs)) {}
};

RdClient::RdClient(std::unique_ptr<Impl> i) noexcept : impl_(std::move(i)) {}

RdClient::~RdClient() = default;

Result<std::unique_ptr<RdClient>> RdClient::create(InitiatorConfig cfg,
                                                    Callbacks cbs) {
  auto impl = std::unique_ptr<Impl>(new Impl(std::move(cbs)));
  impl->app.set_password(cfg.password);
  if (auto r = impl->initiator.start(cfg, impl->app); !r) {
    return r.error();
  }
  return std::unique_ptr<RdClient>(new RdClient(std::move(impl)));
}

Result<std::string> RdClient::subscribe_all(const std::string& id) {
  return impl_->app.subscribe_all(id);
}
Result<void> RdClient::logout() { return impl_->app.logout(); }
Result<void> RdClient::login()  { return impl_->app.login(); }
FixSessionState RdClient::state() const noexcept { return impl_->app.state(); }

// ============================================================================
// DcClient
// ============================================================================

struct DcClient::Impl {
  detail::DcApp           app;
  detail::FixInitiator    initiator;

  explicit Impl(Callbacks cbs) : app(std::move(cbs)) {}
};

DcClient::DcClient(std::unique_ptr<Impl> i) noexcept : impl_(std::move(i)) {}

DcClient::~DcClient() = default;

Result<std::unique_ptr<DcClient>> DcClient::create(InitiatorConfig cfg,
                                                    Callbacks cbs) {
  auto impl = std::unique_ptr<Impl>(new Impl(std::move(cbs)));
  if (auto r = impl->initiator.start(cfg, impl->app); !r) {
    return r.error();
  }
  return std::unique_ptr<DcClient>(new DcClient(std::move(impl)));
}

Result<void> DcClient::logout() { return impl_->app.logout(); }
Result<void> DcClient::login()  { return impl_->app.login(); }
FixSessionState DcClient::state() const noexcept { return impl_->app.state(); }

}  // namespace bist::fix

#endif  // BIST_HAS_QUICKFIX
