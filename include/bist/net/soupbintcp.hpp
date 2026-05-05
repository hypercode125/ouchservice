#pragma once
//
// bist/net/soupbintcp.hpp — SoupBinTCP framing for OUCH.
//
// SoupBinTCP is the lower-layer protocol wrapping every OUCH inbound and
// outbound message. It provides:
//   - login/logout handshake with Username (6) + Password (10)
//   - per-session sequence numbering for outbound messages
//   - 1-second heartbeat in both directions
//   - end-of-session marker
//
// Wire layout for every packet:
//
//   ┌────────────────┬───────────────┬──────────────────────────┐
//   │ uint16 length  │ uint8 type    │ payload (length - 1 B)   │
//   │ (network order)│  (ASCII char) │                          │
//   └────────────────┴───────────────┴──────────────────────────┘
//
// "length" is the size of (type byte + payload).  The minimum legal length is
// 1 (a heartbeat packet). The session/sequence handshake fields are all
// fixed-width ASCII space-padded text per the SoupBinTCP spec.

#include <cstdint>
#include <cstring>

#include "bist/core/endian.hpp"

namespace bist::soup {

// --- Packet type bytes -------------------------------------------------------

namespace pkt {
inline constexpr char kClientHeartbeat   = 'R';   // C -> S
inline constexpr char kServerHeartbeat   = 'H';   // S -> C
inline constexpr char kLoginRequest      = 'L';   // C -> S
inline constexpr char kLoginAccepted     = 'A';   // S -> C
inline constexpr char kLoginRejected     = 'J';   // S -> C
inline constexpr char kSequencedData     = 'S';   // S -> C
inline constexpr char kUnsequencedData   = 'U';   // C -> S
inline constexpr char kLogoutRequest     = 'O';   // C -> S
inline constexpr char kEndOfSession      = 'Z';   // S -> C
inline constexpr char kDebug             = '+';
}  // namespace pkt

// --- Field widths ------------------------------------------------------------

inline constexpr std::size_t kUsernameLen   = 6;
inline constexpr std::size_t kPasswordLen   = 10;
inline constexpr std::size_t kSessionLen    = 10;
inline constexpr std::size_t kSequenceLen   = 20;

// --- Heartbeat cadence -------------------------------------------------------
//
// Both sides must send a heartbeat after 1 second of inactivity. We use
// a slightly tighter send-side budget and a more lenient receive-side
// dead-mans-switch.

inline constexpr std::uint32_t kHeartbeatSendNs    = 800'000'000;   // 800 ms
inline constexpr std::uint32_t kHeartbeatTimeoutNs = 2'500'000'000; // 2.5 s

#pragma pack(push, 1)

// --- Common 3-byte header ----------------------------------------------------

struct [[gnu::packed]] Header {
  be_u16 length;        // offset 0, length 2  (covers type + payload)
  char   packet_type;   // offset 2, length 1
};
static_assert(sizeof(Header) == 3,
              "SoupBinTCP header must be 3 bytes (length + type)");

// --- Login Request (C -> S) --------------------------------------------------

struct [[gnu::packed]] LoginRequest {
  Header header;
  char   username[kUsernameLen];
  char   password[kPasswordLen];
  char   requested_session[kSessionLen];
  char   requested_sequence_number[kSequenceLen];
};
static_assert(sizeof(LoginRequest) == 3 + 6 + 10 + 10 + 20,
              "LoginRequest must be 49 bytes per SoupBinTCP spec");

// --- Login Accepted (S -> C) -------------------------------------------------

struct [[gnu::packed]] LoginAccepted {
  Header header;
  char   session[kSessionLen];
  char   sequence_number[kSequenceLen];
};
static_assert(sizeof(LoginAccepted) == 3 + 10 + 20,
              "LoginAccepted must be 33 bytes per SoupBinTCP spec");

// --- Login Rejected (S -> C) -------------------------------------------------

struct [[gnu::packed]] LoginRejected {
  Header header;
  char   reject_reason_code;  // 'A' = invalid user/password, 'S' = session not available
};
static_assert(sizeof(LoginRejected) == 4);

// --- Logout Request (C -> S) -------------------------------------------------

struct [[gnu::packed]] LogoutRequest {
  Header header;
};
static_assert(sizeof(LogoutRequest) == 3);

// --- Heartbeats (zero-payload) -----------------------------------------------

struct [[gnu::packed]] Heartbeat {
  Header header;
};
static_assert(sizeof(Heartbeat) == 3);

// --- End of Session ----------------------------------------------------------

struct [[gnu::packed]] EndOfSession {
  Header header;
};
static_assert(sizeof(EndOfSession) == 3);

#pragma pack(pop)

// --- Header helpers ----------------------------------------------------------
//
// payload_len is the size in bytes of the OUCH (or other) message including
// its leading message-type byte. SoupBinTCP "length" = payload_len + 1
// (for the SoupBinTCP packet_type byte that precedes the payload).
//
// Wait — read carefully: SoupBinTCP "length" covers (packet_type + payload).
// So given an OUCH message of N bytes that we want to wrap in a Sequenced or
// Unsequenced Data packet, we set length = N + 1.

inline void make_header(Header& h, char packet_type, std::uint16_t inner_len) noexcept {
  h.length.set(static_cast<std::uint16_t>(inner_len + 1));
  h.packet_type = packet_type;
}

[[nodiscard]] inline std::uint16_t total_packet_size(const Header& h) noexcept {
  // Length field excludes the 2-byte length prefix itself.
  return static_cast<std::uint16_t>(h.length.get() + 2);
}

}  // namespace bist::soup
