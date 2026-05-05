#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "bist/ouch/codec.hpp"
#include "bist/runner/fix_replay.hpp"
#include "bist/runner/replay.hpp"

namespace {

TEST(ReplayMatching, OuchRejectChecksTokenAndRejectCode) {
  bist::ouch::OrderRejected msg{};
  std::memset(&msg, 0, sizeof(msg));
  msg.message_type = bist::ouch::msg_type::kOrderRejected;
  bist::ouch::token_set(msg.order_token, bist::OrderToken{"70"});
  msg.reject_code.set(-420131);

  bist::runner::Expect exp{};
  exp.msg = "order_rejected";
  exp.fields["token"] = "70";
  exp.fields["reject_code"] = "-420131";

  std::string detail;
  EXPECT_TRUE(bist::runner::ouch_event_matches(exp, bist::runner::SessionEvent{msg}, detail))
      << detail;

  exp.fields["reject_code"] = "-1";
  detail.clear();
  EXPECT_FALSE(bist::runner::ouch_event_matches(exp, bist::runner::SessionEvent{msg}, detail));
  EXPECT_NE(detail.find("reject_code"), std::string::npos) << detail;
}

TEST(ReplayMatching, OuchUnknownExpectedFieldFails) {
  bist::ouch::OrderAccepted msg{};
  std::memset(&msg, 0, sizeof(msg));
  msg.message_type = bist::ouch::msg_type::kOrderAccepted;

  bist::runner::Expect exp{};
  exp.msg = "order_accepted";
  exp.fields["not_a_real_field"] = "x";

  std::string detail;
  EXPECT_FALSE(bist::runner::ouch_event_matches(exp, bist::runner::SessionEvent{msg}, detail));
  EXPECT_NE(detail.find("not_a_real_field"), std::string::npos) << detail;
}

TEST(ReplayMatching, OuchMassQuoteAckFieldsUseCorrectStructMembers) {
  bist::ouch::MassQuoteAck msg{};
  std::memset(&msg, 0, sizeof(msg));
  msg.message_type = bist::ouch::msg_type::kMassQuoteAck;
  bist::ouch::token_set(msg.order_token, bist::OrderToken{"MQ1"});
  msg.order_book_id.set(70616);
  msg.quantity.set(500);
  msg.traded_quantity.set(12);
  msg.price.set(5110);
  msg.side = 'S';
  msg.quote_status.set(2);

  bist::runner::Expect exp{};
  exp.msg = "mass_quote_ack";
  exp.fields["token"] = "MQ1";
  exp.fields["order_book_id"] = "70616";
  exp.fields["quantity"] = "500";
  exp.fields["traded_quantity"] = "12";
  exp.fields["price"] = "5110";
  exp.fields["side"] = "S";
  exp.fields["quote_status"] = "2";

  std::string detail;
  EXPECT_TRUE(bist::runner::ouch_event_matches(exp, bist::runner::SessionEvent{msg}, detail))
      << detail;
}

TEST(ReplayMatching, FixLogonChecksSessionStatus) {
  bist::fix::LogonResult msg{};
  msg.state = bist::fix::FixSessionState::PasswordChanged;
  msg.session_status = 1;
  msg.detail = "password changed";

  bist::runner::Expect exp{};
  exp.msg = "fix_logon_response";
  exp.fields["session_status"] = "1";

  std::string detail;
  EXPECT_TRUE(bist::runner::fix_event_matches(exp, bist::runner::FixEvent{msg}, detail))
      << detail;

  exp.fields["session_status"] = "0";
  detail.clear();
  EXPECT_FALSE(bist::runner::fix_event_matches(exp, bist::runner::FixEvent{msg}, detail));
  EXPECT_NE(detail.find("session_status"), std::string::npos) << detail;
}

TEST(ReplayMatching, FixExecutionReportChecksCoreFields) {
  bist::fix::ExecutionReportEvent msg{};
  msg.cl_ord_id = "100";
  msg.order_id = "BIST-1";
  msg.symbol = "GARAN.E";
  msg.side = bist::Side::Buy;
  msg.exec_type = '0';
  msg.ord_status = '0';
  msg.leaves_qty = 200;
  msg.cum_qty = 0;
  msg.last_qty = 0;
  msg.last_price = 6200;

  bist::runner::Expect exp{};
  exp.msg = "fix_execution_report";
  exp.fields["ClOrdID"] = "100";
  exp.fields["OrderID"] = "BIST-1";
  exp.fields["Symbol"] = "GARAN.E";
  exp.fields["Side"] = "B";
  exp.fields["ExecType"] = "0";
  exp.fields["OrdStatus"] = "0";
  exp.fields["LeavesQty"] = "200";
  exp.fields["CumQty"] = "0";
  exp.fields["LastPx"] = "6200";

  std::string detail;
  EXPECT_TRUE(bist::runner::fix_event_matches(exp, bist::runner::FixEvent{msg}, detail))
      << detail;
}

}  // namespace
