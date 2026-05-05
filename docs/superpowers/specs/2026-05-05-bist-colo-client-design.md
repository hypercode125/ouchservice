# BIST Co-Location Trading Client — Design Spec

- Date: 2026-05-05
- Project root: `~/Desktop/ouch`
- Author: assistant + Mehmet Taban (locked decisions)
- Status: APPROVED for implementation

## 1. Goal

Build a single-binary, Linux x86-64 co-location trading client that passes the full **BIST Pay Piyasası BISTECH Sertifikasyon Programı** for:

- OUCH (Genium INET BIST OUCH 2.11) — Bölüm 1 sistem bağlantı, Bölüm 2 emir iletim/throttling/gateway failover, Bölüm 3 piyasa yapıcılık (MassQuote)
- FIX 5.0 SP2 Order Entry — Temel Seviye (Logon, password change, sequence reset, resend request, order types, gateway failover, drop copy)
- FIX 5.0 SP2 Reference Data — Temel Seviye (subscription via Application Message Request)

Software is **end-to-end production-grade**: not just a test driver. Architectural seams allow later migration to kernel-bypass NICs (Solarflare ef_vi / DPDK) and C-only hot path.

## 2. Locked decisions

| # | Decision | Choice |
|---|----------|--------|
| 1 | Scope | OUCH + FIX OE Temel + FIX RD Temel |
| 2 | Architecture | Monolithic single-process, single hot thread (epoll) |
| 3 | Cert scenario runner | Hybrid YAML scenario file + interactive CLI |
| 4 | OUCH spec source | BIST OUCH Protocol Spec R2.11 (1 July 2020) — verified |
| 5 | FIX engine | Hybrid: QuickFIX/C++ for FIX, custom OUCH |
| 6 | C++ standard | C++20 |
| 7 | Build | CMake 3.25+ + FetchContent (no external pkg manager) |
| 8 | Logging | spdlog (async mode) |
| 9 | Test framework | GoogleTest |
| 10 | Config | TOML (toml++) |
| 11 | Drop Copy | Same binary, separate thread |
| 12 | OS | Linux x86-64 (Ubuntu 22.04+) |
| 13 | Compiler | GCC 12+ / Clang 16+ |
| 14 | Sanitizer | ASan + UBSan in debug build |
| 15 | CI | GitHub Actions |

## 3. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  bist-colo-client  (Linux x86-64)                               │
│                                                                 │
│  ┌────────────────── Reactor (epoll, hot thread) ────────────┐ │
│  │  OUCH OE  │  FIX OE  │  FIX RD  │  Scenario Runner        │ │
│  │  custom   │ QuickFIX │ QuickFIX │  YAML replay + CLI step │ │
│  └────┬──────────┬─────────┬───────────┬──────────────────────┘ │
│       │          │         │           │                        │
│  ┌────▼──────────▼─────────▼───────────▼─────────────────────┐ │
│  │     OrderState / TokenRegistry / Throttler (shared)       │ │
│  └────────────────────────┬───────────────────────────────────┘ │
│                           │ SPSC ring                           │
│   ┌──── DropCopy ─────┬───┴───┬──── Logger ────┬── CLI ────┐   │
│   │ thread (FIX DC)   │       │ (spdlog async) │ (stdin)   │   │
│   └───────────────────┴───────┴────────────────┴───────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

Hot thread: pinned via `taskset -c <core>` on isolated CPU. TCP_NODELAY, SO_BUSY_POLL where supported. No allocations on hot path beyond bounded ring slot acquisition. Logging never blocks.

## 4. Module breakdown (header layout)

```
include/bist/
├── core/      types.hpp result.hpp ring_buffer.hpp time.hpp
├── net/       reactor.hpp tcp_socket.hpp soupbintcp.hpp
├── ouch/      messages.hpp codec.hpp session.hpp client.hpp
├── fix/       application.hpp oe_session.hpp rd_session.hpp dc_listener.hpp
│              data_dict/BIST_FIX50SP2.xml
├── domain/    order_book.hpp token_registry.hpp throttler.hpp instrument.hpp
├── runner/    scenario.hpp replay.hpp repl.hpp
└── observability/ logger.hpp metrics.hpp audit.hpp

src/      mirror of include
tests/    unit/ integration/ cert_replay/
config/   bist_prod_like.toml bist_pre_prod.toml bist_t_plus_1.toml
scenarios/  ouch_bolum1_baglanti.yaml  …  fix_rd_temel.yaml
apps/     bist_colo.cpp tools/{ouch_decode,scenario_lint}.cpp
docs/     ARCHITECTURE.md  superpowers/specs/  superpowers/plans/
CMakeLists.txt README.md
```

### Boundary rules
- `core/`, `net/` carry no BIST-specific knowledge (portable)
- `ouch/`, `fix/` are protocol-specific; pump events into `domain/`
- `runner/` only calls public API (`OuchClient`, `FixOeClient`) — protocol stays opaque
- Every external IO sits behind an interface (`IReactor`, `ITcpSocket`) for unit-testability

## 5. Threading model

| Thread | Pinned? | Purpose | Latency-critical |
|--------|---------|---------|------------------|
| Hot (main) | yes | epoll: read/write OUCH+FIX OE+FIX RD; scenario step dispatch | yes |
| DropCopy | no | FIX DC session → SPSC ring → audit | no |
| Logger | no | drain spdlog ring → file | no |
| CLI | no | stdin readline; commands → SPSC ring | no |

Cross-thread: lock-free SPSC ring (1<<20 slots). Tail/head are `std::atomic<uint64_t>`, cache-line padded.

## 6. Hot path: `place()`

```
CLI thread:    "place ACSEL.E BUY 200 6.200" → SPSC enqueue
Hot thread:
  1. dequeue command
  2. throttler.try_acquire()                 # token bucket 100/s default
  3. token = token_registry.allocate()       # 14-byte alpha
  4. EnterOrder serialize                    # 114 bytes big-endian
  5. soupbintcp.wrap (UnsequencedDataPacket)
  6. tcp_socket.write_nb (TCP_NODELAY)
  7. order_book.insert(token, Pending{symbol, side, qty, price})
  8. audit.write(SENT, hex_dump, t_ns)
  ... epoll_wait() ...
  9. tcp_socket.read → SoupBinTCP demux
 10. OrderAccepted (135 byte) parse
 11. order_book.transition(token, Accepted, OrderID)
 12. audit.write(RECV, ...)
 13. scenario.notify(EXPECT_ACCEPTED)
```

## 7. OUCH wire layout (BIST Spec R2.11)

Tipler:
- `Numeric` — unsigned big-endian (1/2/4/8/12 byte)
- `Price` — signed int32 big-endian; decimal sayısı `Order book Directory` (ITCH) veya `Security Definition` (FIX RD) ile alınır
- `Alpha` — ISO-8859-9 (Latin-9), left-justified, right-padded with spaces
- `Timestamp` — uint64 ns since UNIX epoch UTC

### Inbound

| Mesaj | Type | Toplam | Alanlar |
|-------|------|--------|---------|
| Enter Order | `O` | 114 B | Token14 + OBID4 + Side1 + Qty8 + Px4 + TIF1 + OC1 + ClientAcc16 + CustInfo15 + ExchInfo32 + DispQty8 + Cat1 + OffHrs1 + Reserved7 |
| Replace Order | `U` | 122 B | ExistingTok14 + ReplTok14 + Qty8 + Px4 + OC1 + ClientAcc16 + CustInfo15 + ExchInfo32 + DispQty8 + Cat1 + Reserved8 |
| Cancel | `X` | 15 B | Token14 |
| Cancel by Order ID | `Y` | 14 B | OBID4 + Side1 + OrderID8 |
| Mass Quote | `Q` | 50 + 28×N B | Token14 + Cat1 + ClientAcc16 + ExchInfo16 + NoQuoteEntries2 + N × (OBID4 + BidPx4 + OfferPx4 + BidSize8 + OfferSize8); N ∈ [1,5] |

### Outbound

| Mesaj | Type | Toplam | Alanlar |
|-------|------|--------|---------|
| Order Accepted | `A` | 135 B | Ts8 + Token14 + OBID4 + Side1 + OrderID8 + Qty8 + Px4 + TIF1 + OC1 + ClientAcc16 + State1 + CustInfo15 + ExchInfo32 + PreTradeQty8 + DispQty8 + Cat1 + OffHrs1 + Reserved3 |
| Order Rejected | `J` | 27 B | Ts8 + Token14 + RejectCode4 (signed) |
| Order Replaced | `U` | 145 B | Ts8 + ReplTok14 + PrevTok14 + OBID4 + Side1 + OrderID8 + Qty8 + Px4 + TIF1 + OC1 + ClientAcc16 + State1 + CustInfo15 + ExchInfo32 + PreTradeQty8 + DispQty8 + Cat1 |
| Order Canceled | `C` | 37 B | Ts8 + Token14 + OBID4 + Side1 + OrderID8 + Reason1 |
| Order Executed | `E` | 68 B | Ts8 + Token14 + OBID4 + TradedQty8 + TradePx4 + MatchID12 + Cat1 + Reserved16 |
| Mass Quote Ack | `K` | 56 B | Ts8 + Token14 + OBID4 + Side1 + Status4 + Qty8 + TradedQty8 + Px4 |
| Mass Quote Reject | `R` | 31 B | Ts8 + Token14 + OBID4 + RejectCode4 |

### Field encodings (kritik)
- Side: `B` Buy, `S` Sell, `T` Short Sell
- TIF: 0 Day, 3 IoC (FaK), 4 FoK
- OC: 0 Default, 1 Open, 2 Close/Net (replace'te 0=no change, 4=default)
- ClientCategory: 1 Client, 2 House, 7 Fund, 9 Investment Trust, 10 Primary Dealer Govt, 11 Primary Dealer Corp, 12 Portfolio Mgmt
- OffHours: 0/1 (türev için)
- OrderState: 1 OnBook, 2 NotOnBook, 98 Paused, 99 Ownership Lost
- Cancel Reason: 1 user, 3 Deal, 4 Inactivated, 5 Altered, … 120-124 PTRM family

### MassQuote işlem matrisi (Spec Appendix B)
| İşlem | BidPx | BidSize | OfferPx | OfferSize |
|-------|-------|---------|---------|-----------|
| New two-sided | >0 | >0 | >0 | >0 |
| New one-sided bid | >0 | >0 | 0 | 0 |
| New one-sided offer | 0 | 0 | >0 | >0 |
| Replace two-sided (Px+Size) | >0 | >0 | >0 | >0 |
| Replace bid Px (offer unchanged) | >0 | 0 | >0 | 0 |
| Replace size only | >0 | >0 | >0 | >0 |
| Cancel two-sided | 0 | 0 | 0 | 0 |
| Cancel bid only | 0 | 0 | >0 | >0 |
| Cancel offer only | >0 | >0 | 0 | 0 |

Ack flow:
- All accepted → 2 K per quote (bid + offer side)
- Some accepted, some rejected → K per accepted side, R (with OBID) per rejected
- All rejected → 1 single R (no OBID)

## 8. SoupBinTCP layer

Wraps every OUCH inbound/outbound:

| Packet | Direction | Format |
|--------|-----------|--------|
| Login Request | C→S | `'L'` + Username6 + Password10 + RequestedSession10 + RequestedSeqNum20 |
| Login Accepted | S→C | `'A'` + Session10 + SeqNum20 |
| Login Rejected | S→C | `'J'` + RejectReasonCode1 (`A` = invalid user/pwd) |
| Sequenced Data | S→C | `'S'` + payload |
| Unsequenced Data | C→S | `'U'` + payload |
| Server Heartbeat | S→C | `'H'` |
| Client Heartbeat | C→S | `'R'` |
| Logout Request | C→S | `'O'` |
| End of Session | S→C | `'Z'` |

Header preceding all packets: `length2 (network byte order)` + `packetType1`.

Heartbeat: 1 second of silence on either side → must send heartbeat. Test scenarios verify both directions.

## 9. FIX integration (QuickFIX)

- Vendor: QuickFIX/C++ (BSD), `quickfix/quickfix` GitHub
- Pulled via `FetchContent_Declare(quickfix GIT_TAG vX.Y.Z)`
- BIST data dictionary `BIST_FIX50SP2.xml` lives in `include/bist/fix/data_dict/`
- Custom fields:
  - `554` Password, `925` NewPassword (logon password change)
  - `453` NoPartyIDs / `448` PartyID / `447` PartyIDSource / `452` PartyRole — used for AFK (`PYM`/`PYP`/`XRM`)
  - `1409` SessionStatus (0 Active, 8 Password Expired, 1409=1 Password Changed)
- **OE session** (`oe_session.hpp`): `NewOrderSingle (D)`, `OrderCancelRequest (F)`, `OrderCancelReplaceRequest (G)`, `ExecutionReport (8)`, `OrderCancelReject (9)`, `TradeCaptureReport (AE)`
- **RD session** (`rd_session.hpp`): `ApplicationMessageRequest (BW)` for subscription; `SecurityDefinitionRequest (c)`, `SecurityStatusRequest (e)`, `MarketDataRequest (V)` for request-reply (İleri Seviye için hazır altyapı)
- **DC listener** (`dc_listener.hpp`): own session, separate thread, ER/QSR/TCR consume

QuickFIX session config (`session.cfg`) generated at runtime from TOML.

## 10. Cert scenario runner

### YAML schema

```yaml
name: ouch_bolum2_emir_iletim
description: "OUCH Bölüm 2 — açılış seansı emir iletim"
preconditions:
  session: ouch
  market: pay
steps:
  - id: 1
    action: login
    expect: login_accepted
  - id: 2
    action: place
    args: { symbol: ADEL.E, side: BUY, qty: 200, price: 6.200, tif: DAY, category: CLIENT, token: 10 }
    expect:
      msg: order_accepted
      fields: { token: 10, side: B }
  - id: 3
    action: cancel_by_token
    args: { token: 20 }
    expect: { msg: order_canceled, fields: { token: 20 } }
  - id: 5
    action: place
    args: { symbol: ADEL.E, side: BUY, qty: 100, price: 10.000, token: 70 }
    expect:
      msg: order_rejected
      fields: { reject_code: -420131 }
```

### Replay engine
- Reads YAML, validates against schema (CMake-time `scenario_lint`)
- Pumps actions into hot thread via SPSC ring
- Listens to events on side queue
- For each step waits up to `timeout_ms` (default 5s) for matching event
- On mismatch: prints diff, marks step FAIL, optionally stops

### CLI commands (REPL)
```
> login
> place ACSEL.E BUY 200 6.200
> replace 30 79 6.070       # token, new_qty, new_price
> cancel 20
> mq ALCAR.E 5.100 500 5.110 500   # bid offer mass quote
> dump book
> dump audit since 5s
> run scenarios/ouch_bolum2_emir_iletim.yaml
```

## 11. Throttler

Token-bucket: capacity `N=100`, refill `100/sec`. Implementation:

```cpp
struct Throttler {
  uint64_t capacity;
  uint64_t refill_per_ns;   // 100 / 1e9
  std::atomic<int64_t> tokens;
  std::atomic<uint64_t> last_refill_ns;

  bool try_acquire() {
    auto now = clock_ns();
    refill(now);
    int64_t expected = tokens.load(std::memory_order_relaxed);
    while (expected > 0) {
      if (tokens.compare_exchange_weak(expected, expected - 1)) return true;
    }
    return false;
  }
};
```

Configurable per `config/*.toml`. Cert test uses 100/s; production may be higher.

## 12. Replace LeavesQty rule (audit-critical)

```cpp
LeavesQty replace(OrderState& s, Quantity new_qty) {
  s.leaves = std::max<int64_t>(0, new_qty - s.executed);
  s.order_state = (s.leaves == 0) ? NotOnBook : OnBook;
  return s.leaves;
}
```

Verified against OUCH Spec 4.1.2.1 examples and BIST cert PDF replace scenarios (ZOREN/ALCAR walkthrough).

## 13. Error handling & recovery

| Failure | Detection | Recovery |
|---------|-----------|----------|
| TCP read returns 0 | reactor | mark session disconnected; failover gateway flow |
| Heartbeat timeout | 2 missed = stale | reconnect, do NOT reset SoupBinTCP sequence |
| OUCH `Order Rejected` (`J`) | parsed | log + audit + scenario.notify(REJECTED, code) |
| FIX SessionStatus 8 | onLogon | password rotate via 554/925 |
| FIX SeqNum gap | QuickFIX | `ResendRequest` automatic; replay session via `GapFillFlag=Y`/`PossDupFlag=Y` |
| Throttle exceeded | local | hold in queue; do not send; metric increment |
| MassQuote Reject (`R`, no OBID) | all-rejected | log full payload; scenario fails fast |
| Process crash | systemd | restart; SoupBinTCP login with last seen seqnum; FIX with stored sequences |

Sequence persistence: rolling file `state/oe.seq`, `state/dc.seq`, `state/rd.seq`. Atomic writes (rename(2)). Replayed on startup.

## 14. Testing strategy

### Unit (`tests/unit/`)
- `codec_test.cpp` — every Inbound/Outbound roundtrip with hex fixtures from spec
- `order_book_test.cpp` — replace LeavesQty math (Spec Example 1, 2)
- `throttler_test.cpp` — burst, refill, exact 100/s rate verification
- `token_registry_test.cpp` — uniqueness, recycle policy
- `soupbintcp_test.cpp` — heartbeat timer, login flow, reject handling

### Integration (`tests/integration/`)
- `mock_ouch_gateway.cpp` — accepts SoupBinTCP, replies to scripted scenarios; runs against real `OuchSession`
- `mock_fix_acceptor.cpp` — QuickFIX acceptor with BIST dictionary
- end-to-end scenario passes against mocks **before** real test environment

### Cert replay (`tests/cert_replay/`)
Each scenario YAML is run as a GoogleTest case. CI gate: every scenario must pass against mock gateway. Real environment runs are operator-driven.

### Sanitizer matrix
- Debug build: ASan + UBSan + `-fno-omit-frame-pointer`
- TSan build: separate target for thread safety on shared state
- Release build: `-O3 -DNDEBUG -march=native -flto`

## 15. Build & run

```bash
# Bootstrap
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Test
ctest --test-dir build --output-on-failure

# Run cert replay against mock
./build/bist_colo --config config/bist_prod_like.toml --mock --replay scenarios/ouch_bolum2_emir_iletim.yaml

# Run interactive against real test env (after VPN)
./build/bist_colo --config config/bist_prod_like.toml --interactive

# Run full cert suite (operator)
./build/bist_colo --config config/bist_prod_like.toml --replay scenarios/
```

## 16. Audit trail

Every sent and received message:
```
2026-05-05T07:32:11.123456789Z  SENT  OUCH-OE  partition=1  seq=423  type=O  hex=4f3030303030303031303030303030...
2026-05-05T07:32:11.123987654Z  RECV  OUCH-OE  partition=1  seq=856  type=A  hex=413132333435363738393030303031...
```
Rotated daily, kept 90 days. Cert auditors get `audit/<date>.log` + decoder tool (`apps/tools/ouch_decode`).

## 17. Out of scope (Phase 2+)

- ITCH market data feed (read-only TOB needed for adaptive market making strategies)
- Kernel-bypass (Solarflare ef_vi / DPDK) — interface exists, impl Phase 2
- VIOP, BAP, KMTP markets (only Pay Piyasası in Phase 1)
- T+1 specific session tweaks (test env supported via config; certification later)
- FIX RD İleri Seviye request-reply (infrastructure ready, scenarios deferred)
- Trade Report (Özel İşlem Bildirimi) — FIX OE İleri Seviye scope
- Strategy/algorithm logic — this is a connectivity client; strategies subscribe via API

## 18. Success criteria

- [ ] All cert scenarios in `scenarios/` pass against mock gateway in CI
- [ ] Manual run against BIST Pre-Prod test env (10.57.3.57): all 30+ test steps reach expected message
- [ ] Operator runs against Prod-Like env (10.57.3.146) for cert day: full pass, audit log delivered
- [ ] BIST issues OUCH + FIX OE Temel + FIX RD Temel sertifikası
- [ ] Hot path 99-percentile latency (mock, localhost) < 50µs end-to-end (place → ack)
- [ ] Zero allocations on hot path post-warmup (verified with allocator hook)

## 19. Risks

| Risk | Mitigation |
|------|------------|
| BIST OUCH wire layout has undocumented variations | Run against test env early; codec has tagged-debug mode that hex-dumps + parses both ways |
| QuickFIX BIST dictionary needs custom field tweaks | Dictionary lives under `include/bist/fix/data_dict/`; iterate in CI mock first |
| Co-location server not yet provisioned | Phase 1 develops on dev machine; binary cross-compiled `-march=skylake` deployed via SCP |
| VPN only available on operator's machine | Operator runs cert; remote pair via SSH+tmux |
| Test env hesabı / şifre / OrderBookID listesi cert günü değişir | TOML config + `--instruments-from-fix-rd` mode auto-discovers |

---

End of design spec. Implementation plan to follow via `superpowers:writing-plans`.
