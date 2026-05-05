// tests/unit/test_sequence_store.cpp — atomic load/save + throttled save_if_due.

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <thread>
#include <unistd.h>

#include "bist/observability/sequence_store.hpp"
#include "gtest/gtest.h"

namespace {

class TmpDir {
 public:
  TmpDir() {
    auto base = std::filesystem::temp_directory_path() / "bist_seq_test";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    static std::atomic<std::uint64_t> seq{0};
    auto unique = base / (std::to_string(static_cast<std::uint64_t>(::getpid())) + "_" +
                          std::to_string(static_cast<std::uint64_t>(::time(nullptr))) + "_" +
                          std::to_string(seq.fetch_add(1)));
    std::filesystem::create_directories(unique, ec);
    path_ = unique;
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TmpDir(const TmpDir&)            = delete;
  TmpDir& operator=(const TmpDir&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST(SequenceStore, LoadAbsentFileReturnsEmpty) {
  TmpDir td;
  bist::observability::SequenceStore store(td.path() / "oe.seq");
  std::string sess;
  std::uint64_t seq = 42;
  auto r = store.load(sess, seq);
  ASSERT_TRUE(r.ok()) << r.error().detail;
  EXPECT_EQ(sess, "");
  EXPECT_EQ(seq, 0u);
}

TEST(SequenceStore, SaveLoadRoundTrip) {
  TmpDir td;
  bist::observability::SequenceStore store(td.path() / "oe.seq");
  ASSERT_TRUE(store.save("OUCH00001", 12345).ok());

  std::string sess;
  std::uint64_t seq = 0;
  ASSERT_TRUE(store.load(sess, seq).ok());
  EXPECT_EQ(sess, "OUCH00001");
  EXPECT_EQ(seq, 12345u);
}

TEST(SequenceStore, SaveAtomicLeavesNoTmp) {
  TmpDir td;
  auto file = td.path() / "oe.seq";
  bist::observability::SequenceStore store(file);
  ASSERT_TRUE(store.save("S", 1).ok());
  ASSERT_TRUE(std::filesystem::exists(file));
  EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp"));
}

TEST(SequenceStore, SaveOverwritesPreviousValue) {
  TmpDir td;
  bist::observability::SequenceStore store(td.path() / "oe.seq");
  ASSERT_TRUE(store.save("S1", 100).ok());
  ASSERT_TRUE(store.save("S2", 200).ok());
  std::string sess;
  std::uint64_t seq = 0;
  ASSERT_TRUE(store.load(sess, seq).ok());
  EXPECT_EQ(sess, "S2");
  EXPECT_EQ(seq, 200u);
}

TEST(SequenceStore, SaveIfDueRespectsSeqDelta) {
  TmpDir td;
  auto file = td.path() / "oe.seq";
  bist::observability::SequenceStore store(file);

  // Initial save establishes the baseline.
  ASSERT_TRUE(store.save("S", 1000).ok());

  // Less than min_seq_delta (100) and shorter than interval — no write.
  ASSERT_TRUE(store.save_if_due("S", 1050, /*min_seq_delta=*/100,
                                /*min_interval_ms=*/10'000).ok());
  EXPECT_EQ(store.last_saved_seq(), 1000u);

  // Crosses the seq delta — write happens.
  ASSERT_TRUE(store.save_if_due("S", 1100, /*min_seq_delta=*/100,
                                /*min_interval_ms=*/10'000).ok());
  EXPECT_EQ(store.last_saved_seq(), 1100u);
}

TEST(SequenceStore, SaveIfDueRespectsTimeInterval) {
  TmpDir td;
  bist::observability::SequenceStore store(td.path() / "oe.seq");
  ASSERT_TRUE(store.save("S", 5000).ok());

  // Below seq delta but past the time window: should write.
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  ASSERT_TRUE(store.save_if_due("S", 5001, /*min_seq_delta=*/100,
                                /*min_interval_ms=*/10).ok());
  EXPECT_EQ(store.last_saved_seq(), 5001u);
}

TEST(SequenceStore, MalformedFileReportsValidationError) {
  TmpDir td;
  auto file = td.path() / "oe.seq";
  {
    std::ofstream f(file);
    f << "no_colon_here\n";
  }
  bist::observability::SequenceStore store(file);
  std::string sess;
  std::uint64_t seq = 0;
  auto r = store.load(sess, seq);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().category, bist::ErrorCategory::Validation);
}
