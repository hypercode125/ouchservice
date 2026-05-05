#pragma once
//
// bist/observability/audit.hpp — append-only audit log used as the cert
// auditor's source of truth.
//
// Every sent and received protocol message is recorded as a single line:
//
//   <ISO-8601 timestamp UTC>  <DIR>  <CHANNEL>  <SEQ?>  <type>  hex=<dump>
//
// e.g.
//   2026-05-05T07:32:11.123456789Z  SENT  OUCH-OE  -      O  hex=4f...
//   2026-05-05T07:32:11.123987654Z  RECV  OUCH-OE  856    A  hex=41...
//
// The writer fsyncs on close and rotates by date. Hot-path callers should
// not block; AuditLog therefore queues writes through an internal SPSC
// ring drained by a worker thread spawned in start().

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "bist/core/result.hpp"
#include "bist/core/ring_buffer.hpp"
#include "bist/core/time.hpp"
#include "bist/ouch/codec.hpp"

namespace bist::observability {

enum class AuditDirection : std::uint8_t { Sent, Recv };

struct AuditRecord {
  TimestampNs       wall_ns{0};
  AuditDirection    dir{AuditDirection::Sent};
  std::string       channel;
  std::uint64_t     seq{0};
  char              type{'\0'};
  std::string       hex;
};

class AuditLog {
 public:
  // Construct without starting; call start() to spawn the worker thread.
  explicit AuditLog(std::string directory) : directory_(std::move(directory)) {}

  AuditLog(const AuditLog&)            = delete;
  AuditLog& operator=(const AuditLog&) = delete;

  ~AuditLog() { stop(); }

  Result<void> start() {
    if (running_.load(std::memory_order_acquire)) return {};
    if (auto r = open_for_today(); !r) return r.error();
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
    return {};
  }

  void stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
    if (file_.is_open()) {
      file_.flush();
      file_.close();
    }
  }

  // Hot-path entry point: copies a record into the queue. Returns false when
  // the queue is full so that the hot thread can decide its drop policy.
  [[nodiscard]] bool record(AuditDirection dir, std::string_view channel,
                            std::uint64_t seq,
                            std::span<const std::uint8_t> bytes) {
    AuditRecord r{};
    r.wall_ns = wall_ns();
    r.dir     = dir;
    r.channel.assign(channel.data(), channel.size());
    r.seq     = seq;
    r.type    = bytes.empty() ? '\0' : static_cast<char>(bytes[0]);
    r.hex     = ouch::hex_dump(bytes);
    return queue_.try_push(std::move(r));
  }

 private:
  void run() {
    while (running_.load(std::memory_order_acquire)) {
      bool drained = false;
      while (auto rec = queue_.try_pop()) {
        write_one(*rec);
        drained = true;
      }
      if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      maybe_rotate();
    }
    // Drain remainder on shutdown.
    while (auto rec = queue_.try_pop()) write_one(*rec);
  }

  Result<void> open_for_today() {
    auto now   = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    today_.assign(buf);
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
      return make_error(ErrorCategory::Io,
                        "create audit directory " + directory_ + ": " + ec.message());
    }
    const std::string path = directory_ + "/" + today_ + ".audit.log";
    file_.open(path, std::ios::app);
    if (!file_) {
      return make_error(ErrorCategory::Io, "open audit log " + path + " failed");
    }
    return {};
  }

  void maybe_rotate() {
    auto now   = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    if (today_ != buf) {
      file_.flush();
      file_.close();
      (void)open_for_today();
    }
  }

  void write_one(const AuditRecord& r) {
    if (!file_.is_open()) return;
    file_ << format_iso8601(r.wall_ns) << "  "
          << (r.dir == AuditDirection::Sent ? "SENT" : "RECV") << "  "
          << r.channel << "  "
          << (r.seq ? std::to_string(r.seq) : std::string{"-"}) << "  "
          << r.type << "  "
          << "hex=" << r.hex << '\n';
  }

  static std::string format_iso8601(TimestampNs ns) {
    const std::time_t secs   = static_cast<std::time_t>(ns / 1'000'000'000ULL);
    const std::uint32_t frac = static_cast<std::uint32_t>(ns % 1'000'000'000ULL);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[64];
    std::snprintf(out, sizeof(out), "%s.%09uZ", buf, frac);
    return std::string{out};
  }

  std::string                  directory_;
  std::string                  today_;
  std::ofstream                file_;
  std::atomic<bool>            running_{false};
  std::thread                  worker_;
  SpscRing<AuditRecord, 1u<<14> queue_;
};

}  // namespace bist::observability
