#!/usr/bin/env bash
#
# scripts/build_deploy.sh — produce a single tarball ready to drop on a
# co-location server. Output: <out-dir>/bist-colo-<version>-<commit>.tar.gz
#
# Usage:  scripts/build_deploy.sh [<out-dir>]   (default: dist/)
#
# Steps:
#   1. clean Release + FIX-enabled builds
#   2. ctest on both lanes
#   3. scenario_lint clean
#   4. mock cert dry-run on both lanes
#   5. assemble deploy tree:
#        bin/         bist_colo (FIX-enabled), preflight, scenario_lint,
#                     ouch_decode, latency_bench, audit_pack.sh
#        scenarios/   every cert YAML
#        config/      bist_prod_like.toml + bist_prod_like.local.toml.example
#        deploy/      bist-colo.service + install.sh
#        docs/        RUNBOOK + readiness plan
#        VERSION      git short-SHA + build date
#   6. tar + sha256

set -euo pipefail

OUT_DIR="${1:-dist}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUT_DIR"

VERSION=$(git -C "$ROOT" rev-parse --short=12 HEAD)
DATE=$(date -u +%Y%m%d)
PKG_NAME="bist-colo-${DATE}-${VERSION}"
STAGE="$OUT_DIR/$PKG_NAME"
TARBALL="$OUT_DIR/$PKG_NAME.tar.gz"
rm -rf "$STAGE" "$TARBALL"
mkdir -p "$STAGE/bin" "$STAGE/scenarios" "$STAGE/config" \
         "$STAGE/deploy" "$STAGE/docs"

echo "===> deploy build: $PKG_NAME"

# 1) clean Release build (no QuickFIX) — used by hosts that opt out of FIX.
echo "[1/6] clean Release build"
cmake -S "$ROOT" -B "$ROOT/build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBIST_ENABLE_QUICKFIX=OFF >/dev/null
cmake --build "$ROOT/build" -j

# 2) FIX-enabled build — primary deploy artifact.
echo "[2/6] FIX-enabled Release build"
cmake -S "$ROOT" -B "$ROOT/build-fix" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBIST_BUILD_FIX=ON \
      -DBIST_ENABLE_QUICKFIX=ON >/dev/null
cmake --build "$ROOT/build-fix" -j

# 3) test gate
echo "[3/6] ctest both lanes"
ctest --test-dir "$ROOT/build"     --output-on-failure
ctest --test-dir "$ROOT/build-fix" --output-on-failure

# 4) lint + mock replay sanity
echo "[4/6] scenario_lint + mock cert dry-run"
"$ROOT/build/apps/tools/scenario_lint" "$ROOT/scenarios"
"$ROOT/build-fix/apps/bist_colo" --mock --replay "$ROOT/scenarios/"

# 5) assemble deploy tree
echo "[5/6] assemble $STAGE"
install -m 0755 "$ROOT/build-fix/apps/bist_colo"               "$STAGE/bin/bist_colo"
install -m 0755 "$ROOT/build-fix/apps/tools/preflight"         "$STAGE/bin/preflight"
install -m 0755 "$ROOT/build-fix/apps/tools/scenario_lint"     "$STAGE/bin/scenario_lint"
install -m 0755 "$ROOT/build-fix/apps/tools/ouch_decode"       "$STAGE/bin/ouch_decode"
install -m 0755 "$ROOT/build-fix/apps/tools/latency_bench"     "$STAGE/bin/latency_bench"
install -m 0755 "$ROOT/scripts/audit_pack.sh"                  "$STAGE/bin/audit_pack.sh"

cp "$ROOT/scenarios"/*.yaml "$STAGE/scenarios/"
cp "$ROOT/config/bist_prod_like.toml"               "$STAGE/config/"
cp "$ROOT/config/bist_prod_like.local.toml.example" "$STAGE/config/"
cp "$ROOT/deploy/bist-colo.service" "$STAGE/deploy/"
cp "$ROOT/deploy/install.sh"        "$STAGE/deploy/"
chmod 0755 "$STAGE/deploy/install.sh"
cp "$ROOT/docs/RUNBOOK.md"                  "$STAGE/docs/"
cp "$ROOT/docs/BIST_10_10_READINESS_PLAN.md" "$STAGE/docs/"
cp "$ROOT/STATUS.md"                        "$STAGE/docs/"

cat > "$STAGE/VERSION" <<EOF
package:    $PKG_NAME
git commit: $(git -C "$ROOT" rev-parse HEAD)
git branch: $(git -C "$ROOT" rev-parse --abbrev-ref HEAD)
build date: $(date -u +%Y-%m-%dT%H:%M:%SZ)
build host: $(uname -a)
EOF

cat > "$STAGE/README.md" <<EOF
# bist-colo deployment package — $PKG_NAME

This tarball is the **only** artifact that needs to land on the co-location
server. Extract, run \`deploy/install.sh\` as root, edit
\`/etc/bist-colo/bist_prod_like.local.toml\` with member credentials, then:

    sudo -u bistcolo /opt/bist-colo/bin/preflight \\
        --config /etc/bist-colo/bist_prod_like.toml
    systemctl enable --now bist-colo
    journalctl -u bist-colo -f

See \`docs/RUNBOOK.md\` for the full cert-day procedure.
EOF

# 6) tar + checksum
echo "[6/6] tar + sha256"
tar -C "$OUT_DIR" -czf "$TARBALL" "$PKG_NAME"
if command -v sha256sum >/dev/null 2>&1; then
  (cd "$OUT_DIR" && sha256sum "$PKG_NAME.tar.gz" > "$PKG_NAME.tar.gz.sha256")
elif command -v shasum >/dev/null 2>&1; then
  (cd "$OUT_DIR" && shasum -a 256 "$PKG_NAME.tar.gz" > "$PKG_NAME.tar.gz.sha256")
fi
rm -rf "$STAGE"

echo
echo "deploy package ready:"
ls -la "$TARBALL" "$TARBALL.sha256"
echo
echo "On the colo server:"
echo "  scp $TARBALL bistcolo@<host>:~/"
echo "  ssh root@<host>"
echo "    tar xzf $PKG_NAME.tar.gz"
echo "    cd $PKG_NAME && deploy/install.sh"
