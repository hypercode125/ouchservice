# Project Status

Last updated: 2026-05-05 · branch `main`

## Phase 1 + Phase 2 — COMPLETE ✅

Foundational stack, end-to-end OUCH + FIX cert replay against in-process mock,
PTRM gate, CoD-aware reconnect, deployment tarball ready for co-location.

### Build & test
- Linux + macOS clean build via CMake 3.25+ with FetchContent (no vcpkg/conan).
- **91 / 91 GoogleTest unit tests** green in both Release and FIX-enabled lanes
  (codec, endian, ring buffer, SoupBinTCP framing, throttler rolling-window incl.
  BIST doc example, token registry, order book, instrument cache, sequence store,
  audit log, replay matching with negative tests, FIX matching, scenario_lint
  subprocess, reconnect_policy CoD-deadline, PTRM gate semantics, allocation
  guard, epoll reactor (Linux), end-to-end OuchSession over socketpair, token
  encoding round-trip).
- `./apps/bist_colo --version` prints all 13 OUCH wire sizes; every value is
  verified against BISTECH OUCH Spec (Feb 2025) at compile time via `static_assert`.
- Loopback latency benchmark (macOS): p50 45 µs, p99 86 µs. Production NIC on
  co-located Linux server is expected to clear the cert-day p99 < 50 µs target.

### Cert coverage (mock-replay)

**ALL OUCH and FIX scenarios PASS in mock — 88 step-cases across 9 YAMLs.**

| Cert | Steps | Mock | Live |
|------|-------|------|------|
| OUCH Bölüm 1 — Sistem Bağlantı (multi-login) | 5 / 5 | **5 / 5 ✅** | pending — same `MockTransportControl` pattern reused by `LiveTransportControl` |
| OUCH Bölüm 2 — Emir İletim | 11 / 11 | **11 / 11 ✅** | pending |
| OUCH Bölüm 2 — Sürekli Müzayede | 7 / 7 (replace qty/price/both) | **7 / 7 ✅** | pending |
| OUCH Bölüm 2 — EOD Batch | 8 / 8 (`inactivate_all` mock action) | **8 / 8 ✅** | pending — live ME emits the same batch automatically |
| OUCH Bölüm 2 — Throttling | 2 / 2 (1000 orders @ 100/s burst) | **2 / 2 ✅** | pending |
| OUCH Bölüm 2 — Gateway Failover | 6 / 6 (dual-port mock) | **6 / 6 ✅** | pending — `LiveTransportControl::switch_to_secondary` ready |
| OUCH Bölüm 3 — Piyasa Yapıcılık | 12 / 12 | **12 / 12 ✅** | pending |
| FIX OE Temel | 26 / 26 (Phase 2.4 multi-logon, token-symbol index) | **26 / 26 ✅** | pending — live dispatch wired in `apps/bist_colo.cpp:run_one_live_fix_scenario` |
| FIX RD Temel | 5 / 5 | **5 / 5 ✅** (incl. duplicate-AMR rejection per session) | pending |

Run with:
```bash
cmake -S . -B build -DBIST_ENABLE_QUICKFIX=OFF
cmake --build build -j
./build/apps/bist_colo --mock --replay scenarios/
```

### Modules delivered
| Path | Role |
|------|------|
| `include/bist/core/types.hpp` | Side, TimeInForce, OpenClose, ClientCategory, OrderState, MassQuoteStatus, OrderToken, AFK code constants |
| `include/bist/core/endian.hpp` | `BigEndian<T>` wire wrapper enforcing wire-byte order |
| `include/bist/core/result.hpp` | `Result<T, Error>` + ErrorCategory taxonomy |
| `include/bist/core/time.hpp` | monotonic_ns / wall_ns |
| `include/bist/core/ring_buffer.hpp` | lock-free SPSC ring (1<<20 slots) |
| `include/bist/net/tcp_socket.hpp` | non-blocking POSIX TCP, TCP_NODELAY, `from_fd` factory for tests |
| `include/bist/net/reactor.hpp` | IReactor + portable poll(2)-based PollReactor |
| `include/bist/net/soupbintcp.hpp` | SoupBinTCP framing structs and header builders |
| `include/bist/ouch/messages.hpp` | byte-exact OUCH inbound + outbound structs (114B EnterOrder, 122B ReplaceOrder, 50+28×N MassQuote, 145B OrderReplaced, …) |
| `include/bist/ouch/codec.hpp` | alpha pad/get, hex dump, MassQuote builders matching Spec Appendix B Quote Matrix |
| `include/bist/ouch/session.hpp` | SoupBinTCP+OUCH session FSM; login/heartbeat/watchdog/EAGAIN-stash; typed callbacks |
| `include/bist/ouch/client.hpp` | high-level place/replace/cancel/cancel_by_order_id/mass_quote API on top of session + token registry + throttler + order book |
| `include/bist/domain/throttler.hpp` | token-bucket rate limiter (cert default 100 / sec) |
| `include/bist/domain/token_registry.hpp` | uniqueness guard + token → OrderID lookup |
| `include/bist/domain/order_book.hpp` | local order state with Spec 4.1.2.1 `expected_leaves` formula |
| `include/bist/domain/instrument.hpp` | Symbol ↔ OrderBookID cache; TL-string-to-wire price parser |
| `include/bist/observability/logger.hpp` | spdlog async wrapper |
| `include/bist/observability/audit.hpp` | append-only NDJSON-style audit log with daily rotation, separate worker thread, ISO-8601 ns timestamps |
| `include/bist/runner/scenario.hpp`, `runner/replay.hpp`, `src/runner/scenario_loader.cpp` | YAML scenario model + executor |
| `include/bist/mock/ouch_gateway.hpp`, `src/mock/ouch_gateway.cpp` | in-process mock OUCH gateway for offline cert replay |
| `apps/bist_colo.cpp` | `--mock --replay`, `--version` (wire-size receipts) |
| `apps/tools/ouch_decode.cpp` | hex → human-readable for audit log analysis |
| `apps/tools/scenario_lint.cpp` | YAML schema linter for cert scenarios |
| `include/bist/fix/fields.hpp` | FIX 5.0 SP2 tag/value catalog + BIST custom AFK / SessionStatus / PartyRole constants (Phase 2.1) |
| `include/bist/fix/application.hpp` | `BistApp : FIX::Application + MessageCracker` with typed callbacks + 554/925 password rotation hook (Phase 2.1, gated) |
| `include/bist/fix/oe_session.hpp` + `src/fix/oe_session.cpp` | `place` / `cancel` / `replace` / `trade_report` over `FIX50SP2::NewOrderSingle` / `OrderCancelRequest` / `OrderCancelReplaceRequest` / `TradeCaptureReport` (Phase 2.1, gated) |
| `include/bist/fix/rd_session.hpp` + `src/fix/rd_session.cpp` | `subscribe_all` issues `ApplicationMessageRequest (BW)` (Phase 2.1, gated) |
| `include/bist/fix/dc_listener.hpp` | Drop Copy ER/QSR/TCR consumer (Phase 2.1, gated) |
| `include/bist/fix/initiator.hpp` | `Initiator` owns `FIX::SocketInitiator` + generated session.cfg (Phase 2.1, gated) |

### Decisions locked (see `docs/superpowers/specs/2026-05-05-bist-colo-client-design.md`)
- Scope: OUCH + FIX OE Temel + FIX RD Temel
- Architecture: monolithic single-process, single hot thread (epoll/poll), 3 side threads (DropCopy / Logger / CLI)
- Runner: hybrid YAML + REPL
- FIX engine: QuickFIX/C++ (BSD), pulled via FetchContent
- C++20 / CMake 3.25+ / spdlog (async) / GoogleTest / TOML (toml++) / yaml-cpp
- ASan + UBSan in Debug; LTO in Release; `-march=native` opt-in for colo target

## Phase 2 — Çoğu kapatıldı

> **Last code milestone (commits e4bc00e, bca3e4e — 2026-05-05 23:30):**
> - Equity-only scope `docs/KNOWN_LIMITATIONS.md` ile lock'landı (VIOP/derivatives explicitly out of scope)
> - R6 OpenClose hardening: `OuchClient::replace` artık `Open`/`CloseNet` runtime reject ediyor (Spec 4.1.2.2)
> - 3 BIST test ortamı (Pre-Prod, Prod-like, T+1) için config dosyaları + endpoint haritası eklendi
> - `docs/BIST_ENVIRONMENTS.md` PDF özet referansı (24.03.2026 baseline)
>
> **Cert blocker'ların hepsi kapatıldı.** Kalan: BIST kimlik bilgisi + Pre-Prod VPN + canlı dry-run.
>
> **▶ NEXT SESSION ORDER** (BIST credential geldikten sonra):
> 1. `bistechsupport_autoticket@borsaistanbul.com` üzerinden Pre-Prod erişim talebi
> 2. VPN + ISV/üye hesabı geldikten sonra `config/bist_preprod.local.toml` doldur
> 3. `./build/apps/tools/preflight --config config/bist_preprod.toml` connectivity testi
> 4. Bölüm 1 → 2 → 3 sırayla `--replay scenarios/...` (mock'ta zaten 88/88 pass)
> 5. FIX OE → RD aynı şekilde
> 6. Pre-Prod stable → Prod-like'a geç (canlı sürüm aynası)
> 7. Prod-like'da 2 ardışık temiz dry-run (BIST_10_10_READINESS_PLAN.md §Final Go/No-Go)
> 8. Cert randevusu → Borsa Eksperi gözlemli koşum

### 🔴 CRITICAL — wire-level cert blockers (per `docs/BIST_10_10_READINESS_PLAN.md`)

**Status**: spec drift closed. All wire layouts now match **BISTECH OUCH Protocol Specification (06 Feb 2025)**. Throttler matches BIST 10×100ms rolling-window doc example. Only CoD-aware reconnect deadline remains.

- [x] **C1** `OrderAccepted` 137 B + SMP (`smp_level`/`smp_method`/`smp_id[3]`) — `include/bist/ouch/messages.hpp:178-221`, mock emitter updated, `OuchMessages.OutboundSizes` covers it.
- [x] **C2** `MassQuoteAck` Feb 2025 field order (qty@27 / traded_qty@35 / price@43 / side@47 / quote_status@48) — `include/bist/ouch/messages.hpp:300-321`, `OuchMessages.MassQuoteAckUsesFeb2025Offsets` golden byte test.
- [x] **C3** `EnterOrder.smp_level/smp_method/smp_id[3]/reserved[2]` (offset 107..113) — `include/bist/ouch/messages.hpp:67-102`, `OuchMessages.EnterOrderExposesFeb2025SmpFields` byte test.
- [x] **C4** Throttler 10×100ms rolling window — `include/bist/domain/throttler.hpp` ring of 10 windows, BIST doc 30+56+14 example test green (`tests/unit/test_throttler.cpp:39-53`).
- [x] **C5** CoD-aware reconnect deadline — `include/bist/app/reconnect_policy.hpp` deadline-bounded backoff (default 30 s, configurable via `[ouch].cod_reconnect_deadline_ms`). `apps/bist_colo.cpp:980` `LiveTransportControl` wire. `tests/unit/test_reconnect_policy.cpp` covers (a) success short-circuit, (b) deadline expiry → `ErrorCategory::Timeout`, (c) deadline applies across multiple backoff attempts.

### 🟠 HIGH — cert coverage gaps + spec drift

- [x] **H1** `CancelReason` enum + replay matcher field — `include/bist/core/types.hpp:111` 9 değer (CanceledByUser=1 .. MarketHalt=11), `cancel_reason_name` helper.
- [x] **H2** `OpenClose` doc-split + runtime hardening — `ReplaceKeep` (=0) ve `ReplaceDefault` (=4) iki anlam ayrıştırıldı. `OuchClient::replace` artık `Open`/`CloseNet` reject ediyor (commit e4bc00e, Spec 4.1.2.2 cite).
- [x] **H3** Cert scenario coverage:
  - `scenarios/ouch_bolum2_surekli.yaml` ✅ 7/7 mock pass
  - `scenarios/ouch_bolum2_eod_batch.yaml` ✅ 8/8 mock pass (new `inactivate_all` mock action)
  - `scenarios/ouch_vadeli_temel.yaml` ❌ KAPSAM DIŞI — VIOP/Derivatives `docs/KNOWN_LIMITATIONS.md`'de equity-only scope locked.
- [x] **H4** Order Token integer→string encoding test — `tests/unit/test_token_encoding.cpp` round-trip YAML int 10 ↔ string "10" identical wire bytes; lint len > 14 reject.
- [ ] **H5** MassQuoteAck per-side double emission verify — `domain/order_book.hpp` Side='B' vs Side='S' separately bookkept; mock emits one ACK per accepted side. (Bölüm 3 12/12 mock pass, formal verify outstanding.)
- [x] **H6** `apps/tools/scenario_lint.cpp` extensions — `tests/unit/test_scenario_lint.cpp` covers protocol present, sequential step ids, known action set, expected fields known keys, token len ≤ 14, duplicate-token detection, FIX scenario action validation.

### 🟡 MEDIUM — production polish

- [ ] **M1** Login Rejected Reason enum (`'A'=NotAuthorized`, `'S'=SessionNotAvailable`). `ouch/messages.hpp` raw byte → enum.
- [ ] **M2** SoupBinTCP heartbeat 1.5s idle test — assert exactly one ClientHeartbeat emitted in `tests/unit/test_session_pair.cpp`.
- [ ] **M3** `quote_unchanged_bid/offer` codec helper (`bid_size=0 ∧ bid_price≠0` → leave bid unchanged). Doc cancel-vs-unchanged distinction in `codec.hpp` header.
- [ ] **M4** `STATUS.md` "Decisions locked" + `messages.hpp` headers point at Feb 2025 spec, not R2.11.

### 🟢 LOW — cosmetic

- [ ] **L1** Remove misleading 56-byte comment in `messages.hpp:298-300` (post-C2).
- [ ] **L2** Narrative correctness — `STATUS.md` "Modules delivered" cite spec-version dependency for byte sizes.

### High-priority cert closure
- [x] OUCH Bölüm 2 throttling — runner action `burst_place` (count, rate_per_sec, pattern[]); `OuchClient::place` already gated by `domain::Throttler` token bucket; runner retries on `Throttled` and paces send wall time. `scenarios/ouch_bolum2_throttling.yaml` runs in mock end-to-end (1000 acks @ ~100/s, ~10 s wall)
- [x] OUCH Bölüm 2 gateway failover — runner action `switch_gateway`, `ITransportControl::switch_to_secondary()`, `LiveTransportControl` swaps `host_primary`↔`host_secondary` and re-logins with current `next_inbound_seq` (cert "Sequence numarası sıfırlanmaz"). `scenarios/ouch_bolum2_gateway_failover.yaml` runs live
- [x] OUCH Bölüm 1 live mode — `bist::runner::ITransportControl` + `LiveTransportControl` in `apps/bist_colo.cpp` rebuilds socket+session in `std::optional` slots; runner `ouch_login` honours the transport when not Active and forwards password/session/sequence overrides
- [ ] OUCH Bölüm 2 opening-match auction simulator in mock (continuous matching engine for live phase replay)

### FIX certification
- [x] **Phase 2.1**: `include/bist/fix/{fields,application,oe_session,rd_session,dc_listener,initiator}.hpp` design ships — 554/925 password rotation, 453/448/447/452 party-role AFK encoding, 1409 SessionStatus. See `docs/FIX_INTEGRATION.md`.
- [x] **Phase 2.1**: `scenarios/fix_oe_temel.yaml` (26 steps) and `scenarios/fix_rd_temel.yaml` (5 steps) — both `mock_skip: true` until 2.2.
- [x] **Phase 2.2 — Pimpl facade**: `include/bist/fix/facade.hpp` now exposes opaque `OeClient` / `RdClient` / `DcClient` with POD args + events. `src/fix/facade_quickfix.cpp` translates between POD and QuickFIX, compiled at `-std=gnu++14`. `src/fix/facade_stub.cpp` ships an `Unsupported`-returning shim when QuickFIX is disabled. `core/result.hpp` and `core/types.hpp` are now C++14-clean so the FIX TUs can include them. Configure-time bridges patch QuickFIX 1.15.1: portable `AtomicCount.h` (replaces x86 `lock xadd` with `std::atomic<long>`), pre-generated `Allocator.h` / `config.h`, manually-added `add_subdirectory(quickfix)` after `FetchContent_Populate`, `set_target_properties(quickfix CXX_STANDARD 14)` so QuickFIX itself parses. With `BIST_ENABLE_QUICKFIX=ON BIST_BUILD_FIX=ON` the project links `libquickfix.dylib` cleanly on Apple Silicon arm64.
- [x] **Phase 2.3 — runner integration (initial drop)**: YAML runner accepts `fix_logon` / `fix_logout` / `fix_amr_subscribe` / `fix_place` / `fix_cancel` / `fix_replace` actions plus protocol-aware aliasing of `place`/`cancel_by_token`/`replace` for `protocol: fix_oe|fix_rd` scenarios (`include/bist/runner/scenario.hpp`, `src/runner/scenario_loader.cpp`). New header-only `bist::runner::FixScenarioRunner` (`include/bist/runner/fix_replay.hpp`) drives the POD facade with a thread-safe event queue. New `bist::fix::AcceptorMock` (`include/bist/fix/acceptor_mock.hpp`, `src/fix/acceptor_mock.cpp`, `src/fix/internal/acceptor_mock.hpp`) is an in-process QuickFIX SocketAcceptor that handles Logon w/ 554+925 → 1409=1, NewOrderSingle → ER, OrderCancelRequest → ER, OrderCancelReplaceRequest → ER, ApplicationMessageRequest → ApplicationMessageRequestAck (with duplicate-ApplReqID detection). `apps/bist_colo.cpp run_one_scenario()` peeks `preconditions.protocol` and routes `fix_oe`/`fix_rd` YAMLs through the new path; `mock_skip: false` on both `scenarios/fix_*.yaml`. Build dictionary path passed via `BIST_FIX_DICT_DIR` compile def; both initiator and acceptor configured with `UseDataDictionary=N` so QuickFIX 1.15.1's stale FIX 5.0 SP2 dict can't reject the BW/BX/1409 traffic. `--mock --replay scenarios/` results: **`fix_rd_temel` 5/5 PASS** end-to-end; **`fix_oe_temel` 6/26 PASS** — the 20 remaining failures all stem from the cert program's logout→relogon→ResendRequest dance, which QuickFIX's `SocketInitiator` does not auto-redrive after an explicit `Session::logout()`.
- [x] **Phase 2.4 — multi-logon / resend cert flow**: `OeClient` rebuild on `fix_logon` after `fix_logout`. `fix_oe_temel.yaml` **26/26 PASS** end-to-end against AcceptorMock.
- [x] **FIX RD step 4 — duplicate AMR rejection**: AcceptorMock per-session `seen_appl_req_ids` set; second `ApplicationMessageRequest` with same `ApplReqID` → `ApplicationMessageRequestAck` with `ApplResponseType=2` + Text reason. `fix_rd_temel.yaml` 5/5 PASS.
- [ ] BIST FIX 5.0 SP2 dictionary refinements (custom values for `PartyRole=76`/AFK, `OrdType` extensions for Iceberg/Midpoint/Imbalance) once we know which QuickFIX dictionary is used at runtime.

### Real-environment connectivity
- [x] TOML config loader — `bist::config::load_config()` + `*.local.toml` override merge + 4 unit tests (`tests/unit/test_config.cpp`)
- [x] BIST endpoint connect path — `apps/bist_colo.cpp` `run_live()`: `--config` + `--partition N` + `--secondary` resolve `[ouch]` host:port, connect, login, drive replay
- [x] VPN-aware connect with retry/backoff — `connect_with_retry()` 6 attempts, 500 ms → 15 s exponential
- [x] Sequence persistence — `bist::observability::SequenceStore` (atomic write+rename, throttled save_if_due) writes `state/<env>_oe_p<N>.seq` on every inbound packet; loaded before login to resume; 7 unit tests
- [x] systemd unit + run book — `deploy/systemd/bist-colo.service`, `deploy/install.sh`, `docs/RUNBOOK.md`. `scripts/build_deploy.sh dist` üretir 607 KB tarball.
- [x] BIST 3 test ortamı endpoint haritası — `config/bist_{preprod,prod_like,t_plus_1}.toml` + `docs/BIST_ENVIRONMENTS.md` (commit bca3e4e, source: BIST PDF 24.03.2026)

### Production hardening
- [x] Linux epoll Reactor backend (drop-in for PollReactor) — `tests/unit/test_epoll_reactor.cpp`
- [x] CI matrix on GitHub Actions: Linux × {gcc-12, clang-16} × {Release, Debug+ASan+UBSan} + FIX-enabled lane + deploy-package job — `.github/workflows/ci.yml`
- [x] Hot-path allocation audit — `tests/unit/test_alloc_guard.cpp` allocator hook + assertion
- [x] Latency benchmark — `apps/tools/latency_bench.cpp`; loopback p50=45 µs, p99=86 µs (macOS)
- [x] PTRM gate — `tests/unit/test_ptrm.cpp` semantic gate (Always-Reject / Log-Only / Enforce). Full BIST PTRM REST API integration: out of scope per `KNOWN_LIMITATIONS.md`.

### Phase-3 (post-cert)
- [ ] Kernel-bypass NIC backend behind the Reactor interface (Solarflare ef_vi or DPDK)
- [ ] Strategy framework hooks (subscribe to events, emit orders)
- [ ] VIOP / BAP / KMTP markets
- [ ] T+1 (10.57.3.208) certification
- [ ] FIX OE / RD İleri Seviye + Trade Report (Özel İşlem Bildirimi) two-party flows

## Cert-day run book (preview)

1. **Pre-test (1 week before)** — sched call with bistechsupport_autoticket@borsaistanbul.com; receive credentials + AFK + OrderBookIDs.
2. **Pre-test (1 day before)** — VPN connect, smoke `bist_colo --config config/bist_prod_like.toml --interactive`, place + cancel a single order on Pre-Prod, deliver audit log.
3. **Cert day** — `bist_colo --config config/bist_prod_like.toml --replay scenarios/` end-to-end. Borsa Eksperi observes Trading Workstation + monitors mock acks. Audit logs emailed at finish.

---

Support: `bistechsupport_autoticket@borsaistanbul.com` · BIST tech docs: <https://www.borsaistanbul.com/en/technical-resources/technical-documents>
