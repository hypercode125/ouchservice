// tests/unit/test_token_encoding.cpp — YAML token → wire OrderToken round-trip.
//
// Cert directive: every order token reaches the wire as Alpha(14), left-
// justified, ASCII-space-padded. YAML tokens may arrive as ints (10) or
// strings ("10") and both must encode identically.

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <string>

#include <yaml-cpp/yaml.h>

#include "bist/core/types.hpp"
#include "bist/ouch/codec.hpp"
#include "bist/ouch/messages.hpp"
#include "bist/runner/scenario.hpp"

namespace {

bist::OrderToken from_yaml_scalar(const std::string& doc) {
  YAML::Node n = YAML::Load(doc);
  return bist::OrderToken{n.as<std::string>()};
}

}  // namespace

TEST(TokenEncoding, IntegerAndStringScalarsEncodeIdentically) {
  const auto t_int = from_yaml_scalar("10");
  const auto t_str = from_yaml_scalar("'10'");

  // Wire bytes are identical.
  EXPECT_EQ(0, std::memcmp(t_int.bytes().data(), t_str.bytes().data(),
                            bist::OrderToken::kSize));

  // Layout: '1' '0' followed by 12 spaces.
  EXPECT_EQ(t_int.bytes()[0], '1');
  EXPECT_EQ(t_int.bytes()[1], '0');
  for (std::size_t i = 2; i < bist::OrderToken::kSize; ++i) {
    EXPECT_EQ(t_int.bytes()[i], ' ') << "byte " << i << " not space-padded";
  }

  // Trimmed printable view drops the pad.
  EXPECT_EQ(t_int.view(), "10");
}

TEST(TokenEncoding, ScenarioLoaderStringifiesNumericTokens) {
  // Build a tiny in-memory YAML so we exercise the loader path that turns
  // numeric tokens into the wire representation via scalar().
  const std::string yaml =
      "name: token-encoding-roundtrip\n"
      "preconditions:\n"
      "  protocol: ouch\n"
      "steps:\n"
      "  - id: 1\n"
      "    action: place\n"
      "    args:\n"
      "      symbol: ACSEL.E\n"
      "      side: BUY\n"
      "      qty: 100\n"
      "      price: '6.20'\n"
      "      token: 10\n"
      "  - id: 2\n"
      "    action: place\n"
      "    args:\n"
      "      symbol: ACSEL.E\n"
      "      side: BUY\n"
      "      qty: 100\n"
      "      price: '6.20'\n"
      "      token: '10'\n";

  // Write to a temp file because load_scenario takes a path.
  const std::string path = "/tmp/bist_token_encoding_test.yaml";
  {
    std::ofstream f(path);
    f << yaml;
  }
  auto sc = bist::runner::load_scenario(path);
  ASSERT_EQ(sc.steps.size(), 2u);

  const auto& a1 = std::get<bist::runner::PlaceArgs>(sc.steps[0].args);
  const auto& a2 = std::get<bist::runner::PlaceArgs>(sc.steps[1].args);
  EXPECT_EQ(a1.token, "10");
  EXPECT_EQ(a2.token, "10");
}

TEST(TokenEncoding, OrderTokenTruncatesOversizeInput) {
  // Defensive: even if a future loader bug leaks a >14-char token through,
  // OrderToken truncates rather than overrunning the wire field.
  bist::OrderToken t{"123456789012345678"};  // 18 chars
  for (std::size_t i = 0; i < bist::OrderToken::kSize; ++i) {
    EXPECT_EQ(t.bytes()[i], "123456789012345678"[i]);
  }
  EXPECT_EQ(t.view().size(), 14u);
}

TEST(TokenEncoding, TokenSetCopiesIntoFixedAlpha) {
  // OUCH inbound EnterOrder uses a raw char[14]; ensure token_set populates
  // it correctly via the codec helper.
  bist::ouch::EnterOrder eo{};
  std::memset(&eo, 0, sizeof(eo));
  bist::OrderToken tok{"42"};
  bist::ouch::token_set(eo.order_token, tok);
  EXPECT_EQ(eo.order_token[0], '4');
  EXPECT_EQ(eo.order_token[1], '2');
  for (std::size_t i = 2; i < bist::ouch::kTokenLen; ++i) {
    EXPECT_EQ(eo.order_token[i], ' ');
  }
}
