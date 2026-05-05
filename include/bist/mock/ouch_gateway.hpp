#pragma once
//
// bist/mock/ouch_gateway.hpp — in-process mock OUCH gateway for tests and
// for `bist_colo --mock`.
//
// Behaviour:
//   - listens on a TCP port (or returns a connected socketpair fd)
//   - performs the SoupBinTCP login handshake, accepting Password "123456"
//     and rejecting everything else with reason 'A'
//   - replies to every Enter Order with an Order Accepted (Order State
//     OnBook) using a synthetic OrderID counter
//   - replies to Cancel with Order Canceled (reason "canceled by user")
//   - replies to Replace with Order Replaced (LeavesQty math)
//   - replies to Mass Quote with 2 K acks per entry (Accept), or R
//     rejections (e.g. when bid >= offer)
//   - sends a Server Heartbeat every 1s of inactivity
//
// The mock is intentionally permissive — it is meant to validate the
// client's encoding and decoding rather than to model BIST exchange logic
// faithfully. Cert-day testing happens against the real BIST gateway.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bist/core/result.hpp"
#include "bist/net/soupbintcp.hpp"
#include "bist/ouch/codec.hpp"
#include "bist/ouch/messages.hpp"

namespace bist::mock {

class OuchMockGateway {
 public:
  OuchMockGateway() = default;
  ~OuchMockGateway() { stop(); }

  // Start a TCP listener on `port` (0 → ephemeral) and accept clients
  // sequentially. Returns the bound port. Spawns the worker thread.
  // `accept_one`=true (default) preserves legacy behaviour where the mock
  // exits after the first client logs out; set to false for Bölüm 1
  // multi-login flows.
  Result<std::uint16_t> start(std::uint16_t port = 0, bool accept_one = true);

  // Convenience for tests: drive the mock against an already-connected fd
  // (e.g. one half of a socketpair).
  void start_on_fd(int fd);

  void stop();

  // Called from the runner to drive end-of-session order inactivation
  // (cert Bölüm 2 EOD batch). Emits OrderCanceled reason=4 for every
  // resting order tracked since login. Returns the number of orders
  // inactivated.
  std::size_t inactivate_all_resting();

  // Multi-session credential override: every login after the next reset
  // accepts only this password. Default is "123456" (cert default).
  void set_accepted_password(std::string pw);

  // Bölüm 1 cert: the second login on the same SoupBinTCP session id
  // must be rejected with reason 'S' (SessionAlreadyExists). The test
  // harness arms this for the scope of the next login attempt.
  void arm_duplicate_login_reject() noexcept { arm_duplicate_reject_.store(true); }

 private:
  // Per-active-session order tracker: token → (order_book_id, side, order_id).
  struct RestingOrder {
    std::uint32_t order_book_id{0};
    char          side{0};
    std::uint64_t order_id{0};
  };

  void run_loop(int client_fd);
  void accept_loop();
  void handle_login(int fd, const std::uint8_t* payload, std::size_t len);
  void handle_unsequenced_payload(int fd, const std::uint8_t* payload,
                                  std::size_t len);
  void send_heartbeat(int fd);
  void send_login_accepted(int fd);
  void send_login_rejected(int fd, char reason);
  void send_sequenced(int fd, const void* msg, std::size_t len);
  void emit_canceled(int fd, const std::string& token,
                     const RestingOrder& order, std::uint8_t reason);

  std::atomic<bool>           running_{false};
  std::atomic<bool>           accept_one_{true};
  std::thread                 worker_;
  int                         listen_fd_{-1};
  std::atomic<int>            client_fd_{-1};       // current accepted fd
  std::uint64_t               order_id_seq_{1};
  std::string                 accepted_password_{"123456"};
  std::atomic<bool>           arm_duplicate_reject_{false};
  bool                        session_used_{false};

  // Tracks tokens we have already accepted so that duplicates trigger the
  // canonical -800002 reject like the real gateway.
  std::mutex                  state_mu_;
  std::vector<std::string>    seen_tokens_;
  std::unordered_map<std::string, RestingOrder> resting_;
  // Tracks per-(OrderBookID,Side) quote presence so MassQuoteAck status can
  // distinguish initial accept, update, and cancel.
  std::unordered_map<std::string, bool> quote_active_;
};

}  // namespace bist::mock
