// tests/unit/test_config.cpp — TOML loader and override-merge behaviour.

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "bist/config/config.hpp"
#include "gtest/gtest.h"

namespace {

// RAII tmpdir for one test. Drops the directory on dtor so failures still
// clean up.
class TmpDir {
 public:
  TmpDir() {
    auto base = std::filesystem::temp_directory_path() / "bist_cfg_test";
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

  std::filesystem::path write(std::string_view name, std::string_view content) {
    auto p = path_ / std::string(name);
    std::ofstream f(p);
    f << content;
    return p;
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

constexpr const char* kBaseToml = R"(
[environment]
name = "test_env"
description = "unit-test fixture"
weekend = false

[ouch]
host_primary    = "10.57.3.146"
host_secondary  = "10.57.3.147"
ports           = [21501, 21502, 21503, 21504]
heartbeat_ms    = 800
username        = ""
password        = ""
session         = ""
sequence        = 0

[ouch.uea]
host  = "10.57.3.144"
ports = [21501, 21502]

[fix]
host_primary = "10.57.3.146"

[throttler]
orders_per_sec = 100
)";

constexpr const char* kLocalToml = R"(
[ouch]
username = "TEST01"
password = "secret"

[fix.password]
current = "pw_curr"
next    = "pw_next"
)";

}  // namespace

TEST(Config, LoadBaseValidatesRequired) {
  TmpDir td;
  auto p = td.write("base.toml", kBaseToml);
  // Without local override, username/password are empty → Validation error.
  auto r = bist::config::load_config(p.string());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().category, bist::ErrorCategory::Validation);
  EXPECT_NE(r.error().detail.find("username"), std::string::npos);
}

TEST(Config, LocalOverrideFillsSecrets) {
  TmpDir td;
  td.write("base.toml", kBaseToml);
  td.write("base.local.toml", kLocalToml);
  auto r = bist::config::load_config((td.path() / "base.toml").string());
  ASSERT_TRUE(r.ok()) << r.error().detail;
  const auto& cfg = r.value();
  EXPECT_EQ(cfg.environment.name, "test_env");
  EXPECT_EQ(cfg.ouch.host_primary, "10.57.3.146");
  EXPECT_EQ(cfg.ouch.host_secondary, "10.57.3.147");
  ASSERT_EQ(cfg.ouch.ports.size(), 4u);
  EXPECT_EQ(cfg.ouch.ports[0], 21501);
  EXPECT_EQ(cfg.ouch.ports[3], 21504);
  EXPECT_EQ(cfg.ouch.username, "TEST01");
  EXPECT_EQ(cfg.ouch.password, "secret");
  EXPECT_EQ(cfg.ouch.uea.host, "10.57.3.144");
  ASSERT_EQ(cfg.ouch.uea.ports.size(), 2u);
  EXPECT_EQ(cfg.fix.password.current, "pw_curr");
  EXPECT_EQ(cfg.fix.password.next,    "pw_next");
  EXPECT_EQ(cfg.throttler.orders_per_sec, 100u);
}

TEST(Config, ResolveOuchEndpointPartitionRange) {
  TmpDir td;
  td.write("base.toml", kBaseToml);
  td.write("base.local.toml", kLocalToml);
  auto cfg = bist::config::load_config((td.path() / "base.toml").string());
  ASSERT_TRUE(cfg.ok());

  auto p1 = bist::config::resolve_ouch_endpoint(cfg.value().ouch, /*partition=*/1, false);
  ASSERT_TRUE(p1.ok());
  EXPECT_EQ(p1.value().host, "10.57.3.146");
  EXPECT_EQ(p1.value().port, 21501);

  auto p4 = bist::config::resolve_ouch_endpoint(cfg.value().ouch, /*partition=*/4, false);
  ASSERT_TRUE(p4.ok());
  EXPECT_EQ(p4.value().port, 21504);

  auto secondary = bist::config::resolve_ouch_endpoint(cfg.value().ouch, /*partition=*/2, true);
  ASSERT_TRUE(secondary.ok());
  EXPECT_EQ(secondary.value().host, "10.57.3.147");

  auto bad = bist::config::resolve_ouch_endpoint(cfg.value().ouch, /*partition=*/9, false);
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().category, bist::ErrorCategory::Validation);
}

TEST(Config, ParseErrorReportsCleanly) {
  TmpDir td;
  auto p = td.write("bad.toml", "[environment\nname = \"x\"\n");
  auto r = bist::config::load_config(p.string());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().category, bist::ErrorCategory::Validation);
  EXPECT_NE(r.error().detail.find("toml parse failed"), std::string::npos);
}
