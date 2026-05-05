# FIX 5.0 SP2 Integration — Status & Plan

Last updated: 2026-05-05

## Where we are

Phase 2.1 ships the design surface and a draft skeleton:

- `include/bist/fix/fields.hpp` — BIST custom field tags + AFK code constants (`PYM`/`PYP`/`XRM`), session status enum, party-role and party-id-source mappings, FIX `Side`/`OrdType`/`TimeInForce` value tables.
- `include/bist/fix/application.hpp` — `BistApp : FIX::Application + FIX::MessageCracker` with typed callbacks and the password-rotation hook for `554`/`925` on outbound Logon when the gateway returns SessionStatus = 8 (`PasswordExpired`).
- `include/bist/fix/oe_session.hpp` + `src/fix/oe_session.cpp` — `place` / `cancel` / `replace` / `trade_report` API in front of `FIX50SP2::NewOrderSingle` / `OrderCancelRequest` / `OrderCancelReplaceRequest` / `TradeCaptureReport`. AFK is stamped via the `NoPartyIDs` repeating group with `PartyRole=76` (DeskID).
- `include/bist/fix/rd_session.hpp` + `src/fix/rd_session.cpp` — `subscribe_all` issues `ApplicationMessageRequest (BW)` with `ApplReqType=3` (RequestAndSubscribe).
- `include/bist/fix/dc_listener.hpp` — Drop Copy listener wrapping `ExecutionReport`, `TradeCaptureReport`, `QuoteStatusReport` for the cert audit trail.
- `include/bist/fix/initiator.hpp` — `Initiator` owns a QuickFIX `SocketInitiator`, generates `session.cfg` on the fly from a `InitiatorConfig` struct.
- `scenarios/fix_oe_temel.yaml` — 26-step FIX OE Temel cert as machine-replayable steps (mock_skip until 2.2).
- `scenarios/fix_rd_temel.yaml` — FIX RD Temel cert: Logon, AMR subscribe, duplicate AMR reject (mock_skip until 2.2).

## Why these aren't built by default

QuickFIX/C++ 1.15.1 (the latest tagged release as of writing) carries three legacy items that conflict with the rest of the project:

1. **`std::auto_ptr`** in `quickfix/DOMDocument.h` and `quickfix/Message.h`. The template was removed from libc++ in C++17.
2. **C++03-era dynamic exception specifications** (`throw(DoNotSend)`, `throw(FieldNotFound, …)`) on the virtual `toApp` / `fromApp` / `fromAdmin` overrides. C++17 removed dynamic exception specifications entirely.
3. **`CMAKE_SOURCE_DIR`-based path resolution** in `cmake/QuickfixPlatformSettings.cmake` — when QuickFIX is included via `FetchContent_MakeAvailable`, that variable points at the consumer (us) instead of QuickFIX itself, so its auto-generated `Allocator.h` and `config.h` end up in the wrong place. We work around (3) at configure time by pre-generating both files at the right path and pre-creating the `include/quickfix` symlink the build expects (see top-level `CMakeLists.txt`).

(1) and (2) cannot both be satisfied on the same translation unit: `auto_ptr` requires C++14, dynamic-exception-spec is rejected by C++17+. The rest of the project is C++20.

## Workaround design (Phase 2.2)

The fix is a Pimpl facade: bist::fix exposes only POD types in its public headers, and the QuickFIX-touching code lives entirely in `.cpp` files compiled with `-std=gnu++14`.

```
include/bist/fix/facade.hpp        # POD args + opaque pimpl, C++20-clean
src/fix/facade.cpp                 # implements the facade in C++14
src/fix/oe_session.cpp             # QuickFIX glue, C++14
src/fix/rd_session.cpp             # ditto
src/fix/dc_listener.cpp            # ditto
```

The facade exposes:

```cpp
namespace bist::fix {
struct PlaceArgs { /* POD */ };
struct ExecutionReportEvent { /* POD */ };

class OeClient {
 public:
  static std::unique_ptr<OeClient> create(const InitiatorConfig& cfg,
                                          OnEvent on_event);
  Result<std::string> place(const PlaceArgs& a);
  // …
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;   // QuickFIX lives here
};
}
```

Consumer (`apps/bist_colo.cpp`, `runner/replay.hpp`) only sees `OeClient` + POD events; the QuickFIX headers never enter a C++20 TU.

## Alternative routes considered

| Option | Status | Reason rejected |
|--------|--------|-----------------|
| Compile bist_fix at C++14 only | Tried | bist_fix consumes `bist_core`, which uses `std::variant` (C++17) and `std::is_nothrow_move_constructible_v` (C++17). Either we duplicate Result for FIX or we Pimpl. |
| Compile bist_fix at gnu++17 with `-Wno-dynamic-exception-spec` | Tried | Apple Clang 16 still hard-errors on `auto_ptr` removal in C++17 mode regardless of GNU extensions. |
| Use `find_package(QuickFix)` against a system-installed package | Possible on Ubuntu (`libquickfix-dev`) but Homebrew has no formula | Pinned dependency surface diverges from the FetchContent story for the rest of the project. |
| Replace QuickFIX with a modern FIX library (HFFIX, FixT, custom) | Open option | HFFIX is header-only and modern but lower level — would need a thin session FSM on top. |
| Patch QuickFIX upstream | Open option | Out-of-scope for this project; pin to a fork after Phase 2.2 if needed. |

## Phase 2.2 work plan

1. Introduce `include/bist/fix/facade.hpp` with POD `PlaceArgs`, `CancelArgs`, `ReplaceArgs`, `TradeReportArgs`, `ExecutionReportEvent`, `TradeReportEvent`, `QuoteStatusEvent`, `LogonResult`, `OnEvent`.
2. Move the current `oe_session.hpp`, `rd_session.hpp`, `dc_listener.hpp`, `initiator.hpp` definitions into `src/fix/internal/` (compiled-only, no install).
3. Rewrite the `.cpp` files to translate between POD args/events and QuickFIX types entirely inside C++14 TUs.
4. Wire `OeClient::create`, `RdClient::create`, `DcClient::create` into `apps/bist_colo.cpp` behind `BIST_HAS_QUICKFIX`.
5. Ship a minimal in-process QuickFIX `Acceptor`-based mock so `--mock --replay scenarios/fix_*.yaml` runs end-to-end.
6. Extend the YAML runner with `fix_logon`, `fix_amr_subscribe`, `fix_amr_ack`, `fix_execution_report`, `fix_logout_complete` actions and matchers.
7. Drop `mock_skip: true` from `fix_oe_temel.yaml` and `fix_rd_temel.yaml`.

## Build flags

| Flag | Default | Effect |
|------|---------|--------|
| `BIST_ENABLE_QUICKFIX` | OFF | Pulls QuickFIX 1.15.1 via FetchContent and applies the configure-time bridges. |
| `BIST_BUILD_FIX` | OFF | Compiles `src/fix/`. Phase 2.2 only — current skeleton needs the Pimpl facade first. |

CI keeps both off so the OUCH cert path stays green; switch to `ON` only when running the FIX leg locally during 2.2 development.
