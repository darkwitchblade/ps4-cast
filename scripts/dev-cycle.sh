#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PS4=${PS4_IP:-192.168.1.4}
HOST=${HOST_IP:-$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)}

PS4_IP="$PS4" HOST_IP="$HOST" scripts/dev-deploy.sh
PS4_IP="$PS4" scripts/test-ps4cast.sh
