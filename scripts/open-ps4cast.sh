#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
VER=${1:-}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

make -C "$ROOT/payloads/ps4cast-control" build/ps4cast-launch.bin >/tmp/ps4cast_control_build.log
python3 "$ROOT/scripts/send-goldhen-payload.py" "$ROOT/payloads/ps4cast-control/build/ps4cast-launch.bin" --ps4 "$PS4"

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
