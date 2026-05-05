#pragma once
//
// bist/runner/transport.hpp — abstract transport control hook for the runner.
//
// A scenario step like "logout, then re-login with a different password"
// requires the runner to drop the underlying TCP connection, dial a new
// one, and replay the SoupBinTCP login handshake. The OUCH session itself
// has no concept of reconnecting — it's a one-shot FSM tied to a single
// socket. We isolate that capability behind an interface so that:
//
//   - mock scenarios (single embedded gateway, no networking) just run
//     without supplying any transport control;
//   - live mode supplies a `LiveTransportControl` that owns the socket
//     and rebuilds the OuchSession/OuchClient pair against a fresh TCP
//     connection.
//
// The runner calls into the interface from the `ouch_login` step when the
// session is not already in `Active` state. Empty / zero override fields
// mean "keep the base LoginParams value".

#include <cstdint>
#include <string_view>

#include "bist/core/result.hpp"

namespace bist::runner {

class ITransportControl {
 public:
  virtual ~ITransportControl() = default;

  // Tear down the current TCP socket, dial a fresh one (with retry/backoff),
  // and re-execute the SoupBinTCP login handshake against it.
  //
  // The runner's `OuchSession&` reference must remain valid after this call
  // returns; implementations are expected to rebuild the session in stable
  // storage (e.g. `std::optional` placement) so the address doesn't move.
  //
  // Returns ok() iff the new session reaches `Active`. Login rejections
  // (wrong password, sequence resume failure) come back as a Reject error
  // with `code` set to the SoupBinTCP reject reason.
  virtual Result<void> reconnect_and_login(std::string_view override_password,
                                           std::string_view override_session,
                                           std::uint64_t    override_sequence) = 0;

  // Drop the TCP connection without sending SoupBinTCP Logout — used to
  // simulate ungraceful disconnects (e.g. cancel-on-disconnect tests).
  virtual Result<void> hard_disconnect() = 0;

  // Switch the active target between the primary and secondary OUCH gateway
  // and re-execute the SoupBinTCP login handshake against the new endpoint.
  // Used by the cert "Birincil/Yedek Gateway Geçişi" drill — the cert calls
  // for the sequence number to NOT reset, so callers typically pass the
  // current `next_inbound_seq` (and the live session id) as overrides.
  virtual Result<void> switch_to_secondary(bool secondary,
                                           std::string_view override_session,
                                           std::uint64_t    override_sequence) = 0;
};

}  // namespace bist::runner
