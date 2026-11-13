#!/bin/sh
#
# DOS-XC /nano/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
NANO_VERSION=8.6

build_ncurses
fetch nano-$NANO_VERSION.tar.xz https://ftpmirror.gnu.org/gnu/nano/nano-$NANO_VERSION.tar.xz
unpack nano-$NANO_VERSION.tar.xz nano
cd "$WORK/nano"
run_logged nano-configure env gl_cv_func_getcwd_path_max=yes ac_cv_func_fork=yes \
    ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/nano \
    --disable-nls --disable-libmagic --disable-speller --enable-utf8 --sysconfdir=/Programs/nano/ETC \
    CC="$CC_PORT" CFLAGS="-O2 -I$SYSROOT/include/ncursesw" NCURSESW_CFLAGS="-I$SYSROOT/include/ncursesw" \
    NCURSESW_LIBS="-lncursesw" LIBS="-lncursesw"
run_logged nano-make make -j"$JOBS"
rm -rf "$WORK/nano-staging"
run_logged nano-install make install DESTDIR="$WORK/nano-staging"
rm -rf "$OUT/nano"
mkdir -p "$OUT/nano/BIN" "$OUT/nano/SYNTAX" "$OUT/nano/ETC"
cp "$WORK/nano-staging/Programs/nano/bin/nano"* "$OUT/nano/BIN/NANO.EXE"
"$TARGET-strip" "$OUT/nano/BIN/NANO.EXE"
cp "$WORK/nano-staging/Programs/nano/share/nano/"*.nanorc "$OUT/nano/SYNTAX/"
printf 'include "/Programs/nano/SYNTAX/*.nanorc"\nset linenumbers\nset mouse\n' > "$OUT/nano/ETC/NANORC"
cp COPYING "$OUT/nano/COPYING.TXT"
echo "nano $NANO_VERSION -> $OUT/nano"
