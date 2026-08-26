#!/usr/bin/env bash
# Launch PS4 Cast through a paired Remote Play session and verify it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PS4=${PS4_IP:-192.168.1.4}
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/ps4-api.sh
VER=${1:-}
HOST=${HOST_IP:-}
if [ -z "$HOST" ]; then
  HOST="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
fi
[ -n "$HOST" ] || { echo "Cannot determine Mac LAN IP; set HOST_IP." >&2; exit 2; }

CTRL="payloads/ps4cast-control"
CB_IP_HEX="$(awk -F. '{printf "0x%02X%02X%02X%02XU", $4, $3, $2, $1}' <<<"$HOST")"
CHIAKI=${PS4CAST_CHIAKI_BIN:-$ROOT/.devtools/chiaki-ng.app/Contents/MacOS/chiaki}
CHIAKI_PROFILE=${PS4CAST_CHIAKI_PROFILE:-ps4cast-dev}
PAIRED=${PS4CAST_REMOTE_PAIRED_MARKER:-$ROOT/.chiaki-remote.paired}
KEY_TOOL=${PS4CAST_KEY_TOOL:-$ROOT/.devtools/ps4cast-send-key}
CHIAKI_LOG=/tmp/ps4cast_chiaki_launch.log
chiaki_pid=""

stop_chiaki() {
  if [ -n "$chiaki_pid" ]; then
    kill "$chiaki_pid" >/dev/null 2>&1 || true
    for _ in $(seq 1 10); do
      kill -0 "$chiaki_pid" >/dev/null 2>&1 || break
      sleep 0.2
    done
    if kill -0 "$chiaki_pid" >/dev/null 2>&1; then
      kill -9 "$chiaki_pid" >/dev/null 2>&1 || true
    fi
    wait "$chiaki_pid" >/dev/null 2>&1 || true
    chiaki_pid=""
  fi
}
trap stop_chiaki EXIT INT TERM

wait_up() {
  local s iu
  for _ in $(seq 1 240); do
    s="$(curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null || true)"
    if [ -n "$s" ] && { [ -z "$VER" ] || printf '%s' "$s" | grep -q "\"ver\":\"$VER\""; }; then
      iu="$(printf '%s' "$s" | sed -n 's/.*iu=0x\([0-9a-fA-F]*\).*/\1/p')"
      if [ -z "$iu" ] || [ "$iu" = "ffffffff" ] || [ "$iu" = "00000000" ]; then
        echo "Launch reached HTTP but initial user is invalid (iu=0x${iu:-?}). Closing it." >&2
        ps4_post "quit" -m3 >/dev/null 2>&1 || true
        return 2
      fi
      echo "$s"
      echo "READY v${VER:-unknown} iu=0x$iu"
      return 0
    fi
    sleep 2
  done
  return 1
}

# Avoid launching twice when this exact build is already healthy. Keep this a
# one-shot probe; the full wait belongs after the launch payload lands.
current="$(curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null || true)"
if [ -n "$current" ] && { [ -z "$VER" ] || printf '%s' "$current" | grep -q "\"ver\":\"$VER\""; }; then
  iu="$(printf '%s' "$current" | sed -n 's/.*iu=0x\([0-9a-fA-F]*\).*/\1/p')"
  if [ -n "$iu" ] && [ "$iu" != "ffffffff" ] && [ "$iu" != "00000000" ]; then
    echo "$current"
    echo "READY v${VER:-unknown} iu=0x$iu (already running)"
    exit 0
  fi
fi

if [ "${PS4CAST_ICON_LAUNCH:-0}" = "1" ]; then
  echo ">> Open PS4 Cast from the home-screen icon." >&2
  wait_up
  exit $?
fi

if [ "${PS4CAST_UNSAFE_PAYLOAD_LAUNCH:-0}" = "1" ]; then
  # Diagnostic only. On FW 11 an injected payload has no authenticated user
  # session; even a copied numeric user id launched PCST00001 as ANONYMOUS and
  # crashed SceShellUI. Never enter this branch during normal development.
  USER_ID=${PS4_USER_ID:-0x168a0466}
  "$ROOT/scripts/setup-payload-deps.sh" >/tmp/ps4cast_payload_deps.log
  make -B -C "$CTRL" CALLBACK_IP="$CB_IP_HEX" USER_ID="$USER_ID" build/ps4cast-launch.bin >/tmp/ps4cast_launch_build.log
  echo "WARNING: using unsafe unauthenticated payload launch" >&2
  python3 scripts/send-goldhen-payload.py "$CTRL/build/ps4cast-launch.bin" --ps4 "$PS4"
else
  if [ ! -x "$CHIAKI" ] || [ ! -s "$PAIRED" ]; then
    echo "Safe auto-launch is not paired yet." >&2
    echo "Run once: PS4_IP=$PS4 HOST_IP=$HOST scripts/setup-remote-launch.sh" >&2
    exit 2
  fi
  nickname="$(sed -n 's/^nickname=//p' "$PAIRED" | head -1)"
  paired_profile="$(sed -n 's/^profile=//p' "$PAIRED" | head -1)"
  [ -n "$paired_profile" ] && CHIAKI_PROFILE="$paired_profile"
  [ -n "$nickname" ] || { echo "Remote Play pairing marker has no nickname." >&2; exit 2; }

  echo "Opening an authenticated Remote Play session for $nickname..."
  : > "$CHIAKI_LOG"
  "$CHIAKI" --profile "$CHIAKI_PROFILE" --exit-app-on-stream-exit stream "$nickname" "$PS4" \
    >"$CHIAKI_LOG" 2>&1 &
  chiaki_pid=$!

  # Send launch input as soon as the Remote Play stream is actually ready.
  # This avoids both a brittle fixed delay and input sent during negotiation.
  stream_ready=""
  for _ in $(seq 1 240); do
    if grep -q "successfully received streaminfo" "$CHIAKI_LOG"; then
      stream_ready=1
      break
    fi
    kill -0 "$chiaki_pid" >/dev/null 2>&1 || break
    sleep 0.25
  done
  if ! kill -0 "$chiaki_pid" >/dev/null 2>&1; then
    echo "Remote Play session exited before launch input:" >&2
    tail -20 "$CHIAKI_LOG" >&2
    exit 1
  fi
  if [ -z "$stream_ready" ]; then
    echo "Remote Play connected but did not expose a ready video stream." >&2
    exit 1
  fi
  # Qt may render a live QML surface without exposing an accessibility window.
  # Post Return/Cross to the exact process we started, which is independent of
  # application labels, window titles, focus, and macOS Spaces.
  # After uninstall/reinstall, Orbis keeps selection on the tile that shifted
  # into PS4 Cast's old slot; the freshly installed PS4 Cast tile is one place
  # to its left. Move back to it only for that post-install launch workflow.
  if [ "${PS4CAST_POST_INSTALL:-0}" = "1" ]; then
    "$KEY_TOOL" "$chiaki_pid" 123 || {
      echo "Could not send D-pad Left to Chiaki process $chiaki_pid." >&2
      exit 1
    }
    sleep 0.25
  fi
  if [ ! -x "$KEY_TOOL" ] || ! "$KEY_TOOL" "$chiaki_pid" 36; then
    echo "Could not send Cross/Enter to Chiaki process $chiaki_pid." >&2
    exit 1
  fi
fi

if wait_up; then
  stop_chiaki
  exit 0
fi
echo "Launch command completed, but the verified app did not become ready." >&2
tail -20 "$CHIAKI_LOG" 2>/dev/null || true
exit 1
