#!/usr/bin/env bash
# Build PS4 Cast into an installable fake-signed .pkg.
# Usage: ./build.sh          (build)
#        ./build.sh clean    (clean)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
export OO_PS4_TOOLCHAIN="$HERE/oo/OpenOrbis/PS4Toolchain"

if [ ! -d "$OO_PS4_TOOLCHAIN" ]; then
    echo "Toolchain missing at $OO_PS4_TOOLCHAIN" >&2
    exit 1
fi

# Ensure the macOS create-fself alias exists (release ships create-fself-macos).
if [ ! -e "$OO_PS4_TOOLCHAIN/bin/macos/create-fself" ]; then
    ln -sf create-fself-macos "$OO_PS4_TOOLCHAIN/bin/macos/create-fself"
fi

cd "$HERE/app"

if [ "${1:-}" = "clean" ]; then
    make clean
    rm -f "$HERE/app/"*.pkg
    exit 0
fi

# Clear stale pkgs and force a full object rebuild so APP_VER (baked into objects
# from the Makefile VERSION) is always current.
rm -f "$HERE/app/"*.pkg "$HERE/app/build/"*.o
make "$@"

VER="$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,"",$2); print $2}' "$HERE/app/Makefile")"
OUT="PS4-Cast-v${VER}.pkg"
PKG="$(ls "$HERE/app/"*.pkg 2>/dev/null | head -1)"
if [ -n "$PKG" ] && [ -f "$PKG" ]; then
    mkdir -p "$HERE/dist"
    cp "$PKG" "$HERE/dist/$OUT"
    echo
    echo "Built: dist/$OUT ($(du -h "$HERE/dist/$OUT" | cut -f1))"
fi
