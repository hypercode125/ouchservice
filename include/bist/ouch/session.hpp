#pragma once
//
// bist/ouch/session.hpp — OUCH/SoupBinTCP session state machine.
//
// Responsibilities:
//   - drive the SoupBinTCP login handshake
//   - frame and send outbound OUCH messages (Enter/Replace/Cancel/MassQuote)
//   - parse inbound SoupBinTCP packets and dispatch decoded OUCH messages to
//     user-supplied callbacks
//   - run client-heartbeat timer (>= 800 ms quiet → 'R')
//   - watchdog server-heartbeat (>= 2.5 s silence → declare dead)
//
// The session is intentionally callback-driven so the same code path can
// run against a real TCP socket on production and against an in-memory
// pipe in tests. The callbacks are invoked from whatever thread calls
// poll_io(): typically the hot reactor thread.

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bist/core/result.hpp"
#include "bist/core/time.hpp"
#include "bist/net/soupbintcp.hpp"
#include "bist/net/tcp_socket.hpp"
#include "bist/ouch/codec.hpp"
#include "bist/ouch/messages.hpp"

namespace bist::ouch {

enum class SessionState : std::uint8_t {
  Disconnected,
  Connecting,
  LoggingIn,
  Active,
  Disconnecting,
  Failed,
};

struct LoginParams {
  std::string username;            // 6 chars; padded
  std::string password;            // up to 10 chars; padded
  std::string requested_session;   // 10 chars; "" for "any"
  std::uint64_t requested_sequence{0};   // 0 means "from current"
};

// Callbacks: each fires after a successful decode. The session owns the
// memory backing each argument; consumers must copy if they need to keep
// the data after returning.

struct Handlers {
  std::function<void(const OrderAccepted&)>      on_accepted;
  std::function<void(const OrderRejected&)>      on_rejected;
  std::function<void(const OrderReplaced&)>      on_replaced;
  std::function<void(const OrderCanceled&)>      on_canceled;
  std::function<void(const OrderExecuted&)>      on_executed;
  std::function<void(const MassQuoteAck&)>       on_mass_quote_ack;
  std::function<void(const MassQuoteRejection&)> on_mass_quote_reject;
  std::function<void(SessionState, const std::string&)> on_state_change;
  std::function<void(std::span<const std::uint8_t>)>    on_audit_sent;
  std::function<void(std::span<const std::uint8_t>)>    on_audit_recv;
};

class OuchSession {
 public:
  OuchSession(net::TcpSocket& socket, Handlers handlers,
              LoginParams login,
              std::uint32_t heartbeat_send_ns    = soup::kHeartbeatSendNs,
              std::uint32_t heartbeat_timeout_ns = soup::kHeartbeatTimeoutNs)
      : socket_(socket),
        handlers_(std::move(handlers)),
        login_(std::move(login)),
        heartbeat_send_ns_(heartbeat_send_ns),
        heartbeat_timeout_ns_(heartbeat_timeout_ns) {}

  // Initiate the SoupBinTCP login handshake. Caller must have already
  // connected the underlying TcpSocket.
  Result<void> begin_login() {
    if (!socket_.is_open()) {
      return make_error(ErrorCategory::Io, "socket not connected");
    }
    set_state(SessionState::LoggingIn, "sending LoginRequest");

    soup::LoginRequest req{};
    soup::make_header(req.header, soup::pkt::kLoginRequest,
                      sizeof(soup::LoginRequest) - sizeof(soup::Header));
    pad(req.username,                  login_.username);
    pad(req.password,                  login_.password);
    pad(req.requested_session,         login_.requested_session);
    char seq[soup::kSequenceLen];
    std::snprintf(seq, sizeof(seq), "%020llu",
                  static_cast<unsigned long long>(login_.requested_sequence));
    std::memcpy(req.requested_sequence_number, seq, soup::kSequenceLen);

    return write_bytes(reinterpret_cast<const std::uint8_t*>(&req), sizeof(req));
  }

  // Send an Enter Order. The caller has already filled every field (we
  // don't help with field defaults here so that the path stays explicit
  // and audit-friendly).
  Result<void> send_enter_order(const EnterOrder& msg) {
    return send_unsequenced(reinterpret_cast<const std::uint8_t*>(&msg),
                            sizeof(msg));
  }

  Result<void> send_replace_order(const ReplaceOrder& msg) {
    return send_unsequenced(reinterpret_cast<const std::uint8_t*>(&msg),
                            sizeof(msg));
  }

  Result<void> send_cancel(const CancelOrder& msg) {
    return send_unsequenced(reinterpret_cast<const std::uint8_t*>(&msg),
                            sizeof(msg));
  }

  Result<void> send_cancel_by_id(const CancelByOrderId& msg) {
    return send_unsequenced(reinterpret_cast<const std::uint8_t*>(&msg),
                            sizeof(msg));
  }

  // Mass Quote is dynamically sized; caller assembles header + entries
  // contiguously and passes the whole blob.
  Result<void> send_mass_quote(std::span<const std::uint8_t> blob) {
    return send_unsequenced(blob.data(), blob.size());
  }

  // Drive the read side: drains as much as is available without blocking,
  // dispatches every fully-framed packet, and triggers heartbeats / watchdog.
  Result<void> poll_io() {
    if (auto r = read_some(); !r) return r.error();
    if (auto r = parse_pending(); !r) return r.error();
    return tick_timers();
  }

  Result<void> request_logout() {
    set_state(SessionState::Disconnecting, "sending LogoutRequest");
    soup::LogoutRequest req{};
    soup::make_header(req.header, soup::pkt::kLogoutRequest, 0);
    return write_bytes(reinterpret_cast<const std::uint8_t*>(&req), sizeof(req));
  }

  [[nodiscard]] SessionState state()                const noexcept { return state_; }
  [[nodiscard]] std::uint64_t outbound_msgs_sent()   const noexcept { return msgs_sent_; }
  [[nodiscard]] std::uint64_t inbound_msgs_received() const noexcept { return msgs_recv_; }
  [[nodiscard]] const std::string& session_id()      const noexcept { return assigned_session_; }
  [[nodiscard]] std::uint64_t next_inbound_seq()     const noexcept { return next_inbound_seq_; }
  [[nodiscard]] char last_login_reject_reason()      const noexcept { return last_login_reject_reason_; }

 private:
  // -- helpers ----------------------------------------------------------------

  void set_state(SessionState s, std::string detail) {
    state_ = s;
    if (handlers_.on_state_change) handlers_.on_state_change(s, detail);
  }

  template <std::size_t N>
  static void pad(char (&dst)[N], std::string_view src) noexcept {
    const std::size_t n = std::min(src.size(), N);
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
    for (std::size_t i = n; i < N; ++i) dst[i] = ' ';
  }

  // Wrap an OUCH message in a SoupBinTCP UnsequencedDataPacket and write
  // it through the socket.
  Result<void> send_unsequenced(const std::uint8_t* payload, std::size_t len) {
    if (state_ != SessionState::Active) {
      return make_error(ErrorCategory::StateMismatch,
                        "session not Active");
    }
    soup::Header h{};
    soup::make_header(h, soup::pkt::kUnsequencedData,
                      static_cast<std::uint16_t>(len));
    if (auto r = write_bytes(reinterpret_cast<const std::uint8_t*>(&h), sizeof(h));
        !r) return r;
    return write_bytes(payload, len);
  }

  Result<void> write_bytes(const std::uint8_t* buf, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
      const auto r = socket_.send(buf + off, len - off);
      if (!r) return r.error();
      const auto wrote = r.value();
      if (wrote == 0) {
        // Socket signalled EAGAIN: stash the remainder. Caller is expected
        // to flush via poll_io which we drive every reactor tick.
        out_pending_.insert(out_pending_.end(), buf + off, buf + len);
        return {};
      }
      off += wrote;
    }
    last_send_ns_ = monotonic_ns();
    if (handlers_.on_audit_sent) {
      handlers_.on_audit_sent(std::span<const std::uint8_t>{buf, len});
    }
    ++msgs_sent_;
    return {};
  }

  Result<void> read_some() {
    if (!socket_.is_open()) {
      return make_error(ErrorCategory::Io, "read on closed socket");
    }
    std::uint8_t scratch[8192];
    while (true) {
      const auto r = socket_.recv(scratch, sizeof(scratch));
      if (!r) {
        if (r.error().category == ErrorCategory::Io &&
            r.error().detail == "eof") {
          set_state(SessionState::Disconnected, "peer closed");
          return r.error();
        }
        return r.error();
      }
      const auto n = r.value();
      if (n == 0) return {};
      in_buffer_.insert(in_buffer_.end(), scratch, scratch + n);
      last_recv_ns_ = monotonic_ns();
    }
  }

  Result<void> parse_pending() {
    while (in_buffer_.size() >= sizeof(soup::Header)) {
      soup::Header h{};
      std::memcpy(&h, in_buffer_.data(), sizeof(h));
      const std::size_t pkt_size =
          static_cast<std::size_t>(h.length.get()) + 2;  // +2 for length itself
      if (in_buffer_.size() < pkt_size) return {};       // wait for more

      const std::uint8_t* payload =
          in_buffer_.data() + sizeof(soup::Header);
      const std::size_t payload_len = pkt_size - sizeof(soup::Header);

      if (handlers_.on_audit_recv) {
        handlers_.on_audit_recv(
            std::span<const std::uint8_t>{in_buffer_.data(), pkt_size});
      }

      if (auto r = dispatch_packet(h.packet_type, payload, payload_len); !r) {
        in_buffer_.erase(in_buffer_.begin(),
                         in_buffer_.begin() + static_cast<long>(pkt_size));
        return r.error();
      }
      in_buffer_.erase(in_buffer_.begin(),
                       in_buffer_.begin() + static_cast<long>(pkt_size));
    }
    return {};
  }

  Result<void> dispatch_packet(char type, const std::uint8_t* payload,
                               std::size_t payload_len) {
    using namespace bist::ouch;
    ++msgs_recv_;
    switch (type) {
      case soup::pkt::kLoginAccepted: {
        if (payload_len < soup::kSessionLen + soup::kSequenceLen) {
          return make_error(ErrorCategory::Protocol, "short LoginAccepted");
        }
        assigned_session_.assign(reinterpret_cast<const char*>(payload),
                                 soup::kSessionLen);
        // sequence number digits follow.
        char seq[soup::kSequenceLen + 1] = {0};
        std::memcpy(seq, payload + soup::kSessionLen, soup::kSequenceLen);
        next_inbound_seq_ = std::strtoull(seq, nullptr, 10);
        last_login_reject_reason_ = '\0';
        set_state(SessionState::Active, "LoginAccepted");
        return {};
      }
      case soup::pkt::kLoginRejected: {
        std::string detail = "LoginRejected";
        if (payload_len >= 1) {
          last_login_reject_reason_ = static_cast<char>(payload[0]);
          detail += "(reason='";
          detail += static_cast<char>(payload[0]);
          detail += "')";
        } else {
          last_login_reject_reason_ = '\0';
        }
        set_state(SessionState::Failed, std::move(detail));
        return {};
      }
      case soup::pkt::kServerHeartbeat:
        return {};
      case soup::pkt::kEndOfSession:
        set_state(SessionState::Disconnected, "EndOfSession");
        return {};
      case soup::pkt::kSequencedData:
        ++next_inbound_seq_;
        return dispatch_ouch(payload, payload_len);
      case soup::pkt::kDebug:
        return {};
      default:
        return make_error(ErrorCategory::Protocol,
                          std::string{"unknown SoupBinTCP type: "} + type);
    }
  }

  Result<void> dispatch_ouch(const std::uint8_t* payload, std::size_t payload_len) {
    if (payload_len == 0) {
      return make_error(ErrorCategory::Protocol, "empty OUCH payload");
    }
    const char type = static_cast<char>(payload[0]);
    switch (type) {
      case msg_type::kOrderAccepted:
        if (payload_len != sizeof(OrderAccepted)) break;
        if (handlers_.on_accepted) {
          OrderAccepted m{};
          std::memcpy(&m, payload, sizeof(m));
          handlers_.on_accepted(m);
        }
        return {};
      case msg_type::kOrderRejected:
        if (payload_len != sizeof(OrderRejected)) break;
        if (handlers_.on_rejected) {
          OrderRejected m{};
          std::memcpy(&m, payload, sizeof(m));
          handlers_.on_rejected(m);
        }
        return {};
      case msg_type::kOrderReplaced:
        if (payload_len != sizeof(OrderReplaced)) break;
        if (handlers_.on_replaced) {
          OrderReplaced m{};
          std::memcpy(&m, payload, sizeof(m));
          handlers_.on_replaced(m);
        }
        return {};
      case msg_type::kOrderCanceled:
        if (payload_len != sizeof(OrderCanceled)) break;
        if (handlers_.on_canceled) {
          OrderCanceled m{};
          std::memcpy(&m, payload, sizeof(m));
          handlers_.on_canceled(m);
        }
        return {};
      case msg_type::kOrderExecuted:
        if (payload_len != sizeof(OrderExecuted)) break;
        if (handlers_.on_executed) {
          OrderExecuted m{};
          std::memcpy(&m, payload, sizeof(m));
          handlers_.on_executed(m);
        }
        return {};
      case msg_type::kMassQuoteAck:
        if (payload_len != sizeof(MassQuoteAck)) break;
        if (handlers_.on_mass_quote_ack) {
          MassQuoteAck m{};
          std::memcpy(&m, payload, sizeof(m));
          handlers_.on_mass_quote_ack(m);
        }
        return {};
      case msg_type::kMassQuoteRejection:
        if (payload_len != sizeof(MassQuoteRejection)) break;
        if (handlers_.on_mass_quote_reject) {
          MassQuoteRejection m{};
          std::memcpy(&m, payload, sizeof(m));
          handlers_.on_mass_quote_reject(m);
        }
        return {};
      default:
        return make_error(ErrorCategory::Protocol,
                          std::string{"unknown OUCH type: "} + type);
    }
    return make_error(ErrorCategory::Protocol,
                      std::string{"OUCH "} + type + " size mismatch");
  }

  // Drain anything stashed during EAGAIN, fire heartbeat if quiet, watchdog
  // if peer has gone silent.
  Result<void> tick_timers() {
    flush_pending();

    if (state_ == SessionState::Active) {
      const auto now = monotonic_ns();
      if (now - last_send_ns_ >= heartbeat_send_ns_) {
        soup::Heartbeat hb{};
        soup::make_header(hb.header, soup::pkt::kClientHeartbeat, 0);
        if (auto r = write_bytes(reinterpret_cast<const std::uint8_t*>(&hb),
                                  sizeof(hb));
            !r)
          return r;
      }
      if (last_recv_ns_ != 0 && now - last_recv_ns_ >= heartbeat_timeout_ns_) {
        set_state(SessionState::Failed, "peer heartbeat timeout");
        return make_error(ErrorCategory::Timeout, "heartbeat watchdog tripped");
      }
    }
    return {};
  }

  void flush_pending() {
    if (out_pending_.empty()) return;
    while (!out_pending_.empty()) {
      const auto r = socket_.send(out_pending_.data(), out_pending_.size());
      if (!r) return;
      const auto n = r.value();
      if (n == 0) return;
      out_pending_.erase(out_pending_.begin(),
                         out_pending_.begin() + static_cast<long>(n));
    }
  }

  // --------------------------------------------------------------------------
  net::TcpSocket&       socket_;
  Handlers              handlers_;
  LoginParams           login_;
  std::uint32_t         heartbeat_send_ns_;
  std::uint32_t         heartbeat_timeout_ns_;
  SessionState          state_{SessionState::Disconnected};
  std::vector<std::uint8_t> in_buffer_;
  std::vector<std::uint8_t> out_pending_;
  TimestampNs           last_send_ns_{0};
  TimestampNs           last_recv_ns_{0};
  std::string           assigned_session_;
  std::uint64_t         next_inbound_seq_{0};
  char                  last_login_reject_reason_{'\0'};
  std::uint64_t         msgs_sent_{0};
  std::uint64_t         msgs_recv_{0};
};

}  // namespace bist::ouch
