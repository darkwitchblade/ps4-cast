#!/usr/bin/env bash
# Wait for the PS4 Cast app web server, trigger the local test clip, print debug.
set -euo pipefail
PS4=${PS4_IP:-192.168.1.253}
SRC="${1:-TEST}"   # TEST = bundled clip; or pass a URL

echo "waiting for app web server on $PS4:8080..."
for i in $(seq 1 120); do
  curl -m 3 -s "http://$PS4:8080/status" 2>/dev/null | grep -q '"status"' && { echo "app up"; break; }
  sleep 2
done
curl -m 5 -s -X POST "http://$PS4:8080/play" -H "Content-Type: text/plain" --data-raw "$SRC" >/dev/null || true
echo "playing '$SRC'; debug:"
for i in $(seq 1 10); do
  sleep 2
  echo "t=$((i*2))s: $(curl -m 5 -s "http://$PS4:8080/status" 2>/dev/null)"
done
