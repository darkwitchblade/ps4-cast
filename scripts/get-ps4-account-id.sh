#!/usr/bin/env bash
# Read the active PS4 user's NP account ID through GoldHEN and print Chiaki base64.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PS4=${PS4_IP:-192.168.1.4}
HOST=${HOST_IP:-}
USER_ID=${PS4_USER_ID:-0x168a0466}
CTRL="$ROOT/payloads/ps4cast-control"
ACCOUNT_CACHE=${PS4CAST_ACCOUNT_ID_CACHE:-$ROOT/.chiaki-account-id}
if [ -z "$HOST" ]; then
  HOST="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
fi
[ -n "$HOST" ] || { echo "Cannot determine Mac LAN IP; set HOST_IP." >&2; exit 2; }

CB_IP_HEX="$(awk -F. '{printf "0x%02X%02X%02X%02XU", $4, $3, $2, $1}' <<<"$HOST")"
LOG="$(mktemp /tmp/ps4cast-account-id.XXXXXX)"
listener=""
cleanup() {
  if [ -n "$listener" ]; then
    kill "$listener" >/dev/null 2>&1 || true
    wait "$listener" >/dev/null 2>&1 || true
  fi
  rm -f "$LOG"
}
trap cleanup EXIT INT TERM

"$ROOT/scripts/setup-payload-deps.sh" >/tmp/ps4cast_payload_deps.log 2>&1
make -B -C "$CTRL" CALLBACK_IP="$CB_IP_HEX" USER_ID="$USER_ID" \
  build/ps4cast-app-status.bin >/tmp/ps4cast_account_build.log 2>&1

nc -l 9899 >"$LOG" &
listener=$!
sleep 1
sent="$(python3 "$ROOT/scripts/send-goldhen-payload.py" \
  "$CTRL/build/ps4cast-app-status.bin" --ps4 "$PS4" 2>&1 || true)"
if printf '%s' "$sent" | grep -qi 'connection refused'; then
  echo "GoldHEN payload listener is closed. Re-enable BinLoader Server on the PS4." >&2
  exit 1
fi

for _ in $(seq 1 12); do
  [ -s "$LOG" ] && break
  sleep 1
done
[ -s "$LOG" ] || {
  echo "PS4 account-ID callback timed out. GoldHEN sender said:" >&2
  printf '%s\n' "$sent" >&2
  exit 1
}

report="$(cat "$LOG")"
rv="$(printf '%s' "$report" | sed -n 's/.*account_rv=\([-0-9]*\).*/\1/p' | head -1)"
hex="$(printf '%s' "$report" | sed -n 's/.*account_id=0x\([0-9A-Fa-f]*\).*/\1/p' | head -1)"
[ "$rv" = "0" ] && [ -n "$hex" ] && [ "$hex" != "0000000000000000" ] || {
  echo "The PS4 did not return a valid NP account ID: $report" >&2
  exit 1
}

account_id="$(python3 -c 'import base64,struct,sys; print(base64.b64encode(struct.pack("<Q", int(sys.argv[1], 16))).decode())' "$hex")"
printf '%s\n' "$account_id" > "$ACCOUNT_CACHE"
chmod 600 "$ACCOUNT_CACHE"
printf '%s\n' "$account_id"
