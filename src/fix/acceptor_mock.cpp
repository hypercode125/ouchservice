// src/fix/acceptor_mock.cpp — implementation of bist::fix::AcceptorMock
// declared in include/bist/fix/acceptor_mock.hpp. Bridges the public POD
// config to the internal QuickFIX-based detail::AcceptorMock.
//
// Pinned to -std=gnu++14 in src/fix/CMakeLists.txt because it includes
// QuickFIX headers via acceptor_mock.hpp.

#if defined(BIST_HAS_QUICKFIX)

#include "bist/fix/acceptor_mock.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <utility>

#include "src/fix/internal/acceptor_mock.hpp"

namespace bist::fix::detail {

int AcceptorMock::pick_ephemeral_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    ::close(fd);
    return -1;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

}  // namespace bist::fix::detail

namespace bist::fix {

struct AcceptorMock::Impl {
  detail::AcceptorMock mock;
};

AcceptorMock::AcceptorMock(std::unique_ptr<Impl> i) noexcept
    : impl_(std::move(i)) {}
AcceptorMock::~AcceptorMock() = default;

int AcceptorMock::port() const noexcept {
  return impl_ ? impl_->mock.port() : 0;
}

Result<std::unique_ptr<AcceptorMock>> AcceptorMock::create(
    AcceptorMockConfig cfg) {
  auto impl = std::unique_ptr<Impl>(new Impl());

  detail::AcceptorEndpoints e;
  e.oe_target_comp_id = std::move(cfg.oe_initiator_sender_comp_id);
  e.oe_sender_comp_id = std::move(cfg.oe_initiator_target_comp_id);
  e.rd_target_comp_id = std::move(cfg.rd_initiator_sender_comp_id);
  e.rd_sender_comp_id = std::move(cfg.rd_initiator_target_comp_id);
  e.app_data_dictionary       = std::move(cfg.app_data_dictionary);
  e.transport_data_dictionary = std::move(cfg.transport_data_dictionary);
  e.store_path = std::move(cfg.store_path);
  e.log_path   = std::move(cfg.log_path);
  e.port       = cfg.port;
  e.heartbeat  = cfg.heartbeat;

  auto r = impl->mock.start(std::move(e));
  if (!r) return r.error();
  return std::unique_ptr<AcceptorMock>(new AcceptorMock(std::move(impl)));
}

}  // namespace bist::fix

#endif  // BIST_HAS_QUICKFIX
