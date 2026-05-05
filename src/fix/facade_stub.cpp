// src/fix/facade_stub.cpp — always-built no-op implementation of the
// public FIX facade. When BIST_HAS_QUICKFIX is defined the real bridge in
// facade_quickfix.cpp wins (it provides full definitions of the same
// symbols and the stub TU is ignored by the linker). When QuickFIX is
// disabled, every facade method returns ErrorCategory::Unsupported so
// consumers get a friendly runtime error rather than a link failure.

#if !defined(BIST_HAS_QUICKFIX)

#include "bist/fix/acceptor_mock.hpp"
#include "bist/fix/facade.hpp"

#include <memory>
#include <utility>

namespace bist::fix {

struct OeClient::Impl {};
struct RdClient::Impl {};
struct DcClient::Impl {};
struct AcceptorMock::Impl {};

OeClient::OeClient(std::unique_ptr<Impl> i) noexcept : impl_(std::move(i)) {}
RdClient::RdClient(std::unique_ptr<Impl> i) noexcept : impl_(std::move(i)) {}
DcClient::DcClient(std::unique_ptr<Impl> i) noexcept : impl_(std::move(i)) {}
AcceptorMock::AcceptorMock(std::unique_ptr<Impl> i) noexcept : impl_(std::move(i)) {}
OeClient::~OeClient() = default;
RdClient::~RdClient() = default;
DcClient::~DcClient() = default;
AcceptorMock::~AcceptorMock() = default;

namespace {
Error disabled() {
  return make_error(ErrorCategory::Unsupported,
                    "FIX disabled: rebuild with BIST_BUILD_FIX=ON");
}
}  // namespace

Result<std::unique_ptr<OeClient>> OeClient::create(InitiatorConfig, Callbacks) { return disabled(); }
Result<std::unique_ptr<RdClient>> RdClient::create(InitiatorConfig, Callbacks) { return disabled(); }
Result<std::unique_ptr<DcClient>> DcClient::create(InitiatorConfig, Callbacks) { return disabled(); }

Result<std::string> OeClient::place(const PlaceArgs&)        { return disabled(); }
Result<std::string> OeClient::cancel(const CancelArgs&)      { return disabled(); }
Result<std::string> OeClient::replace(const ReplaceArgs&)    { return disabled(); }
Result<std::string> OeClient::trade_report(const TradeReportArgs&) { return disabled(); }
Result<void>        OeClient::logout()                       { return disabled(); }
Result<void>        OeClient::login()                        { return disabled(); }
FixSessionState     OeClient::state() const noexcept         { return FixSessionState::Disconnected; }

Result<std::string> RdClient::subscribe_all(const std::string&) { return disabled(); }
Result<void>        RdClient::logout()                          { return disabled(); }
Result<void>        RdClient::login()                           { return disabled(); }
FixSessionState     RdClient::state() const noexcept            { return FixSessionState::Disconnected; }

Result<void>        DcClient::logout()                          { return disabled(); }
Result<void>        DcClient::login()                           { return disabled(); }
FixSessionState     DcClient::state() const noexcept            { return FixSessionState::Disconnected; }

Result<std::unique_ptr<AcceptorMock>> AcceptorMock::create(AcceptorMockConfig) { return disabled(); }
int AcceptorMock::port() const noexcept { return 0; }

}  // namespace bist::fix

#endif  // !BIST_HAS_QUICKFIX
