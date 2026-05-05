// apps/tools/latency_bench.cpp — place→ack latency on the in-process mock.
//
// Cert-day target: p99 < 50µs on a quiet co-location host. Loopback
// numbers are inherently faster than the production NIC path; this tool
// is a regression guard for the encode/decode/socket pipeline.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "bist/core/result.hpp"
#include "bist/core/time.hpp"
#include "bist/domain/instrument.hpp"
#include "bist/domain/order_book.hpp"
#include "bist/domain/throttler.hpp"
#include "bist/domain/token_registry.hpp"
#include "bist/mock/ouch_gateway.hpp"
#include "bist/net/tcp_socket.hpp"
#include "bist/ouch/client.hpp"
#include "bist/ouch/messages.hpp"
#include "bist/ouch/session.hpp"

namespace {

constexpr std::size_t kDefaultSamples = 5000;
constexpr std::size_t kWarmup        = 200;

double percentile(std::vector<std::uint64_t>& v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t idx =
      static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
  return static_cast<double>(v[idx]);
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t samples = kDefaultSamples;
  for (int i = 1; i < argc; ++i) {
    if (std::string{argv[i]} == "--samples" && i + 1 < argc) {
      samples = static_cast<std::size_t>(std::atoll(argv[++i]));
    }
  }
  if (samples < 100) samples = 100;

  bist::mock::OuchMockGateway gateway;
  auto port_r = gateway.start(0, /*accept_one=*/true);
  if (!port_r) {
    std::fprintf(stderr, "mock start: %s\n", port_r.error().detail.c_str());
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  bist::net::TcpSocket sock;
  if (auto cr = sock.connect("127.0.0.1", port_r.value(), false); !cr) {
    std::fprintf(stderr, "connect: %s\n", cr.error().detail.c_str());
    return 1;
  }

  bist::domain::InstrumentCache instruments;
  instruments.put({"ACSEL.E", 70616, 3, 1, 6000});
  bist::domain::TokenRegistry tokens;
  bist::domain::Throttler     throttler(100'000, 100'000);    // bench: no throttle
  bist::domain::OrderBook     book;

  std::vector<std::uint64_t> rtt_ns;
  rtt_ns.reserve(samples);

  bool got_ack = false;
  std::uint64_t last_send_ns = 0;

  bist::ouch::Handlers h{};
  h.on_accepted = [&](const bist::ouch::OrderAccepted&) {
    rtt_ns.push_back(bist::monotonic_ns() - last_send_ns);
    got_ack = true;
  };

  bist::ouch::LoginParams lp{};
  lp.username = "TEST01";
  lp.password = "123456";
  bist::ouch::OuchSession session(sock, h, lp);
  if (auto r = session.begin_login(); !r) {
    std::fprintf(stderr, "login begin: %s\n", r.error().detail.c_str());
    return 1;
  }
  while (session.state() == bist::ouch::SessionState::LoggingIn) {
    if (auto r = session.poll_io(); !r) {
      std::fprintf(stderr, "login poll: %s\n", r.error().detail.c_str());
      return 1;
    }
  }
  if (session.state() != bist::ouch::SessionState::Active) {
    std::fprintf(stderr, "login not Active\n");
    return 1;
  }

  bist::ouch::OuchClient client(session, tokens, throttler, book);

  for (std::size_t i = 0; i < samples + kWarmup; ++i) {
    bist::ouch::PlaceArgs pa{};
    pa.symbol        = "ACSEL.E";
    pa.order_book_id = 70616;
    pa.side          = bist::Side::Buy;
    pa.quantity      = 1;
    pa.price         = 6000;
    pa.tif           = bist::TimeInForce::Day;
    pa.category      = bist::ClientCategory::Client;
    pa.explicit_token = "T" + std::to_string(i);

    got_ack       = false;
    last_send_ns  = bist::monotonic_ns();
    auto pr = client.place(pa);
    if (!pr) {
      std::fprintf(stderr, "place #%zu failed: %s\n", i, pr.error().detail.c_str());
      return 1;
    }
    while (!got_ack) {
      if (auto rr = session.poll_io(); !rr) {
        std::fprintf(stderr, "poll: %s\n", rr.error().detail.c_str());
        return 1;
      }
    }
    if (i < kWarmup) rtt_ns.pop_back();    // discard warm-up samples
  }

  const auto p50 = percentile(rtt_ns, 0.50) / 1000.0;
  const auto p90 = percentile(rtt_ns, 0.90) / 1000.0;
  const auto p99 = percentile(rtt_ns, 0.99) / 1000.0;
  const auto p999 = percentile(rtt_ns, 0.999) / 1000.0;
  const auto pmax = percentile(rtt_ns, 1.0)  / 1000.0;

  std::printf("=== bist_colo place→ack latency (in-process mock, %zu samples) ===\n",
              rtt_ns.size());
  std::printf("  p50    %8.2f µs\n", p50);
  std::printf("  p90    %8.2f µs\n", p90);
  std::printf("  p99    %8.2f µs\n", p99);
  std::printf("  p99.9  %8.2f µs\n", p999);
  std::printf("  pmax   %8.2f µs\n", pmax);
  std::printf("Cert target on co-located NIC: p99 < 50 µs.\n");
  return 0;
}
