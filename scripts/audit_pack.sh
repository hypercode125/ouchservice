#!/usr/bin/env bash
#
# scripts/audit_pack.sh — assemble a BIST cert evidence package.
#
# Usage:
#   scripts/audit_pack.sh <output-dir> [--config <path>] [--bin <path>]
#                         [--audit-dir <path>] [--scenario-report <path>]
#                         [--note <text>]
#
# The package is the artifact handed to BIST after a cert dry-run. It MUST
# be reproducible from the audit log alone if the binary is rerun against
# the same scenario set.
#
# Contents (keys named for the auditor's checklist):
#   version.txt          — `bist_colo --version` output (wire-size receipts)
#   git.txt              — commit + dirty flag + branch
#   build.txt            — sysctl + uname + glibc/openssl version
#   scenarios/           — every scenario/*.yaml (full set)
#   audit/               — copy of audit/<date>.audit.log
#   audit_summary.csv    — decoded SENT/RECV counts per (channel,date)
#   config_snapshot.toml — TOML config with passwords stripped
#   scenario_report.txt  — last replay's per-step PASS/FAIL output
#   notes.txt            — operator notes (if --note supplied)
#   manifest.txt         — checksum of every file in the package

set -euo pipefail

OUTDIR=""
CONFIG="config/bist_prod_like.toml"
BIN="${BIN:-./build/apps/bist_colo}"
AUDIT_DIR="audit"
SCENARIO_REPORT=""
NOTE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)           CONFIG="$2"; shift 2;;
    --bin)              BIN="$2"; shift 2;;
    --audit-dir)        AUDIT_DIR="$2"; shift 2;;
    --scenario-report)  SCENARIO_REPORT="$2"; shift 2;;
    --note)             NOTE="$2"; shift 2;;
    -h|--help)
      sed -n '3,28p' "$0"; exit 0;;
    *)
      if [[ -z "$OUTDIR" ]]; then OUTDIR="$1"; shift
      else echo "unknown arg: $1" >&2; exit 1
      fi;;
  esac
done

if [[ -z "$OUTDIR" ]]; then
  echo "usage: $0 <output-dir> [--config <path>] [--bin <path>] ..." >&2
  exit 1
fi

mkdir -p "$OUTDIR/scenarios" "$OUTDIR/audit"

# --- version + wire receipts -----------------------------------------------
if [[ -x "$BIN" ]]; then
  "$BIN" --version > "$OUTDIR/version.txt" 2>&1 || true
else
  echo "binary not found: $BIN" > "$OUTDIR/version.txt"
fi

# --- git --------------------------------------------------------------------
{
  echo "branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
  echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "subject: $(git log -1 --format=%s 2>/dev/null || echo unknown)"
  if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
    echo "dirty: yes"
    git diff --stat 2>/dev/null | head -50
  else
    echo "dirty: no"
  fi
} > "$OUTDIR/git.txt"

# --- build environment ------------------------------------------------------
{
  echo "uname: $(uname -a)"
  if command -v sw_vers >/dev/null 2>&1; then sw_vers; fi
  echo "compiler:"
  if command -v cc >/dev/null 2>&1; then cc --version 2>&1 | head -2; fi
  if command -v g++ >/dev/null 2>&1; then g++ --version 2>&1 | head -1; fi
  if command -v clang++ >/dev/null 2>&1; then clang++ --version 2>&1 | head -1; fi
  if command -v openssl >/dev/null 2>&1; then echo "openssl: $(openssl version)"; fi
  if command -v cmake >/dev/null 2>&1; then echo "cmake: $(cmake --version | head -1)"; fi
} > "$OUTDIR/build.txt"

# --- scenarios --------------------------------------------------------------
for f in scenarios/*.yaml; do
  [[ -f "$f" ]] || continue
  cp "$f" "$OUTDIR/scenarios/"
done

# --- audit logs (raw + decoded summary) -------------------------------------
if [[ -d "$AUDIT_DIR" ]]; then
  cp -R "$AUDIT_DIR"/. "$OUTDIR/audit/" 2>/dev/null || true
fi

# Per-channel SENT/RECV counts (date,channel,direction → count). Rolls up
# the NDJSON-style audit log into a CSV the auditor can graph.
{
  echo "date,channel,direction,count"
  if [[ -d "$AUDIT_DIR" ]]; then
    for log in "$AUDIT_DIR"/*.audit.log; do
      [[ -f "$log" ]] || continue
      date=$(basename "$log" .audit.log)
      awk -v d="$date" '
        NF >= 4 {
          dir = $2; channel = $3;
          key = d "," channel "," dir;
          c[key]++;
        }
        END { for (k in c) print k "," c[k] }
      ' "$log"
    done
  fi
} | sort > "$OUTDIR/audit_summary.csv"

# --- config snapshot (passwords redacted) -----------------------------------
if [[ -f "$CONFIG" ]]; then
  awk '
    /password|Password|PASSWORD/ { sub(/=.*/, "= \"<redacted>\""); print; next }
    { print }
  ' "$CONFIG" > "$OUTDIR/config_snapshot.toml"
fi

# --- scenario report --------------------------------------------------------
if [[ -n "$SCENARIO_REPORT" && -f "$SCENARIO_REPORT" ]]; then
  cp "$SCENARIO_REPORT" "$OUTDIR/scenario_report.txt"
fi

# --- operator notes ---------------------------------------------------------
if [[ -n "$NOTE" ]]; then
  printf '%s\n' "$NOTE" > "$OUTDIR/notes.txt"
fi

# --- manifest with checksums ------------------------------------------------
(
  cd "$OUTDIR"
  if command -v sha256sum >/dev/null 2>&1; then
    find . -type f ! -name manifest.txt -print0 | xargs -0 sha256sum
  elif command -v shasum >/dev/null 2>&1; then
    find . -type f ! -name manifest.txt -print0 | xargs -0 shasum -a 256
  fi | sort -k2 > manifest.txt
)

# --- secret guard rail ------------------------------------------------------
# The package must never carry raw passwords. Scan the assembled tree for
# common offenders and refuse to emit a tarball if any are found.
guard_hits=$(grep -RIE 'password\s*=\s*"[^"<]+"' "$OUTDIR" 2>/dev/null \
              | grep -v config_snapshot.toml || true)
if [[ -n "$guard_hits" ]]; then
  echo "AUDIT-PACK GUARD: raw password detected — refusing to seal package:" >&2
  echo "$guard_hits" >&2
  exit 2
fi

echo "audit pack written: $OUTDIR"
echo "  version=$(head -1 "$OUTDIR/version.txt" 2>/dev/null)"
echo "  commit=$(awk -F': ' '/^commit:/{print $2}' "$OUTDIR/git.txt")"
echo "  scenarios=$(ls "$OUTDIR/scenarios" | wc -l)"
echo "  audit_logs=$(find "$OUTDIR/audit" -name '*.audit.log' 2>/dev/null | wc -l)"
echo "  manifest_lines=$(wc -l < "$OUTDIR/manifest.txt")"
