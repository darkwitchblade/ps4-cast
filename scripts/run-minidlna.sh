#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONF=${CONF:-"$ROOT/scripts/minidlna-ps4cast.conf"}
PID=${PID:-/private/tmp/ps4cast-minidlna.pid}

mkdir -p /private/tmp/ps4cast-minidlna-db
exec /opt/homebrew/opt/minidlna/sbin/minidlnad -d -R -f "$CONF" -P "$PID"
