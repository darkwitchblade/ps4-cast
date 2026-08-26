#!/usr/bin/env bash
# One ritual for cutting a release:
#   scripts/release.sh 04.49        bump VERSION, build, append STEERING note
#   scripts/release.sh 04.49 -n     same but skip the build (notes only)
# Appends a "## vX: release" section to STEERING.md with the pkg SHA-256.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VER="${1:-}"
[ -n "$VER" ] || { echo "usage: scripts/release.sh <version> [-n]" >&2; exit 2; }
NOBUILD="${2:-}"

current="$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,"",$2); print $2}' app/Makefile)"
if [ "$VER" != "$current" ]; then
    sed -i '' "s/^VERSION     := $current/VERSION     := $VER/" app/Makefile
    echo "VERSION: $current -> $VER"
fi

if [ "$NOBUILD" != "-n" ]; then
    ./build.sh
fi

pkg="dist/PS4-Cast-v$VER.pkg"
[ -f "$pkg" ] || { echo "missing $pkg" >&2; exit 1; }
sha="$(shasum -a 256 "$pkg" | awk '{print $1}')"
size="$(du -h "$pkg" | cut -f1)"

if ! grep -q "Built package: \`$pkg\`" STEERING.md; then
    tmp="$(mktemp)"
    {
        echo
        echo "## v$VER: release"
        echo
        echo "- Built package: \`$pkg\`, SHA-256 \`$sha\` ($size)."
        echo "- (Fill in the change summary before committing.)"
        echo
        cat STEERING.md
    } > "$tmp"
    mv "$tmp" STEERING.md
    echo "STEERING.md: prepended v$VER release note"
fi
echo "release ready: $pkg ($size)"
echo "  sha256 $sha"
