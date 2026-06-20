#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PS4=${PS4_IP:-192.168.1.253}
HOST=${HOST_IP:-192.168.1.139}

free_port() {
  python3 - "$HOST" "$1" <<'PY'
import socket, sys
host = sys.argv[1]
start = int(sys.argv[2])
for port in range(start, start + 200):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind((host, port))
        except OSError:
            continue
        print(port)
        break
else:
    raise SystemExit("no free port")
PY
}

PKG_PORT=${PS4CAST_PKG_PORT:-$(free_port 8000)}
MANIFEST_PORT=${PS4CAST_MANIFEST_PORT:-$(free_port 9898)}

./build.sh
scripts/close-ps4cast.sh || true
if [ "${PS4CAST_DELETE_BEFORE_INSTALL:-1}" = "1" ]; then
  scripts/delete-ps4cast.sh || true
  sleep 5
fi
python3 scripts/push-goldhen-dpi.py --host "$HOST" --ps4 "$PS4" \
  --pkg-port "$PKG_PORT" --manifest-port "$MANIFEST_PORT" --keepalive 180

VER="$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,"",$2);print $2}' app/Makefile)"

if ! scripts/open-ps4cast.sh "$VER"; then
  echo "Open PS4 Cast on the console, then run: scripts/test-ps4cast.sh" >&2
  exit 1
fi

scripts/test-ps4cast.sh
