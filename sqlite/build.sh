#!/bin/sh
#
# DOS-XC /sqlite/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
SQLITE_VERSION=3500400

fetch sqlite-autoconf-$SQLITE_VERSION.tar.gz https://www.sqlite.org/2025/sqlite-autoconf-$SQLITE_VERSION.tar.gz
unpack sqlite-autoconf-$SQLITE_VERSION.tar.gz sqlite
cd "$WORK/sqlite"
run_logged sqlite-configure ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/SQLite \
    --disable-shared --enable-static --disable-readline --disable-editline --enable-threadsafe \
    CC="$CC_PORT" CFLAGS="-O2 -DSQLITE_OMIT_LOAD_EXTENSION"
run_logged sqlite-make make -j"$JOBS" sqlite3
rm -rf "$OUT/SQLite"
mkdir -p "$OUT/SQLite/BIN"
cp sqlite3.exe "$OUT/SQLite/BIN/SQLITE3.EXE" 2>/dev/null || cp sqlite3 "$OUT/SQLite/BIN/SQLITE3.EXE"
"$TARGET-strip" "$OUT/SQLite/BIN/SQLITE3.EXE"
echo "SQLite $SQLITE_VERSION -> $OUT/SQLite"
