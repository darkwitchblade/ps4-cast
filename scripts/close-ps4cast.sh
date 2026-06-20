#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if curl -sS -m 5 -X POST "http://$PS4:8080/quit" >/dev/null 2>&1; then
  for _ in $(seq 1 12); do
    if ! curl -sS -m 2 "http://$PS4:8080/status" >/dev/null 2>&1; then
      echo "PS4 Cast closed gracefully."
      exit 0
    fi
    sleep 1
  done
fi

make -C "$ROOT/payloads/ps4cast-control" build/ps4cast-kill.bin >/tmp/ps4cast_control_build.log
python3 "$ROOT/scripts/send-goldhen-payload.py" "$ROOT/payloads/ps4cast-control/build/ps4cast-kill.bin" --ps4 "$PS4"

for _ in $(seq 1 20); do
  if ! curl -sS -m 2 "http://$PS4:8080/status" >/dev/null 2>&1; then
    echo "PS4 Cast closed."
    exit 0
  fi
  sleep 1
done

echo "PS4 Cast did not close within the timeout." >&2
exit 1
