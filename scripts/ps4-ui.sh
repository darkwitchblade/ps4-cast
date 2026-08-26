#!/usr/bin/env bash
# Drive the PS4 UI through the pinned chiaki-ng Remote Play devtool.
# Subcommands:
#   start            open the authenticated stream session (background)
#   stop             close it
#   key CODE         send one Mac keycode into the stream window
#   seq CODE...      send several keys with short gaps
#   shot FILE        screenshot the stream window for visual verification
# Buttons used by callers (macOS hid usage codes):
#   36 Cross/Enter  49 Circle/Esc  123 Left  124 Right  125 Down  126 Up
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

PS4=${PS4_IP:-192.168.1.4}
CHIAKI=${PS4CAST_CHIAKI_BIN:-$ROOT/.devtools/chiaki-ng.app/Contents/MacOS/chiaki}
CHIAKI_PROFILE=${PS4CAST_CHIAKI_PROFILE:-ps4cast-dev}
PAIRED=${PS4CAST_REMOTE_PAIRED_MARKER:-$ROOT/.chiaki-remote.paired}
KEY_TOOL=${PS4CAST_KEY_TOOL:-$ROOT/.devtools/ps4cast-send-key}
PIDFILE=/tmp/ps4cast_chiaki_ui.pid
LOG=/tmp/ps4cast_chiaki_ui.log

session_pid() {
  [ -f "$PIDFILE" ] || return 1
  local p; p="$(cat "$PIDFILE")"
  kill -0 "$p" >/dev/null 2>&1 || return 1
  printf '%s' "$p"
}

stream_ready() {
  local p; p="$(session_pid)" || return 1
  grep -q "successfully received streaminfo" "$LOG" 2>/dev/null
}

cmd="${1:-}"; shift || true
case "$cmd" in
  start)
    if session_pid >/dev/null && stream_ready; then echo "stream already up"; exit 0; fi
    [ -s "$PAIRED" ] || { echo "not paired: run scripts/setup-remote-launch.sh first" >&2; exit 2; }
    nickname="$(sed -n 's/^nickname=//p' "$PAIRED" | head -1)"
    : > "$LOG"
    "$CHIAKI" --profile "$CHIAKI_PROFILE" --exit-app-on-stream-exit stream "$nickname" "$PS4" \
      >"$LOG" 2>&1 &
    echo $! > "$PIDFILE"
    for _ in $(seq 1 240); do
      stream_ready && { echo "stream ready"; exit 0; }
      kill -0 "$(cat "$PIDFILE")" 2>/dev/null || break
      sleep 0.25
    done
    echo "stream failed:" >&2; tail -20 "$LOG" >&2
    exit 1
    ;;
  stop)
    if p="$(session_pid)"; then
      kill "$p" 2>/dev/null || true
      rm -f "$PIDFILE"
      echo "stream stopped"
    fi
    ;;
  key|seq)
    p="$(session_pid)" || { echo "no stream session; run: $0 start" >&2; exit 2; }
    stream_ready || { echo "stream not ready yet" >&2; exit 2; }
    for code in "$@"; do
      "$KEY_TOOL" "$p" "$code" || { echo "key $code failed" >&2; exit 1; }
      sleep 0.35
    done
    ;;
  shot)
    out="${1:?usage: $0 shot FILE}"
    p="$(session_pid)" || { echo "no stream session" >&2; exit 2; }
    # Find the chiaki stream window and capture just that window.
    win="$(python3 - "$p" <<'PY'
import Quartz, sys
pid = int(sys.argv[1])
best = None
for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID):
    if w.get('kCGWindowOwnerPID') == pid and w.get('kCGWindowLayer') == 0:
        b = w.get('kCGWindowBounds')
        area = b['Width'] * b['Height']
        if best is None or area > best[1]:
            best = (w['kCGWindowNumber'], area)
print(best[0] if best else '')
PY
)"
    [ -n "$win" ] || { echo "chiaki window not found" >&2; exit 1; }
    screencapture -x -o -l "$win" "$out"
    echo "$out"
    ;;
  *)
    grep -q . <<EOF
usage: $0 start|stop|shot FILE|key CODE|seq CODE...
EOF
    exit 2 ;;
esac