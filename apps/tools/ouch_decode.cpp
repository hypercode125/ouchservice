// apps/tools/ouch_decode.cpp — render an OUCH hex dump in human-readable form.
//
// This tool consumes a single hex string (no separators) on stdin or a file
// argument and prints the decoded message to stdout. It is the companion to
// the audit log: every entry there is a hex dump that can be replayed
// through this binary for post-incident analysis.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "bist/ouch/codec.hpp"
#include "bist/ouch/messages.hpp"

namespace {

std::vector<std::uint8_t> hex_to_bytes(std::string_view hex) {
  std::vector<std::uint8_t> out;
  out.reserve(hex.size() / 2);
  std::uint8_t cur  = 0;
  bool         high = true;
  for (char c : hex) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    std::uint8_t nibble = 0;
    if (c >= '0' && c <= '9')      nibble = static_cast<std::uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f') nibble = static_cast<std::uint8_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') nibble = static_cast<std::uint8_t>(c - 'A' + 10);
    else {
      std::fprintf(stderr, "invalid hex character: '%c'\n", c);
      std::exit(EXIT_FAILURE);
    }
    if (high) {
      cur  = static_cast<std::uint8_t>(nibble << 4);
      high = false;
    } else {
      cur  = static_cast<std::uint8_t>(cur | nibble);
      out.push_back(cur);
      high = true;
    }
  }
  if (!high) {
    std::fprintf(stderr, "odd number of hex digits\n");
    std::exit(EXIT_FAILURE);
  }
  return out;
}

void describe(const std::vector<std::uint8_t>& bytes) {
  if (bytes.empty()) {
    std::printf("(empty)\n");
    return;
  }
  const char type = static_cast<char>(bytes[0]);
  std::printf("type='%c'  size=%zu bytes\n", type, bytes.size());

  // Cast against the appropriate struct based on the type byte. For now we
  // only print the message type and size; richer decoding will arrive in a
  // later phase together with a structured pretty-printer.
  switch (type) {
    case bist::ouch::msg_type::kEnterOrder:
      if (bytes.size() != sizeof(bist::ouch::EnterOrder)) {
        std::printf("  WARN: expected %zu bytes for EnterOrder\n",
                    sizeof(bist::ouch::EnterOrder));
      }
      break;
    case bist::ouch::msg_type::kOrderAccepted:
      if (bytes.size() != sizeof(bist::ouch::OrderAccepted)) {
        std::printf("  WARN: expected %zu bytes for OrderAccepted\n",
                    sizeof(bist::ouch::OrderAccepted));
      }
      break;
    default:
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string hex;
  if (argc == 2) {
    std::ifstream in(argv[1]);
    if (!in) {
      std::fprintf(stderr, "cannot open %s\n", argv[1]);
      return EXIT_FAILURE;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    hex = buf.str();
  } else {
    std::stringstream buf;
    buf << std::cin.rdbuf();
    hex = buf.str();
  }

  const auto bytes = hex_to_bytes(hex);
  describe(bytes);
  return EXIT_SUCCESS;
}
