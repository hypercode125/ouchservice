// tests/unit/test_ptrm.cpp — pre-trade risk gate semantics.

#include <gtest/gtest.h>

#include "bist/risk/ptrm.hpp"

using namespace bist::risk;

TEST(Ptrm, AlwaysAcceptAllowsEverything) {
  PtrmAlwaysAccept gate;
  PreTradeOrder o{"GARAN.E", 70616, bist::Side::Buy, 100, 5000, 3, "ACC1"};
  auto d = gate.check(o);
  EXPECT_TRUE(d.allow);
  EXPECT_EQ(d.rx, RxCategory::None);
}

TEST(Ptrm, AlwaysRejectCarriesRxAndDetail) {
  PtrmAlwaysReject gate{RxCategory::RxKillSwitch, "kill switch active"};
  PreTradeOrder o{};
  auto d = gate.check(o);
  EXPECT_FALSE(d.allow);
  EXPECT_EQ(d.rx, RxCategory::RxKillSwitch);
  EXPECT_NE(d.detail.find("kill"), std::string::npos);
}

TEST(Ptrm, ModeDisabledShortCircuits) {
  PtrmAlwaysReject gate{RxCategory::RxOther, "should not be observed"};
  PreTradeOrder o{};
  auto d = evaluate(gate, PtrmMode::Disabled, o);
  EXPECT_TRUE(d.allow);
  EXPECT_EQ(d.rx, RxCategory::None);
}

TEST(Ptrm, ModeLogOnlyAllowsButTagsDetail) {
  PtrmAlwaysReject gate{RxCategory::RxPositionLimit, "over the cap"};
  PreTradeOrder o{};
  auto d = evaluate(gate, PtrmMode::LogOnly, o);
  EXPECT_TRUE(d.allow);
  EXPECT_NE(d.detail.find("[log_only]"), std::string::npos);
}

TEST(Ptrm, ModeEnforceFollowsGate) {
  PtrmAlwaysReject gate{RxCategory::RxAccountSuspended, "halt"};
  PreTradeOrder o{};
  auto d = evaluate(gate, PtrmMode::Enforce, o);
  EXPECT_FALSE(d.allow);
  EXPECT_EQ(d.rx, RxCategory::RxAccountSuspended);
}

TEST(Ptrm, RxNamesMatchSpec) {
  EXPECT_STREQ("kill_switch",       rx_name(RxCategory::RxKillSwitch));
  EXPECT_STREQ("position_limit",    rx_name(RxCategory::RxPositionLimit));
  EXPECT_STREQ("price_collar",      rx_name(RxCategory::RxPriceCollar));
}

TEST(Ptrm, RestStubReportsUnsupportedInEnforceMode) {
  PtrmRestClient::Config cfg;
  cfg.mode = PtrmMode::Enforce;
  auto r = PtrmRestClient::create(cfg);
  ASSERT_TRUE(r);
  auto cli = std::move(r).value();
  PreTradeOrder o{};
  auto d = cli->check(o);
  EXPECT_FALSE(d.allow);
  EXPECT_EQ(d.rx, RxCategory::RxOther);
  EXPECT_NE(d.detail.find("stub"), std::string::npos);
}

TEST(Ptrm, RestStubAllowsInDisabledMode) {
  PtrmRestClient::Config cfg;
  cfg.mode = PtrmMode::Disabled;
  auto cli = std::move(PtrmRestClient::create(cfg)).value();
  PreTradeOrder o{};
  auto d = cli->check(o);
  EXPECT_TRUE(d.allow);
}
