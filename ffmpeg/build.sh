#!/bin/sh
#
# DOS-XC /ffmpeg/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
FFMPEG_VERSION=6.1.2

build_openssl
build_zlib
fetch ffmpeg-$FFMPEG_VERSION.tar.xz https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz
unpack ffmpeg-$FFMPEG_VERSION.tar.xz ffmpeg
cd "$WORK/ffmpeg"
case "$ARCH" in x86_64) asm= ;; *) asm=--disable-asm ;; esac
# Static libraries in the port sysroot (VLC links them) plus the ffmpeg and
# ffprobe programs.  DOS-XC programs are position independent.
run_logged ffmpeg-configure ./configure --prefix="$SYSROOT" --enable-cross-compile --cross-prefix=$TARGET- \
    --arch=$ARCH --target-os=linux --cc="$CC_PORT" --cxx="$TARGET-g++ --sysroot=$SYSROOT" --nm=$TARGET-nm \
    --ar=$TARGET-ar --ranlib=$TARGET-ranlib --strip=$TARGET-strip --pkg-config=false --enable-pic $asm \
    --disable-shared --enable-static --disable-doc --disable-debug --disable-ffplay --enable-ffmpeg --enable-ffprobe \
    --disable-autodetect --enable-zlib --enable-openssl --enable-version3 --enable-pthreads --disable-vaapi --disable-vdpau \
    --disable-v4l2-m2m --disable-indev=v4l2 --disable-outdev=v4l2 --disable-indev=fbdev --disable-outdev=fbdev \
    --extra-cflags="-O2 -I$SYSROOT/include" --extra-ldflags="-L$SYSROOT/lib" --extra-libs="-lssl -lcrypto -lz"
run_logged ffmpeg-make make -j"$JOBS" EXESUF=.exe
run_logged ffmpeg-install make install EXESUF=.exe
rm -rf "$OUT/ffmpeg"
mkdir -p "$OUT/ffmpeg/BIN"
for program in ffmpeg ffprobe; do
    target=$(echo "$program" | tr a-z A-Z).EXE
    cp "$program" "$OUT/ffmpeg/BIN/$target" 2>/dev/null || cp "$program.exe" "$OUT/ffmpeg/BIN/$target"
    "$TARGET-strip" "$OUT/ffmpeg/BIN/$target"
done
cp COPYING.GPLv3 "$OUT/ffmpeg/COPYING.TXT" 2>/dev/null || cp COPYING.LGPLv2.1 "$OUT/ffmpeg/COPYING.TXT"
touch "$SYSROOT/lib/.ffmpeg-$FFMPEG_VERSION"
echo "FFmpeg $FFMPEG_VERSION -> $OUT/ffmpeg"
