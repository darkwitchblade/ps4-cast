#!/usr/bin/env bash
# Robust one-shot deploy: build -> close -> uninstall/install -> launch.
#
# The first deploy after a PS4 reboot bootstraps a resident installer through
# GoldHEN's one-shot :9090 loader. It then listens on :9192 for every later build,
# so iterative deploys need no rearm. Each command removes the old title, installs
# the replacement, and replies only after AppInstUtil reports completion.
#
# Usage: scripts/redeploy.sh            (build current source + deploy)
#        scripts/redeploy.sh nobuild    (deploy the already-built pkg)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
PS4=${PS4_IP:-192.168.1.4}
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/ps4-api.sh
HOST=${HOST_IP:-$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)}
VER="$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,"",$2);print $2}' app/Makefile)"
DPI_PAYLOAD=payloads/ps4cast-dpi/build/ps4cast-dpi.bin

free_ports(){ for p in 8000 9898; do lsof -ti :$p 2>/dev/null | xargs kill -9 2>/dev/null; done; pkill -f push-goldhen-dpi 2>/dev/null; true; }

if [ "${1:-}" != "nobuild" ]; then
  echo "[1/5] build v$VER"
  ./build.sh >/tmp/redeploy_build.log 2>&1 || { echo "BUILD FAILED:"; tail -8 /tmp/redeploy_build.log; exit 1; }
fi

echo "[2/5] close running app cleanly + verify"
# Best practice: close the OLD version cleanly, VERIFY it's gone, then proceed.
# Never install over a running/frozen app. /quit now does LoadExec("exit") — a
# normal return-to-home with NO crash dialog (v03.05+).
app_up() { curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null | grep -q '"ver":'; }
closed=""
for attempt in 1 2 3 4 5; do
  if ! app_up; then closed=1; break; fi
  echo "    close attempt $attempt: clean /quit"
  ps4_post "quit" -m5 >/dev/null 2>&1 || true
  sleep 4
  if ! app_up; then closed=1; break; fi
done
# Verify truly closed: must be unreachable on 3 consecutive checks (not a blip).
if [ -n "$closed" ]; then
  downs=0
  for _ in 1 2 3 4 5 6; do
    if app_up; then downs=0; else downs=$((downs+1)); fi
    [ "$downs" -ge 3 ] && break
    sleep 2
  done
  [ "$downs" -ge 3 ] || closed=""
fi
if [ -z "$closed" ]; then
  oldver="$(curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null | sed -n 's/.*"ver":"\([^"]*\)".*/\1/p')"
  echo "CLOSE FAILED: app still responding/frozen on v${oldver:-unknown}."
  echo "Close it from the console before deploying; :9090 was left untouched."
  exit 2
fi
echo "    verified closed"

echo "[3/5] build install-ready DPI payload"
make -B -C payloads/ps4cast-dpi >/tmp/ps4cast_dpi_payload_build.log 2>&1 || {
  echo "DPI PAYLOAD BUILD FAILED:" >&2
  tail -12 /tmp/ps4cast_dpi_payload_build.log >&2
  exit 1
}

echo "[4/5] install/update v$VER and verify AppInstUtil completion"
free_ports
python3 -u scripts/push-goldhen-dpi.py \
  --host "$HOST" --ps4 "$PS4" --pkg "dist/PS4-Cast-v$VER.pkg" \
  --payload "$DPI_PAYLOAD" --agent-port 9192 --ready-timeout 180 \
  2>&1 | tee /tmp/ps4cast_dpi_deploy.log
rc=${PIPESTATUS[0]}
if [ "$rc" -ne 0 ] || ! grep -q "install ready: READY " /tmp/ps4cast_dpi_deploy.log; then
  echo "INSTALL FAILED: DPI did not confirm AppInstUtil completion. Rearm GoldHEN :9090 once, then rerun."
  exit 1
fi

echo "[5/5] launch verified v$VER"
PS4CAST_POST_INSTALL=1 PS4_IP="$PS4" HOST_IP="$HOST" \
  scripts/open-ps4cast.sh "$VER" || exit 1
echo "DEPLOY OK"
