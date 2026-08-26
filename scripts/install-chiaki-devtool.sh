#!/usr/bin/env bash
# Install the pinned, signed Apple-silicon chiaki-ng build for dev automation.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/.devtools/chiaki-ng.app"
PATCHER="$ROOT/scripts/patch-chiaki-discovery.py"
VERSION="1.10.0"
DMG_SHA256="72eed7494614477dc1012b951e90ebeb212d84cdbcfa04e16adb330a022ad467"
URL="https://github.com/streetpea/chiaki-ng/releases/download/v${VERSION}/chiaki-ng-macos_arm64-Release.dmg"

[ "$(uname -m)" = "arm64" ] || {
  echo "The pinned devtool package currently supports Apple silicon only." >&2
  exit 2
}

if [ -x "$DEST/Contents/MacOS/chiaki" ]; then
  python3 "$PATCHER" "$DEST/Contents/MacOS/chiaki"
  codesign --force --deep --sign - --preserve-metadata=entitlements,requirements,flags "$DEST" >/dev/null
  codesign --verify --deep --strict "$DEST" >/dev/null
  echo "Patched chiaki-ng v$VERSION is already installed: $DEST"
  exit 0
fi

mkdir -p "$ROOT/.devtools"
TMP="$(mktemp -d /tmp/ps4cast-chiaki.XXXXXX)"
MOUNT="$TMP/mount"
DMG="$TMP/chiaki-ng.dmg"
mounted=0
cleanup() {
  if [ "$mounted" = "1" ]; then
    hdiutil detach "$MOUNT" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

echo "Downloading chiaki-ng v$VERSION from the official GitHub release..."
curl -fL --retry 3 --connect-timeout 15 -o "$DMG" "$URL"
actual="$(shasum -a 256 "$DMG" | awk '{print $1}')"
[ "$actual" = "$DMG_SHA256" ] || {
  echo "chiaki-ng checksum mismatch: $actual" >&2
  exit 1
}

mkdir "$MOUNT"
hdiutil attach -nobrowse -readonly -mountpoint "$MOUNT" "$DMG" >/dev/null
mounted=1
ditto "$MOUNT/chiaki-ng.app" "$DEST"
codesign --verify --deep --strict "$DEST" >/dev/null
python3 "$PATCHER" "$DEST/Contents/MacOS/chiaki"
codesign --force --deep --sign - --preserve-metadata=entitlements,requirements,flags "$DEST" >/dev/null
codesign --verify --deep --strict "$DEST" >/dev/null
echo "Installed verified and patched chiaki-ng v$VERSION: $DEST"
