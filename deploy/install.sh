#!/usr/bin/env bash
#
# deploy/install.sh — install bist_colo on a co-location server.
#
# Run as root on the target host after extracting the deployment tarball.
# Idempotent: safe to re-run for upgrades.
#
# Layout produced:
#   /opt/bist-colo/bin/         binaries (bist_colo, preflight, audit_pack.sh,
#                               scenario_lint, ouch_decode, latency_bench)
#   /opt/bist-colo/scenarios/   cert YAMLs
#   /opt/bist-colo/docs/        RUNBOOK + plan
#   /etc/bist-colo/             config files (bist_prod_like.toml + .local.toml)
#   /var/lib/bist-colo/state/   sequence persistence
#   /var/log/bist-colo/audit/   NDJSON audit logs
#   /var/log/bist-colo/log/     spdlog runtime logs

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "install.sh must run as root" >&2
  exit 1
fi

TARBALL_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_OPT="/opt/bist-colo"
DEST_ETC="/etc/bist-colo"
DEST_LIB="/var/lib/bist-colo"
DEST_LOG="/var/log/bist-colo"
SVC_USER="bistcolo"
SVC_GROUP="bistcolo"

echo "[1/7] create service user $SVC_USER"
if ! id -u "$SVC_USER" >/dev/null 2>&1; then
  useradd --system --home "$DEST_LIB" --shell /usr/sbin/nologin \
          --user-group "$SVC_USER"
fi

echo "[2/7] create directory tree"
install -d -m 0755 "$DEST_OPT/bin" "$DEST_OPT/scenarios" "$DEST_OPT/docs"
install -d -m 0750 -o "$SVC_USER" -g "$SVC_GROUP" "$DEST_ETC"
install -d -m 0750 -o "$SVC_USER" -g "$SVC_GROUP" \
        "$DEST_LIB" "$DEST_LIB/state" \
        "$DEST_LOG" "$DEST_LOG/audit" "$DEST_LOG/log"

echo "[3/7] install binaries"
for bin in bist_colo preflight scenario_lint ouch_decode latency_bench; do
  if [[ -f "$TARBALL_ROOT/bin/$bin" ]]; then
    install -m 0755 "$TARBALL_ROOT/bin/$bin" "$DEST_OPT/bin/$bin"
  fi
done
install -m 0755 "$TARBALL_ROOT/bin/audit_pack.sh" "$DEST_OPT/bin/audit_pack.sh"

echo "[4/7] install scenarios + docs"
cp -R "$TARBALL_ROOT/scenarios/." "$DEST_OPT/scenarios/"
cp -R "$TARBALL_ROOT/docs/." "$DEST_OPT/docs/"

echo "[5/7] install config templates (DOES NOT OVERWRITE existing files)"
if [[ ! -f "$DEST_ETC/bist_prod_like.toml" ]]; then
  install -m 0640 -o "$SVC_USER" -g "$SVC_GROUP" \
          "$TARBALL_ROOT/config/bist_prod_like.toml" \
          "$DEST_ETC/bist_prod_like.toml"
fi
if [[ ! -f "$DEST_ETC/bist_prod_like.local.toml" ]]; then
  install -m 0640 -o "$SVC_USER" -g "$SVC_GROUP" \
          "$TARBALL_ROOT/config/bist_prod_like.local.toml.example" \
          "$DEST_ETC/bist_prod_like.local.toml"
  echo "  → edit $DEST_ETC/bist_prod_like.local.toml with member credentials"
fi
if [[ ! -f "$DEST_ETC/env" ]]; then
  cat > "$DEST_ETC/env" <<EOF
BIST_PARTITION=1
EOF
  chown "$SVC_USER:$SVC_GROUP" "$DEST_ETC/env"
  chmod 0640 "$DEST_ETC/env"
fi

echo "[6/7] install systemd unit"
install -m 0644 "$TARBALL_ROOT/deploy/bist-colo.service" \
        /etc/systemd/system/bist-colo.service
systemctl daemon-reload

echo "[7/7] done"
echo
echo "Next steps:"
echo "  1. Edit $DEST_ETC/bist_prod_like.local.toml — fill member creds"
echo "  2. Adjust BIST_PARTITION in $DEST_ETC/env (default 1)"
echo "  3. Run preflight by hand:"
echo "       sudo -u $SVC_USER $DEST_OPT/bin/preflight \\"
echo "             --config $DEST_ETC/bist_prod_like.toml"
echo "  4. Enable + start the service:"
echo "       systemctl enable --now bist-colo"
echo "  5. Tail logs:"
echo "       journalctl -u bist-colo -f"
echo "       tail -f $DEST_LOG/audit/\$(date -u +%F).audit.log"
