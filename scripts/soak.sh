#!/usr/bin/env bash
# Autonomous live-HLS soak: keep the local live-sim casting, auto-recover on any
# crash, and log every crash + its /crashlog so we capture a GFX-FAULT / pin the
# intermittent GPU crash. Pair with klog-capture.py for the system-side reason.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PS4=${PS4_IP:-192.168.1.33}
HOST=${HOST_IP:-192.168.1.157}
URL=${SOAK_URL:-http://$HOST:8010/live.m3u8}
LOG=${SOAK_LOG:-/tmp/soak.log}
n=0; crashes=0
echo "==== soak start $(date +%H:%M:%S) url=$URL ====" >> "$LOG"
while true; do
  n=$((n+1))
  st=$(curl -s -m5 "http://$PS4:8080/status" 2>/dev/null)
  if [ -z "$st" ]; then
    crashes=$((crashes+1))
    cl=$(curl -s -m4 "http://$PS4:8080/crashlog" 2>/dev/null)   # often unreachable while down
    echo "[$(date +%H:%M:%S)] CRASH #$crashes (iter $n) recovering; crashlog(pre)=$cl" >> "$LOG"
    PS4_IP=$PS4 bash "$ROOT/scripts/auto-recover.sh" >> "$LOG" 2>&1
    sleep 2
    echo "[$(date +%H:%M:%S)] post-recover crashlog=$(curl -s -m4 "http://$PS4:8080/crashlog" 2>/dev/null)" >> "$LOG"
    curl -s -m6 -X POST "http://$PS4:8080/play" --data "$URL" >/dev/null 2>&1
    echo "[$(date +%H:%M:%S)] re-cast after recover" >> "$LOG"
  elif ! echo "$st" | grep -q '"status":"playing'; then
    curl -s -m6 -X POST "http://$PS4:8080/play" --data "$URL" >/dev/null 2>&1
    echo "[$(date +%H:%M:%S)] (re)cast live sim (was idle/stopped)" >> "$LOG"
  else
    # If the app drifts far behind the live edge (lag huge), re-cast to reset to
    # the edge so the soak keeps exercising fresh live decode rather than chasing.
    lag=$(echo "$st" | sed -n 's/.*lag=\([0-9]*\)ms.*/\1/p')
    if [ -n "$lag" ] && [ "$lag" -gt 15000 ]; then
      curl -s -m6 -X POST "http://$PS4:8080/play" --data "$URL" >/dev/null 2>&1
      echo "[$(date +%H:%M:%S)] re-cast (lag=${lag}ms > 15s, resetting to live edge)" >> "$LOG"
    fi
  fi
  sleep 20
done
