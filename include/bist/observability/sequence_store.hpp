#pragma once
//
// bist/observability/sequence_store.hpp — durable next-inbound-sequence cursor.
//
// SoupBinTCP/OUCH gateways assign a session ID + monotonically-increasing
// sequence number per Sequenced Data packet. To resume a session after a
// crash or restart, the client must re-login with the previously assigned
// session ID and the next-expected sequence number; otherwise the gateway
// either replays from the beginning (waste) or rejects the resume.
//
// The store keeps a single line per channel:
//
//     <session_id_padded>:<next_inbound_sequence>\n
//
// Writes are atomic (write-then-rename) so a crash mid-write never produces
// a partially overwritten cursor. The save path is throttled to keep the
// hot path off the synchronous filesystem cost: callers invoke
// `save_if_due()` from the reactor and the actual fsync+rename only fires
// when either `min_seq_delta` packets accumulated or `min_interval_ms`
// elapsed since the last persistent write.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "bist/core/result.hpp"
#include "bist/core/time.hpp"

namespace bist::observability {

class SequenceStore {
 public:
  explicit SequenceStore(std::filesystem::path file)
      : file_(std::move(file)) {}

  // Load the persisted (session, seq) pair. Returns ok with both empty/0
  // when the file is absent — this is the cold-start case, not an error.
  Result<void> load(std::string& session, std::uint64_t& seq) const {
    session.clear();
    seq = 0;
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) return {};

    std::ifstream f(file_);
    if (!f) {
      return make_error(ErrorCategory::Io,
                        std::string{"open "} + file_.string() + ": "
                        + std::strerror(errno));
    }
    std::string line;
    std::getline(f, line);
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      return make_error(ErrorCategory::Validation,
                        "sequence file malformed (missing ':')");
    }
    session = line.substr(0, colon);
    try {
      seq = std::stoull(line.substr(colon + 1));
    } catch (const std::exception& e) {
      return make_error(ErrorCategory::Validation,
                        std::string{"sequence file malformed: "} + e.what());
    }
    return {};
  }

  // Atomically write (session, seq) to disk. Writes to a sibling
  // `<file>.tmp` then renames over `<file>` so a partial write can never
  // be observed.
  Result<void> save(std::string_view session, std::uint64_t seq) {
    std::error_code ec;
    if (auto parent = file_.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent, ec);
    }
    auto tmp = file_;
    tmp += ".tmp";

    {
      std::ofstream f(tmp, std::ios::out | std::ios::trunc);
      if (!f) {
        return make_error(ErrorCategory::Io,
                          std::string{"open "} + tmp.string() + ": "
                          + std::strerror(errno));
      }
      f << session << ':' << seq << '\n';
      f.flush();
      if (!f) {
        return make_error(ErrorCategory::Io,
                          std::string{"write "} + tmp.string() + " failed");
      }
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
      return make_error(ErrorCategory::Io,
                        std::string{"rename "} + tmp.string() + " -> "
                        + file_.string() + ": " + ec.message());
    }
    last_saved_seq_ = seq;
    last_save_ns_   = monotonic_ns();
    return {};
  }

  // Throttled save: writes only when the seq has advanced by at least
  // `min_seq_delta` OR `min_interval_ms` has elapsed since the last save.
  Result<void> save_if_due(std::string_view session, std::uint64_t seq,
                           std::uint64_t min_seq_delta = 100,
                           std::uint32_t min_interval_ms = 100) {
    if (seq == last_saved_seq_) return {};
    const auto now = monotonic_ns();
    const std::uint64_t interval_ns =
        static_cast<std::uint64_t>(min_interval_ms) * 1'000'000ULL;
    const bool seq_due  = seq - last_saved_seq_ >= min_seq_delta;
    const bool time_due = last_save_ns_ == 0 ||
                          now - last_save_ns_ >= interval_ns;
    if (!seq_due && !time_due) return {};
    return save(session, seq);
  }

  // Force a final flush — used at shutdown / logout.
  Result<void> flush(std::string_view session, std::uint64_t seq) {
    if (seq == last_saved_seq_) return {};
    return save(session, seq);
  }

  [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
  [[nodiscard]] std::uint64_t last_saved_seq() const noexcept { return last_saved_seq_; }

 private:
  std::filesystem::path file_;
  std::uint64_t last_saved_seq_{0};
  TimestampNs   last_save_ns_{0};
};

}  // namespace bist::observability
