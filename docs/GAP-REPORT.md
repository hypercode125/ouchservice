# GAP-REPORT — bist-colo-client vs current BIST PDFs

- Date: 2026-05-05
- Author: protocol-audit pass (parallel agent)
- Scope: every PDF in `ouchpdfs/` + `~/Desktop/OUCH_ProtSpec_BIST_va2413.pdf`
- Question answered: "Do our services match the current BIST specs? Will the system work flawlessly?"

**Bottom line: NO.** Project is built against BIST OUCH **Release 2.11 (May 2020)**, but the authoritative current spec is **BISTECH OUCH Protocol Specification, published 06 February 2025**. Two CRITICAL wire-level bugs will desync framing on the very first OrderAccepted; one CRITICAL bug will misparse every MassQuote ack. Cert program coverage is partial. See severity-tagged findings below.

---

## 0. Spec / cert version inventory

| Document | Version found | Location | Source-of-truth date |
|----------|---------------|----------|---------------------|
| BISTECH OUCH Protocol Specification | (no R-number printed) — **Publish 06 February 2025** | `ouchpdfs/BISTECH OUCH Protocol Specification.pdf` | 2025-02-06 |
| OUCH Protocol Spec (legacy) | **Release 2.11**, May 2020 | `~/Desktop/OUCH_ProtSpec_BIST_va2413.pdf` | 2020-05 |
| Equity Market BISTECH OUCH Cert Program | **v1.7**, August 2024 | `ouchpdfs/Equity Market OUCH Certification Program v5.0.pdf` (PDF filename misleading; doc body says v1.7) | 2024-08 |
| Pay Piyasası OUCH Sertifikasyon Programı (TR) | **Sürüm 1.7**, Ağustos 2024 | `ouchpdfs/bistech-ouch-sertifikasyon-programi.pdf` | 2024-08 |
| Derivatives Market BISTECH OUCH Cert Program | **v1.8**, April 2026 | `ouchpdfs/Derivatives Market OUCH Certification Program.pdf` | 2026-04 |
| Procedure for Cancel On Disconnect | (no version) — dated **April 20, 2026** | `ouchpdfs/CancelOnDisconnect Annotation.pdf` | 2026-04-20 |
| The Concept of Throttling FIX & OUCH | (no version) — **2022** | `ouchpdfs/The Concept of Throttling FIX & OUCH API.pdf` | 2022 |

Project's locked baseline (per `STATUS.md` and `messages.hpp` headers): "BIST OUCH Spec R2.11 (1 July 2020)". **Stale by ~5 years for protocol and ~21 months for cert program.**

---

## 1. CRITICAL bugs — system will not work flawlessly as-is

### 🔴 C1. `OrderAccepted` struct is 135 B, spec mandates 137 B
- File: `include/bist/ouch/messages.hpp:172-194`
- Spec (Feb 2025) Table 7 ends at offset **134 + 3 = 137** because it now carries `SMP Level (132,1) + SMP Method (133,1) + SMP ID (134,3)` after `OffHours`. R2.11 ended at `Reserved (132,3)` = 135.
- Our `OrderAccepted` keeps the 135-B layout and the static_assert at line 193 enforces 135.
- Effect: `dispatch_ouch()` in `session.hpp:319` does `if (payload_len != sizeof(OrderAccepted)) break;` and falls through to `"OUCH A size mismatch"`. **Every OrderAccepted from prod or pre-prod will be rejected by the parser**, no fills will be acked, the test fails immediately at the first order.
- Fix: append `std::uint8_t smp_level; std::uint8_t smp_method; char smp_id[3];` and bump the assert to 137. Update `OrderReplaced` only if it grows too — see C4 (it does NOT — Replace Order does not carry SMP fields per spec, and the outbound 'U' Order Replaced echoes Replace Order, not Enter Order).

### 🔴 C2. `MassQuoteAck` field offsets reversed
- File: `include/bist/ouch/messages.hpp:285-300`
- Spec (Feb 2025) Table 12 layout:
  ```
  0  (1)  message_type 'K'
  1  (8)  timestamp_ns
  9  (14) order_token
  23 (4)  order_book_id
  27 (8)  quantity              (leaves)
  35 (8)  traded_quantity
  43 (4)  price
  47 (1)  side
  48 (4)  quote_status
  total = 52 B
  ```
- Our struct lays it out as `order_book_id, side, quote_status, quantity, traded_quantity, price`. Total bytes match (52 B), but **every field after offset 23 is wrong**.
- Effect: every MassQuote acknowledgement is misparsed: `quote_status` reads garbage, `traded_quantity` is read where `quote_status` lives, etc. Bölüm 3 (market making) cert step fails on step 1.
- Fix: reorder fields and update offset asserts.

### 🔴 C3. `EnterOrder` bytes 107–113 mislabeled
- File: `include/bist/ouch/messages.hpp:67-95`
- Spec (Feb 2025) Table 2: bytes 107..111 = `SMP Level(1) + SMP Method(1) + SMP ID(3)`, bytes 112..113 = Reserved. Total 114 B (unchanged).
- Our struct has `reserved[7]` at offset 107.
- Effect on the wire: total length is fine (still 114 B) and zero-filled SMP fields are equivalent to "Empty / no SMP" per spec. **Sending works, BUT** receiving any system that echoes the SMP fields back (e.g. Order Accepted) will parse correctly only after C1 is fixed.
- Decision needed: do we need to support self-match prevention for cert? Cert v1.7 (Aug 2024) does not exercise SMP; cert can pass with zero SMP. Still: re-name `reserved[7]` → `smp_level / smp_method / smp_id / reserved[2]` so future enablement doesn't require a wire-layout patch.

### 🔴 C4. `OrderReplaced` outbound — verify against spec
- File: `include/bist/ouch/messages.hpp:218-249`
- Cross-checked: spec Table 9 ends at `Client Category (144,1)` = 145 B. Matches our struct ✅. **No bug**, but spec adds `Order State = 99 ("OUCH order ownership lost")` to the enum on this message. Our `types.hpp:OrderState::OuchOwnershipLost = 99` ✅ already included — good.

### 🔴 C5. Throttler is token-bucket, BIST is 10×100 ms sliding window
- File: `include/bist/domain/throttler.hpp` (whole file)
- "The Concept of Throttling FIX & OUCH API" (2022) p.7: BIST runs **ten 100 ms sliding windows** rolling backward; the limit applies to the SUM across the last 10 windows. Our token bucket is monotonic refill, fundamentally different.
- Concrete divergence from the PDF example (p.9):
  - Window seq 1..10 carries: 30, 56, 14, 0, 0, 0, 0, 0, 0, 0 → sum=100 (at quota)
  - At window 11 the user sends 100 more → BIST rejects 70 of them because windows 2 and 3 still total 70.
  - A token bucket with rate=100/s, capacity=100 would, by t=1.0s, have ≥100 tokens (exactly 100 at perfect refill, more if briefly idle), so it would happily accept all 100 → **runaway against the BIST gateway, which then closes the OUCH session per the spec**.
- Effect on cert: Bölüm 2 throttling test (1000 orders, 10 s, 100/s) probably masquerades as passing because the test deliberately paces at 100/s, but **production bursty load will trip the BIST gateway and disconnect us**, which under CoD triggers order inactivation 55–62 s later (see C6).
- Fix: replace `Throttler` with a 10-slot ring of (window_start_ns, count) and accept iff sum_last_10 < limit. Keep the same public API.

### 🔴 C6. No CoD-aware reconnect policy
- "Procedure for Inactivating Orders Automatically with the CoD Functionality" (April 20, 2026) §2 (OUCH API): **GW closes session 15 s after last activity; ME inactivates orders after a further 40–47 s; total 55–62 s.** If we re-logon before that window expires, no inactivation occurs.
- Our SoupBinTCP watchdog declares dead at 2.5 s (`net/soupbintcp.hpp:60`), which is fine for fast detection. But there is **no documented retry/backoff policy that guarantees we are back inside the 55 s window** — Phase 2 task "VPN-aware connect with retry/backoff" in `STATUS.md` is still TODO.
- Fix: add a `cod_reconnect_deadline_ns` parameter (default 30 s) to the reconnector; on disconnect, retry with exponential backoff but cap total elapsed time so we re-logon before the 55 s ME inactivation begins. Document in run book.

---

## 2. HIGH-severity issues

### 🟠 H1. `EnterOrder` `Open Close` enum semantics changed
- File: `include/bist/core/types.hpp:OpenClose`
- Feb 2025 Table 2 (Enter Order) defines `0 = Default for the account, 1 = Open, 2 = Close/Net`. The R2.11-era value `4 = Default for the account` is **gone from Enter Order** but is still on Replace Order with the *opposite* semantics (Replace `0 = No change, 4 = Default`).
- Our enum names `DefaultForAccount = 0` and `ReplaceDefault = 4` reflect the post-change Enter Order semantics, which is correct now, but the comment does not flag the per-message semantic difference and could mislead callers.
- Fix: split into `OpenCloseEnter` and `OpenCloseReplace` enums OR document the difference loudly at the call site.

### 🟠 H2. `OrderCanceled` Reason codes drifted
- File: `include/bist/ouch/messages.hpp:251-262`, but more importantly the *enum* should live in `types.hpp` (does not exist today; reasons are raw `uint8_t`).
- Feb 2025 Table 10 reason codes (extract): `1 = Canceled by user / Canceled by Other User, 3 = Trade, 4 = Inactivate, 5 = Replaced by User, 6 = New, 8 = Converted by System, 9 = Canceled by System, 10 = …` (truncated in PDF). R2.11 had: `1 Canceled by user, 3 Deal, 4 Order inactivated, 5 Order altered, 6 Order added or activated, 7 Market order converted, 8 Order price changed, 9 Canceled by system, 10 Canceled by proxy, 12 …`.
- Effect: scenario assertions like `fields: { reason: 1 }` (`scenarios/ouch_bolum2_emir_iletim.yaml:51`) still match for "user cancel". Other reason values are now different; any logic that matches on Reason==4 ("Inactivate" now vs "Order inactivated" before) needs review.
- Fix: introduce `enum class CancelReason : std::uint8_t` in `types.hpp` and mirror the Feb 2025 catalog. Re-extract the full table from the PDF (only first 8 codes were visible in the extracted text — see Action items).

### 🟠 H3. `MassQuoteAck` semantics: spec emits TWO 'K' messages per accepted entry (bid + offer), not one
- File: `include/bist/ouch/session.hpp:dispatch_ouch` and `apps/bist_colo.cpp` callers
- Spec §4.2 / Workflow Appendix A: "If the individual quote in mass quote is accepted, system send two Mass Quote Acknowledgement messages for bid and offer side."
- Our handler `Handlers::on_mass_quote_ack` is a single per-message callback, which is correct; the issue is that scenario expectations need `occurrences: 2` (already done in `bolum3:step 4`) and the Order Book bookkeeping must distinguish Side='B' vs Side='S' on each ack — verify `domain/order_book.hpp` does (post-C2 fix).

### 🟠 H4. Cert scenarios cover ~30 % of the cert program
- Files: `scenarios/ouch_bolum1_baglanti.yaml`, `ouch_bolum2_emir_iletim.yaml`, `ouch_bolum3_piyasa_yapicilik.yaml`
- Equity OUCH Cert v1.7 has SECTION 1 (5 steps), SECTION 2 (15+ steps incl. opening, continuous auction with three replace-leaves variations on GARAN/KAREL/VAKKO, throttling, failover, EOD), SECTION 3 (8 steps, market maker).
- Our coverage:
  - ✅ Section 1: full (5/5)
  - ⚠️ Section 2: **5/15** — only the opening-auction ADEL.E block is encoded. Continuous auction (GARAN/KAREL/VAKKO replace + leaves recalc), throttling (1000 orders @100/s), failover to secondary GW, EOD batch are **all absent**.
  - ✅ Section 3: full (4/4 of the documented MM scenarios — minor gap on `quote_status` per-side decomposition, noted in YAML comment at step 12)
- Derivatives Cert v1.8 (April 2026): **0/22** — no scenario file exists. Has different account (DE-1), Position=Open/Close/Default semantics, a Paused-state Order Replaced test (step 5), and an inter-month spread step that exercises **negative price** (`SELL -0.12 TL` → step 11.ii of Deriv cert).
- Fix: write `ouch_bolum2_surekli_acilis.yaml`, `ouch_throttling.yaml`, `ouch_failover.yaml`, `ouch_eod_batch.yaml`, `ouch_vadeli_cert.yaml`.

### 🟠 H5. Order Token encoding from YAML
- Files: `src/runner/scenario_loader.cpp`, `include/bist/runner/scenario.hpp`
- Cert PDFs use plain integers like `Order Token=10`, `Order Token=10000`. Spec mandates **Alpha(14), left-justified, space-padded**.
- Our `OrderToken(std::string_view s)` constructor pads with spaces, which works iff the loader passes the digits as a string. Need to confirm `scenario_loader.cpp` does `std::to_string(int)` first rather than embedding raw integers — otherwise the tokens go on the wire as `0x0000…0a` instead of `"10            "` (12 spaces).
- Verify: `scenarios/ouch_bolum2_emir_iletim.yaml:20` writes `token: 10`. YAML parses that as int 10. The loader must stringify before constructing the OrderToken, AND the on-wire bytes must match what the cert reviewer's tool decodes — they test for "Order Token=10" in the audit log.
- Action: add a unit test that loads a scenario, encodes one EnterOrder, and asserts wire bytes 1..14 equal `"10\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"`.

---

## 3. MEDIUM-severity issues

### 🟡 M1. AFK case sensitivity
- Spec (Feb 2025) Mass Quote table values: `PYM`, `PYP`, **uppercase**. R2.11 spec showed `pym`, `pyp` lowercase. Cert v1.7 Section 3 says `PartyID of MassQuote messages must be "PYM"`.
- Our `core/types.hpp:afk` uses uppercase `PYM`, `PYP`, `XRM` ✅. The R2.11 lowercase variant is gone. **No bug** — flagging to confirm during cert.

### 🟡 M2. SoupBinTCP heartbeat send cadence
- Spec / cert: server sends ServerHeartbeat after 1 s of inactivity; client must do the same.
- We send at **800 ms** (`net/soupbintcp.hpp:kHeartbeatSendNs`), which is conservative-correct.
- Spec also implies client must not allow >1 s of silence outbound. 800 ms is safe; on a heavily loaded hot thread, monotonic_ns() is sampled every reactor tick — confirm tick rate so we never miss the deadline. Add a unit test that drives a 1.5 s idle period and asserts exactly one ClientHeartbeat is emitted.

### 🟡 M3. Login Rejected reason codes not exhaustive
- Spec lists `'A' = Not Authorized` and `'S' = Session not Available`. Cert step 4 specifies `Reject Reason Code=A`. Our session prints whatever byte arrives; no mapping, no enum. Acceptable for cert but should grow into an enum for production telemetry.

### 🟡 M4. `MassQuote` builders cover cancel but not "leave one side unchanged"
- Spec: `bid_size = 0` AND `bid_price ≠ 0` → leave bid unchanged. `bid_size = 0` AND `bid_price = 0` → cancel bid.
- `codec.hpp:quote_cancel_bid` correctly sets BOTH to zero. Missing: `quote_unchanged_bid(QuoteEntry&, OrderBookId, PriceInt offer_px, Quantity offer_size)` that uses the most recent bid_price echoed back from the system. Documenting this distinction in the codec header would prevent a foot-gun.

### 🟡 M5. Derivatives Position field handling
- Deriv Cert v1.8 step 1.ii: `Position = Close`. Step 1.viii: `Position = Default`. Spec Table 2 maps Position (Open Close field) values 0/1/2 for derivs (no value 4). YAML scenario format does not yet have a `position:` arg key — every place call assumes `OpenClose::DefaultForAccount`. Add `position` arg, default 0.

---

## 4. LOW-severity / cosmetic / informational

- **L1.** `OrderRejected.reject_code` typed as `be_i32` (signed). Spec says "Numeric". BIST returns negative codes (e.g. -420131, -800002), so signed is correct in practice — keep.
- **L2.** `messages.hpp:MassQuoteAck` size assert at 52 B is correct per Feb 2025 Table 12. The comment block at lines 298-300 about "the 56-byte figure that appears in some BIST handouts" is misleading now that the field count is finalized — remove.
- **L3.** `STATUS.md` "Modules delivered" table cites "114 B EnterOrder, 122 B ReplaceOrder, 50+28×N MassQuote, 145 B OrderReplaced" as though these were universally correct. EnterOrder is 114 B in *both* R2.11 and Feb 2025 (length unchanged), but the field-byte usage differs (see C3). The narrative should make the spec-version dependency explicit.
- **L4.** `core/types.hpp:OrderState` already has `OuchOwnershipLost = 99` — good. But there is no symbol for the Replace-only `Paused = 98` test (Deriv Cert step 5). Already in enum ✅. Ensure scenario assertions can match `order_state: 98`.

---

## 5. What works (so we don't fix what isn't broken)

- ✅ SoupBinTCP framing (header, login req/accepted/rejected, heartbeat, end-of-session, sequenced/unsequenced data) byte sizes match spec.
- ✅ `EnterOrder`, `ReplaceOrder`, `CancelOrder`, `CancelByOrderId` total sizes (114 / 122 / 15 / 14 B) match Feb 2025 spec.
- ✅ `OrderRejected` (27 B), `OrderReplaced` (145 B), `OrderCanceled` (37 B), `OrderExecuted` (68 B), `MassQuoteRejection` (31 B), `MassQuoteHeader` (50 B), `QuoteEntry` (28 B): all match.
- ✅ Throttler **public API** (`try_acquire`, `available`, `capacity`) is reasonable — only the internal algorithm needs swapping.
- ✅ Session FSM stages (Disconnected → Connecting → LoggingIn → Active → Disconnecting → Failed) cover the cert flow.
- ✅ Audit hook (`on_audit_sent`/`on_audit_recv`) is in place to feed the NDJSON audit log; cert reviewer can read replay.
- ✅ Mass Quote Quote Matrix builders (`quote_new_two_sided`, `quote_cancel_two_sided`, `quote_cancel_bid`, `quote_cancel_offer`) match Appendix B intent — modulo the missing "unchanged-side" helper (M4).

---

## 6. Action plan to reach "flawless"

Ordered by blast radius. Items 1-3 are blocking; 4-7 are needed for full cert; 8+ are hardening.

1. 🔴 Fix `OrderAccepted` size: 135 → 137 B + add SMP fields (`messages.hpp`) + update mock gateway emitter (`src/mock/ouch_gateway.cpp`) + size assert. (~30 min)
2. 🔴 Fix `MassQuoteAck` field order: move `qty/traded_qty/price` before `side/quote_status` (`messages.hpp`) + mock emitter + tests. (~30 min)
3. 🔴 Rename `EnterOrder.reserved[7]` to `smp_level / smp_method / smp_id[3] / reserved[2]` (`messages.hpp`). Optional: pipe SMP through `OuchClient::place()`. (~15 min if just renaming)
4. 🔴 Replace `Throttler` token bucket with 10 × 100 ms sliding window (`domain/throttler.hpp`) + property test against the PDF p.9 example. (~1 h)
5. 🔴 Add CoD-aware reconnector: deadline-bounded backoff that re-logons inside 30 s of disconnect (`net/reactor.hpp` or new `net/reconnector.hpp`). Wire into `apps/bist_colo.cpp`. (~2 h incl. integration test)
6. 🟠 Re-extract full `OrderCanceled` Reason code table from the Feb 2025 PDF (text was truncated mid-table) + add `enum class CancelReason` to `types.hpp`. (~30 min PDF + 15 min code)
7. 🟠 Fill cert-scenario gaps: `ouch_bolum2_surekli.yaml` (continuous auction with 3 replace-leaves cases), `ouch_throttling.yaml`, `ouch_failover.yaml`, `ouch_eod_batch.yaml`, `ouch_vadeli_cert.yaml` (full Deriv cert). (~3 h scenario YAML + scenario_loader extensions for new actions)
8. 🟡 Confirm `scenario_loader.cpp` stringifies integer tokens before constructing `OrderToken`; add unit test for wire-byte token encoding.
9. 🟡 Document Open Close enum semantic difference between Enter and Replace; consider splitting into two enums.
10. 🟡 Add `quote_unchanged_bid` / `quote_unchanged_offer` helpers to codec; document the cancel-vs-unchanged distinction.
11. 🟡 Move `STATUS.md` "Decisions locked" to point at Feb 2025 spec, not R2.11.

---

## 7. Method note

PDFs were extracted to `/tmp/ouch_extract/*.txt` via `pypdf` 6.10.2 (no `pdftotext` on macOS). Field-level deltas were derived from the byte tables on PDF pages 9-22 of `BISTECH OUCH Protocol Specification.pdf` (Feb 2025) and pages 9-22 of `OUCH_ProtSpec_BIST_va2413.pdf` (May 2020 R2.11). Cert step counts taken from `Equity Market OUCH Certification Program v5.0.pdf` (v1.7 inside) and `Derivatives Market OUCH Certification Program.pdf` (v1.8). Throttling algorithm from `The Concept of Throttling FIX & OUCH API.pdf` (2022). CoD timing from `CancelOnDisconnect Annotation.pdf` (April 20, 2026).

If any of the field-by-field offsets above look off, re-run the extractor — pypdf occasionally interleaves table cells across columns, especially on the Mass Quote QuoteSet repeating-group page. Spot-checked offsets against R2.11 PDF cross-reference; deltas reported here are the ones consistent in BOTH passes.
