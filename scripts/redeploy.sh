#!/usr/bin/env bash
# Robust one-shot deploy: build -> close -> uninstall -> install -> launch.
#
# GoldHEN's :9090 payload server is single-shot and needs a few seconds to
# re-arm between payloads; hammering it fast gives "connection refused". This
# script paces each step and retries patiently so the whole cycle runs
# autonomously. Always uninstalls the old build before installing (per request).
#
# Usage: scripts/redeploy.sh            (build current source + deploy)
#        scripts/redeploy.sh nobuild    (deploy the already-built pkg)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
PS4=${PS4_IP:-192.168.1.253}
HOST=${HOST_IP:-192.168.1.139}
CTRL="payloads/ps4cast-control"
VER="$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,"",$2);print $2}' app/Makefile)"

free_ports(){ for p in 8000 9898; do lsof -ti :$p 2>/dev/null | xargs kill -9 2>/dev/null; done; pkill -f push-goldhen-dpi 2>/dev/null; true; }

# Patiently POST a payload to :9090 until it lands (it re-arms slowly).
send_payload(){ # $1 = built .bin
  local bin="$1" i out
  make -C "$CTRL" "$bin" >/tmp/ctrl_build.log 2>&1 || true
  for i in $(seq 1 18); do
    out=$(python3 scripts/send-goldhen-payload.py "$CTRL/$bin" --ps4 "$PS4" 2>&1)
    echo "$out" | grep -qiE "HTTP 200" && { echo "    ok (try $i)"; return 0; }
    sleep 4
  done
  echo "    payload did not land after retries"; return 1
}

if [ "${1:-}" != "nobuild" ]; then
  echo "[1/5] build v$VER"
  ./build.sh >/tmp/redeploy_build.log 2>&1 || { echo "BUILD FAILED:"; tail -8 /tmp/redeploy_build.log; exit 1; }
fi

echo "[2/5] close running app"
# Prefer the external control payload. The in-app /quit path can surface as a
# PS4 crash dialog on this console, so keep it out of the normal deploy loop.
send_payload build/ps4cast-kill.bin || true
sleep 5                         # let GoldHEN re-arm :9090

echo "[3/5] uninstall old app"
send_payload build/ps4cast-uninstall.bin || true
sleep 10                        # removal + re-arm takes a moment

echo "[4/5] install v$VER (DPI)"
ok=""
for i in $(seq 1 12); do
  free_ports; sleep 2
  out=$(python3 scripts/push-goldhen-dpi.py --host "$HOST" --ps4 "$PS4" --keepalive 70 2>&1)
  echo "$out" | grep -qiE "manifest fetched by PS4|callback from" && { echo "    delivered (try $i)"; ok=1; break; }
  sleep 5
done
[ -z "$ok" ] && { echo "INSTALL FAILED (:9090 not accepting). pkg is hosted; install via Remote Pkg Installer: http://$HOST:8000/PS4-Cast-v$VER.pkg"; (cd dist && python3 -m http.server 8000 >/tmp/h.log 2>&1 &); exit 1; }
sleep 2                         # let BGFT start; launch loop below retries until install is ready

echo "[5/5] wait for install to finish, then launch once and wait for READY"
# Let the system finish the background install (BGFT) BEFORE launching — launching
# mid-install is what shows the long "stuck" screen. Then launch once and poll
# /status patiently; /status responding == the app's "ready to cast" toast.
sleep 25                        # install settle (15MB pkg + system processing)
for round in $(seq 1 4); do
  echo "    launch attempt $round; waiting for ready toast (/status)…"
  send_payload build/ps4cast-launch.bin || true
  for j in $(seq 1 25); do      # ~75s patient wait per launch (no re-launch churn)
    s=$(curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null)
    if echo "$s" | grep -q "\"ver\":\"$VER\""; then
      echo "    READY — app open on v$VER"
      (cd dist && python3 -m http.server 8000 >/tmp/h.log 2>&1 &); echo "DEPLOY OK"; exit 0
    fi
    sleep 3
  done
done
echo "installed but auto-launch didn't take — open PS4 Cast on the console (then it's v$VER)"
(cd dist && python3 -m http.server 8000 >/tmp/h.log 2>&1 &)
exit 0
