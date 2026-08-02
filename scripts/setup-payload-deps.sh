#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/DirectPackageInstaller"
REPO="https://github.com/marcussacana/DirectPackageInstaller.git"
# Known-good DN6 source used by the original working PS4 Cast control payloads.
REV="711bacfcfe812280414fbe36f4a6193354c40848"

if [ ! -d "$DEST/.git" ]; then
  mkdir -p "$(dirname "$DEST")"
  git clone --filter=blob:none --no-checkout "$REPO" "$DEST"
fi

if [ -n "$(git -C "$DEST" status --porcelain 2>/dev/null)" ]; then
  echo "Payload dependency has local changes: $DEST" >&2
  exit 1
fi

if ! git -C "$DEST" cat-file -e "$REV^{commit}" 2>/dev/null; then
  git -C "$DEST" fetch --depth 1 origin "$REV"
fi
git -C "$DEST" checkout --quiet --detach "$REV"

test -f "$DEST/Payload/ps4-libjbc/jailbreak.h"
test -f "$DEST/Payload/lib/crt.asm"
echo "DirectPackageInstaller payload dependency ready at $REV"
