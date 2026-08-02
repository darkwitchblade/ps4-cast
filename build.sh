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

# Regenerate the embedded web UI header from its .html source so the served page
# is always in sync with web_ui_src.html (the editable source of truth).
if [ -f "$HERE/app/src/web_ui_src.html" ]; then
    python3 "$HERE/scripts/gen-web-ui.py"
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
if ! make "$@"; then
    # The bundled x86_64 macOS PkgTool uses an old AppleCrypto bridge that no
    # longer imports its RSA key on macOS 26. Compilation/linking and GP4
    # generation have already succeeded at this point, so retry only the signer
    # with OpenOrbis's matching Linux binary in its required .NET 3.1 runtime.
    if [ "$(uname -s)" = "Darwin" ] && [ -f pkg.gp4 ] && command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        echo "macOS PkgTool failed; retrying package signing in Docker..."
        docker run --platform linux/amd64 --rm \
          -e DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
          -v "$HERE:/work" -w /work/app \
          mcr.microsoft.com/dotnet/runtime:3.1-buster-slim \
          /work/oo/OpenOrbis/PS4Toolchain/bin/linux/PkgTool.Core pkg_build pkg.gp4 .
    else
        exit 1
    fi
fi

VER="$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,"",$2); print $2}' "$HERE/app/Makefile")"
OUT="PS4-Cast-v${VER}.pkg"
PKG="$(ls "$HERE/app/"*.pkg 2>/dev/null | head -1)"
if [ -n "$PKG" ] && [ -f "$PKG" ]; then
    mkdir -p "$HERE/dist"
    cp "$PKG" "$HERE/dist/$OUT"
    echo
    echo "Built: dist/$OUT ($(du -h "$HERE/dist/$OUT" | cut -f1))"
fi
