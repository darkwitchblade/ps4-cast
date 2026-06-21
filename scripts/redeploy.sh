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
CB_IP_HEX="$(awk -F. '{printf "0x%02X%02X%02X%02XU", $4, $3, $2, $1}' <<<"$HOST")"

free_ports(){ for p in 8000 9898; do lsof -ti :$p 2>/dev/null | xargs kill -9 2>/dev/null; done; pkill -f push-goldhen-dpi 2>/dev/null; true; }

# Patiently POST a payload to :9090 until it lands (it re-arms slowly).
send_payload(){ # $1 = built .bin
  local bin="$1" i out
  make -C "$CTRL" CALLBACK_IP="$CB_IP_HEX" "$bin" >/tmp/ctrl_build.log 2>&1 || true
  for i in $(seq 1 18); do
    out=$(python3 scripts/send-goldhen-payload.py "$CTRL/$bin" --ps4 "$PS4" 2>&1)
    echo "$out" | grep -qiE "HTTP 200" && { echo "    ok (try $i)"; return 0; }
    sleep 4
  done
  echo "    payload did not land after retries"; return 1
}

payload_callback(){ # $1 = built .bin, stdout = callback text
  local bin="$1" log="/tmp/ps4cast_payload_cb.$$" pid out
  rm -f "$log"
  nc -l 9899 >"$log" &
  pid=$!
  sleep 1
  make -C "$CTRL" CALLBACK_IP="$CB_IP_HEX" "$bin" >/tmp/ctrl_build.log 2>&1 || true
  out=$(python3 scripts/send-goldhen-payload.py "$CTRL/$bin" --ps4 "$PS4" 2>&1 || true)
  if ! echo "$out" | grep -qiE "HTTP 200"; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$log"
    return 1
  fi
  for _ in $(seq 1 8); do
    if [ -s "$log" ]; then
      cat "$log"
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      rm -f "$log"
      return 0
    fi
    sleep 1
  done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  rm -f "$log"
  return 1
}

app_registered(){
  local cb progress pirc installing updating
  cb="$(payload_callback build/ps4cast-app-status.bin 2>/dev/null || true)"
  progress="$(printf '%s' "$cb" | sed -n 's/.*progress=\([-0-9]\+\).*/\1/p' | tail -1)"
  pirc="$(printf '%s' "$cb" | sed -n 's/.*pirc=\([-0-9]\+\).*/\1/p' | tail -1)"
  installing="$(printf '%s' "$cb" | sed -n 's/.*installing=\([-0-9]\+\).*/\1/p' | tail -1)"
  updating="$(printf '%s' "$cb" | sed -n 's/.*updating=\([-0-9]\+\).*/\1/p' | tail -1)"
  [ "$pirc" = "0" ] && [ "$progress" = "100" ] && [ "${installing:-0}" = "0" ] && [ "${updating:-0}" != "1" ] && return 0
  return 1
}

if [ "${1:-}" != "nobuild" ]; then
  echo "[1/5] build v$VER"
  ./build.sh >/tmp/redeploy_build.log 2>&1 || { echo "BUILD FAILED:"; tail -8 /tmp/redeploy_build.log; exit 1; }
fi

echo "[2/5] close running app cleanly + verify"
# Best practice: close the OLD version cleanly, VERIFY it's gone, then proceed.
# Never install over a running/frozen app. /quit now does LoadExec("exit") — a
# normal return-to-home with NO crash dialog (v03.05+), so it's the preferred
# clean close; the force-kill payload is the fallback for a wedged app.
app_up() { curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null | grep -q '"ver":'; }
closed=""
for attempt in 1 2 3 4 5; do
  if ! app_up; then closed=1; break; fi
  echo "    close attempt $attempt: clean /quit, then force-kill"
  curl -sS -m5 -X POST "http://$PS4:8080/quit" >/dev/null 2>&1 || true
  sleep 4
  if ! app_up; then closed=1; break; fi
  send_payload build/ps4cast-kill.bin || true
  sleep 5                       # also lets GoldHEN re-arm :9090
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
  echo "CLOSE FAILED: app still responding/frozen on v${oldver:-unknown} and :9090 not accepting the kill."
  echo "On the console: PS button -> Close Application (dismiss any dialog), re-arm GoldHEN :9090, then rerun."
  exit 2
fi
echo "    verified closed"

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
[ -z "$ok" ] && { echo "INSTALL FAILED (:9090 not accepting). pkg is hosted; install via Remote Pkg Installer: http://$HOST:8000/PS4-Cast-v$VER.pkg"; (cd dist && python3 -m http.server 8000 --bind "$HOST" >/tmp/h.log 2>&1 &); exit 1; }

echo "[5/5] launch after BGFT settle"
# push-goldhen-dpi.py now returns only after serving the final package byte.
# Give the shell a short promotion/indexing moment, then launch once. If the
# title is still settling, the late retry below catches it without payload spam.
sleep 4

for round in 1 2; do
  echo "    launch attempt $round; waiting for ready toast (/status)…"
  send_payload build/ps4cast-launch.bin || true
  for j in $(seq 1 30); do
    s=$(curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null)
    if echo "$s" | grep -q "\"ver\":\"$VER\""; then
      echo "    READY — app open on v$VER"
      (cd dist && python3 -m http.server 8000 --bind "$HOST" >/tmp/h.log 2>&1 &); echo "DEPLOY OK"; exit 0
    fi
    sleep 3
  done
  echo "    not ready yet; waiting for BGFT settle before one retry"
  sleep 20
done
echo "installed but auto-launch didn't take — open PS4 Cast on the console (then it's v$VER)"
(cd dist && python3 -m http.server 8000 --bind "$HOST" >/tmp/h.log 2>&1 &)
exit 0
