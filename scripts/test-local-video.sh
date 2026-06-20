#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
HOST=${HOST_IP:-192.168.1.139}
PORT=${LOCAL_VIDEO_PORT:-8001}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VIDEO=${1:-test.mp4}

python3 -u -m http.server "$PORT" --bind "$HOST" --directory "$ROOT/app/assets" >/tmp/ps4cast_local_video.log 2>&1 &
SERVER_PID=$!
cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

sleep 1
URL="http://$HOST:$PORT/$VIDEO"
echo "Playing $URL"
curl -sS -m 5 -X POST "http://$PS4:8080/play" --data-binary "$URL"
echo

for _ in $(seq 1 12); do
  sleep 2
  status=$(curl -sS -m 5 "http://$PS4:8080/status")
  echo "$status"
  if printf '%s' "$status" | grep -q '"status":"finished"'; then
    exit 0
  fi
done

echo "Local video did not finish within timeout." >&2
exit 1
