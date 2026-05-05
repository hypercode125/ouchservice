#pragma once
//
// bist/net/epoll_reactor.hpp — Linux epoll(7) backend for IReactor.
//
// Drop-in replacement for PollReactor on Linux. Co-location target: lower
// per-event syscall cost (one epoll_wait vs O(N) poll scan) and constant-
// time fd add/remove. macOS / dev workstations stay on PollReactor.
//
// Compile-time selection: when __linux__ is defined and EpollReactor is
// available, callers may type-erase via `IReactor` and pick at startup.
// All other platforms: use PollReactor.

#if defined(__linux__)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/epoll.h>
#include <unistd.h>

#include "bist/core/result.hpp"
#include "bist/net/reactor.hpp"

namespace bist::net {

class EpollReactor final : public IReactor {
 public:
  EpollReactor() {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  }

  ~EpollReactor() override {
    if (epfd_ >= 0) ::close(epfd_);
  }

  EpollReactor(const EpollReactor&)            = delete;
  EpollReactor& operator=(const EpollReactor&) = delete;

  Result<void> add(int fd, IoEvent interest, IoCallback cb) override {
    if (epfd_ < 0) return make_error(ErrorCategory::Io, "epoll_create1 failed");
    if (callbacks_.count(fd)) {
      return make_error(ErrorCategory::Validation, "fd already registered");
    }
    epoll_event ev{};
    ev.events  = events_for(interest);
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
      return make_error(ErrorCategory::Io, "epoll_ctl ADD failed");
    }
    callbacks_.emplace(fd, std::move(cb));
    return {};
  }

  Result<void> modify(int fd, IoEvent interest) override {
    auto it = callbacks_.find(fd);
    if (it == callbacks_.end()) {
      return make_error(ErrorCategory::Validation, "fd not registered");
    }
    epoll_event ev{};
    ev.events  = events_for(interest);
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
      return make_error(ErrorCategory::Io, "epoll_ctl MOD failed");
    }
    return {};
  }

  Result<void> remove(int fd) override {
    auto it = callbacks_.find(fd);
    if (it == callbacks_.end()) {
      return make_error(ErrorCategory::Validation, "fd not registered");
    }
    if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
      return make_error(ErrorCategory::Io, "epoll_ctl DEL failed");
    }
    callbacks_.erase(it);
    return {};
  }

  Result<std::size_t> run_for(std::chrono::milliseconds max_wait) override {
    if (callbacks_.empty()) {
      std::this_thread::sleep_for(max_wait);
      return std::size_t{0};
    }
    constexpr int kMaxBatch = 64;
    epoll_event events[kMaxBatch];
    const int timeout_ms = static_cast<int>(max_wait.count());
    const int n = ::epoll_wait(epfd_, events, kMaxBatch, timeout_ms);
    if (n < 0) return make_error(ErrorCategory::Io, "epoll_wait failed");
    std::size_t fired = 0;
    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      const std::uint32_t e = events[i].events;
      IoEvent ev = IoEvent::None;
      if (e & EPOLLIN)  ev = ev | IoEvent::Readable;
      if (e & EPOLLOUT) ev = ev | IoEvent::Writable;
      if (e & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) ev = ev | IoEvent::Error;
      auto it = callbacks_.find(fd);
      if (it != callbacks_.end()) {
        it->second(ev);
        ++fired;
      }
    }
    return fired;
  }

 private:
  static std::uint32_t events_for(IoEvent e) noexcept {
    std::uint32_t ev = EPOLLET;     // edge-triggered for hot-path latency
    if (has(e, IoEvent::Readable)) ev |= EPOLLIN | EPOLLRDHUP;
    if (has(e, IoEvent::Writable)) ev |= EPOLLOUT;
    return ev;
  }

  int                                       epfd_{-1};
  std::unordered_map<int, IoCallback>       callbacks_;
};

}  // namespace bist::net

#endif  // __linux__
