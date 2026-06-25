#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
VER=${1:-}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# The :9090 payload launcher binds the app's INITIAL user to ANONYMOUS, which
# crashes SceShellUI (CE-36329-3) ~1-in-3 launches. A home-screen ICON launch
# binds the real user and never crashes. So default to asking for an icon launch;
# set PS4CAST_PAYLOAD_LAUNCH=1 to force the crash-prone payload path.
if [ "${PS4CAST_PAYLOAD_LAUNCH:-0}" = "1" ]; then
  echo "PS4CAST_PAYLOAD_LAUNCH=1 -> crash-prone payload launch (anon user)" >&2
  python3 "$ROOT/scripts/send-goldhen-payload.py" "$ROOT/payloads/ps4cast-control/build/ps4cast-launch.bin" --ps4 "$PS4"
else
  echo ">> Open PS4 Cast from the home-screen ICON (clean launch). Waiting for it to come up…" >&2
fi

for _ in $(seq 1 80); do
  ps4_status=$(curl -sS -m 3 "http://$PS4:8080/status" 2>/dev/null || true)
  if [ -n "$ps4_status" ]; then
    echo "$ps4_status"
    if [ -z "$VER" ] || printf '%s' "$ps4_status" | grep -q "\"ver\":\"$VER\""; then
      exit 0
    fi
  fi
  sleep 3
done

echo "PS4 Cast did not open automatically. Open it manually on the PS4." >&2
exit 1
