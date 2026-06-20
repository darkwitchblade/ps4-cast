#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CALLBACK_LOG=${CALLBACK_LOG:-/tmp/ps4cast_lapy20002_launch.txt}
CALLBACK_PID=${CALLBACK_PID:-/tmp/ps4cast_lapy20002_launch.pid}

rm -f "$CALLBACK_LOG" "$CALLBACK_PID"
nc -l 9899 >"$CALLBACK_LOG" &
echo $! >"$CALLBACK_PID"
sleep 1

make -C "$ROOT/payloads/ps4cast-control" build/ps4cast-launch-lapy20002.bin >/tmp/ps4cast_control_build.log
python3 "$ROOT/scripts/send-goldhen-payload.py" "$ROOT/payloads/ps4cast-control/build/ps4cast-launch-lapy20002.bin" --ps4 "$PS4"

for _ in $(seq 1 10); do
  if [ -s "$CALLBACK_LOG" ]; then
    cat "$CALLBACK_LOG"
    kill "$(cat "$CALLBACK_PID")" 2>/dev/null || true
    exit 0
  fi
  sleep 1
done

kill "$(cat "$CALLBACK_PID")" 2>/dev/null || true
echo "No launch callback received."
