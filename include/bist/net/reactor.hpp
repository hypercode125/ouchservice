#pragma once
//
// bist/net/reactor.hpp — reactor (event loop) interface and a portable
// poll(2)-based default implementation.
//
// The OUCH and FIX session layers register their TCP socket with the
// reactor and expose two callbacks: on_readable and on_writable. The hot
// thread spins run_for() to drain events and dispatch.
//
// poll(2) is portable across Linux and macOS, which matters for development
// ergonomics. A Linux-only epoll reactor is anticipated as a drop-in
// replacement under the same interface in a later phase; the call sites
// don't need to know which backend is active.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <poll.h>

#include "bist/core/result.hpp"
#include "bist/core/time.hpp"

namespace bist::net {

enum class IoEvent : std::uint8_t {
  None     = 0,
  Readable = 1u << 0,
  Writable = 1u << 1,
  Error    = 1u << 2,
};

[[nodiscard]] inline IoEvent operator|(IoEvent a, IoEvent b) noexcept {
  return static_cast<IoEvent>(static_cast<std::uint8_t>(a) |
                              static_cast<std::uint8_t>(b));
}
[[nodiscard]] inline bool has(IoEvent set, IoEvent flag) noexcept {
  return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

using IoCallback = std::function<void(IoEvent)>;

class IReactor {
 public:
  virtual ~IReactor() = default;
  virtual Result<void> add(int fd, IoEvent interest, IoCallback cb)        = 0;
  virtual Result<void> modify(int fd, IoEvent interest)                    = 0;
  virtual Result<void> remove(int fd)                                      = 0;
  // Block up to `max_wait` for events; dispatches callbacks for ready fds.
  virtual Result<std::size_t> run_for(std::chrono::milliseconds max_wait)  = 0;
};

// --- Portable poll(2) reactor -----------------------------------------------

class PollReactor final : public IReactor {
 public:
  Result<void> add(int fd, IoEvent interest, IoCallback cb) override {
    if (find_index(fd) >= 0) {
      return make_error(ErrorCategory::Validation, "fd already registered");
    }
    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = events_for(interest);
    pollfds_.push_back(pfd);
    callbacks_.push_back(std::move(cb));
    return {};
  }

  Result<void> modify(int fd, IoEvent interest) override {
    const int idx = find_index(fd);
    if (idx < 0) return make_error(ErrorCategory::Validation, "fd not registered");
    pollfds_[static_cast<std::size_t>(idx)].events = events_for(interest);
    return {};
  }

  Result<void> remove(int fd) override {
    const int idx = find_index(fd);
    if (idx < 0) return make_error(ErrorCategory::Validation, "fd not registered");
    const auto i = static_cast<std::size_t>(idx);
    pollfds_.erase(pollfds_.begin() + idx);
    callbacks_.erase(callbacks_.begin() + idx);
    (void)i;
    return {};
  }

  Result<std::size_t> run_for(std::chrono::milliseconds max_wait) override {
    if (pollfds_.empty()) {
      // Nothing to wait for — yield instead of busy-looping.
      std::this_thread_sleep_for_(max_wait);
      return std::size_t{0};
    }
    const int timeout_ms =
        static_cast<int>(std::min<std::chrono::milliseconds::rep>(
            max_wait.count(), 1'000'000));
    const int rc = ::poll(pollfds_.data(),
                          static_cast<nfds_t>(pollfds_.size()),
                          timeout_ms);
    if (rc < 0) {
      return make_error(ErrorCategory::Io, "poll failed");
    }
    if (rc == 0) return std::size_t{0};

    std::size_t fired = 0;
    // Snapshot revents and zero them so that re-entrant modify/remove from
    // a callback cannot disturb iteration.
    for (std::size_t i = 0; i < pollfds_.size(); ++i) {
      const short re = pollfds_[i].revents;
      pollfds_[i].revents = 0;
      if (re == 0) continue;
      IoEvent ev = IoEvent::None;
      if (re & POLLIN)  ev = ev | IoEvent::Readable;
      if (re & POLLOUT) ev = ev | IoEvent::Writable;
      if (re & (POLLERR | POLLHUP | POLLNVAL)) ev = ev | IoEvent::Error;
      callbacks_[i](ev);
      ++fired;
    }
    return fired;
  }

 private:
  static short events_for(IoEvent e) noexcept {
    short ev = 0;
    if (has(e, IoEvent::Readable)) ev |= POLLIN;
    if (has(e, IoEvent::Writable)) ev |= POLLOUT;
    return ev;
  }

  int find_index(int fd) const noexcept {
    for (std::size_t i = 0; i < pollfds_.size(); ++i) {
      if (pollfds_[i].fd == fd) return static_cast<int>(i);
    }
    return -1;
  }

  // Tiny helper to avoid pulling <thread> header into every consumer when
  // sleep is needed only on the empty-fd path.
  static void std_this_thread_sleep_for_unused();
  inline static void std_this_thread_sleep_for_(std::chrono::milliseconds d);

  std::vector<pollfd>     pollfds_;
  std::vector<IoCallback> callbacks_;
};

// inline implementation below to keep <thread> out of the public surface
// for callers that just need the interface.

}  // namespace bist::net

#include <thread>

namespace bist::net {
inline void PollReactor::std_this_thread_sleep_for_(std::chrono::milliseconds d) {
  std::this_thread::sleep_for(d);
}
}  // namespace bist::net
