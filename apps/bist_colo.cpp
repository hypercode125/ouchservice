// apps/bist_colo.cpp — main entry point.
//
// Phase-1 wiring:
//   --version                       print and exit
//   --help                          print usage
//   --mock                          spin up the embedded mock gateway
//   --config <path>                 load the gateway endpoints from a TOML
//   --partition <n>                 OUCH partition (1..N, default 1)
//   --secondary                     dial host_secondary instead of host_primary
//   --replay <path>                 run a YAML scenario or a directory
//                                   of scenarios end-to-end
//
// The binary connects an OuchSession to either the mock gateway (loopback)
// or the real BIST endpoint, drives the SoupBinTCP login handshake, and
// executes the requested cert scenarios. Per-step pass/fail results are
// printed to stdout and the audit log records every wire byte.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <unistd.h>

#include "bist/app/reconnect_policy.hpp"
#include "bist/config/config.hpp"
#include "bist/core/result.hpp"
#include "bist/core/time.hpp"
#include "bist/domain/instrument.hpp"
#include "bist/domain/order_book.hpp"
#include "bist/domain/throttler.hpp"
#include "bist/domain/token_registry.hpp"
#include "bist/fix/acceptor_mock.hpp"
#include "bist/fix/facade.hpp"
#include "bist/mock/ouch_gateway.hpp"
#include "bist/net/tcp_socket.hpp"
#include "bist/observability/audit.hpp"
#include "bist/observability/sequence_store.hpp"
#include "bist/ouch/client.hpp"
#include "bist/ouch/session.hpp"
#include "bist/runner/fix_replay.hpp"
#include "bist/runner/replay.hpp"
#include "bist/runner/scenario.hpp"
#include "bist/runner/transport.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};

void request_stop(int) {
  g_stop_requested.store(true, std::memory_order_release);
}

void print_usage(const char* prog) {
  std::printf(
      "usage: %s [--config <path>] [--mock] [--partition N] [--secondary]\n"
      "       %*s [--interactive] [--replay <dir|file>]\n"
      "       %s --version\n"
      "\n"
      "Options:\n"
      "  --config <path>     TOML config with gateway endpoints + creds.\n"
      "  --mock              Run the embedded mock gateway (no network).\n"
      "  --partition <n>     OUCH partition (1..N) — selects port from [ouch].ports.\n"
      "  --secondary         Dial [ouch].host_secondary (gateway failover drill).\n"
      "  --interactive       Drop into a REPL after sessions reach Active.\n"
      "  --replay <path>     Replay a YAML scenario file or every YAML in a dir.\n"
      "  --version           Print version + wire-size receipts and exit.\n",
      prog, static_cast<int>(std::string_view{prog}.size()), "", prog);
}

void print_version() {
  std::printf("bist_colo 0.1.0  (BISTECH OUCH Feb 2025 + FIX 5.0 SP2 OE/RD/DC Temel)\n");
  std::printf("  OUCH wire sizes (compile-time verified):\n");
  std::printf("    EnterOrder            = %zu B (114)\n",  sizeof(bist::ouch::EnterOrder));
  std::printf("    ReplaceOrder          = %zu B (122)\n",  sizeof(bist::ouch::ReplaceOrder));
  std::printf("    CancelOrder           = %zu B (15)\n",   sizeof(bist::ouch::CancelOrder));
  std::printf("    CancelByOrderId       = %zu B (14)\n",   sizeof(bist::ouch::CancelByOrderId));
  std::printf("    MassQuoteHeader       = %zu B (50)\n",   sizeof(bist::ouch::MassQuoteHeader));
  std::printf("    QuoteEntry            = %zu B (28)\n",   sizeof(bist::ouch::QuoteEntry));
  std::printf("    OrderAccepted         = %zu B (137)\n",  sizeof(bist::ouch::OrderAccepted));
  std::printf("    OrderRejected         = %zu B (27)\n",   sizeof(bist::ouch::OrderRejected));
  std::printf("    OrderReplaced         = %zu B (145)\n",  sizeof(bist::ouch::OrderReplaced));
  std::printf("    OrderCanceled         = %zu B (37)\n",   sizeof(bist::ouch::OrderCanceled));
  std::printf("    OrderExecuted         = %zu B (68)\n",   sizeof(bist::ouch::OrderExecuted));
  std::printf("    MassQuoteAck          = %zu B (52)\n",   sizeof(bist::ouch::MassQuoteAck));
  std::printf("    MassQuoteRejection    = %zu B (31)\n",   sizeof(bist::ouch::MassQuoteRejection));
}

// Hard-coded subset of the BIST cert symbol table (Pay piyasası v1.7).
// Real deployments populate this from FIX RD Security Definition stream.
void seed_cert_instruments(bist::domain::InstrumentCache& cache) {
  using bist::domain::Instrument;
  // {symbol, OrderBookID, decimals, partition, base*1000}
  cache.put(Instrument{"ACSEL.E",   70616, 3, 1, 6000});
  cache.put(Instrument{"ADEL.E",    70676, 3, 1, 6000});
  cache.put(Instrument{"AKBNK.E",   70796, 3, 1, 5000});
  cache.put(Instrument{"ALCAR.E",   71116, 3, 1, 5000});
  cache.put(Instrument{"ARCLK.E",   71376, 3, 1, 5000});
  cache.put(Instrument{"ASELS.E",   71536, 3, 1, 5000});
  cache.put(Instrument{"ALBRK.E",   71096, 3, 1, 6000});
  cache.put(Instrument{"GARAN.E",   74196, 3, 2, 5000});
  cache.put(Instrument{"GEREL.E",   74416, 3, 2, 6000});
  cache.put(Instrument{"HATEK.E",   74816, 3, 2, 5000});
  cache.put(Instrument{"ECILC.E",   73396, 3, 2, 6000});
  cache.put(Instrument{"DJIST.F",   73156, 3, 2, 5000});
  cache.put(Instrument{"AKBNK.AOF", 201868, 3, 1, 5000});
  cache.put(Instrument{"GARAN.AOF", 336340, 3, 2, 5000});
  cache.put(Instrument{"KAREL.E",   75576, 3, 3, 5000});
  cache.put(Instrument{"MGROS.E",   76576, 3, 3, 5000});
  cache.put(Instrument{"OYAKC.E",   77046, 3, 3, 5000});
  cache.put(Instrument{"MAVI.E",    76386, 3, 3, 6000});
  cache.put(Instrument{"ZOREN.E",   79596, 3, 4, 5000});
  cache.put(Instrument{"TCELL.E",   78296, 3, 4, 7000});
  cache.put(Instrument{"VAKKO.E",   79096, 3, 4, 5000});
  cache.put(Instrument{"TBORG.E",   78276, 3, 4, 6000});
}

struct CliArgs {
  std::string config;
  bool        mock{false};
  bool        interactive{false};
  bool        secondary{false};
  int         partition{1};
  std::string replay;
};

CliArgs parse_args(int argc, char** argv) {
  CliArgs a;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--config" && i + 1 < argc) {
      a.config = argv[++i];
    } else if (arg == "--mock") {
      a.mock = true;
    } else if (arg == "--interactive") {
      a.interactive = true;
    } else if (arg == "--secondary") {
      a.secondary = true;
    } else if (arg == "--partition" && i + 1 < argc) {
      a.partition = std::atoi(argv[++i]);
      if (a.partition < 1) {
        std::fprintf(stderr, "--partition must be >= 1\n");
        std::exit(EXIT_FAILURE);
      }
    } else if (arg == "--replay" && i + 1 < argc) {
      a.replay = argv[++i];
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      print_usage(argv[0]);
      std::exit(EXIT_FAILURE);
    }
  }
  return a;
}

std::vector<std::filesystem::path> collect_scenarios(const std::string& path) {
  std::filesystem::path p{path};
  std::vector<std::filesystem::path> out;
  if (std::filesystem::is_directory(p)) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(p)) {
      if (e.is_regular_file()) {
        const auto ext = e.path().extension();
        if (ext == ".yaml" || ext == ".yml") out.push_back(e.path());
      }
    }
    std::sort(out.begin(), out.end());
  } else if (std::filesystem::is_regular_file(p)) {
    out.push_back(p);
  }
  return out;
}

// Dial host:port with exponential backoff bounded by a wall-clock deadline.
//
// The deadline is the **CoD reconnect budget**: the BIST matching engine
// inactivates orders 55-62 s after the OUCH session disappears, so we need
// to come back well before that. Deadline default 30 s; configurable from
// `[ouch].cod_reconnect_deadline_ms`.
//
// On deadline expiry the function returns Timeout and stamps `cod_risk` in
// the supplied state so the caller can write a "cod_risk" audit record.
bist::Result<bist::net::TcpSocket> connect_with_retry(
    std::string_view host, std::uint16_t port, bool tcp_nodelay,
    bist::app::ReconnectPolicy policy = {},
    bist::app::ReconnectState* state_out = nullptr) {
  bist::app::ReconnectState local_state{};
  auto& state = state_out ? *state_out : local_state;
  bist::net::TcpSocket result_sock;

  auto attempt_fn = [&](std::uint32_t attempt) -> bist::Result<void> {
    bist::net::TcpSocket sock;
    auto r = sock.connect(host, port, tcp_nodelay);
    if (r) {
      std::printf("connect %.*s:%u OK (attempt %u, deadline %u ms)\n",
                  static_cast<int>(host.size()), host.data(), port,
                  attempt, policy.deadline_ms);
      result_sock = std::move(sock);
      return {};
    }
    std::fprintf(stderr,
                 "connect %.*s:%u failed (attempt %u, deadline %u ms): %s\n",
                 static_cast<int>(host.size()), host.data(), port,
                 attempt, policy.deadline_ms, r.error().detail.c_str());
    return r.error();
  };
  auto rr = bist::app::reconnect_with_deadline(attempt_fn, policy, state);
  if (!rr) {
    if (state.cod_risk) {
      std::fprintf(stderr,
                   "WARNING: CoD reconnect budget (%u ms) elapsed across %u attempt(s); "
                   "BIST matching engine may inactivate resting orders within "
                   "the next 55-62 s window.\n",
                   policy.deadline_ms, state.attempts);
    }
    return rr.error();
  }
  return std::move(result_sock);
}

// Drive the SoupBinTCP login handshake to completion (Active or Failed).
bist::Result<void> drive_login(bist::ouch::OuchSession& session,
                               std::uint64_t timeout_ns = 5'000'000'000ULL) {
  if (auto r = session.begin_login(); !r) return r.error();
  const auto deadline = bist::monotonic_ns() + timeout_ns;
  while (session.state() == bist::ouch::SessionState::LoggingIn) {
    if (auto r = session.poll_io(); !r) return r.error();
    if (bist::monotonic_ns() > deadline) {
      return bist::make_error(bist::ErrorCategory::Timeout, "login timeout");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (session.state() != bist::ouch::SessionState::Active) {
    return bist::make_error(bist::ErrorCategory::Reject, "login rejected");
  }
  return {};
}

// Look up "protocol" in the scenario preconditions without parsing the
// full YAML body. Returns "ouch" by default for back-compat.
std::string scenario_protocol(const bist::runner::Scenario& sc) {
  auto it = sc.preconditions.find("protocol");
  if (it == sc.preconditions.end() || it->second.empty()) return "ouch";
  return it->second;
}

// Run one FIX scenario against the in-process AcceptorMock. Spawns one
// OeClient (for fix_oe scenarios) or one RdClient (for fix_rd) and drives
// the per-step dispatcher in FixScenarioRunner.
int run_one_fix_scenario(const std::filesystem::path& path,
                         const bist::runner::Scenario& sc,
                         const std::string& protocol) {
#if !defined(BIST_HAS_QUICKFIX)
  (void)path; (void)sc; (void)protocol;
  std::printf("--- skipping FIX scenario (build with BIST_BUILD_FIX=ON)\n");
  return 0;
#else
  (void)path;
  // SessionID swap: initiator side = CLIENT_*, mock acceptor = BIST_*.
  bist::fix::AcceptorMockConfig acfg;
  acfg.oe_initiator_sender_comp_id = "CLIENT_OE";
  acfg.oe_initiator_target_comp_id = "BIST_OE";
  acfg.rd_initiator_sender_comp_id = "CLIENT_RD";
  acfg.rd_initiator_target_comp_id = "BIST_RD";
  acfg.app_data_dictionary       = std::string(BIST_FIX_DICT_DIR) + "/FIX50SP2.xml";
  acfg.transport_data_dictionary = std::string(BIST_FIX_DICT_DIR) + "/FIXT11.xml";
  acfg.store_path = "state/fix_mock_" + std::to_string(::getpid());
  acfg.log_path   = "log/fix_mock_"   + std::to_string(::getpid());

  auto mock_r = bist::fix::AcceptorMock::create(acfg);
  if (!mock_r) {
    std::fprintf(stderr, "FIX acceptor mock: %s\n",
                 mock_r.error().detail.c_str());
    return 1;
  }
  auto mock = std::move(mock_r).value();
  const int port = mock->port();

  bist::runner::FixScenarioRunner runner(nullptr, nullptr, nullptr);

  std::unique_ptr<bist::fix::OeClient> oe;
  std::unique_ptr<bist::fix::RdClient> rd;

  // Common initiator config bits.
  auto fill_init_cfg = [&](bist::fix::InitiatorConfig& c,
                           const std::string& sender,
                           const std::string& target,
                           const std::string& tag) {
    c.sender_comp_id = sender;
    c.target_comp_id = target;
    c.host = "127.0.0.1";
    c.port = static_cast<std::uint16_t>(port);
    c.username = "TEST01";
    c.password = "LLL";
    if (sc.steps.size() && sc.steps.front().action == "fix_logon") {
      if (const auto* la =
              std::get_if<bist::runner::FixLogonArgs>(&sc.steps.front().args)) {
        if (!la->password.empty())     c.password     = la->password;
        if (!la->new_password.empty()) c.new_password = la->new_password;
        c.heartbeat_secs = static_cast<int>(la->heartbeat_secs);
      }
    }
    c.app_data_dictionary       = acfg.app_data_dictionary;
    c.transport_data_dictionary = acfg.transport_data_dictionary;
    c.store_path = "state/fix_init_" + tag + "_" + std::to_string(::getpid());
    c.log_path   = "log/fix_init_"   + tag + "_" + std::to_string(::getpid());
  };

  if (protocol == "fix_oe") {
    bist::fix::InitiatorConfig icfg;
    fill_init_cfg(icfg, acfg.oe_initiator_sender_comp_id,
                  acfg.oe_initiator_target_comp_id, "oe");
    bist::fix::OeClient::Callbacks cbs;
    cbs.on_session       = [&](const bist::fix::LogonResult& r)            { runner.on_oe_session(r); };
    cbs.on_execution     = [&](const bist::fix::ExecutionReportEvent& e)   { runner.on_oe_execution(e); };
    cbs.on_cancel_reject = [&](const bist::fix::CancelRejectEvent& e)      { runner.on_oe_cancel_reject(e); };
    cbs.on_trade_report  = [&](const bist::fix::TradeReportEvent& e)       { runner.on_oe_trade_report(e); };
    auto cli_r = bist::fix::OeClient::create(icfg, cbs);
    if (!cli_r) {
      std::fprintf(stderr, "OeClient::create: %s\n",
                   cli_r.error().detail.c_str());
      return 1;
    }
    oe = std::move(cli_r).value();
  } else if (protocol == "fix_rd") {
    bist::fix::InitiatorConfig icfg;
    fill_init_cfg(icfg, acfg.rd_initiator_sender_comp_id,
                  acfg.rd_initiator_target_comp_id, "rd");
    bist::fix::RdClient::Callbacks cbs;
    cbs.on_session = [&](const bist::fix::LogonResult& r) { runner.on_rd_session(r); };
    cbs.on_amr_ack = [&](bool ok, std::string d) { runner.on_rd_amr_ack(ok, std::move(d)); };
    auto cli_r = bist::fix::RdClient::create(icfg, cbs);
    if (!cli_r) {
      std::fprintf(stderr, "RdClient::create: %s\n",
                   cli_r.error().detail.c_str());
      return 1;
    }
    rd = std::move(cli_r).value();
  } else {
    std::fprintf(stderr, "unknown FIX protocol: %s\n", protocol.c_str());
    return 1;
  }

  // Bind the live clients into the runner so its dispatchers reach them.
  runner.set_clients(oe.get(), rd.get(), nullptr);

  std::printf("\n=== %s ===\n", sc.name.c_str());
  // Give the initiator's worker a moment to logon before the scenario
  // dispatches. The FIX runner expects fix_logon_response inside
  // default_timeout_ms anyway, so this is just a courtesy.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  auto results = runner.run(sc);
  int passed = 0, failed = 0;
  for (const auto& r : results) {
    std::printf("  %3d %s %-32s %s\n",
                r.id, r.passed ? "PASS" : "FAIL",
                r.action.c_str(), r.detail.c_str());
    if (r.passed) ++passed; else ++failed;
  }
  std::printf("--- %s: %d/%zu passed\n",
              sc.name.c_str(), passed, results.size());
  return failed == 0 ? 0 : 1;
#endif
}

#if defined(BIST_HAS_QUICKFIX)
// Run one FIX scenario against the live BIST FIX endpoint. The initiator
// config comes from `[fix.*]`; per-channel SenderCompID/TargetCompID fall
// back to the base values when the per-channel block is empty.
int run_one_live_fix_scenario(const std::filesystem::path& path,
                              const bist::runner::Scenario& sc,
                              const std::string& protocol,
                              const bist::config::Config& cfg) {
  (void)path;
  bist::runner::FixScenarioRunner runner(nullptr, nullptr, nullptr);
  std::unique_ptr<bist::fix::OeClient> oe;
  std::unique_ptr<bist::fix::RdClient> rd;

  auto pick = [&](const std::string& per_channel,
                  const std::string& base, const char* suffix) {
    if (!per_channel.empty()) return per_channel;
    if (base.empty()) return std::string{};
    return base + suffix;
  };

  bist::fix::InitiatorConfig icfg{};
  icfg.host = cfg.fix.host_primary;
  icfg.username = cfg.fix.username;
  icfg.password = cfg.fix.password.current;
  icfg.new_password = cfg.fix.password.next;
  icfg.heartbeat_secs = static_cast<int>(cfg.fix.heartbeat_secs);
  icfg.app_data_dictionary = !cfg.fix.data_dictionary.empty()
      ? cfg.fix.data_dictionary
      : std::string(BIST_FIX_DICT_DIR) + "/FIX50SP2.xml";
  icfg.transport_data_dictionary = !cfg.fix.transport_dictionary.empty()
      ? cfg.fix.transport_dictionary
      : std::string(BIST_FIX_DICT_DIR) + "/FIXT11.xml";
  icfg.store_path = !cfg.fix.session_storage.empty()
      ? cfg.fix.session_storage + "/" + protocol
      : "state/fix_live_" + protocol + "_" + std::to_string(::getpid());
  icfg.log_path = "log/fix_live_" + protocol + "_" + std::to_string(::getpid());

  if (protocol == "fix_oe") {
    icfg.port = cfg.fix.oe_port;
    icfg.sender_comp_id = pick(cfg.fix.oe.sender_comp_id, cfg.fix.sender_comp_id, "_OE");
    icfg.target_comp_id = pick(cfg.fix.oe.target_comp_id, cfg.fix.target_comp_id, "_OE");
    if (!cfg.fix.oe.username.empty()) icfg.username = cfg.fix.oe.username;
    bist::fix::OeClient::Callbacks cbs;
    cbs.on_session       = [&](const bist::fix::LogonResult& r)            { runner.on_oe_session(r); };
    cbs.on_execution     = [&](const bist::fix::ExecutionReportEvent& e)   { runner.on_oe_execution(e); };
    cbs.on_cancel_reject = [&](const bist::fix::CancelRejectEvent& e)      { runner.on_oe_cancel_reject(e); };
    cbs.on_trade_report  = [&](const bist::fix::TradeReportEvent& e)       { runner.on_oe_trade_report(e); };
    auto cli_r = bist::fix::OeClient::create(icfg, cbs);
    if (!cli_r) {
      std::fprintf(stderr, "live OeClient::create: %s\n", cli_r.error().detail.c_str());
      return 1;
    }
    oe = std::move(cli_r).value();
  } else if (protocol == "fix_rd") {
    icfg.port = cfg.fix.rd_port;
    icfg.sender_comp_id = pick(cfg.fix.rd.sender_comp_id, cfg.fix.sender_comp_id, "_RD");
    icfg.target_comp_id = pick(cfg.fix.rd.target_comp_id, cfg.fix.target_comp_id, "_RD");
    if (!cfg.fix.rd.username.empty()) icfg.username = cfg.fix.rd.username;
    bist::fix::RdClient::Callbacks cbs;
    cbs.on_session = [&](const bist::fix::LogonResult& r) { runner.on_rd_session(r); };
    cbs.on_amr_ack = [&](bool ok, std::string d) { runner.on_rd_amr_ack(ok, std::move(d)); };
    auto cli_r = bist::fix::RdClient::create(icfg, cbs);
    if (!cli_r) {
      std::fprintf(stderr, "live RdClient::create: %s\n", cli_r.error().detail.c_str());
      return 1;
    }
    rd = std::move(cli_r).value();
  } else {
    std::fprintf(stderr, "live FIX: unknown protocol %s\n", protocol.c_str());
    return 1;
  }

  runner.set_clients(oe.get(), rd.get(), nullptr);

  std::printf("\n=== %s (live) ===\n", sc.name.c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let logon settle

  auto results = runner.run(sc);
  int passed = 0, failed = 0;
  for (const auto& r : results) {
    std::printf("  %3d %s %-32s %s\n", r.id,
                r.passed ? "PASS" : "FAIL",
                r.action.c_str(), r.detail.c_str());
    if (r.passed) ++passed; else ++failed;
  }
  std::printf("--- %s (live): %d/%zu passed\n",
              sc.name.c_str(), passed, results.size());
  return failed == 0 ? 0 : 1;
}
#endif

// MockTransportControl — drives reconnect_and_login against an in-process
// mock gateway. Mirrors LiveTransportControl's storage discipline so the
// runner's references survive the rebuild. Used for Bölüm 1 multi-login
// scenarios under --mock.
class MockTransportControl : public bist::runner::ITransportControl {
 public:
  MockTransportControl(std::uint16_t port_primary,
                       std::uint16_t port_secondary,
                       bist::ouch::LoginParams base_lp,
                       const bist::ouch::Handlers& handlers,
                       std::optional<bist::net::TcpSocket>& sock_slot,
                       std::optional<bist::ouch::OuchSession>& session_slot,
                       std::optional<bist::ouch::OuchClient>& client_slot,
                       bist::ouch::OuchSession** session_ptr_for_handler,
                       bist::domain::TokenRegistry& tokens,
                       bist::domain::Throttler& throttler,
                       bist::domain::OrderBook& book)
      : port_primary_(port_primary), port_secondary_(port_secondary),
        base_lp_(std::move(base_lp)),
        handlers_(handlers), sock_slot_(sock_slot), session_slot_(session_slot),
        client_slot_(client_slot),
        session_ptr_for_handler_(session_ptr_for_handler),
        tokens_(tokens), throttler_(throttler), book_(book) {}

  bist::Result<void> reconnect_and_login(std::string_view override_password,
                                         std::string_view override_session,
                                         std::uint64_t    override_sequence) override {
    return rebuild(active_port(), override_password, override_session, override_sequence);
  }

  bist::Result<void> hard_disconnect() override {
    client_slot_.reset();
    session_slot_.reset();
    sock_slot_.reset();
    *session_ptr_for_handler_ = nullptr;
    return {};
  }

  bist::Result<void> switch_to_secondary(bool secondary,
                                         std::string_view override_session,
                                         std::uint64_t    override_sequence) override {
    on_secondary_ = secondary;
    if (active_port() == 0) {
      return bist::make_error(bist::ErrorCategory::Validation,
                              "mock transport: no secondary port configured");
    }
    return rebuild(active_port(), "", override_session, override_sequence);
  }

 private:
  std::uint16_t active_port() const noexcept {
    return on_secondary_ ? port_secondary_ : port_primary_;
  }

  bist::Result<void> rebuild(std::uint16_t port,
                             std::string_view override_password,
                             std::string_view override_session,
                             std::uint64_t    override_sequence) {
    client_slot_.reset();
    session_slot_.reset();
    sock_slot_.reset();
    *session_ptr_for_handler_ = nullptr;

    bist::net::TcpSocket sock;
    if (auto cr = sock.connect("127.0.0.1", port, /*tcp_nodelay=*/false); !cr) {
      return cr.error();
    }
    sock_slot_.emplace(std::move(sock));

    bist::ouch::LoginParams lp = base_lp_;
    if (!override_password.empty()) lp.password = std::string{override_password};
    if (!override_session.empty())  lp.requested_session = std::string{override_session};
    if (override_sequence != 0)     lp.requested_sequence = override_sequence;

    session_slot_.emplace(*sock_slot_, handlers_, lp);
    *session_ptr_for_handler_ = &*session_slot_;
    client_slot_.emplace(*session_slot_, tokens_, throttler_, book_);

    if (auto r = drive_login(*session_slot_); !r) return r.error();
    return {};
  }

  std::uint16_t                              port_primary_;
  std::uint16_t                              port_secondary_;
  bool                                       on_secondary_{false};
  bist::ouch::LoginParams                    base_lp_;
  bist::ouch::Handlers                       handlers_;
  std::optional<bist::net::TcpSocket>&       sock_slot_;
  std::optional<bist::ouch::OuchSession>&    session_slot_;
  std::optional<bist::ouch::OuchClient>&     client_slot_;
  bist::ouch::OuchSession**                  session_ptr_for_handler_;
  bist::domain::TokenRegistry&               tokens_;
  bist::domain::Throttler&                   throttler_;
  bist::domain::OrderBook&                   book_;
};

// Build a fresh mock gateway + OuchSession + OuchClient + ScenarioRunner
// for one scenario, run it, and tear everything down. Each scenario gets a
// clean session so that scripts that include logout (Bölüm 1) don't poison
// later scenarios.
int run_one_scenario(const std::filesystem::path& path) {
  // Peek the scenario protocol before standing up the OUCH stack.
  try {
    auto sc = bist::runner::load_scenario(path.string());
    const auto proto = scenario_protocol(sc);
    if (proto == "fix_oe" || proto == "fix_rd") {
      if (sc.mock_skip) {
        std::printf("\n=== %s ===\n", sc.name.c_str());
        std::printf("--- skipping (mock_skip=true; live-only)\n");
        return 0;
      }
      return run_one_fix_scenario(path, sc, proto);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "scenario %s: %s\n", path.c_str(), e.what());
    return 1;
  }

  bist::mock::OuchMockGateway gateway;
  bist::mock::OuchMockGateway gateway_secondary;
  // accept_one=false so the mock can re-accept after ouch_logout (Bölüm 1
  // multi-login flow). Single-session scenarios are unaffected because they
  // never trigger a second connect. A secondary mock instance backs the
  // gateway-failover scenario via MockTransportControl::switch_to_secondary.
  auto port_r = gateway.start(/*port=*/0, /*accept_one=*/false);
  if (!port_r) {
    std::fprintf(stderr, "mock start: %s\n", port_r.error().detail.c_str());
    return 1;
  }
  auto port2_r = gateway_secondary.start(/*port=*/0, /*accept_one=*/false);
  if (!port2_r) {
    std::fprintf(stderr, "mock start (secondary): %s\n",
                 port2_r.error().detail.c_str());
    return 1;
  }
  const std::uint16_t mock_port           = port_r.value();
  const std::uint16_t mock_port_secondary = port2_r.value();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  bist::domain::InstrumentCache instruments;
  seed_cert_instruments(instruments);
  bist::domain::TokenRegistry tokens;
  bist::domain::Throttler     throttler(100, 100);
  bist::domain::OrderBook     book;

  bist::runner::ScenarioRunner* runner_ptr = nullptr;

  std::optional<bist::net::TcpSocket>      sock_slot;
  std::optional<bist::ouch::OuchSession>   session_slot;
  std::optional<bist::ouch::OuchClient>    client_slot;
  bist::ouch::OuchSession*                 session_ptr = nullptr;

  bist::ouch::Handlers handlers{};
  handlers.on_state_change = [](bist::ouch::SessionState s, const std::string& d) {
    std::printf("session: state=%d detail=%s\n", static_cast<int>(s), d.c_str());
  };
  handlers.on_accepted = [&](const bist::ouch::OrderAccepted& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_rejected = [&](const bist::ouch::OrderRejected& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_replaced = [&](const bist::ouch::OrderReplaced& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_canceled = [&](const bist::ouch::OrderCanceled& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_executed = [&](const bist::ouch::OrderExecuted& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_mass_quote_ack = [&](const bist::ouch::MassQuoteAck& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_mass_quote_reject = [&](const bist::ouch::MassQuoteRejection& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };

  bist::ouch::LoginParams base_lp{};
  base_lp.username = "TEST01";
  base_lp.password = "123456";

  MockTransportControl transport(mock_port, mock_port_secondary,
                                 base_lp, handlers,
                                 sock_slot, session_slot, client_slot,
                                 &session_ptr, tokens, throttler, book);
  if (auto r = transport.reconnect_and_login("", "", 0); !r) {
    std::fprintf(stderr, "mock connect/login: %s\n", r.error().detail.c_str());
    return 1;
  }
  std::printf("login accepted, session=%s\n",
              std::string{session_slot->session_id()}.c_str());

  bist::runner::ScenarioRunner  runner(*session_slot, *client_slot,
                                        instruments, &transport);
  runner.set_eod_trigger([&] { (void)gateway.inactivate_all_resting(); });
  runner_ptr = &runner;

  try {
    auto sc = bist::runner::load_scenario(path.string());
    std::printf("\n=== %s ===\n", sc.name.c_str());
    if (sc.mock_skip) {
      std::printf("--- skipping (mock_skip=true; live-only)\n");
      return 0;
    }
    auto results = runner.run(sc);
    int passed = 0, failed = 0;
    for (const auto& r : results) {
      std::printf("  %3d %s %-32s %s\n",
                  r.id, r.passed ? "PASS" : "FAIL",
                  r.action.c_str(), r.detail.c_str());
      if (r.passed) ++passed; else ++failed;
    }
    std::printf("--- %s: %d/%zu passed\n",
                sc.name.c_str(), passed, results.size());
    return failed == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "scenario %s: %s\n", path.c_str(), e.what());
    return 1;
  }
}

int run_mock_replay(const CliArgs& args) {
  if (args.replay.empty()) {
    std::printf("no --replay given; nothing to do (use --interactive).\n");
    return EXIT_SUCCESS;
  }
  const auto files = collect_scenarios(args.replay);
  if (files.empty()) {
    std::fprintf(stderr, "no scenarios found at %s\n", args.replay.c_str());
    return EXIT_FAILURE;
  }
  int failures = 0;
  for (const auto& f : files) {
    if (run_one_scenario(f) != 0) ++failures;
  }
  std::printf("\n=== overall: %zu scenarios, %d failed ===\n",
              files.size(), failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Live transport controller — owns the socket+session pair and rebuilds it
// for scenarios that re-login (Bölüm 1: logout → wrong-pw → correct-pw).
//
// Stable storage (`std::optional`) keeps the OuchSession/OuchClient
// addresses fixed across rebuilds so the ScenarioRunner's references stay
// valid.
class LiveTransportControl : public bist::runner::ITransportControl {
 public:
  LiveTransportControl(std::string host_primary, std::string host_secondary,
                       std::uint16_t port, bool tcp_nodelay, bool start_secondary,
                       bist::ouch::LoginParams base_lp,
                       std::uint32_t hb_send_ns,
                       const bist::ouch::Handlers& handlers,
                       std::optional<bist::net::TcpSocket>& sock_slot,
                       std::optional<bist::ouch::OuchSession>& session_slot,
                       std::optional<bist::ouch::OuchClient>& client_slot,
                       bist::ouch::OuchSession** session_ptr_for_handler,
                       bist::observability::SequenceStore& seq_store,
                       bist::domain::TokenRegistry& tokens,
                       bist::domain::Throttler& throttler,
                       bist::domain::OrderBook& book,
                       bist::app::ReconnectPolicy reconnect_policy)
      : host_primary_(std::move(host_primary)),
        host_secondary_(std::move(host_secondary)),
        port_(port), tcp_nodelay_(tcp_nodelay),
        on_secondary_(start_secondary),
        base_lp_(std::move(base_lp)), hb_send_ns_(hb_send_ns),
        handlers_(handlers), sock_slot_(sock_slot), session_slot_(session_slot),
        client_slot_(client_slot),
        session_ptr_for_handler_(session_ptr_for_handler),
        seq_store_(seq_store), tokens_(tokens), throttler_(throttler),
        book_(book), reconnect_policy_(reconnect_policy) {}

  bool last_reconnect_was_cod_risk() const noexcept { return last_state_.cod_risk; }
  std::uint32_t last_reconnect_attempts() const noexcept { return last_state_.attempts; }

  bist::Result<void> reconnect_and_login(std::string_view override_password,
                                         std::string_view override_session,
                                         std::uint64_t    override_sequence) override {
    return rebuild(active_host(), override_password, override_session,
                   override_sequence);
  }

  bist::Result<void> hard_disconnect() override {
    client_slot_.reset();
    session_slot_.reset();
    sock_slot_.reset();
    *session_ptr_for_handler_ = nullptr;
    return {};
  }

  bist::Result<void> switch_to_secondary(bool secondary,
                                         std::string_view override_session,
                                         std::uint64_t    override_sequence) override {
    on_secondary_ = secondary;
    const auto& target = active_host();
    if (target.empty()) {
      return bist::make_error(bist::ErrorCategory::Validation,
                              std::string{"switch_to_secondary: "} +
                              (secondary ? "host_secondary" : "host_primary") +
                              " is empty in [ouch] config");
    }
    std::printf("switch_gateway: target=%s host=%s:%u\n",
                secondary ? "secondary" : "primary",
                target.c_str(), port_);
    return rebuild(target, "", override_session, override_sequence);
  }

 private:
  const std::string& active_host() const noexcept {
    return on_secondary_ ? host_secondary_ : host_primary_;
  }

  bist::Result<void> rebuild(const std::string& host,
                             std::string_view override_password,
                             std::string_view override_session,
                             std::uint64_t    override_sequence) {
    // Drop client → session → socket in dependency order, then dial a fresh
    // socket and rebuild the stack on the same storage addresses.
    client_slot_.reset();
    session_slot_.reset();
    sock_slot_.reset();
    *session_ptr_for_handler_ = nullptr;

    auto sr = connect_with_retry(host, port_, tcp_nodelay_, reconnect_policy_,
                                  &last_state_);
    if (!sr) return sr.error();
    sock_slot_.emplace(std::move(sr).value());

    bist::ouch::LoginParams lp = base_lp_;
    if (!override_password.empty()) lp.password = std::string{override_password};
    if (!override_session.empty())  lp.requested_session = std::string{override_session};
    if (override_sequence != 0)     lp.requested_sequence = override_sequence;

    session_slot_.emplace(*sock_slot_, handlers_, lp, hb_send_ns_);
    *session_ptr_for_handler_ = &*session_slot_;

    client_slot_.emplace(*session_slot_, tokens_, throttler_, book_);

    if (auto r = drive_login(*session_slot_); !r) return r.error();

    (void)seq_store_.save(session_slot_->session_id(),
                          session_slot_->next_inbound_seq());
    return {};
  }

  std::string                                host_primary_;
  std::string                                host_secondary_;
  std::uint16_t                              port_;
  bool                                       tcp_nodelay_;
  bool                                       on_secondary_;
  bist::ouch::LoginParams                    base_lp_;
  std::uint32_t                              hb_send_ns_;
  bist::ouch::Handlers                       handlers_;
  std::optional<bist::net::TcpSocket>&       sock_slot_;
  std::optional<bist::ouch::OuchSession>&    session_slot_;
  std::optional<bist::ouch::OuchClient>&     client_slot_;
  bist::ouch::OuchSession**                  session_ptr_for_handler_;
  bist::observability::SequenceStore&        seq_store_;
  bist::domain::TokenRegistry&               tokens_;
  bist::domain::Throttler&                   throttler_;
  bist::domain::OrderBook&                   book_;
  bist::app::ReconnectPolicy                 reconnect_policy_{};
  bist::app::ReconnectState                  last_state_{};
};

// Live mode: load TOML, resolve endpoint, connect with retry/backoff, login,
// run replay (or idle if no --replay).
int run_live(const CliArgs& args) {
  if (args.config.empty()) {
    std::fprintf(stderr, "live mode requires --config <path.toml>\n");
    return EXIT_FAILURE;
  }
  if (args.replay.empty() && args.interactive) {
    std::fprintf(stderr,
                 "interactive REPL is not implemented; start without --interactive "
                 "for resident service mode or provide --replay for certification.\n");
    return EXIT_FAILURE;
  }
  auto cfg_r = bist::config::load_config(args.config);
  if (!cfg_r) {
    std::fprintf(stderr, "config: %s\n", cfg_r.error().detail.c_str());
    return EXIT_FAILURE;
  }
  const auto& cfg = cfg_r.value();
  std::printf("environment=%s (%s) partitions=%zu\n",
              cfg.environment.name.c_str(),
              cfg.environment.description.c_str(),
              cfg.ouch.ports.size());

  auto ep_primary_r = bist::config::resolve_ouch_endpoint(cfg.ouch,
                                                          args.partition,
                                                          /*secondary=*/false);
  if (!ep_primary_r) {
    std::fprintf(stderr, "endpoint: %s\n", ep_primary_r.error().detail.c_str());
    return EXIT_FAILURE;
  }
  auto ep_secondary_r = bist::config::resolve_ouch_endpoint(cfg.ouch,
                                                            args.partition,
                                                            /*secondary=*/true);
  if (!ep_secondary_r) {
    std::fprintf(stderr, "endpoint: %s\n", ep_secondary_r.error().detail.c_str());
    return EXIT_FAILURE;
  }
  const auto ep_primary   = ep_primary_r.value();
  const auto ep_secondary = ep_secondary_r.value();
  const auto ep = args.secondary ? ep_secondary : ep_primary;

  const std::string audit_dir =
      cfg.logging.audit_dir.empty() ? std::string{"audit"} : cfg.logging.audit_dir;
  bist::observability::AuditLog audit(audit_dir);
  if (auto r = audit.start(); !r) {
    std::fprintf(stderr, "audit: %s\n", r.error().detail.c_str());
    return EXIT_FAILURE;
  }
  bool audit_drop_warned = false;

  bist::domain::InstrumentCache instruments;
  seed_cert_instruments(instruments);
  bist::domain::TokenRegistry tokens;
  bist::domain::Throttler     throttler(cfg.throttler.orders_per_sec,
                                        cfg.throttler.orders_per_sec);
  bist::domain::OrderBook     book;

  bist::runner::ScenarioRunner* runner_ptr = nullptr;

  // Sequence persistence — load any saved cursor so login can resume.
  std::filesystem::path state_path =
      std::filesystem::path("state") /
      (cfg.environment.name + "_oe_p" + std::to_string(args.partition) + ".seq");
  bist::observability::SequenceStore seq_store(state_path);
  std::string  saved_session;
  std::uint64_t saved_seq = 0;
  if (auto r = seq_store.load(saved_session, saved_seq); !r) {
    std::fprintf(stderr, "sequence_store load: %s — starting cold\n",
                 r.error().detail.c_str());
    saved_session.clear();
    saved_seq = 0;
  }

  // Stable storage for socket/session/client; LiveTransportControl rebuilds
  // them in place during reconnect_and_login() so refs into them stay valid.
  std::optional<bist::net::TcpSocket>      sock_slot;
  std::optional<bist::ouch::OuchSession>   session_slot;
  std::optional<bist::ouch::OuchClient>    client_slot;
  bist::ouch::OuchSession*                 session_ptr = nullptr;

  bist::ouch::Handlers handlers{};
  handlers.on_state_change = [](bist::ouch::SessionState s, const std::string& d) {
    std::printf("session: state=%d detail=%s\n", static_cast<int>(s), d.c_str());
  };
  handlers.on_accepted = [&](const bist::ouch::OrderAccepted& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_rejected = [&](const bist::ouch::OrderRejected& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_replaced = [&](const bist::ouch::OrderReplaced& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_canceled = [&](const bist::ouch::OrderCanceled& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_executed = [&](const bist::ouch::OrderExecuted& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_mass_quote_ack = [&](const bist::ouch::MassQuoteAck& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_mass_quote_reject = [&](const bist::ouch::MassQuoteRejection& m) {
    if (runner_ptr) runner_ptr->push_event(m);
  };
  handlers.on_audit_sent = [&audit, &session_ptr, &audit_drop_warned](
                               std::span<const std::uint8_t> bytes) {
    const std::uint64_t seq = session_ptr ? session_ptr->outbound_msgs_sent() + 1 : 0;
    if (!audit.record(bist::observability::AuditDirection::Sent,
                      "OUCH-OE", seq, bytes) && !audit_drop_warned) {
      audit_drop_warned = true;
      std::fprintf(stderr, "audit queue full; subsequent drops suppressed\n");
    }
  };
  // Throttled persistence + audit capture — fires on every inbound packet.
  handlers.on_audit_recv = [&audit, &seq_store, &session_ptr, &audit_drop_warned](
                               std::span<const std::uint8_t> bytes) {
    const std::uint64_t seq = session_ptr ? session_ptr->next_inbound_seq() : 0;
    if (!audit.record(bist::observability::AuditDirection::Recv,
                      "OUCH-OE", seq, bytes) && !audit_drop_warned) {
      audit_drop_warned = true;
      std::fprintf(stderr, "audit queue full; subsequent drops suppressed\n");
    }
    if (!session_ptr) return;
    (void)seq_store.save_if_due(session_ptr->session_id(),
                                session_ptr->next_inbound_seq());
  };

  bist::ouch::LoginParams base_lp{};
  base_lp.username           = cfg.ouch.username;
  base_lp.password           = cfg.ouch.password;
  base_lp.requested_session  = !cfg.ouch.session.empty() ? cfg.ouch.session
                                                         : saved_session;
  base_lp.requested_sequence = cfg.ouch.sequence != 0 ? cfg.ouch.sequence
                                                      : saved_seq;
  if (!saved_session.empty() || saved_seq != 0) {
    std::printf("resuming from %s session='%s' seq=%llu\n",
                state_path.c_str(),
                base_lp.requested_session.c_str(),
                static_cast<unsigned long long>(base_lp.requested_sequence));
  }

  const std::uint32_t hb_send_ns =
      static_cast<std::uint32_t>(cfg.ouch.heartbeat_ms) * 1'000'000U;

  bist::app::ReconnectPolicy reconnect_policy{cfg.ouch.cod_reconnect_deadline_ms};
  std::printf("CoD reconnect deadline: %u ms (BIST inactivation window starts ~55 s)\n",
              reconnect_policy.deadline_ms);
  LiveTransportControl transport(ep_primary.host, ep_secondary.host, ep.port,
                                 cfg.reactor.tcp_nodelay,
                                 /*start_secondary=*/args.secondary,
                                 base_lp, hb_send_ns, handlers,
                                 sock_slot, session_slot, client_slot,
                                 &session_ptr, seq_store, tokens, throttler,
                                 book, reconnect_policy);

  // Initial connect + login through the transport so the rebuild path is
  // exercised even on the first session.
  if (auto r = transport.reconnect_and_login("", "", 0); !r) {
    std::fprintf(stderr,
                 "could not establish OUCH gateway connection / login at %s:%u: %s\n",
                 ep.host.c_str(), ep.port, r.error().detail.c_str());
    return EXIT_FAILURE;
  }
  std::printf("login accepted, session=%s next_inbound_seq=%llu\n",
              std::string{session_slot->session_id()}.c_str(),
              static_cast<unsigned long long>(session_slot->next_inbound_seq()));

  if (args.replay.empty()) {
    std::printf("no --replay; resident service mode active. Send SIGINT/SIGTERM to stop.\n");
    int exit_code = EXIT_SUCCESS;
    while (!g_stop_requested.load(std::memory_order_acquire)) {
      if (session_slot) {
        if (auto r = session_slot->poll_io(); !r) {
          std::fprintf(stderr, "resident poll_io: %s\n", r.error().detail.c_str());
          exit_code = EXIT_FAILURE;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    (void)seq_store.flush(session_slot->session_id(),
                          session_slot->next_inbound_seq());
    (void)session_slot->request_logout();
    return exit_code;
  }

  const auto files = collect_scenarios(args.replay);
  if (files.empty()) {
    std::fprintf(stderr, "no scenarios found at %s\n", args.replay.c_str());
    (void)seq_store.flush(session_slot->session_id(),
                          session_slot->next_inbound_seq());
    (void)session_slot->request_logout();
    return EXIT_FAILURE;
  }

  bist::runner::ScenarioRunner runner(*session_slot, *client_slot, instruments,
                                      &transport);
  runner_ptr = &runner;

  int failures = 0;
  for (const auto& f : files) {
    try {
      auto sc = bist::runner::load_scenario(f.string());
      std::printf("\n=== %s ===\n", sc.name.c_str());
      // mock_skip means "mock can't simulate this"; live mode runs it.
      const auto proto = scenario_protocol(sc);
      if (proto == "fix_oe" || proto == "fix_rd") {
#if !defined(BIST_HAS_QUICKFIX)
        std::fprintf(stderr,
                     "  FAIL live FIX scenario requires BIST_BUILD_FIX=ON and "
                     "BIST_ENABLE_QUICKFIX=ON: %s\n", proto.c_str());
        ++failures;
        continue;
#else
        if (cfg.fix.host_primary.empty() ||
            (proto == "fix_oe" && cfg.fix.oe_port == 0) ||
            (proto == "fix_rd" && cfg.fix.rd_port == 0)) {
          std::fprintf(stderr,
                       "  FAIL %s requires [fix].host_primary + matching port; "
                       "populate %s.local.toml before live cert.\n",
                       proto.c_str(), args.config.c_str());
          ++failures;
          continue;
        }
        if (run_one_live_fix_scenario(f, sc, proto, cfg) != 0) ++failures;
        continue;
#endif
      }
      auto results = runner.run(sc);
      int passed = 0, failed = 0;
      for (const auto& r : results) {
        std::printf("  %3d %s %-32s %s\n",
                    r.id, r.passed ? "PASS" : "FAIL",
                    r.action.c_str(), r.detail.c_str());
        if (r.passed) ++passed; else ++failed;
      }
      std::printf("--- %s: %d/%zu passed\n",
                  sc.name.c_str(), passed, results.size());
      if (failed != 0) ++failures;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "scenario %s: %s\n", f.c_str(), e.what());
      ++failures;
    }
  }

  std::printf("\n=== overall: %zu scenarios, %d failed ===\n",
              files.size(), failures);
  if (session_slot) {
    (void)seq_store.flush(session_slot->session_id(),
                          session_slot->next_inbound_seq());
    (void)session_slot->request_logout();
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);

  if (argc == 1) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }
  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if (a == "--version") { print_version(); return EXIT_SUCCESS; }
    if (a == "-h" || a == "--help") { print_usage(argv[0]); return EXIT_SUCCESS; }
  }

  const auto args = parse_args(argc, argv);
  if (args.mock) return run_mock_replay(args);
  return run_live(args);
}
