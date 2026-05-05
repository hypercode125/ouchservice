// src/mock/ouch_gateway.cpp — implementation of the mock OUCH gateway.

#include "bist/mock/ouch_gateway.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "bist/core/time.hpp"

namespace bist::mock {

namespace {

constexpr std::uint64_t kHeartbeatNs = 1'000'000'000ULL;

bool read_n(int fd, void* buf, std::size_t n) {
  auto* p = static_cast<std::uint8_t*>(buf);
  std::size_t off = 0;
  while (off < n) {
    const ssize_t r = ::recv(fd, p + off, n - off, 0);
    if (r > 0) { off += static_cast<std::size_t>(r); continue; }
    if (r == 0) return false;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      pollfd pfd{fd, POLLIN, 0};
      ::poll(&pfd, 1, 200);
      continue;
    }
    return false;
  }
  return true;
}

void write_all(int fd, const void* buf, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(buf);
  std::size_t off = 0;
  while (off < n) {
    const ssize_t r = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
    if (r > 0) { off += static_cast<std::size_t>(r); continue; }
    if (r < 0 && (errno == EINTR ||
                  errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    return;
  }
}

}  // namespace

Result<std::uint16_t> OuchMockGateway::start(std::uint16_t port, bool accept_one) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return make_error(ErrorCategory::Io, "socket");
  }
  const int on = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return make_error(ErrorCategory::Io, "bind");
  }
  if (::listen(listen_fd_, 4) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return make_error(ErrorCategory::Io, "listen");
  }
  socklen_t alen = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
  const std::uint16_t bound = ntohs(addr.sin_port);

  accept_one_.store(accept_one);
  running_.store(true);
  worker_ = std::thread([this] { accept_loop(); });
  return bound;
}

void OuchMockGateway::accept_loop() {
  do {
    sockaddr_in cli{};
    socklen_t   clen = sizeof(cli);
    int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &clen);
    if (client < 0) return;
    client_fd_.store(client);
    run_loop(client);
    client_fd_.store(-1);
  } while (running_.load() && !accept_one_.load());
}

void OuchMockGateway::start_on_fd(int fd) {
  running_.store(true);
  worker_ = std::thread([this, fd] { run_loop(fd); });
}

void OuchMockGateway::stop() {
  running_.store(false);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (worker_.joinable()) worker_.join();
}

void OuchMockGateway::run_loop(int client_fd) {
  // Read the SoupBinTCP login handshake.
  soup::Header h{};
  if (!read_n(client_fd, &h, sizeof(h))) {
    ::close(client_fd);
    return;
  }
  const std::size_t inner = static_cast<std::size_t>(h.length.get()) - 1;
  std::vector<std::uint8_t> payload(inner);
  if (inner && !read_n(client_fd, payload.data(), inner)) {
    ::close(client_fd);
    return;
  }
  if (h.packet_type == soup::pkt::kLoginRequest) {
    handle_login(client_fd, payload.data(), payload.size());
  }

  // Main read loop.
  TimestampNs last_send = monotonic_ns();
  while (running_.load()) {
    pollfd pfd{client_fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 200);
    if (pr < 0) break;

    if (pr > 0 && (pfd.revents & POLLIN)) {
      if (!read_n(client_fd, &h, sizeof(h))) break;
      const std::size_t pl = static_cast<std::size_t>(h.length.get()) - 1;
      payload.assign(pl, 0);
      if (pl && !read_n(client_fd, payload.data(), pl)) break;

      if (h.packet_type == soup::pkt::kClientHeartbeat) {
        // nothing to do; we'll heartbeat back when our own timer fires
      } else if (h.packet_type == soup::pkt::kLogoutRequest) {
        soup::EndOfSession eos{};
        soup::make_header(eos.header, soup::pkt::kEndOfSession, 0);
        write_all(client_fd, &eos, sizeof(eos));
        last_send = monotonic_ns();
        break;
      } else if (h.packet_type == soup::pkt::kUnsequencedData) {
        handle_unsequenced_payload(client_fd, payload.data(), payload.size());
        last_send = monotonic_ns();
      }
    }

    const auto now = monotonic_ns();
    if (now - last_send >= kHeartbeatNs) {
      send_heartbeat(client_fd);
      last_send = now;
    }
  }
  ::close(client_fd);
}

void OuchMockGateway::handle_login(int fd, const std::uint8_t* payload,
                                   std::size_t len) {
  if (len < soup::kUsernameLen + soup::kPasswordLen) {
    send_login_rejected(fd, 'A');
    return;
  }
  if (arm_duplicate_reject_.exchange(false)) {
    // Bölüm 1: explicitly fail the next login as if the same SoupBinTCP
    // session is already bound elsewhere. Reason 'S' = SessionUnavailable.
    send_login_rejected(fd, 'S');
    return;
  }
  std::string accepted;
  {
    std::lock_guard<std::mutex> g(state_mu_);
    accepted = accepted_password_;
  }
  std::string_view pwd{
      reinterpret_cast<const char*>(payload + soup::kUsernameLen),
      soup::kPasswordLen};
  while (!pwd.empty() && pwd.back() == ' ') pwd.remove_suffix(1);
  if (std::string(pwd) != accepted) {
    send_login_rejected(fd, 'A');
    return;
  }
  send_login_accepted(fd);
}

void OuchMockGateway::set_accepted_password(std::string pw) {
  std::lock_guard<std::mutex> g(state_mu_);
  accepted_password_ = std::move(pw);
}

void OuchMockGateway::emit_canceled(int fd, const std::string& token,
                                    const RestingOrder& order,
                                    std::uint8_t reason) {
  using namespace bist::ouch;
  OrderCanceled oc{};
  std::memset(&oc, 0, sizeof(oc));
  oc.message_type = msg_type::kOrderCanceled;
  oc.timestamp_ns.set(wall_ns());
  std::memcpy(oc.order_token, token.data(),
              std::min(token.size(), kTokenLen));
  oc.order_book_id.set(order.order_book_id);
  oc.side    = order.side;
  oc.order_id.set(order.order_id);
  oc.reason  = reason;
  send_sequenced(fd, &oc, sizeof(oc));
}

std::size_t OuchMockGateway::inactivate_all_resting() {
  const int fd = client_fd_.load();
  if (fd < 0) return 0;
  std::vector<std::pair<std::string, RestingOrder>> snapshot;
  {
    std::lock_guard<std::mutex> g(state_mu_);
    snapshot.assign(resting_.begin(), resting_.end());
    resting_.clear();
  }
  for (const auto& [tok, ord] : snapshot) {
    emit_canceled(fd, tok, ord, /*reason=*/4);   // 4 = Inactivate
  }
  return snapshot.size();
}

void OuchMockGateway::send_login_accepted(int fd) {
  soup::LoginAccepted msg{};
  soup::make_header(msg.header, soup::pkt::kLoginAccepted,
                    soup::kSessionLen + soup::kSequenceLen);
  std::memset(msg.session, ' ', soup::kSessionLen);
  std::memcpy(msg.session, "MOCKSESS01", 10);
  std::memset(msg.sequence_number, '0', soup::kSequenceLen);
  msg.sequence_number[soup::kSequenceLen - 1] = '1';
  write_all(fd, &msg, sizeof(msg));
}

void OuchMockGateway::send_login_rejected(int fd, char reason) {
  soup::LoginRejected msg{};
  soup::make_header(msg.header, soup::pkt::kLoginRejected, 1);
  msg.reject_reason_code = reason;
  write_all(fd, &msg, sizeof(msg));
}

void OuchMockGateway::send_heartbeat(int fd) {
  soup::Heartbeat hb{};
  soup::make_header(hb.header, soup::pkt::kServerHeartbeat, 0);
  write_all(fd, &hb, sizeof(hb));
}

void OuchMockGateway::send_sequenced(int fd, const void* msg, std::size_t len) {
  soup::Header h{};
  soup::make_header(h, soup::pkt::kSequencedData, static_cast<std::uint16_t>(len));
  write_all(fd, &h, sizeof(h));
  write_all(fd, msg, len);
}

void OuchMockGateway::handle_unsequenced_payload(int fd,
                                                 const std::uint8_t* payload,
                                                 std::size_t len) {
  if (len == 0) return;
  const char type = static_cast<char>(payload[0]);

  using namespace bist::ouch;

  switch (type) {
    case msg_type::kEnterOrder: {
      if (len != sizeof(EnterOrder)) return;
      EnterOrder eo{};
      std::memcpy(&eo, payload, sizeof(eo));

      const std::string tok(eo.order_token, kTokenLen);
      const auto reject = [&](std::int32_t code) {
        OrderRejected j{};
        std::memset(&j, 0, sizeof(j));
        j.message_type = msg_type::kOrderRejected;
        j.timestamp_ns.set(wall_ns());
        std::memcpy(j.order_token, eo.order_token, kTokenLen);
        j.reject_code.set(code);
        send_sequenced(fd, &j, sizeof(j));
      };

      // Duplicate token => -800002. Resting / inactivation tracking is
      // mutex-guarded so the runner-driven inactivate_all_resting() can
      // safely interleave.
      {
        std::lock_guard<std::mutex> g(state_mu_);
        for (const auto& seen : seen_tokens_) {
          if (seen == tok) { reject(reject_code::kTokenNotUnique); return; }
        }
        seen_tokens_.push_back(tok);
      }

      // Heuristic price-limit gate: cert PDFs trigger -420131 with
      // 10.000 TL on a 6.000 TL stock. Anything beyond 9000 wire units
      // reproduces that failure mode without needing a real LULD model.
      if (eo.price.get() > 9000) {
        reject(reject_code::kPriceOutsideLimits);
        return;
      }

      const std::uint64_t oid = order_id_seq_++;
      OrderAccepted oa{};
      std::memset(&oa, 0, sizeof(oa));
      oa.message_type = msg_type::kOrderAccepted;
      oa.timestamp_ns.set(wall_ns());
      std::memcpy(oa.order_token, eo.order_token, kTokenLen);
      oa.order_book_id.set(eo.order_book_id.get());
      oa.side          = eo.side;
      oa.order_id.set(oid);
      oa.quantity.set(eo.quantity.get());
      oa.price.set(eo.price.get());
      oa.time_in_force = eo.time_in_force;
      oa.open_close    = eo.open_close;
      std::memcpy(oa.client_account, eo.client_account, kClientAccLen);
      oa.order_state = static_cast<std::uint8_t>(OrderState::OnBook);
      std::memcpy(oa.customer_info, eo.customer_info, kCustomerInfoLen);
      std::memcpy(oa.exchange_info, eo.exchange_info, kExchangeInfoLen);
      oa.pre_trade_quantity.set(eo.quantity.get());
      oa.display_quantity.set(eo.display_quantity.get());
      oa.client_category = eo.client_category;
      oa.off_hours       = eo.off_hours;
      send_sequenced(fd, &oa, sizeof(oa));
      // Track as a resting order so EOD batch can inactivate it.
      // IOC / FOK orders do not rest after match; in the mock we have no
      // matching engine, so we only register Day orders.
      if (eo.time_in_force == 0) {
        std::lock_guard<std::mutex> g(state_mu_);
        resting_[tok] = RestingOrder{eo.order_book_id.get(), eo.side, oid};
      }
      return;
    }
    case msg_type::kCancelOrder: {
      if (len != sizeof(CancelOrder)) return;
      CancelOrder co{};
      std::memcpy(&co, payload, sizeof(co));
      const std::string tok(co.order_token, kTokenLen);
      OrderCanceled oc{};
      std::memset(&oc, 0, sizeof(oc));
      oc.message_type = msg_type::kOrderCanceled;
      oc.timestamp_ns.set(wall_ns());
      std::memcpy(oc.order_token, co.order_token, kTokenLen);
      oc.reason = 1;  // canceled by user
      {
        std::lock_guard<std::mutex> g(state_mu_);
        auto it = resting_.find(tok);
        if (it != resting_.end()) {
          oc.order_book_id.set(it->second.order_book_id);
          oc.side    = it->second.side;
          oc.order_id.set(it->second.order_id);
          resting_.erase(it);
        }
      }
      send_sequenced(fd, &oc, sizeof(oc));
      return;
    }
    case msg_type::kCancelByOrderId: {
      if (len != sizeof(CancelByOrderId)) return;
      CancelByOrderId cy{};
      std::memcpy(&cy, payload, sizeof(cy));
      OrderCanceled oc{};
      std::memset(&oc, 0, sizeof(oc));
      oc.message_type = msg_type::kOrderCanceled;
      oc.timestamp_ns.set(wall_ns());
      // Token left blank because the original entry token isn't in scope here.
      oc.order_book_id.set(cy.order_book_id.get());
      oc.side    = cy.side;
      oc.order_id.set(cy.order_id.get());
      oc.reason  = 1;
      send_sequenced(fd, &oc, sizeof(oc));
      return;
    }
    case msg_type::kReplaceOrder: {
      if (len != sizeof(ReplaceOrder)) return;
      ReplaceOrder ro{};
      std::memcpy(&ro, payload, sizeof(ro));
      const std::string old_tok(ro.existing_order_token, kTokenLen);
      const std::string new_tok(ro.replacement_order_token, kTokenLen);
      const std::uint64_t oid = order_id_seq_++;
      OrderReplaced rep{};
      std::memset(&rep, 0, sizeof(rep));
      rep.message_type = msg_type::kOrderReplaced;
      rep.timestamp_ns.set(wall_ns());
      std::memcpy(rep.replacement_order_token, ro.replacement_order_token,
                  kTokenLen);
      std::memcpy(rep.previous_order_token, ro.existing_order_token, kTokenLen);
      rep.order_id.set(oid);
      rep.quantity.set(ro.quantity.get());
      rep.price.set(ro.price.get());
      rep.open_close = ro.open_close;
      std::memcpy(rep.client_account, ro.client_account, kClientAccLen);
      rep.order_state = static_cast<std::uint8_t>(OrderState::OnBook);
      std::memcpy(rep.customer_info, ro.customer_info, kCustomerInfoLen);
      std::memcpy(rep.exchange_info, ro.exchange_info, kExchangeInfoLen);
      rep.pre_trade_quantity.set(ro.quantity.get());
      rep.display_quantity.set(ro.display_quantity.get());
      rep.client_category = ro.client_category;
      {
        std::uint32_t ob = 0;
        char sd = 0;
        std::lock_guard<std::mutex> g(state_mu_);
        auto it = resting_.find(old_tok);
        if (it != resting_.end()) {
          ob = it->second.order_book_id;
          sd = it->second.side;
          resting_.erase(it);
        }
        rep.order_book_id.set(ob);
        rep.side = sd;
        resting_[new_tok] = RestingOrder{ob, sd, oid};
      }
      send_sequenced(fd, &rep, sizeof(rep));
      return;
    }
    case msg_type::kMassQuote: {
      if (len < kMassQuoteHdrLen + kQuoteEntryLen) return;
      MassQuoteHeader hdr{};
      std::memcpy(&hdr, payload, sizeof(hdr));
      const std::uint16_t n = hdr.no_quote_entries.get();
      const auto* entries =
          reinterpret_cast<const QuoteEntry*>(payload + kMassQuoteHdrLen);

      for (std::uint16_t i = 0; i < n; ++i) {
        const auto& e = entries[i];
        const auto bid_px = e.bid_price.get();
        const auto off_px = e.offer_price.get();
        const bool bid_active = e.bid_size.get() > 0 && bid_px > 0;
        const bool off_active = e.offer_size.get() > 0 && off_px > 0;
        const bool crossed    = bid_active && off_active && bid_px >= off_px;

        if (crossed) {
          MassQuoteRejection r{};
          std::memset(&r, 0, sizeof(r));
          r.message_type = msg_type::kMassQuoteRejection;
          r.timestamp_ns.set(wall_ns());
          std::memcpy(r.order_token, hdr.order_token, kTokenLen);
          r.order_book_id.set(e.order_book_id.get());
          r.reject_code.set(static_cast<std::int32_t>(-420131));
          send_sequenced(fd, &r, sizeof(r));
          continue;
        }

        const auto status_for_side = [&](char side, bool active_now) {
          const std::string key =
              std::to_string(e.order_book_id.get()) + ":" + std::string(1, side);
          const bool was_active = quote_active_[key];
          quote_active_[key] = active_now;
          if (active_now) {
            return was_active ? MassQuoteStatus::Updated : MassQuoteStatus::Accept;
          }
          return MassQuoteStatus::Canceled;
        };

        for (char side : {'B', 'S'}) {
          const bool active_now = side == 'B' ? bid_active : off_active;
          MassQuoteAck ack{};
          std::memset(&ack, 0, sizeof(ack));
          ack.message_type = msg_type::kMassQuoteAck;
          ack.timestamp_ns.set(wall_ns());
          std::memcpy(ack.order_token, hdr.order_token, kTokenLen);
          ack.order_book_id.set(e.order_book_id.get());
          ack.side = side;
          ack.quote_status.set(
              static_cast<std::uint32_t>(status_for_side(side, active_now)));
          if (side == 'B') {
            ack.quantity.set(e.bid_size.get());
            ack.price.set(bid_px);
          } else {
            ack.quantity.set(e.offer_size.get());
            ack.price.set(off_px);
          }
          send_sequenced(fd, &ack, sizeof(ack));
        }
      }
      return;
    }
    default:
      return;  // unknown OUCH type — ignore
  }
}

}  // namespace bist::mock
