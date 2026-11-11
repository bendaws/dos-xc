#!/bin/sh
#
# DOS-XC /xz/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
XZ_VERSION=5.8.1

fetch xz-$XZ_VERSION.tar.gz https://github.com/tukaani-project/xz/releases/download/v$XZ_VERSION/xz-$XZ_VERSION.tar.gz
unpack xz-$XZ_VERSION.tar.gz xz
cd "$WORK/xz"
# argv[0] is "C:/PATH/XZCAT.EXE": strip the path and suffix and lower
# the case as on other DOS-like systems, so XZCAT and UNXZ work.
sed -i.orig 's/^#ifdef TUKLIB_DOSLIKE$/#if defined(TUKLIB_DOSLIKE) || defined(__dosxc__)/' src/common/tuklib_progname.c
run_logged xz-configure ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/XZ \
    --disable-shared --enable-static --disable-threads --disable-nls --disable-doc --disable-scripts \
    --disable-lzmadec --disable-lzmainfo --disable-lzma-links --disable-xzdec \
    --disable-sandbox CC="$CC_PORT" CFLAGS="-O2"
run_logged xz-make make -j"$JOBS"
rm -rf "$WORK/xz-staging"
run_logged xz-install make install-strip DESTDIR="$WORK/xz-staging"
rm -rf "$OUT/xz"
mkdir -p "$OUT/xz/BIN"
cp "$WORK/xz-staging/Programs/XZ/bin/xz.exe" "$OUT/xz/BIN/XZ.EXE"
cp "$OUT/xz/BIN/XZ.EXE" "$OUT/xz/BIN/UNXZ.EXE"
cp "$OUT/xz/BIN/XZ.EXE" "$OUT/xz/BIN/XZCAT.EXE"
cp "$WORK/xz/COPYING" "$OUT/xz/COPYING.TXT"
echo "xz $XZ_VERSION -> $OUT/xz"
