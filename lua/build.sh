#!/bin/sh
#
# DOS-XC /lua/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
LUA_VERSION=5.4.7

fetch lua-$LUA_VERSION.tar.gz https://www.lua.org/ftp/lua-$LUA_VERSION.tar.gz
unpack lua-$LUA_VERSION.tar.gz lua
cd "$WORK/lua/src"
run_logged lua-make make -j"$JOBS" posix CC="$CC_PORT" AR="$TARGET-ar rcu" RANLIB="$TARGET-ranlib" MYCFLAGS="-O2"
rm -rf "$OUT/Lua"
mkdir -p "$OUT/Lua/BIN"
for program in lua luac; do
    [ -f "$program.exe" ] && cp "$program.exe" "$OUT/Lua/BIN/$(echo $program | tr a-z A-Z).EXE" || cp "$program" "$OUT/Lua/BIN/$(echo $program | tr a-z A-Z).EXE"
    "$TARGET-strip" "$OUT/Lua/BIN/$(echo $program | tr a-z A-Z).EXE"
done
sed -n '/^\* Copyright (C) 1994/,/SOFTWARE\./p' lua.h > "$OUT/Lua/LICENSE.TXT"
echo "Lua $LUA_VERSION -> $OUT/Lua"
