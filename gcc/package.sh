#!/bin/sh
#
# DOS-XC /gcc/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    dist=$ROOT/toolchain/dist/$arch/GCC
    target=$PACKAGES/GCC-$bits.dsp
    if [ ! -d "$dist" ] || { [ -f "$target" ] && [ -z "$(find "$dist" "$sysroot/lib/libc.a" -newer "$target" -print -quit)" ]; }; then
        on_media "GCC-$bits.dsp"
        continue
    fi
    echo "Packing GCC for the $bits-bit edition"
    rm -rf "$PART/gcc$bits"
    cp -R "$dist" "$PART/gcc$bits"
    cp -R "$sysroot/include/." "$PART/gcc$bits/include/"
    cp "$sysroot/lib/"*.o "$sysroot/lib/"*.a "$sysroot/lib/"*.so "$PART/gcc$bits/lib/"
    pack "$PART/gcc$bits" "GCC-$bits.dsp" Name=GCC Version=15.2.0 \
        Author="Free Software Foundation" Description="GNU Compiler Collection" Arch="$bits" Path=BIN
    rm -rf "$PART/gcc$bits"
done
