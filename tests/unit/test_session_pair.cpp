// tests/unit/test_session_pair.cpp — end-to-end OuchSession smoke against an
// in-process mock peer connected via socketpair.
//
// We don't go through the full SoupBinTCP login dialog; the test instead
// verifies that the session's send path frames an Enter Order correctly
// inside an UnsequencedDataPacket wrapper, and that the receive path parses
// an Order Accepted that the mock writes back.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

#include "bist/net/soupbintcp.hpp"
#include "bist/net/tcp_socket.hpp"
#include "bist/ouch/codec.hpp"
#include "bist/ouch/messages.hpp"
#include "bist/ouch/session.hpp"

namespace {

// Helper that writes a length-prefixed SoupBinTCP packet to a raw fd.
void write_soup_packet(int fd, char type, const void* payload,
                       std::size_t payload_len) {
  bist::soup::Header h{};
  bist::soup::make_header(h, type, static_cast<std::uint16_t>(payload_len));
  ::send(fd, &h, sizeof(h), 0);
  if (payload_len) ::send(fd, payload, payload_len, 0);
}

}  // namespace

TEST(SessionPair, ReceivesLoginAcceptedAndOrderAccepted) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  auto client_sock = bist::net::TcpSocket::from_fd(fds[0]);
  const int peer = fds[1];

  std::atomic<int> accepted_count{0};
  std::atomic<int> state_changes{0};

  bist::ouch::Handlers h{};
  h.on_state_change = [&](bist::ouch::SessionState, const std::string&) {
    ++state_changes;
  };
  h.on_accepted = [&](const bist::ouch::OrderAccepted&) {
    ++accepted_count;
  };

  bist::ouch::LoginParams lp{};
  lp.username = "USR001";
  lp.password = "PWD001";
  bist::ouch::OuchSession sess(client_sock, h, lp);

  ASSERT_TRUE(sess.begin_login());
  EXPECT_EQ(sess.state(), bist::ouch::SessionState::LoggingIn);

  // Drain the LoginRequest the session just produced so subsequent reads
  // observe only the EnterOrder bytes.
  {
    std::uint8_t scratch[256];
    ssize_t total = 0;
    for (int spin = 0; spin < 20 && total < static_cast<ssize_t>(
                            sizeof(bist::soup::LoginRequest)); ++spin) {
      ssize_t r = ::recv(peer, scratch + total,
                         sizeof(scratch) - static_cast<std::size_t>(total),
                         MSG_DONTWAIT);
      if (r > 0) total += r;
      else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(total, static_cast<ssize_t>(sizeof(bist::soup::LoginRequest)));
    EXPECT_EQ(static_cast<char>(scratch[2]), bist::soup::pkt::kLoginRequest);
  }

  // Mock peer: write LoginAccepted with session "TESTSESS  " and seq 42.
  {
    char payload[bist::soup::kSessionLen + bist::soup::kSequenceLen];
    std::memset(payload, ' ', sizeof(payload));
    std::memcpy(payload, "TESTSESS01", 10);
    std::memset(payload + bist::soup::kSessionLen, '0',
                bist::soup::kSequenceLen);
    payload[bist::soup::kSessionLen + bist::soup::kSequenceLen - 2] = '4';
    payload[bist::soup::kSessionLen + bist::soup::kSequenceLen - 1] = '2';
    write_soup_packet(peer, bist::soup::pkt::kLoginAccepted, payload,
                      sizeof(payload));
  }

  ASSERT_TRUE(sess.poll_io());
  EXPECT_EQ(sess.state(), bist::ouch::SessionState::Active);
  EXPECT_EQ(sess.session_id(), "TESTSESS01");
  EXPECT_EQ(sess.next_inbound_seq(), 42u);

  // Send an Enter Order through the session and verify the bytes the peer
  // observes start with 'U' (Unsequenced) followed by the OUCH 'O' message.
  bist::ouch::EnterOrder eo{};
  std::memset(&eo, 0, sizeof(eo));
  eo.message_type = bist::ouch::msg_type::kEnterOrder;
  bist::ouch::token_set(eo.order_token, bist::OrderToken{"TOK-0001"});
  eo.order_book_id.set(70616u);
  eo.side = 'B';
  eo.quantity.set(200u);
  eo.price.set(6200);
  ASSERT_TRUE(sess.send_enter_order(eo));

  std::uint8_t buf[256];
  ssize_t n = 0;
  // Peer may take more than one read to cover header + payload.
  for (int spin = 0; spin < 10 && n < static_cast<ssize_t>(
                          sizeof(bist::soup::Header) + sizeof(eo)); ++spin) {
    ssize_t r = ::recv(peer, buf + n, sizeof(buf) - static_cast<std::size_t>(n), 0);
    if (r > 0) n += r;
    else std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GE(n, static_cast<ssize_t>(sizeof(bist::soup::Header) + sizeof(eo)));
  EXPECT_EQ(static_cast<char>(buf[2]), bist::soup::pkt::kUnsequencedData);
  EXPECT_EQ(static_cast<char>(buf[3]), bist::ouch::msg_type::kEnterOrder);

  // Mock peer: write a SequencedData wrapping an OrderAccepted echoing
  // back the same token.
  bist::ouch::OrderAccepted oa{};
  std::memset(&oa, 0, sizeof(oa));
  oa.message_type = bist::ouch::msg_type::kOrderAccepted;
  oa.timestamp_ns.set(1ull);
  bist::ouch::token_set(oa.order_token, bist::OrderToken{"TOK-0001"});
  oa.order_book_id.set(70616u);
  oa.side = 'B';
  oa.order_id.set(99u);
  oa.quantity.set(200u);
  oa.price.set(6200);
  oa.order_state = static_cast<std::uint8_t>(bist::OrderState::OnBook);
  write_soup_packet(peer, bist::soup::pkt::kSequencedData, &oa, sizeof(oa));

  ASSERT_TRUE(sess.poll_io());
  EXPECT_EQ(accepted_count.load(), 1);

  ::close(peer);
  // client_sock destructor closes fds[0].
}

TEST(SessionPair, CapturesLoginRejectedReasonCode) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  auto client_sock = bist::net::TcpSocket::from_fd(fds[0]);
  const int peer = fds[1];

  bist::ouch::Handlers h{};
  bist::ouch::LoginParams lp{};
  lp.username = "USR001";
  lp.password = "BADPWD";
  bist::ouch::OuchSession sess(client_sock, h, lp);

  ASSERT_TRUE(sess.begin_login());

  {
    std::uint8_t scratch[256];
    ssize_t total = 0;
    for (int spin = 0; spin < 20 && total < static_cast<ssize_t>(
                            sizeof(bist::soup::LoginRequest)); ++spin) {
      ssize_t r = ::recv(peer, scratch + total,
                         sizeof(scratch) - static_cast<std::size_t>(total),
                         MSG_DONTWAIT);
      if (r > 0) total += r;
      else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(total, static_cast<ssize_t>(sizeof(bist::soup::LoginRequest)));
  }

  const char reason = 'A';
  write_soup_packet(peer, bist::soup::pkt::kLoginRejected, &reason, 1);

  ASSERT_TRUE(sess.poll_io());
  EXPECT_EQ(sess.state(), bist::ouch::SessionState::Failed);
  EXPECT_EQ(sess.last_login_reject_reason(), 'A');

  ::close(peer);
}
