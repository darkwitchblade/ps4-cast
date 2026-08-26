#!/usr/bin/env bash
# Fetch the vendored portlibs source tarballs into portlibs/src/.
# The trees are extracted by portlibs/build-*.sh; nothing here is tracked by git.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
mkdir -p "$SRC"

fetch() { # url expected_sha256 out
    local url="$1" want="$2" out="$SRC/$3"
    if [ -f "$out" ]; then
        echo "have $3"
    else
        echo "fetching $3 ..."
        curl -fL --retry 3 -o "$out" "$url"
    fi
    local got; got="$(shasum -a 256 "$out" | awk '{print $1}')"
    if [ "$got" != "$want" ]; then
        echo "SHA-256 mismatch for $3: got $got want $want" >&2
        exit 1
    fi
}

# Pinned upstream tarballs. Update the hashes together with build-*..sh.
fetch https://bearssl.org/bearssl-0.6.tar.gz \
    6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14 bearssl.tar.gz

fetch https://ffmpeg.org/releases/ffmpeg-6.1.6.tar.xz \
    d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a ffmpeg.tar.xz

if [ ! -f "$SRC/mbedtls.tar.bz2" ]; then
    echo "mbedtls.tar.bz2: optional; only needed by the native HTTP experiments" >&2
fi

# Extract pinned trees if the build scripts' expected directories are missing.
[ -d "$SRC/bearssl-0.6" ]  || tar -xzf "$SRC/bearssl.tar.gz"  -C "$SRC"
[ -d "$SRC/ffmpeg-6.1.6" ] || tar -xJf "$SRC/ffmpeg.tar.xz" -C "$SRC"

echo "portlibs sources ready"
