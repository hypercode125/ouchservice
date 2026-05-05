# BIST 10/10 Codebase and Certification Readiness Plan

Date: 2026-05-05 (revised 21:30 GMT+3)
Project: `bist-colo-client`
Target: BIST Pay Piyasasi co-location client, OUCH + FIX OE/RD certification readiness

## Revision Note

This document was originally generated against an older snapshot where wire-level
correctness gates were open. As of revision time:

- B1 (`MassQuoteAck` Feb 2025 layout) — **DONE**, verified by `OuchMessages.MassQuoteAckUsesFeb2025Offsets`.
- B3 (BIST 10x100 ms rolling-window throttler) — **DONE**, verified by `Throttler.BistDocumentExampleRejectsResidualSecondBurst`.
- B4 (resident live mode + signal-driven graceful shutdown) — **DONE**, `apps/bist_colo.cpp` polls in 5 ms cycles until `g_stop_requested`.
- B6 (NDJSON-style audit log with daily rotation, sent + recv handlers, audit dir auto-create) — **DONE**, `tests/unit/test_audit_log.cpp` green.
- B2 (strict OUCH + FIX field assertions, unknown-field error path) — **DONE**, 5 ReplayMatching tests green (#58–62).
- C1/C3 (`OrderAccepted` 137B + SMP, `EnterOrder` SMP rename) — **DONE**, byte-level tests green.

Remaining open work is enumerated in the new "Real remaining gaps" section below;
the legacy B/H/P sections are kept as a verification checklist.

## Executive Summary

Current state (revised):
- Codebase technical foundation: **8/10**
- Certification readiness: **6/10** (mock-only; live cert is gated by C5 + B5-live + Phase 2.4)
- Safe answer for production/certification today: **Do not enter BIST certification until C5 (CoD reconnect deadline), B5-live (live FIX dispatch), and Phase 2.4 (FIX OE multi-logon) close. Mock cert dry-run can be claimed for OUCH Bölüm 2 + 3 today.**

Target state:
- Codebase technical foundation: **10/10**
- Certification readiness: **10/10**
- Safe answer after completion: **The system can be used as the primary BIST certification runner and as a controlled live co-location client, assuming BIST test credentials, network access, and member-specific parameters are correct.**

The difference between 6/10 and 10/10 is not "more features". The missing pieces are correctness gates: current-spec wire layout, strict scenario assertions, BIST-accurate throttling, live resident behavior, auditability, FIX readiness, full scenario coverage, and repeatable operational runbooks.

## Source of Truth

Use these documents as mandatory sources before changing protocol behavior:

- `ouchpdfs/txt/BISTECH OUCH Protocol Specification.txt`
- `ouchpdfs/txt/Equity Market OUCH Certification Program v5.0.txt`
- `ouchpdfs/txt/bistech-ouch-sertifikasyon-programi.txt`
- `ouchpdfs/txt/The Concept of Throttling FIX & OUCH API.txt`
- `ouchpdfs/txt/CancelOnDisconnect Annotation.txt`
- `ouchpdfs/txt/Derivatives Market OUCH Certification Program.txt`
- BIST official technical documents page: `https://www.borsaistanbul.com/en/technical-resources/technical-documents`

Protocol comments in the code must no longer say the active baseline is only "OUCH R2.11 / 2020" unless the file is explicitly describing legacy behavior. The current implementation must be aligned to the February 2025 OUCH specification and the 2024/2026 certification programs stored under `ouchpdfs/`.

## What 10/10 Means

### Codebase 10/10

The project reaches 10/10 technical quality when all of these are true:

- Every binary wire struct has static size checks, offset checks, encode/decode round-trip tests, and at least one golden test derived from the BIST PDF tables.
- Scenario replay fails when any asserted field is wrong, not only when the message type is wrong.
- Mock gateway behavior is spec-accurate enough to catch wire layout regressions instead of mirroring incorrect local structs.
- Throttling matches BIST's 10 x 100 ms rolling-window model, not an approximate token-bucket model.
- Live mode is a resident process with explicit lifecycle, shutdown, reconnect, and Cancel-on-Disconnect behavior.
- Sent and received audit logs are append-only, timestamped, durable, and readable by a certification reviewer.
- FIX OE/RD/Drop Copy are either fully implemented and enabled in a cert build, or clearly excluded from the claimed certification scope.
- Configuration separates public defaults, member-specific settings, and secrets. A production-like config validates all required fields before connecting.
- Unit, integration, mock replay, and cert-preflight commands are documented and run cleanly.
- CI has Release, Debug, sanitizer, and FIX-enabled build lanes.
- There is an operator runbook for cert day and production day.

### Certification Readiness 10/10

The system reaches 10/10 certification readiness when all of these are true:

- OUCH Basic scenarios are complete and strict.
- OUCH Market Maker scenarios are complete and strict.
- Throttling, gateway failover, EOD batch cancellation, and Cancel-on-Disconnect behavior are tested.
- FIX OE Basic scenarios run through the FIX runner in mock and live mode.
- FIX RD Basic scenarios run through the FIX runner in mock and live mode.
- Drop Copy expectations are asserted for execution reports, trade capture reports, and quote status reports where the certification program expects them.
- Every scenario file has `protocol`, `section`, `step_id`, `expected`, and audit labels that map directly to the BIST certification document.
- A full dry run can produce a certification package: config snapshot without secrets, scenario report, audit logs, binary version, git commit, and environment report.
- The binary stays connected during certification steps and does not reconnect between scenario steps unless the BIST scenario explicitly requires failover/reconnect.

## Real remaining gaps (post-revision)

The blockers below replace the original "must-fix" list. Each item names a file,
an acceptance test, and a verification command.

### R1. CoD-aware reconnect deadline (was C5)
File: new `include/bist/app/reconnect_policy.hpp`, wire into `apps/bist_colo.cpp` `LiveTransportControl`.
Acceptance:
- `connect_with_retry(...)` accepts a `deadline_ns` parameter; default 30 s, configurable from `[ouch].cod_reconnect_deadline_ms` in TOML.
- When the deadline elapses without a successful logon, the function returns an error tagged `ErrorCategory::Timeout` and stamps an audit record `category=cod_risk` so the operator log shows the order book may have entered the BIST 55–62 s CoD inactivation window.
- New unit test `tests/unit/test_reconnect_policy.cpp` proves: (a) success short-circuits, (b) deadline expiry returns Timeout, (c) deadline applies across multiple backoff attempts.

### R2. Live FIX scenario dispatch (was B5)
File: `apps/bist_colo.cpp:822` currently fails live FIX scenarios with `"live FIX scenario dispatch is not wired in this build path yet"`.
Acceptance:
- `run_live` routes `protocol: fix_oe` and `fix_rd` scenarios into `bist::runner::FixScenarioRunner` with an initiator built from `[fix.oe]` / `[fix.rd]` in TOML.
- When `BIST_BUILD_FIX=OFF` the failure is detected at scenario load time (`scenario_lint`) so we never reach the network with the wrong build.
- `config/bist_prod_like.local.toml.example` includes the required `[fix.oe]` and `[fix.rd]` blocks (host, port, sender_comp_id, target_comp_id, username, password, password_change, heartbeat).

### R3. FIX OE multi-logon rebuild (Phase 2.4)
File: `apps/bist_colo.cpp run_one_fix_scenario`, `include/bist/runner/fix_replay.hpp`, `src/fix/internal/initiator.hpp`.
Acceptance:
- `scenarios/fix_oe_temel.yaml` steps 8 onwards (logout → relogon → ResendRequest) drive end-to-end against `AcceptorMock` without hanging.
- The runner rebuilds `OeClient` (and therefore the QuickFIX `SocketInitiator`) on every `fix_logon` step that follows a `fix_logout`.
- `--mock --replay scenarios/fix_oe_temel.yaml` reports 26/26 PASS (current 6/26).

### R4. FIX RD duplicate AMR rejection
File: `src/fix/acceptor_mock.cpp` (and its internal `internal/acceptor_mock.hpp`).
Acceptance:
- A second `ApplicationMessageRequest` with the same `ApplReqID` produces an `ApplicationMessageRequestAck` with `ApplResponseType=2` and a non-empty `Text` reason.
- `scenarios/fix_rd_temel.yaml` step 4 expectation (`fix_amr_ack` with `ok=false`) succeeds; current run times out.

### R5. CancelReason enum + replay matcher field (was H1)
File: `include/bist/core/types.hpp`, `include/bist/runner/replay.hpp`.
Acceptance:
- `enum class CancelReason : std::uint8_t { CanceledByUser=1, Trade=3, Inactivate=4, ReplacedByUser=5, New=6, ConvertedBySystem=8, CanceledBySystem=9, CodInactivate=10, MarketHalt=11 }` per BISTECH Feb 2025 Table 10.
- Replay runner exposes both `reason` (numeric) and `cancel_reason_name` (enum string), both assertable in scenario YAML.
- A scenario asserting `cancel_reason_name: cod_inactivate` PASSes when the canceled message carries reason byte 10.

### R6. OpenClose split (was H2)
File: `include/bist/core/types.hpp`, `include/bist/ouch/client.hpp`.
Acceptance:
- `OpenClose::ReplaceDefault` is renamed `OpenClose::ReplaceKeep` and has a Doxygen comment naming the spec section.
- `OuchClient::replace` rejects callers passing `Open` or `CloseNet` because those values are illegal in Replace per Spec 4.1.2.2.

### R7. Token encoding tests + lint enforcement (was H4 + part of H6)
File: new `tests/unit/test_token_encoding.cpp`, `apps/tools/scenario_lint.cpp`.
Acceptance:
- Round-trip test: YAML scalar `10` (int) and YAML scalar `"10"` (string) both encode to `'1' '0' ' '*12` on the wire.
- Tokens with size > 14 are rejected by `scenario_lint`.
- Duplicate tokens within a scenario are flagged unless the offending step has `expect: order_rejected`.

### R8. Scenario lint extensions (was H6)
File: `apps/tools/scenario_lint.cpp`, `tests/unit/test_scenario_lint.cpp` (new).
Acceptance:
- Lint requires `preconditions.protocol` to be one of `ouch | fix_oe | fix_rd | drop_copy`.
- Step ids must be unique and monotonically increasing.
- `expect.fields` keys are validated against the runner's known set per protocol; unknown keys fail.
- FIX scenarios must use `fix_*` actions; OUCH scenarios must use bare action names.

### R9. New cert YAMLs (was H3)
Files:
- `scenarios/ouch_bolum2_surekli.yaml` — continuous-auction GARAN/KAREL/VAKKO replace + leaves recalc, three variants (price-only, qty-only, both).
- `scenarios/ouch_bolum2_eod_batch.yaml` — EOD inactivate batch via new `inactivate_all` mock action.
- `scenarios/ouch_vadeli_temel.yaml` — Derivatives v1.8 Cert Program 22 steps with `position` arg (Open/Close/Default).
Acceptance:
- All three pass under `--mock --replay scenarios/`.
- `runner/scenario.hpp` has a typed `position` field with default 0 (Pay) and 0/1/2 (Derivs).
- `scenario_lint` recognises every new action.

### R10. Cert package generator (was P3)
File: new `apps/tools/audit_pack.cpp` or `scripts/audit_pack.sh`.
Acceptance:
- `audit_pack <run-dir>` emits a directory with: binary version, git commit + dirty flag, build flags, scenario PASS/FAIL summary, raw NDJSON audit log, decoded summary CSV, redacted config snapshot (passwords stripped), environment report (uname, glibc, openssl version), operator notes.
- The package is reproducible from the audit log alone if the binary is rerun.
- A new docs/RUNBOOK.md describes how to ship the package to BIST after a cert dry-run.

---

## Legacy non-negotiable blockers (kept as verification checklist)

These items remain in the document as the audit checklist used to *prove* the
work above is closed. Run the verification commands listed here against the
current build before claiming any of them done.

### B1. Fix `MassQuoteAck` Wire Layout

Severity: P0

Files:
- `include/bist/ouch/messages.hpp`
- `src/mock/ouch_gateway.cpp`
- `tests/unit/test_messages.cpp`
- Add or extend: `tests/unit/test_mock_gateway.cpp`

Required behavior:

The current `MassQuoteAck` layout places `side` and `quote_status` before quantity fields. The 2025 OUCH spec places fields in this order:

```text
message_type        offset 0   length 1
timestamp_ns        offset 1   length 8
order_token         offset 9   length 14
order_book_id       offset 23  length 4
quantity            offset 27  length 8
traded_quantity     offset 35  length 8
price               offset 43  length 4
side                offset 47  length 1
quote_status        offset 48  length 4
total                       52
```

Acceptance criteria:

- `static_assert(sizeof(MassQuoteAck) == 52)` remains true.
- Offset asserts validate every field above.
- Golden byte test proves `quantity`, `traded_quantity`, `price`, `side`, and `quote_status` decode from the exact BIST offsets.
- Mock gateway emits bytes using the corrected layout.
- Market Maker scenario fails before the fix and passes after the fix.

Verification commands:

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure -R 'test_messages|mock|scenario'
./build/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/ouch_bolum3_piyasa_yapicilik.yaml
```

### B2. Implement Strict Scenario Field Assertions

Severity: P0

Files:
- `include/bist/runner/replay.hpp`
- `include/bist/runner/fix_replay.hpp`
- `include/bist/runner/scenario.hpp`
- `src/runner/scenario_loader.cpp`
- Add or extend: `tests/unit/test_replay_runner.cpp`
- Add or extend: `tests/unit/test_fix_replay_runner.cpp`

Required behavior:

The runner must compare all expected fields in scenario YAML files. It is not enough to match only the message type.

Minimum fields to support for OUCH:

- `token`
- `order_book_id`
- `side`
- `quantity`
- `leaves_quantity`
- `traded_quantity`
- `price`
- `reject_code`
- `reason`
- `order_state`
- `quote_status`
- `smp_level`
- `smp_method`
- `smp_id`

Minimum fields to support for FIX:

- `MsgType`
- `ClOrdID`
- `OrigClOrdID`
- `OrderID`
- `ExecType`
- `OrdStatus`
- `OrdRejReason`
- `CxlRejReason`
- `Symbol`
- `Side`
- `OrderQty`
- `LeavesQty`
- `CumQty`
- `Price`
- `SessionStatus`
- `ApplReqID`
- `ApplResponseType`

Acceptance criteria:

- A scenario with the correct message type and wrong `reject_code` fails.
- A scenario with the correct message type and wrong `token` fails.
- A scenario with an unknown expected field fails with a clear error.
- The failure report prints expected value, actual value, scenario name, section, and step id.
- Both OUCH and FIX runners use the same comparison policy where practical.

Verification commands:

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure -R 'replay|scenario_loader'
./build/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/
```

### B3. Replace Token Bucket Throttler With BIST Rolling Window

Severity: P0

Files:
- `include/bist/domain/throttler.hpp`
- `tests/unit/test_throttler.cpp`
- Add: `tests/unit/test_throttler_bist_examples.cpp`

Required behavior:

BIST throttling is based on ten 100 ms windows. The effective order count is the sum of the last ten windows. A new request is accepted only when that sum is below the configured limit.

Acceptance criteria:

- Public API remains simple: `try_acquire(now)`, `available(now)`, `capacity()`.
- Unit tests reproduce the BIST document example where a token bucket would incorrectly accept a second burst.
- Boundary tests cover:
  - exactly 100 orders in one second
  - 101st order in the same rolling second
  - orders crossing a 100 ms boundary
  - inactivity that clears old windows
  - monotonic timestamp jumps
- Production config exposes the throttle rate and window size explicitly.

Verification commands:

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure -R throttler
./build/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/ouch_bolum2_throttling.yaml
```

### B4. Make Live Mode Resident

Severity: P1

Files:
- `apps/bist_colo.cpp`
- Add: `include/bist/app/live_runtime.hpp`
- Add: `tests/unit/test_live_runtime.cpp`

Required behavior:

When the binary starts in live mode without `--replay`, it must not log out after two seconds. It must run as a resident process until an explicit shutdown signal is received.

Expected runtime modes:

- `--mock --replay`: batch scenario replay against mock gateway.
- `--replay`: batch scenario replay against live BIST environment.
- `--interactive`: connected live command loop, only if interactive mode is implemented.
- no `--replay` and no `--interactive`: resident live service loop.

Acceptance criteria:

- `SIGINT` and `SIGTERM` trigger graceful logout and audit flush.
- Resident mode keeps heartbeats active.
- Resident mode never exits successfully just because there was no replay file.
- Interactive mode is either implemented or rejected at startup with a non-zero exit and a clear message.

Verification commands:

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure -R live_runtime
timeout 10s ./build/apps/bist_colo --config config/bist_prod_like.toml --mock
```

The `timeout` command should terminate the process. The process should not exit by itself before the timeout.

### B5. Route Live FIX Scenarios Through FIX Runner

Severity: P1

Files:
- `apps/bist_colo.cpp`
- `include/bist/runner/fix_replay.hpp`
- `src/fix/facade_stub.cpp`
- `src/fix/CMakeLists.txt`
- `config/bist_prod_like.toml`
- `config/bist_prod_like.local.toml.example`
- Add or extend: `tests/unit/test_live_runtime.cpp`

Required behavior:

Mock mode already checks `protocol`. Live mode must do the same.

Routing rules:

- `protocol: ouch` -> OUCH `ScenarioRunner`
- `protocol: fix_oe` -> FIX OE runner
- `protocol: fix_rd` -> FIX RD runner
- missing protocol -> fail during scenario lint, except for legacy OUCH scenarios explicitly migrated by loader compatibility

Acceptance criteria:

- Live replay of `fix_oe_temel.yaml` does not enter the OUCH runner.
- Live replay of `fix_rd_temel.yaml` does not enter the OUCH runner.
- If `BIST_BUILD_FIX=OFF`, FIX scenario replay fails clearly before network connection.
- `config/bist_prod_like.local.toml.example` includes all required FIX ports and CompIDs, not only passwords.

Verification commands:

```bash
cmake -S . -B build-fix -DCMAKE_BUILD_TYPE=Release -DBIST_BUILD_FIX=ON -DBIST_ENABLE_QUICKFIX=ON
cmake --build build-fix -j 4
ctest --test-dir build-fix --output-on-failure -R 'fix|live_runtime'
./build-fix/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/fix_oe_temel.yaml
./build-fix/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/fix_rd_temel.yaml
```

### B6. Wire Append-Only Audit Trail

Severity: P1

Files:
- `apps/bist_colo.cpp`
- `include/bist/observability/audit.hpp`
- Add or extend: `tests/unit/test_audit_log.cpp`

Required behavior:

Every sent and received OUCH/FIX frame must be recorded with:

- monotonic timestamp
- wall-clock timestamp
- direction: sent or received
- protocol: soup, ouch, fix_oe, fix_rd, drop_copy
- session id
- sequence number where available
- raw hex payload
- decoded message type where available
- scenario name and step id where running replay

Acceptance criteria:

- Audit directory is created automatically if missing.
- Failure to open audit log is a startup error in live/cert mode.
- Sent and received callbacks are both registered.
- Audit flush happens on graceful shutdown.
- Audit format is stable NDJSON so it can be filtered and replayed.

Verification commands:

```bash
rm -rf audit
cmake --build build -j 4
ctest --test-dir build --output-on-failure -R audit
./build/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/ouch_bolum2_emir_iletim.yaml
test -s audit/$(date +%F).log
```

## High-Priority Work After Blockers

### H1. Current-Spec OUCH Struct Audit

Files:
- `include/bist/ouch/messages.hpp`
- `include/bist/core/types.hpp`
- `tests/unit/test_messages.cpp`

Required work:

- Change stale comments from 2020/R2.11 baseline to the active 2025 baseline.
- Rename `EnterOrder.reserved[7]` into explicit SMP fields:
  - `smp_level`
  - `smp_method`
  - `smp_id[3]`
  - `reserved[2]`
- Keep total `EnterOrder` size at 114 bytes.
- Keep `OrderAccepted` at 137 bytes with SMP fields.
- Verify `OrderReplaced` remains 145 bytes and does not accidentally receive SMP fields.
- Introduce explicit enums for:
  - cancel reason
  - login reject reason
  - quote status
  - open/close semantics for Enter Order and Replace Order

Acceptance criteria:

- Every OUCH inbound/outbound message in `messages.hpp` has size and offset checks.
- Tests fail if any offset drifts.
- Comments cite document date and table name, not vague legacy wording.

### H2. Full Equity OUCH Scenario Coverage

Files:
- `scenarios/ouch_bolum1_baglanti.yaml`
- `scenarios/ouch_bolum2_emir_iletim.yaml`
- `scenarios/ouch_bolum2_throttling.yaml`
- `scenarios/ouch_bolum2_gateway_failover.yaml`
- Add: `scenarios/ouch_bolum2_surekli_muzayede.yaml`
- Add: `scenarios/ouch_bolum2_eod_batch.yaml`
- `scenarios/ouch_bolum3_piyasa_yapicilik.yaml`
- `src/runner/scenario_loader.cpp`
- `include/bist/runner/scenario.hpp`

Required coverage:

- Section 1 connection/login:
  - successful login
  - wrong password reject
  - duplicate login/session behavior
  - server heartbeat
  - logout/end-of-session
- Section 2 order entry:
  - opening auction ADEL steps
  - continuous auction GARAN/KAREL/VAKKO steps
  - replace orders with leaves quantity checks
  - expected rejects
  - cancel by token
  - cancel by order id
  - throttling
  - gateway failover
  - EOD batch cancellation
- Section 3 market maker:
  - mass quote accepted
  - mass quote rejected
  - cancel bid/offer/two-sided
  - quote status and drop copy correlation

Acceptance criteria:

- Each certification step maps to a scenario step id.
- No scenario depends on the order of unrelated map iteration.
- No scenario passes with only message-type matching.
- Mock replay reports skipped steps explicitly and the reason is documented.
- Live-only scenarios have a preflight validator that checks required live connection settings before starting.

### H3. FIX OE/RD/Drop Copy Completion

Files:
- `docs/FIX_INTEGRATION.md`
- `include/bist/fix/facade.hpp`
- `src/fix/facade.cpp`
- `src/fix/facade_stub.cpp`
- `src/fix/internal/initiator.hpp`
- `src/fix/internal/acceptor_mock.hpp`
- `src/fix/acceptor_mock.cpp`
- `include/bist/runner/fix_replay.hpp`
- `scenarios/fix_oe_temel.yaml`
- `scenarios/fix_rd_temel.yaml`

Required behavior:

- FIX build lane compiles with QuickFIX isolated behind a facade.
- FIX OE supports NewOrderSingle, cancel, replace, trade report, logout, resend, sequence reset, and password-change logon.
- FIX RD supports ApplicationMessageRequest subscribe and duplicate request rejection.
- Drop Copy listener captures ExecutionReport, TradeCaptureReport, and QuoteStatusReport.
- FIX mock acceptor can run the existing FIX scenarios end-to-end.

Acceptance criteria:

- `BIST_BUILD_FIX=ON` and `BIST_ENABLE_QUICKFIX=ON` build passes.
- `fix_oe_temel.yaml` passes in mock mode.
- `fix_rd_temel.yaml` passes in mock mode.
- Live FIX scenario dispatch reaches FIX runner.
- FIX disabled builds fail gracefully when asked to run FIX scenarios.

### H4. CoD-Aware Reconnect Policy

Files:
- `apps/bist_colo.cpp`
- Add: `include/bist/app/reconnect_policy.hpp`
- Add: `tests/unit/test_reconnect_policy.cpp`
- Update: `README.md`
- Add: `docs/RUNBOOK.md`

Required behavior:

BIST Cancel-on-Disconnect timing means reconnect policy must be bounded. The client should detect disconnect fast, retry aggressively but safely, and aim to re-logon before order inactivation starts.

Acceptance criteria:

- Disconnect detection is faster than the BIST CoD inactivation window.
- Reconnect attempts have a maximum elapsed deadline, default 30 seconds.
- The policy records when reconnect did not complete inside the configured deadline.
- Operator logs state whether orders may have entered CoD risk window.
- Gateway failover scenario can force primary failure and reconnect to secondary gateway.

### H5. Instrument Master and Decimal Safety

Files:
- `apps/bist_colo.cpp`
- `include/bist/core/instrument.hpp`
- `include/bist/runner/replay.hpp`
- `include/bist/fix/rd_session.hpp`
- `src/fix/rd_session.cpp`

Required behavior:

Hard-coded cert instruments are acceptable for mock mode only. Live mode must not silently rely on stale instrument ids, symbols, or decimal precision.

Acceptance criteria:

- Mock mode can seed static cert instruments.
- Live mode loads instrument metadata from FIX RD, validated config, or an explicit certified instrument file.
- Price conversion for replace orders uses the original instrument, not `find_by_symbol("")`.
- Startup fails if a scenario references an unknown symbol in live mode.
- Scenario report includes resolved `symbol`, `order_book_id`, and decimals.

### H6. Token Encoding and Scenario Lint

Files:
- `src/runner/scenario_loader.cpp`
- `include/bist/runner/scenario.hpp`
- Add: `apps/tools/scenario_lint.cpp` if the tool does not exist
- Add or extend: `tests/unit/test_scenario_loader.cpp`

Required behavior:

All tokens in YAML must become Alpha(14), left-justified, space-padded tokens on the wire.

Acceptance criteria:

- Numeric YAML token `10` encodes as `"10            "`.
- String YAML token `"10"` encodes identically.
- Tokens longer than 14 characters are rejected by scenario lint.
- Duplicate token use is detected unless the scenario explicitly expects duplicate-token reject.
- Scenario lint validates protocol, section, step id, actions, expected fields, and live-only markers.

## Production-Quality Hardening

### P1. Build and CI Matrix

Required lanes:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBIST_ENABLE_SANITIZERS=ON
cmake --build build-debug -j 4
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-fix -DCMAKE_BUILD_TYPE=Release -DBIST_BUILD_FIX=ON -DBIST_ENABLE_QUICKFIX=ON
cmake --build build-fix -j 4
ctest --test-dir build-fix --output-on-failure
```

Acceptance criteria:

- CI runs all three lanes.
- Sanitizers run without leaks, undefined behavior, or data races in supported tests.
- The project has a documented supported compiler matrix.
- Release binary embeds git commit, build flags, spec baseline, and config hash.

### P2. Operational Configuration

Files:
- `config/bist_prod_like.toml`
- `config/bist_prod_like.local.toml.example`
- Add: `docs/CONFIGURATION.md`

Acceptance criteria:

- Required OUCH fields are validated before connect.
- Required FIX OE/RD/DC fields are validated before connect.
- Local example includes host, port, CompID, SenderCompID, TargetCompID, username, password, account, and AFK fields as applicable.
- Secrets are not committed.
- Startup prints which config files were loaded and which fields came from environment variables.

### P3. Runbook and Certification Package

Files:
- Add: `docs/RUNBOOK.md`
- Add: `docs/CERTIFICATION_PACKAGE.md`
- Add: `apps/tools/audit_pack.cpp` or script equivalent if preferred by existing tooling

Certification package contents:

- binary version output
- git commit
- build flags
- scenario list
- scenario PASS/FAIL report
- audit NDJSON logs
- decoded audit summary
- config snapshot with secrets redacted
- environment report
- operator notes

Acceptance criteria:

- One command produces the package after a mock or live replay.
- Package contains no raw passwords or private keys.
- Package can be regenerated deterministically from audit logs.

### P4. Observability and Failure Modes

Acceptance criteria:

- Every network disconnect has a reason category.
- Every failed scenario step includes last observed protocol messages.
- Every reject code is decoded when known and printed raw when unknown.
- Metrics include orders sent, acks, rejects, cancels, fills, throttled locally, throttled by gateway, reconnects, heartbeats, audit write failures.
- Logs distinguish certification failure from infrastructure failure.

## Scoring Rubric

### Technical Foundation

| Score | Meaning |
| --- | --- |
| 6/10 | Builds and has useful protocol scaffolding, but critical correctness gates are weak. |
| 7/10 | Wire layout blockers fixed, unit tests cover current spec offsets. |
| 8/10 | Strict replay assertions, BIST throttler, audit trail, and resident mode complete. |
| 9/10 | FIX build/routing, scenario lint, CoD reconnect, instrument metadata, and CI matrix complete. |
| 10/10 | Full mock/live cert dry-run package, runbook, failure diagnostics, and current-spec traceability complete. |

### Certification Readiness

| Score | Meaning |
| --- | --- |
| 3/10 | Mock replay can pass while skipping or under-asserting critical behavior. |
| 5/10 | OUCH Basic scenarios strict in mock mode, but live/failover/audit gaps remain. |
| 7/10 | OUCH Basic + Market Maker strict in mock mode; live dry run works with audit. |
| 8/10 | FIX OE/RD mock strict; live dispatch and config validated. |
| 9/10 | Full equity cert dry run passes in BIST test environment with certification package. |
| 10/10 | Repeatable cert-day runbook passes twice cleanly, with no manual patching, no skipped required steps, and complete audit evidence. |

## Implementation Order

### Phase 0: Freeze Baseline

- [ ] Record current git commit and dirty files.
- [ ] Confirm active spec versions from `ouchpdfs/txt`.
- [ ] Update README/STATUS wording so no one claims full certification readiness early.
- [ ] Add a `docs/KNOWN_LIMITATIONS.md` file if limitations need to be shared externally.

Exit criteria:

- Project status accurately says what is ready and what is not ready.
- No outdated "R2.11 only" claim remains in active readiness docs.

### Phase 1: Protocol Correctness

- [ ] Fix `MassQuoteAck` layout.
- [ ] Expose `EnterOrder` SMP fields.
- [ ] Audit all OUCH struct offsets against 2025 spec.
- [ ] Add golden message tests.
- [ ] Update mock gateway to emit spec-accurate bytes.

Exit criteria:

- `ctest --test-dir build --output-on-failure -R test_messages` passes.
- A deliberately broken offset test fails.

### Phase 2: Replay Correctness

- [ ] Add strict OUCH field matcher.
- [ ] Add strict FIX field matcher.
- [ ] Add scenario lint.
- [ ] Add negative tests proving wrong expected fields fail.

Exit criteria:

- Mock replay can no longer pass if a token, price, side, or reject code is wrong.

### Phase 3: BIST Runtime Behavior

- [ ] Replace throttler algorithm.
- [ ] Add live resident mode.
- [ ] Add graceful shutdown.
- [ ] Add CoD-aware reconnect policy.
- [ ] Add primary/secondary gateway failover tests.

Exit criteria:

- Resident mode does not exit by itself.
- CoD reconnect deadline is tested.
- Throttler matches BIST examples.

### Phase 4: Audit and Certification Evidence

- [ ] Wire sent-side audit.
- [ ] Wire received-side audit.
- [ ] Create audit directory automatically.
- [ ] Fail startup if audit cannot be opened in live/cert mode.
- [ ] Add audit package generator.

Exit criteria:

- Every scenario run produces an audit file and summary.
- Audit logs can be decoded after the run.

### Phase 5: Scenario Coverage

- [ ] Complete OUCH Section 1.
- [ ] Complete OUCH Section 2 opening auction.
- [ ] Complete OUCH Section 2 continuous auction.
- [ ] Complete OUCH Section 2 throttling.
- [ ] Complete OUCH Section 2 failover.
- [ ] Complete OUCH Section 2 EOD.
- [ ] Complete OUCH Section 3 Market Maker.
- [ ] Add derivatives scenarios only if derivatives certification is in scope.

Exit criteria:

- Required equity OUCH certification scenarios run without undocumented skips.
- Every skip is either removed or explicitly accepted as live-only with a preflight check.

### Phase 6: FIX Completion

- [ ] Finish QuickFIX facade or choose a modern FIX engine alternative.
- [ ] Build FIX in a dedicated CI lane.
- [ ] Run FIX OE scenario through mock.
- [ ] Run FIX RD scenario through mock.
- [ ] Wire Drop Copy expectations.
- [ ] Route live FIX scenarios correctly.

Exit criteria:

- FIX OE/RD mock replay passes with strict assertions.
- FIX live preflight validates all required connection fields.

### Phase 7: Cert-Day Dry Run

- [ ] Run full mock replay from a clean build.
- [ ] Run full live test-environment replay with BIST credentials.
- [ ] Produce certification package.
- [ ] Review audit evidence manually.
- [ ] Repeat the live dry run on a second clean build.

Exit criteria:

- Two consecutive dry runs pass without code changes.
- Certification package is complete and redacted.
- Operator runbook is accurate.

## Required Final Verification Before Claiming 10/10

Run from a clean checkout or clean worktree:

```bash
rm -rf build build-debug build-fix audit state log

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBIST_ENABLE_SANITIZERS=ON
cmake --build build-debug -j 4
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-fix -DCMAKE_BUILD_TYPE=Release -DBIST_BUILD_FIX=ON -DBIST_ENABLE_QUICKFIX=ON
cmake --build build-fix -j 4
ctest --test-dir build-fix --output-on-failure
./build-fix/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/fix_oe_temel.yaml
./build-fix/apps/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/fix_rd_temel.yaml
```

Live test-environment verification:

```bash
./build-fix/apps/bist_colo --config config/bist_prod_like.toml --replay scenarios/
```

The live command must be run only after config validation, BIST test connectivity, member credentials, and cert-day instrument parameters are confirmed.

## Final Go / No-Go Checklist

Enter BIST certification only when every item below is checked:

- [ ] No P0 or P1 findings remain open.
- [ ] Current BIST PDF versions are recorded in the build output.
- [ ] OUCH wire layout tests pass.
- [ ] FIX build lane passes if FIX certification is in scope.
- [ ] Strict scenario assertions are enabled for OUCH and FIX.
- [ ] Full mock replay passes without required-step skips.
- [ ] Live preflight validates host, port, account, CompID, username, password source, and instrument metadata.
- [ ] Resident mode stays connected until operator shutdown.
- [ ] CoD reconnect deadline is configured and tested.
- [ ] Audit log captures sent and received messages.
- [ ] Certification package generation works.
- [ ] Operator runbook has been executed once by someone other than the implementer.
- [ ] Two consecutive BIST test-environment dry runs pass without code changes.

## Current Recommended Priority

Do the work in this exact order:

1. `MassQuoteAck` layout.
2. Strict OUCH/FIX field assertions.
3. BIST rolling-window throttler.
4. Audit sent/received wiring.
5. Live resident mode.
6. Live protocol dispatch for FIX.
7. Full OUCH scenario coverage.
8. FIX OE/RD/Drop Copy completion.
9. CoD-aware reconnect and failover hardening.
10. Certification package and runbook.

Only after item 10 should the project be described as 10/10 for certification readiness.
