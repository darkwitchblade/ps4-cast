#!/usr/bin/env bash
# One-command development pipeline: build -> close -> uninstall -> install-ready
# -> launch with signed-in user -> verify exact version.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

export PS4_IP=${PS4_IP:-192.168.1.4}
export PS4_USER_ID=${PS4_USER_ID:?"set PS4_USER_ID to your console local user id"}
if [ -z "${HOST_IP:-}" ]; then
  HOST_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
  [ -n "$HOST_IP" ] || { echo "Cannot determine Mac LAN IP; set HOST_IP." >&2; exit 2; }
  export HOST_IP
fi

# Refresh the authenticated-launch pairing when available. An absent pairing
# must NOT block the build/install half of the pipeline: install goes through
# the resident DPI agent regardless; only the final launch needs Chiaki, and a
# missing pairing degrades to "tap the PS4 Cast icon manually" instead of
# failing the whole run.
PS4_IP="$PS4_IP" HOST_IP="$HOST_IP" scripts/setup-remote-launch.sh || {
  echo "NOTE: Remote Play pairing unavailable -- install will proceed, launch manually from the home screen."
}

case "${1:-}" in
  "")      exec scripts/redeploy.sh ;;
  nobuild) exec scripts/redeploy.sh nobuild ;;
  test)
    scripts/redeploy.sh
    exec scripts/streamtest/run.sh
    ;;
  test-nobuild)
    scripts/redeploy.sh nobuild
    exec scripts/streamtest/run.sh
    ;;
  *) echo "usage: scripts/dev-deploy.sh [nobuild|test|test-nobuild]" >&2; exit 2 ;;
esac
