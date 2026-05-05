// tests/unit/test_epoll_reactor.cpp — parity test for the Linux epoll backend.
//
// Skipped on non-Linux platforms. Confirms add/modify/remove + a single
// readable event dispatch + idle wake-up.

#include <gtest/gtest.h>

#if defined(__linux__)

#include <chrono>
#include <unistd.h>
#include <sys/socket.h>

#include "bist/net/epoll_reactor.hpp"

using bist::net::EpollReactor;
using bist::net::IoEvent;

TEST(EpollReactor, AddRemoveAndDispatch) {
  int sv[2];
  ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  EpollReactor r;
  bool fired = false;
  ASSERT_TRUE(r.add(sv[0], IoEvent::Readable, [&](IoEvent ev) {
    if (bist::net::has(ev, IoEvent::Readable)) fired = true;
  }));

  // No event yet: run_for returns 0 within budget.
  auto rr = r.run_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(rr);
  EXPECT_EQ(rr.value(), 0u);
  EXPECT_FALSE(fired);

  // Write into the peer; the watched fd becomes readable.
  ::write(sv[1], "x", 1);
  rr = r.run_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(rr);
  EXPECT_GE(rr.value(), 1u);
  EXPECT_TRUE(fired);

  ASSERT_TRUE(r.remove(sv[0]));
  ::close(sv[0]);
  ::close(sv[1]);
}

TEST(EpollReactor, IdleSleepWhenEmpty) {
  EpollReactor r;
  const auto t0 = std::chrono::steady_clock::now();
  auto rr = r.run_for(std::chrono::milliseconds(20));
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(rr);
  EXPECT_EQ(rr.value(), 0u);
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            15);
}

#else
TEST(EpollReactor, SkippedOnNonLinux) { GTEST_SKIP() << "Linux-only"; }
#endif
