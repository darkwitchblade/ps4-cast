#!/usr/bin/env bash
# One-time setup for authenticated launch through PS4 Remote Play.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
umask 077

PS4=${PS4_IP:-192.168.1.4}
CHIAKI=${PS4CAST_CHIAKI_BIN:-$ROOT/.devtools/chiaki-ng.app/Contents/MacOS/chiaki}
CHIAKI_PROFILE=${PS4CAST_CHIAKI_PROFILE:-ps4cast-dev}
PAIRED=${PS4CAST_REMOTE_PAIRED_MARKER:-$ROOT/.chiaki-remote.paired}
ACCOUNT_CACHE=${PS4CAST_ACCOUNT_ID_CACHE:-$ROOT/.chiaki-account-id}
KEY_SOURCE=$ROOT/scripts/macos-send-key.c
KEY_TOOL=$ROOT/.devtools/ps4cast-send-key

if [ ! -x "$CHIAKI" ]; then
  "$ROOT/scripts/install-chiaki-devtool.sh"
fi

if [ ! -x "$KEY_TOOL" ] || [ "$KEY_SOURCE" -nt "$KEY_TOOL" ]; then
  /usr/bin/clang -O2 "$KEY_SOURCE" -framework ApplicationServices -o "$KEY_TOOL"
  chmod 700 "$KEY_TOOL"
fi

list="$($CHIAKI --profile "$CHIAKI_PROFILE" list 2>/dev/null || true)"
nickname="$(printf '%s\n' "$list" | sed -n 's/^Host:[[:space:]]*//p' | sed 's/[[:space:]]*$//' | head -1)"
if [ -n "$nickname" ]; then
  printf 'nickname=%s\nhost=%s\nprofile=%s\n' "$nickname" "$PS4" "$CHIAKI_PROFILE" > "$PAIRED"
  chmod 600 "$PAIRED"
  echo "REMOTE PLAY READY: $nickname at $PS4"
  exit 0
fi

account_hint="$(sed -n '1p' "$ACCOUNT_CACHE" 2>/dev/null || true)"
# NOTE: never probe GoldHEN :9090 with a TCP connect here -- the payload server
# is single-shot per rearm and a bare connection can burn it. Try the read-only
# app-status request directly instead and tolerate failure.
if [ -z "$account_hint" ]; then
  account_hint="$(PS4_IP="$PS4" "$ROOT/scripts/get-ps4-account-id.sh" 2>/dev/null || true)"
fi
if [ -n "$account_hint" ]; then
  printf '%s' "$account_hint" | pbcopy
  echo "Copied this console's Chiaki Account ID to the clipboard: $account_hint"
fi

open "$ROOT/.devtools/chiaki-ng.app" --args --profile "$CHIAKI_PROFILE"
cat >&2 <<EOF
One-time Remote Play registration is required.

1. In chiaki-ng, select the discovered PS4 (or add $PS4 manually).
2. Enter the same PSN Account ID used by Chiaki-Up.
3. On PS4: Settings -> Remote Play Connection Settings -> Add Device.
4. Enter the fresh 8-digit Remote Play PIN in chiaki-ng and register.
5. Rerun scripts/setup-remote-launch.sh.

No PSN password is stored by this project. After registration, deploy and
launch can run unattended through the local Remote Play identity.
EOF
exit 2
