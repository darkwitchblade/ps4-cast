#!/usr/bin/env bash
# Detect a crashed/hung PS4 Cast and auto-recover it (force-kill + relaunch) via
# the GoldHEN :9090 payload loader, capturing /crashlog first. Lets test/dev
# loops self-heal through crashes instead of getting stuck.
#
# Crash dialog: the app's fatal-signal handler _exit(0)s and the freeze-watchdog
# _exit(0)s on hangs, so a clean exit normally PREVENTS the system CE-error
# dialog. force-kill here also clears a stuck app. (A hard fault that bypasses the
# handler could still show a modal needing a manual OK — rare.)
#
# Exit: 0 recovered / already up, 1 :9090 not accepting (needs manual re-arm),
#       2 console unreachable (power/network).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PS4=${PS4_IP:-192.168.1.33}
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/ps4-api.sh
KILL="$ROOT/payloads/ps4cast-control/build/ps4cast-kill.bin"
LAUNCH="$ROOT/payloads/ps4cast-control/build/ps4cast-launch.bin"

send()   { python3 "$ROOT/scripts/send-goldhen-payload.py" "$1" --ps4 "$PS4" 2>&1 | tail -1; }
status() { curl -s -m5 "http://$PS4:8080/status" 2>/dev/null; }

if [ -n "$(status)" ]; then echo "app UP — no recovery needed"; exit 0; fi
if ! ping -c1 -t3 "$PS4" >/dev/null 2>&1; then
  echo "console UNREACHABLE — power/network issue, manual intervention needed"; exit 2
fi
echo "app DOWN, console alive — recovering..."

cl="$(curl -s -m4 "http://$PS4:8080/crashlog" 2>/dev/null)"
[ -n "$cl" ] && echo "crashlog(pre-relaunch): $cl"

for attempt in 1 2 3; do
  echo "  attempt $attempt: kill + launch"
  send "$KILL"
  sleep 2
  send "$LAUNCH"
  for i in $(seq 1 10); do
    sleep 3
    if [ -n "$(status)" ]; then
      echo "RECOVERED on attempt $attempt"
      curl -s -m4 "http://$PS4:8080/crashlog" 2>/dev/null | sed 's/^/crashlog: /'
      exit 0
    fi
  done
  echo "  attempt $attempt did not bring the app back"
done
echo "RECOVERY FAILED — :9090 not accepting payloads; re-arm the GoldHEN loader on the console"
exit 1
