#pragma once
//
// bist/net/tcp_socket.hpp — minimal POSIX TCP client socket wrapper.
//
// We deliberately keep the abstraction surface tiny: connect, send, recv,
// close, and a fd accessor for poll/epoll registration. No transparent
// reconnection, no buffering — the OUCH and FIX session layers own those
// concerns.
//
// The wrapper is non-blocking after connect(): callers manage readability
// and writability through a Reactor.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "bist/core/result.hpp"

namespace bist::net {

class TcpSocket {
 public:
  TcpSocket() noexcept = default;
  ~TcpSocket() noexcept { close(); }

  TcpSocket(const TcpSocket&)            = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  TcpSocket& operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
      close();
      fd_       = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // Adopt an already-connected file descriptor (e.g. socketpair(2) for tests
  // or a passed-in unit-test pipe). The caller must ensure the fd is a
  // stream socket. Sets non-blocking mode but does not enable TCP_NODELAY
  // because UNIX-domain sockets used in tests do not support that option.
  static TcpSocket from_fd(int fd) noexcept {
    TcpSocket s;
    if (fd >= 0) {
      if (const int flags = ::fcntl(fd, F_GETFL, 0); flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
    }
    s.fd_ = fd;
    return s;
  }

  // Connect to host:port using TCP. Sets TCP_NODELAY. The socket is left in
  // non-blocking mode regardless of how the connect itself completed.
  [[nodiscard]] Result<void> connect(std::string_view host, std::uint16_t port,
                                     bool tcp_nodelay = true) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string portstr = std::to_string(port);
    const std::string hoststr{host};
    if (const int rc = ::getaddrinfo(hoststr.c_str(), portstr.c_str(), &hints, &res);
        rc != 0) {
      return make_error(ErrorCategory::Io,
                        std::string{"getaddrinfo: "} + ::gai_strerror(rc));
    }

    int fd = -1;
    for (const addrinfo* p = res; p != nullptr; p = p->ai_next) {
      fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd < 0) continue;
      if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
      ::close(fd);
      fd = -1;
    }
    ::freeaddrinfo(res);

    if (fd < 0) {
      return make_error(ErrorCategory::Io,
                        std::string{"connect failed: "} + std::strerror(errno));
    }

    // Make the socket non-blocking and apply TCP options.
    if (const int flags = ::fcntl(fd, F_GETFL, 0); flags >= 0) {
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    if (tcp_nodelay) {
      const int on = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }

    fd_ = fd;
    return {};
  }

  // Send up to len bytes; returns the number actually written (>= 0) or an
  // Error on EAGAIN/EWOULDBLOCK callers should treat as "wait writable".
  [[nodiscard]] Result<std::size_t> send(const void* buf, std::size_t len) noexcept {
    if (fd_ < 0) return make_error(ErrorCategory::Io, "send on closed socket");
    const ssize_t n = ::send(fd_, buf, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return std::size_t{0};
      }
      return make_error(ErrorCategory::Io,
                        std::string{"send: "} + std::strerror(errno));
    }
    return static_cast<std::size_t>(n);
  }

  // Recv up to len bytes; returns 0 on EAGAIN. Returns Error("eof") on
  // peer-closed connection so that the session layer can react.
  [[nodiscard]] Result<std::size_t> recv(void* buf, std::size_t len) noexcept {
    if (fd_ < 0) return make_error(ErrorCategory::Io, "recv on closed socket");
    const ssize_t n = ::recv(fd_, buf, len, 0);
    if (n == 0) return make_error(ErrorCategory::Io, "eof");
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return std::size_t{0};
      }
      return make_error(ErrorCategory::Io,
                        std::string{"recv: "} + std::strerror(errno));
    }
    return static_cast<std::size_t>(n);
  }

  void close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  [[nodiscard]] int  fd() const noexcept { return fd_; }
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

 private:
  int fd_{-1};
};

}  // namespace bist::net
