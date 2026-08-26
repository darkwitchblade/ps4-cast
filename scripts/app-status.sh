#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
HOST=${HOST_IP:-192.168.1.139}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CALLBACK_LOG=${CALLBACK_LOG:-/tmp/ps4cast_app_status.txt}
CALLBACK_PID=${CALLBACK_PID:-/tmp/ps4cast_app_status.pid}
CB_IP_HEX="$(awk -F. '{printf "0x%02X%02X%02X%02XU", $4, $3, $2, $1}' <<<"$HOST")"
USER_ID=${PS4_USER_ID:?set PS4_USER_ID to your console's local user id}

rm -f "$CALLBACK_LOG" "$CALLBACK_PID"
nc -l 9899 >"$CALLBACK_LOG" &
echo $! >"$CALLBACK_PID"
sleep 1

"$ROOT/scripts/setup-payload-deps.sh" >/tmp/ps4cast_payload_deps.log
make -B -C "$ROOT/payloads/ps4cast-control" CALLBACK_IP="$CB_IP_HEX" USER_ID="$USER_ID" build/ps4cast-app-status.bin >/tmp/ps4cast_control_build.log
python3 "$ROOT/scripts/send-goldhen-payload.py" "$ROOT/payloads/ps4cast-control/build/ps4cast-app-status.bin" --ps4 "$PS4"

for _ in $(seq 1 10); do
  if [ -s "$CALLBACK_LOG" ]; then
    cat "$CALLBACK_LOG"
    kill "$(cat "$CALLBACK_PID")" 2>/dev/null || true
    exit 0
  fi
  sleep 1
done

kill "$(cat "$CALLBACK_PID")" 2>/dev/null || true
echo "No app status callback received."
