#!/usr/bin/env bash
# Build current source and push to PS4 through GoldHEN's HTTP payload server.
# Then launch "PS4 Cast <ver>" on the console through GoldHEN and run tests.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
PS4=${PS4_IP:-192.168.1.253}; HOST=${HOST_IP:-192.168.1.139}

./build.sh >/tmp/ps4cast_build.log 2>&1 || { echo "BUILD FAILED"; tail -25 /tmp/ps4cast_build.log; exit 1; }
VER="$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,"",$2);print $2}' app/Makefile)"
PKG="PS4-Cast-v${VER}.pkg"
echo "Built $PKG  (title: PS4 Cast $VER)"

scripts/close-ps4cast.sh || true
python3 scripts/push-goldhen-dpi.py --host "$HOST" --ps4 "$PS4"
scripts/open-ps4cast.sh "$VER"
