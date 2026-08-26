#!/usr/bin/env bash
# Drive DirectPackageInstaller's official headless CLI for one local PKG.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PS4=${PS4_IP:-192.168.1.4}
HOST=${HOST_IP:-}
PKG=${1:-}
DPI=${PS4CAST_DPI_BIN:-/Applications/DirectPackageInstaller.app/Contents/MacOS/DirectPackageInstaller.Desktop}
TIMEOUT=${PS4CAST_DPI_TIMEOUT:-180}
LOG=/tmp/ps4cast_dpi_cli.log

[ -n "$HOST" ] || { echo "HOST_IP is required." >&2; exit 2; }
[ -n "$PKG" ] || { echo "usage: $0 /absolute/path/to.pkg" >&2; exit 2; }
case "$PKG" in
  /*) ;;
  *) PKG="$ROOT/$PKG" ;;
esac
[ -f "$PKG" ] || { echo "Missing package: $PKG" >&2; exit 2; }
[ -x "$DPI" ] || { echo "DirectPackageInstaller CLI not found: $DPI" >&2; exit 2; }

pid=""
cleanup() {
  if [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1; then
    kill "$pid" >/dev/null 2>&1 || true
    sleep 1
    kill -9 "$pid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

: > "$LOG"
(
  cd "$(dirname "$DPI")"
  "./$(basename "$DPI")" -Server "$HOST" -PS4 "$PS4" -Port 9090 "$PKG"
) >"$LOG" 2>&1 &
pid=$!

for _ in $(seq 1 "$TIMEOUT"); do
  kill -0 "$pid" >/dev/null 2>&1 || break
  sleep 1
done

if kill -0 "$pid" >/dev/null 2>&1; then
  echo "DirectPackageInstaller timed out after ${TIMEOUT}s." >&2
  cat "$LOG"
  exit 1
fi

set +e
wait "$pid"
rc=$?
set -e
pid=""
cat "$LOG"
[ "$rc" -eq 0 ] || exit "$rc"
grep -q "Sent!" "$LOG" || {
  echo "DirectPackageInstaller exited without confirming package delivery." >&2
  exit 1
}

