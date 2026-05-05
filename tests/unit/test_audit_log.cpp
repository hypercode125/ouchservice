#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "bist/observability/audit.hpp"

namespace {

class TmpDir {
 public:
  TmpDir() {
    auto base = std::filesystem::temp_directory_path() / "bist_audit_test";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    static std::atomic<std::uint64_t> seq{0};
    path_ = base / (std::to_string(static_cast<std::uint64_t>(::getpid())) + "_" +
                    std::to_string(static_cast<std::uint64_t>(::time(nullptr))) + "_" +
                    std::to_string(seq.fetch_add(1)));
    std::filesystem::create_directories(path_, ec);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST(AuditLog, StartCreatesDirectoryAndWritesRecords) {
  TmpDir td;
  const auto audit_dir = td.path() / "nested" / "audit";
  ASSERT_FALSE(std::filesystem::exists(audit_dir));

  bist::observability::AuditLog audit(audit_dir.string());
  auto started = audit.start();
  ASSERT_TRUE(started.ok()) << started.error().detail;

  const std::uint8_t sent[] = {'O', '1'};
  const std::uint8_t recv[] = {'A', '2'};
  EXPECT_TRUE(audit.record(bist::observability::AuditDirection::Sent,
                           "OUCH-OE", 0, sent));
  EXPECT_TRUE(audit.record(bist::observability::AuditDirection::Recv,
                           "OUCH-OE", 42, recv));
  audit.stop();

  ASSERT_TRUE(std::filesystem::exists(audit_dir));
  std::string contents;
  for (const auto& e : std::filesystem::directory_iterator(audit_dir)) {
    if (!e.is_regular_file()) continue;
    std::ifstream f(e.path());
    contents.assign(std::istreambuf_iterator<char>(f),
                    std::istreambuf_iterator<char>());
  }
  EXPECT_NE(contents.find("SENT  OUCH-OE"), std::string::npos) << contents;
  EXPECT_NE(contents.find("RECV  OUCH-OE  42"), std::string::npos) << contents;
  EXPECT_NE(contents.find("hex=4f31"), std::string::npos) << contents;
  EXPECT_NE(contents.find("hex=4132"), std::string::npos) << contents;
}
