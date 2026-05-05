# Known Limitations and Scope

Last reviewed: 2026-05-05

This document is the authoritative scope statement for `bist-colo-client`.
Any work outside the items listed under **In Scope** is explicitly excluded
and requires a new project / new readiness plan before being considered.

## In Scope (certification target)

The system targets BIST **Pay Piyasası (Equity Market)** certification only:

- **OUCH** (BISTECH OUCH Spec, Feb 2025 baseline)
  - Bölüm 1 — Sistem Bağlantı (multi-login, heartbeat, logout)
  - Bölüm 2 — Emir İletim (open auction, continuous auction, throttling,
    gateway failover, EOD batch cancellation)
  - Bölüm 3 — Piyasa Yapıcılık (Mass Quote, quote status, drop copy
    correlation)
- **FIX 5.0 SP2 OE Temel** (NewOrderSingle, OrderCancelRequest,
  OrderCancelReplaceRequest, TradeCaptureReport, logout, resend, sequence
  reset, 554/925 password change)
- **FIX 5.0 SP2 RD Temel** (ApplicationMessageRequest subscribe + duplicate
  ApplReqID rejection)
- **Drop Copy** consumption of ExecutionReport, TradeCaptureReport, and
  QuoteStatusReport for cross-checks during cert scenarios

## Out of Scope (explicit exclusions)

The following BIST programs and features are **not** in scope. The codebase
contains no production-quality implementation, no scenario coverage, and no
runtime validation for them. Do not claim certification readiness for any
item below.

### 1. Vadeli İşlem ve Opsiyon Piyasası (VIOP / Derivatives)

- The Derivatives OUCH protocol differs from Equity OUCH on:
  - 6-partition gateway layout vs 4-partition Equity
  - `Position` field on `EnterOrder` / `ReplaceOrder`
  - Different `OrderType` codes (8 / 16 / 32)
  - Different `MassQuote` semantics (per-partition routing)
- Wire structs in `include/bist/ouch/messages.hpp` are sized and laid out for
  the Equity baseline. Adding VIOP requires either a parallel struct family
  or a runtime branch that has not been designed.
- No `scenarios/ouch_vadeli_*.yaml` exists. The cert program for VIOP is a
  separate document (`Derivatives Market OUCH Certification Program`) and
  has not been encoded into the runner.
- BIST member firms are credentialed separately for Equity and VIOP — the
  same `DE-1` / `123456` test account does not necessarily span both.

If VIOP is required, open a new milestone: scope a derivatives readiness
plan, add the `Position` field, write the Derivatives v1.8 cert YAML, and
provision separate VIOP test credentials.

### 2. ITCH / GLIMPSE / MoldUDP64 (Market Data)

The client is order-entry only. It does not consume BIST market data feeds.
Reference data (instrument master, tick/lot tables) is loaded from FIX RD,
not from ITCH snapshots or GLIMPSE recovery.

### 3. PTRM REST API beyond the local gate

`docs/superpowers/specs/...` describes a local PTRM gate for risk types
RX_*. The full BIST PTRM REST/JWT API (15 risk types, throttle tiers,
cancel-reason 115–125) is **not** integrated. The local gate is a
defensive pre-trade check, not a replacement for the BIST PTRM service.

### 4. Multi-firm / multi-account routing

The binary connects as a single member with a single account at a time.
Multi-firm aggregation, broker-of-broker layering, and tenant isolation are
out of scope.

### 5. Exchange resiliency beyond primary/secondary OUCH gateway

- Cross-DC failover, hot-standby BIST-side member infrastructure, and
  application-level replay across heterogeneous gateway versions are out
  of scope.
- The supported failover model is the BIST-documented primary →
  secondary OUCH gateway switch on the same partition.

### 6. Stateful order persistence across binary restarts

When the binary restarts, the local OrderBook is rebuilt from the cert
audit log, not from a durable database. There is no warm-restart story
for production trading — the binary is expected to stay resident through
the trading day, and the operator runbook treats restart as a
controlled-recovery event.

## Operational Limitations

- **Live BIST connectivity** — requires BIST member credentials
  (`DE-1` / `123456` for Pre-Prod, member-specific for cert/prod), VPN
  access, and Pre-Prod IP allowlisting (`10.57.3.57:25502`). The
  binary runs entirely offline against `--mock`; live mode has been
  designed but not yet exercised against a real BIST endpoint.
- **Two consecutive cert-day dry runs** — required by the
  `BIST_10_10_READINESS_PLAN.md` final go/no-go checklist before
  claiming live certification readiness. Not yet executed.
- **Operator runbook validation** — `docs/RUNBOOK.md` has been
  authored by the implementer and not yet executed end-to-end by an
  independent operator.

## Status Snapshot (2026-05-05)

- Codebase technical foundation: **9.5 / 10** (mock-only)
- Equity certification readiness: **8 / 10** — gated on live dry-run
  + independent operator runbook walkthrough
- VIOP (Derivatives) certification readiness: **out of scope**
