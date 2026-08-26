#!/usr/bin/env bash
# Fetch the OpenOrbis PS4 toolchain (LLVM 18 build) and unpack it.
# Produces oo/OpenOrbis/PS4Toolchain. The 154MB tarball is cached outside the
# repo in ../ps4-cast-artifacts/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="${PS4CAST_ARTIFACTS:-$ROOT/../ps4-cast-artifacts}"
TARBALL="$CACHE/toolchain-llvm-18.tar.gz"
URL="https://github.com/OpenOrbis/OpenOrbisToolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz"
WANT="3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526"

mkdir -p "$CACHE" "$ROOT/oo"
if [ ! -f "$TARBALL" ]; then
    echo "fetching toolchain tarball ..."
    curl -fL --retry 3 -o "$TARBALL" "$URL"
fi
got="$(shasum -a 256 "$TARBALL" | awk '{print $1}')"
if [ "$got" != "$WANT" ]; then
    echo "SHA-256 mismatch: got $got want $WANT" >&2
    exit 1
fi
if [ ! -d "$ROOT/oo/OpenOrbis/PS4Toolchain" ]; then
    echo "unpacking into oo/ ..."
    tar -xzf "$TARBALL" -C "$ROOT/oo"
fi
echo "toolchain ready: $ROOT/oo/OpenOrbis/PS4Toolchain"
