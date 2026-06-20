#!/usr/bin/env bash
# Cross-compile BearSSL (TLS 1.2 client) as a static lib for the PS4 / OpenOrbis
# target. BearSSL has no platform dependencies (no sockets/files/time/entropy
# modules), which makes it the safest TLS lib to drop into a homebrew ELF — I/O
# and randomness are supplied by the caller (see app/src/tls.c).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TC="${OO_PS4_TOOLCHAIN:-$HERE/../oo/OpenOrbis/PS4Toolchain}"
SRC="$HERE/src/bearssl-0.6"
OUT="$HERE/bearssl"
LLVM="/opt/homebrew/opt/llvm/bin"
TARGET="x86_64-pc-freebsd12-elf"

CFLAGS="--target=$TARGET -isysroot $TC -isystem $TC/include -I$SRC/src -I$SRC/inc \
  -fPIC -O2 -ffunction-sections -fdata-sections -D_GNU_SOURCE -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0"

rm -rf "$OUT/obj"; mkdir -p "$OUT/obj" "$OUT/lib" "$OUT/include"

n=0
while IFS= read -r f; do
    obj="$OUT/obj/$(echo "$f" | sed 's#/#_#g').o"
    "$LLVM/clang" $CFLAGS -c "$SRC/$f" -o "$obj"
    n=$((n+1))
done < <(cd "$SRC" && find src -name '*.c')

"$LLVM/llvm-ar" rcs "$OUT/lib/libbearssl.a" "$OUT"/obj/*.o
cp "$SRC"/inc/*.h "$OUT/include/"
echo "built libbearssl.a from $n objects:"
ls -la "$OUT/lib/libbearssl.a"
