#!/usr/bin/env bash
# On-device playback matrix: liveness, HW/SW routing, presented FPS, split
# drops, and transport controls. Starts the local fixture server when needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PS4=${PS4_IP:-192.168.1.4}
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/ps4-api.sh
HOST=${HOST_IP:-$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)}
PORT=${STREAMTEST_PORT:-8000}
BASE="http://$HOST:$PORT/streamtest"
SERVER_PID=""

[ -n "$HOST" ] || { echo "Cannot determine Mac LAN IP; set HOST_IP." >&2; exit 2; }

cleanup() {
  ps4_post "stop" -m3 >/dev/null 2>&1 || true
  if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT INT TERM

status() { curl -sS -m4 "http://$PS4:8080/status" 2>/dev/null; }
json_num() { printf '%s' "$1" | sed -n "s/.*\"$2\":\([-0-9]*\).*/\1/p"; }
diag_num() { printf '%s' "$1" | sed -n "s/.*$2=\([-0-9]*\).*/\1/p"; }
json_text() { printf '%s' "$1" | sed -n "s/.*\"$2\":\"\([^\"]*\)\".*/\1/p"; }

if ! curl -sS -m2 "$BASE/h264_360p.mp4" -o /dev/null 2>/dev/null; then
  echo "Starting fixture server at http://$HOST:$PORT"
  (cd "$ROOT/dist" && python3 -m http.server "$PORT" --bind "$HOST" >/tmp/ps4cast_streamtest_http.log 2>&1) &
  SERVER_PID=$!
  sleep 1
fi

initial="$(status || true)"
[ -n "$initial" ] || { echo "PS4 Cast is not reachable at $PS4:8080." >&2; exit 2; }
echo "Testing PS4 Cast v$(json_text "$initial" ver) on $PS4"

# H.264 at 1080p or below should use HW. Other codecs prove SW fallback.
CASES=(
  "h264_360p.mp4|HW"
  "h264_720p.mp4|HW"
  "h264_1080p_high_10mbps.mp4|HW"
  "h264_720p_60fps.mp4|HW"
  "h264_1080p60.mp4|HW"
  "h264_720p.mkv|HW"
  "h264_720p.ts|HW"
  "mpeg4_480p.avi|SW"
  "hevc_720p.mp4|SW"
  "vp9_720p.webm|SW"
  "hls/master.m3u8|HW"
  "hls_sep/master.m3u8|HW"
)

printf "%-31s %-4s %5s %7s %18s %-12s\n" "SOURCE" "PATH" "FPS" "FRAMES" "DROPS(q/l/r)" "RESULT"
printf "%-31s %-4s %5s %7s %18s %-12s\n" "------" "----" "---" "------" "------------" "------"

failures=0
for spec in "${CASES[@]}"; do
  clip=${spec%%|*}
  expected=${spec##*|}
  ps4_post "stop" -m4 >/dev/null 2>&1 || true
  sleep 1
  ps4_post "avplay" -m6 --data-binary "$BASE/$clip" >/dev/null

  maxfps=0
  last=""
  alive=1
  for _ in 1 2 3 4 5 6 7 8; do
    sleep 1
    last="$(status || true)"
    if [ -z "$last" ]; then alive=0; break; fi
    fps=$(json_num "$last" fps); fps=${fps:-0}
    [ "$fps" -gt "$maxfps" ] && maxfps=$fps
  done

  result=PASS
  if [ "$alive" -eq 0 ]; then
    result=CRASH; failures=$((failures + 1))
    frames=0; qd=0; ld=0; rd=0; path=?
  else
    diag=$(json_text "$last" diag)
    frames=$(diag_num "$diag" fr); frames=${frames:-0}
    qd=$(printf '%s' "$diag" | sed -n 's/.*drop=[0-9]*(q\([0-9]*\)\/l.*/\1/p'); qd=${qd:-0}
    ld=$(printf '%s' "$diag" | sed -n 's/.*drop=[0-9]*(q[0-9]*\/l\([0-9]*\)\/r.*/\1/p'); ld=${ld:-0}
    rd=$(printf '%s' "$diag" | sed -n 's/.*drop=[0-9]*(q[0-9]*\/l[0-9]*\/r\([0-9]*\)).*/\1/p'); rd=${rd:-0}
    case "$diag" in *ff/HW*) path=HW ;; *) path=SW ;; esac
    err=$(json_text "$last" error_code)
    if [ "$frames" -le 0 ] || [ -n "$err" ] || [ "$path" != "$expected" ]; then
      result=FAIL; failures=$((failures + 1))
    elif [ "$maxfps" -le 0 ]; then
      result=WARN
    fi
  fi
  printf "%-31s %-4s %5s %7s %5s/%-5s/%-5s %-12s\n" \
    "$clip" "$path" "$maxfps" "$frames" "$qd" "$ld" "$rd" "$result"
  [ "$alive" -eq 1 ] || break
done

if [ "$failures" -eq 0 ]; then
  echo "Transport controls: pause, resume, seek"
  ps4_post "avplay" -m6 --data-binary "$BASE/h264_1080p_4mbps.mp4" >/dev/null
  sleep 3
  ps4_post "pause" -m4 --data-binary 1 >/dev/null
  paused="$(status)"
  [ "$(json_num "$paused" paused)" = "1" ] || { echo "FAIL: pause state was not applied"; failures=$((failures + 1)); }
  before=$(json_num "$paused" cur); before=${before:-0}
  sleep 2
  held="$(status)"; after=$(json_num "$held" cur); after=${after:-0}
  [ $((after - before)) -le 1 ] || { echo "FAIL: paused clock advanced from $before to $after"; failures=$((failures + 1)); }
  ps4_post "pause" -m4 --data-binary 0 >/dev/null
  ps4_post "seek" -m4 --data-binary 5 >/dev/null
  sleep 2
  resumed="$(status)"
  [ "$(json_num "$resumed" paused)" = "0" ] || { echo "FAIL: resume state was not applied"; failures=$((failures + 1)); }
fi

if [ "$failures" -gt 0 ]; then
  echo "Playback suite failed: $failures blocking case(s)." >&2
  exit 1
fi
echo "Playback suite passed. Review WARN rows and on-TV visual quality separately."
