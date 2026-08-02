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
  if ! make -C "$CTRL" CALLBACK_IP="$CB_IP_HEX" "$bin" >/tmp/ctrl_build.log 2>&1; then
    echo "    payload build failed: $bin"
    tail -8 /tmp/ctrl_build.log
    return 1
  fi
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
  if ! make -C "$CTRL" CALLBACK_IP="$CB_IP_HEX" "$bin" >/tmp/ctrl_build.log 2>&1; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$log"
    return 1
  fi
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

"$ROOT/scripts/setup-payload-deps.sh" >/tmp/ps4cast_payload_deps.log 2>&1 || {
  echo "PAYLOAD TOOLCHAIN SETUP FAILED:"
  tail -8 /tmp/ps4cast_payload_deps.log
  exit 1
}

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

echo "[5/5] launch"
# The launch payload now binds the SIGNED-IN user via the system-wide login-user
# list (sceUserServiceGetLoginUserIdList), so a payload launch comes up with a
# valid initial user and no longer crashes SceShellUI (CE-36329-3). Payload launch
# is therefore the DEFAULT again (hands-off deploy). It still verifies the initial
# user below and warns if it somehow came up ANONYMOUS (e.g. no user signed in).
# Set PS4CAST_ICON_LAUNCH=1 to skip the payload and ask for a manual icon launch.
sleep 4
(cd dist && python3 -m http.server 8000 --bind "$HOST" >/tmp/h.log 2>&1 &)

wait_up() {  # poll /status up to ~90s for VER; on success check the initial user
  for j in $(seq 1 30); do
    s=$(curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null)
    if echo "$s" | grep -q "\"ver\":\"$VER\""; then
      iu=$(printf '%s' "$s" | sed -n 's/.*iu=0x\([0-9a-fA-F]*\).*/\1/p')
      if [ "$iu" = "ffffffff" ] || [ -z "$iu" ]; then
        echo "    READY on v$VER  —  WARNING: initial user is ANONYMOUS (iu=0x${iu:-?}); SceShellUI may have crashed. Prefer an ICON launch."
      else
        echo "    READY on v$VER  (iu=0x$iu, clean launch)"
      fi
      echo "DEPLOY OK"; return 0
    fi
    sleep 3
  done
  return 1
}

# Payload launch is OPT-IN: the login-user binding is unreliable in practice
# (sometimes still ANONYMOUS -> SceShellUI crash), so the ICON launch (always
# clean) is the default. Set PS4CAST_PAYLOAD_LAUNCH=1 to attempt the payload.
if [ "${PS4CAST_PAYLOAD_LAUNCH:-0}" = "1" ]; then
  echo "    payload launch (may bind anon -> crash)…"
  send_payload build/ps4cast-launch.bin || true
  wait_up && exit 0
  echo "    payload launch didn't come up — open PS4 Cast on the console (icon)"
else
  echo "    >> Open PS4 Cast from the home-screen ICON now (clean launch)."
  echo "    waiting up to 90s for it to come up on v${VER}..."
  wait_up && exit 0
  echo "    not up yet — open PS4 Cast on the console (icon), it is installed as v$VER"
fi
exit 0
