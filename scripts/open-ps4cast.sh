#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
VER=${1:-}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# The launch payload now binds the signed-in user (login-user list), so a payload
# launch is clean (valid initial user, no SceShellUI CE-36329-3). Payload launch is
# the default. Set PS4CAST_ICON_LAUNCH=1 to skip it and open from the icon manually.
if [ "${PS4CAST_ICON_LAUNCH:-0}" = "1" ]; then
  echo ">> Open PS4 Cast from the home-screen ICON. Waiting for it to come up…" >&2
else
  python3 "$ROOT/scripts/send-goldhen-payload.py" "$ROOT/payloads/ps4cast-control/build/ps4cast-launch.bin" --ps4 "$PS4"
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
