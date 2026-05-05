# Cert-day / Production Runbook

Audience: operator running `bist_colo` against the BIST Pre-Prod, Prod-like,
or co-location gateway. Use this checklist verbatim — do not improvise.

## 1. One week before cert

- Schedule the cert call: `bistechsupport_autoticket@borsaistanbul.com`.
- Receive: `username`, `password`, AFK, OrderBookID list, FIX SenderCompID
  / TargetCompID, FIX 554/925 password rotation pair.
- Populate `config/bist_prod_like.local.toml` from the
  `.example` template; verify it is in `.gitignore`.

## 2. One day before cert (smoke)

```bash
# Build all three lanes from a clean tree.
rm -rf build build-debug build-fix audit state log
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBIST_ENABLE_SANITIZERS=ON
cmake --build build-debug -j 4
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-fix -DCMAKE_BUILD_TYPE=Release \
      -DBIST_BUILD_FIX=ON -DBIST_ENABLE_QUICKFIX=ON
cmake --build build-fix -j 4
ctest --test-dir build-fix --output-on-failure
```

Baseline: 67 / 67 unit tests green. If anything fails, do not connect.

```bash
# Mock dry-run — must be 8 / 8 OUCH scenarios + 1 / 1 FIX RD + 1 / 1 FIX OE.
./build/apps/bist_colo --mock --replay scenarios/

# FIX dry-run uses the FIX-enabled binary.
./build-fix/apps/bist_colo --mock --replay scenarios/
```

VPN smoke against Pre-Prod (10.57.3.57) once credentials are loaded:

```bash
./build/apps/bist_colo --config config/bist_prod_like.toml \
                      --partition 1 --replay scenarios/ouch_bolum2_emir_iletim.yaml
```

The CoD reconnect deadline (default 30 s) prints on startup. It must be
shorter than the BIST inactivation window (55–62 s). Tighten via
`[ouch].cod_reconnect_deadline_ms` only after this dry-run survives a
forced VPN flap.

## 3. Cert day

```bash
# Full cert dry-run. Borsa Eksperi observes Trading Workstation + monitors
# mock acks while the live binary streams against Pre-Prod.
./build-fix/apps/bist_colo --config config/bist_prod_like.toml \
                           --partition 1 --replay scenarios/
```

Tail the audit log in another terminal:

```bash
tail -f audit/$(date -u +%F).audit.log
```

If a step FAILs, do not retry blindly:
1. Capture the per-step output.
2. Read the matching audit-log SENT/RECV pair.
3. Decide with the BIST Cert Eksperi whether the failure is a real cert
   failure or an infrastructure glitch (VPN flap, gateway overload, etc.).
4. Resume from the failing scenario only after both sides agree.

## 4. Cert package delivery

After the cert run completes, freeze the evidence package:

```bash
./scripts/audit_pack.sh /tmp/bist_cert_$(date -u +%Y%m%d) \
    --config config/bist_prod_like.toml \
    --bin build-fix/apps/bist_colo \
    --audit-dir audit \
    --note "BIST Pay Piyasası OUCH+FIX Temel Sertifikasyon, partition 1"
```

The script refuses to seal the package if any raw password appears in the
output — a hard guard before delivery. Email the sealed directory (or a
tarball thereof) to the Cert Eksperi. The package is reproducible from
the audit log alone, so the auditor can re-decode it independently.

## 5. Production day

- Resident mode (no `--replay`):

  ```bash
  ./build-fix/apps/bist_colo --config config/bist_prod_like.toml --partition 1
  ```

  Process polls in 5 ms cycles until SIGINT or SIGTERM is received; on
  signal it flushes the sequence store, requests a clean OUCH logout, and
  exits 0. Sequence persistence lives at
  `state/<env>_oe_p<N>.seq`; the next start resumes from that cursor.

- Gateway failover: the `LiveTransportControl` exposes
  `switch_to_secondary` to the runner, but operators may also restart
  with `--secondary` to dial `host_secondary` directly. CoD-aware
  reconnect kicks in automatically — the deadline applies across both
  primary and secondary attempts.

- Hard rule: **never edit `config/bist_prod_like.local.toml` while the
  resident process is running.** Stop, edit, restart.

## 6. After-action

- Archive `audit/`, `state/`, and `log/` together with the cert package.
- Tag the git commit used for the cert run: `git tag cert-YYYYMMDD-pX`.
- File any cert findings as GitHub issues; reference the audit-log line
  number in each.
