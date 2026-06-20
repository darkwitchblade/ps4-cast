#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

"$ROOT/scripts/close-ps4cast.sh" || true

make -C "$ROOT/payloads/ps4cast-control" build/ps4cast-uninstall.bin >/tmp/ps4cast_control_build.log
python3 "$ROOT/scripts/send-goldhen-payload.py" "$ROOT/payloads/ps4cast-control/build/ps4cast-uninstall.bin" --ps4 "$PS4"

echo "PS4 Cast uninstall payload sent."
