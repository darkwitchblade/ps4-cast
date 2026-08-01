#!/usr/bin/env bash
# Cross-compile a minimal ffmpeg (demux + software decode only) for the PS4 /
# OpenOrbis target. Networking is DISABLED on purpose: OpenOrbis has no BSD
# sockets routed to the console net stack, so PS4 Cast feeds ffmpeg through a
# custom AVIO callback backed by our sceNet reader (httpsrc). https therefore
# lives in our reader (TLS), not in ffmpeg.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TC="${OO_PS4_TOOLCHAIN:-$HERE/../oo/OpenOrbis/PS4Toolchain}"
SRC="$HERE/src/ffmpeg-6.1.6"
PREFIX="$HERE/ffmpeg"

LLVM="/opt/homebrew/opt/llvm/bin"
LLD="/opt/homebrew/opt/lld/bin"
export PATH="$LLD:$LLVM:$PATH"   # so clang -fuse-ld=lld finds ld.lld
TARGET="x86_64-pc-freebsd12-elf"
SYSFLAGS="--target=$TARGET -I$HERE/compat -isysroot $TC -isystem $TC/include -fPIC -D_GNU_SOURCE -O2 -ffunction-sections -fdata-sections"
# Link flags that make configure's trivial-exe test succeed against OpenOrbis
# (crt1.o + link script + the base libs). We never ship an exe — only .a libs —
# but configure insists on a working linker.
LDF="--target=$TARGET -fuse-ld=lld -nostdlib -Wl,--script=$TC/link.x -Wl,-pie -L$TC/lib $TC/lib/crt1.o -lc -lkernel -lc++"

cd "$SRC"

make distclean >/dev/null 2>&1 || true

./configure \
  --prefix="$PREFIX" \
  --cc="$LLVM/clang" \
  --cxx="$LLVM/clang++" \
  --ar="$LLVM/llvm-ar" \
  --nm="$LLVM/llvm-nm" \
  --ranlib="$LLVM/llvm-ranlib" \
  --strip="$LLVM/llvm-strip" \
  --enable-cross-compile \
  --arch=x86_64 \
  --target-os=freebsd \
  --extra-cflags="$SYSFLAGS" \
  --extra-cxxflags="$SYSFLAGS" \
  --extra-ldflags="$LDF" \
  --pkg-config=false \
  --disable-everything \
  --disable-network \
  --disable-autodetect \
  --disable-programs \
  --disable-doc \
  --disable-debug \
  --disable-shared \
  --enable-static \
  --enable-asm \
  --enable-x86asm \
  --x86asmexe=nasm \
  --enable-pthreads \
  --disable-iconv \
  --disable-xlib \
  --disable-zlib \
  --disable-bzlib \
  --disable-lzma \
  --disable-sdl2 \
  --disable-protocols \
  --disable-devices \
  --disable-filters \
  --enable-avformat \
  --enable-avcodec \
  --enable-swscale \
  --enable-swresample \
  --enable-decoder=h264,hevc,mpeg4,mpeg2video,mpeg1video,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,vc1,h263,vp8,vp9,aac,aac_latm,mp3,ac3,eac3,opus,vorbis,flac,pcm_s16le,pcm_s16be,mjpeg \
  --enable-parser=h264,hevc,mpeg4video,mpegvideo,aac,aac_latm,mpegaudio,vp8,vp9,opus,vc1,h263 \
  --enable-demuxer=mov,matroska,mpegts,mpegps,mpegvideo,mp3,flac,wav,flv,hls,aac,ogg,avi,asf,m4v \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc

echo "=== configure OK; building libs ==="
make -j"$(sysctl -n hw.ncpu)"
make install
echo "=== ffmpeg libs installed to $PREFIX/lib ==="
ls -la "$PREFIX/lib" 2>/dev/null || true
