// tests/unit/test_scenario_lint.cpp — exercise scenario_lint as a subprocess.
//
// The lint binary returns 0 on clean and non-zero on issues. We synthesize
// minimal YAMLs in /tmp and run the binary against them.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

// Path to the binary, set by CMake via BIST_SCENARIO_LINT_BIN compile def.
#ifndef BIST_SCENARIO_LINT_BIN
#  error "BIST_SCENARIO_LINT_BIN must be defined"
#endif

constexpr const char* kLintBin = BIST_SCENARIO_LINT_BIN;

int run_lint(const std::string& path) {
  std::string cmd = std::string(kLintBin) + " " + path + " >/dev/null 2>&1";
  return std::system(cmd.c_str());
}

void write(const std::string& path, const std::string& body) {
  std::ofstream f(path);
  f << body;
}

}  // namespace

TEST(ScenarioLint, AcceptsMinimalOuchScenario) {
  const std::string p = "/tmp/bist_lint_ok.yaml";
  write(p,
        "name: ok\n"
        "preconditions:\n"
        "  protocol: ouch\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: ouch_login\n"
        "  - id: 2\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', token: 100 }\n"
        "    expect: order_accepted\n");
  EXPECT_EQ(0, run_lint(p));
}

TEST(ScenarioLint, RejectsMissingProtocol) {
  const std::string p = "/tmp/bist_lint_no_proto.yaml";
  write(p,
        "name: missing-proto\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: ouch_login\n");
  EXPECT_NE(0, run_lint(p));
}

TEST(ScenarioLint, RejectsUnknownProtocol) {
  const std::string p = "/tmp/bist_lint_bad_proto.yaml";
  write(p,
        "name: bad-proto\n"
        "preconditions:\n"
        "  protocol: ix_oe\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: fix_logon\n");
  EXPECT_NE(0, run_lint(p));
}

TEST(ScenarioLint, RejectsOversizeToken) {
  const std::string p = "/tmp/bist_lint_long_token.yaml";
  write(p,
        "name: long-token\n"
        "preconditions:\n"
        "  protocol: ouch\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', "
        "            token: ABCDEFGHIJKLMNO }\n");  // 15 chars
  EXPECT_NE(0, run_lint(p));
}

TEST(ScenarioLint, RejectsDuplicatePlaceTokenWithoutRejectExpectation) {
  const std::string p = "/tmp/bist_lint_dup_token.yaml";
  write(p,
        "name: dup-token\n"
        "preconditions:\n"
        "  protocol: ouch\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', token: 10 }\n"
        "    expect: order_accepted\n"
        "  - id: 2\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', token: 10 }\n"
        "    expect: order_accepted\n");
  EXPECT_NE(0, run_lint(p));
}

TEST(ScenarioLint, AcceptsDuplicatePlaceTokenIfRejectExpected) {
  const std::string p = "/tmp/bist_lint_dup_token_ok.yaml";
  write(p,
        "name: dup-token-rej\n"
        "preconditions:\n"
        "  protocol: ouch\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', token: 10 }\n"
        "    expect: order_accepted\n"
        "  - id: 2\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', token: 10 }\n"
        "    expect:\n"
        "      msg: order_rejected\n"
        "      fields: { reject_code: '-800002' }\n");
  EXPECT_EQ(0, run_lint(p));
}

TEST(ScenarioLint, RejectsUnknownExpectField) {
  const std::string p = "/tmp/bist_lint_bad_field.yaml";
  write(p,
        "name: bad-field\n"
        "preconditions:\n"
        "  protocol: ouch\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', token: 10 }\n"
        "    expect:\n"
        "      msg: order_accepted\n"
        "      fields: { not_a_real_key: x }\n");
  EXPECT_NE(0, run_lint(p));
}

TEST(ScenarioLint, RejectsDuplicateStepId) {
  const std::string p = "/tmp/bist_lint_dup_id.yaml";
  write(p,
        "name: dup-id\n"
        "preconditions:\n"
        "  protocol: ouch\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: ouch_login\n"
        "  - id: 1\n"
        "    action: ouch_logout\n");
  EXPECT_NE(0, run_lint(p));
}

TEST(ScenarioLint, AcceptsCancelByTokenReferencingPriorPlace) {
  const std::string p = "/tmp/bist_lint_cancel_ref.yaml";
  write(p,
        "name: cancel-ref\n"
        "preconditions:\n"
        "  protocol: ouch\n"
        "steps:\n"
        "  - id: 1\n"
        "    action: place\n"
        "    args: { symbol: ACSEL.E, side: BUY, qty: 1, price: '6.0', token: 20 }\n"
        "  - id: 2\n"
        "    action: cancel_by_token\n"
        "    args: { token: 20 }\n");
  EXPECT_EQ(0, run_lint(p));
}
