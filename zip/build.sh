#!/bin/sh
#
# DOS-XC /zip/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"

fetch zip30.tar.gz https://downloads.sourceforge.net/infozip/zip30.tar.gz
fetch unzip60.tar.gz https://downloads.sourceforge.net/infozip/unzip60.tar.gz
unpack zip30.tar.gz zip
unpack unzip60.tar.gz unzip
# Old code: C23's stricter rules and missing prototypes are left as they were.
LEGACY="-O2 -std=gnu89 -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types"
cd "$WORK/zip"
# zip's configure probes with programs GCC 15 rejects, so its answers are given here.
run_logged zip-make make -f unix/Makefile zips CC="$CC_PORT" CPP="$CC_PORT -E" OBJA= OCRCU8=crc32_.o OCRCTB= \
    CFLAGS="-I. -DUNIX $LEGACY -DNO_BZIP2_SUPPORT -DLARGE_FILE_SUPPORT -DUIDGID_NOT_16BIT -DUNICODE_SUPPORT -DHAVE_DIRENT_H -DHAVE_TERMIOS_H"
cd "$WORK/unzip"
run_logged unzip-make make -f unix/Makefile unzips CC="$CC_PORT" LD="$CC_PORT" \
    CF="-I. -DUNIX $LEGACY -DLARGE_FILE_SUPPORT -DNO_LCHMOD -DNO_LCHOWN -DDIRENT -DHAVE_TERMIOS_H -DUNICODE_SUPPORT"
rm -rf "$OUT/zip"
mkdir -p "$OUT/zip/BIN"
for program in "$WORK/zip/zip" "$WORK/zip/zipnote" "$WORK/zip/zipsplit" "$WORK/zip/zipcloak" "$WORK/unzip/unzip" \
               "$WORK/unzip/funzip" "$WORK/unzip/unzipsfx"; do
    [ -f "$program.exe" ] && program=$program.exe
    target=$(basename "$program" .exe | tr a-z A-Z)
    cp "$program" "$OUT/zip/BIN/$target.EXE"
    "$TARGET-strip" "$OUT/zip/BIN/$target.EXE"
done
cp "$WORK/zip/LICENSE" "$OUT/zip/LICENSE.TXT"
echo "Info-ZIP zip 3.0 and unzip 6.0 -> $OUT/zip"
