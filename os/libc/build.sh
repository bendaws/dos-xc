#!/bin/sh
#
# DOS-XC /os/libc/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/../../scripts/parallel.sh"
ARCH=$1
OUT=$2
CC=${3:-clang}
AR=${AR:-llvm-ar}
command -v "$AR" >/dev/null 2>&1 || AR=/opt/homebrew/opt/llvm/bin/llvm-ar
command -v "$AR" >/dev/null 2>&1 || AR=ar

case "$ARCH" in
x86_64) target=x86_64-unknown-elf; machine="-mno-red-zone" ;;
i386) target=i386-unknown-elf; machine="-march=i686" ;;
*) echo "error: unknown architecture $ARCH" >&2; exit 1 ;;
esac
case "$CC" in
*clang*) flags="--target=$target -nostdinc -isystem $HERE/include -isystem $("$CC" -print-resource-dir)/include" ;;
*) flags="-nostdinc -isystem $HERE/include -isystem $("$CC" -print-file-name=include)" ;;
esac
flags="$flags $machine -std=gnu11 -O2 -fPIC -ffreestanding -fno-stack-protector -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"

work="$OUT/.libc-$ARCH"
rm -rf "$work"
mkdir -p "$work" "$OUT/lib" "$OUT/include"
objects=
for source in "$HERE"/src/*.c; do
    object="$work/$(basename "$source" .c).o"
    spawn $CC $flags -c "$source" -o "$object"
    objects="$objects $object"
done
finish_jobs
$CC $flags -c "$HERE/crt/setjmp-$ARCH.S" -o "$work/setjmp.o"
rm -f "$OUT/lib/libc.a" "$OUT/lib/libm.a"
"$AR" rcs "$OUT/lib/libc.a" $objects "$work/setjmp.o"
# The math functions live in libc.a; libm.a exists so -lm links.  It holds
# one tiny object because GNU ld rejects archives without a symbol index.
echo 'char __dosxc_libm;' | $CC $flags -x c -c - -o "$work/libm.o"
"$AR" rcs "$OUT/lib/libm.a" "$work/libm.o"
echo 'char __dosxc_libpthread;' | $CC $flags -x c -c - -o "$work/libpthread.o"
"$AR" rcs "$OUT/lib/libpthread.a" "$work/libpthread.o"
echo 'char __dosxc_libdl;' | $CC $flags -x c -c - -o "$work/libdl.o"
"$AR" rcs "$OUT/lib/libdl.a" "$work/libdl.o"
for stub in rt resolv; do
    echo "char __dosxc_lib$stub;" | $CC $flags -x c -c - -o "$work/lib$stub.o"
    "$AR" rcs "$OUT/lib/lib$stub.a" "$work/lib$stub.o"
done
LLD=${LLD:-ld.lld}
case "$ARCH" in x86_64) emulation=elf_x86_64 triple=x86_64-dosxc ;; *) emulation=elf_i386 triple=i686-dosxc ;; esac
if [ -z "${LIBGCC:-}" ] && [ -x "$HERE/../../toolchain/cross/bin/$triple-gcc" ]; then
    LIBGCC=$("$HERE/../../toolchain/cross/bin/$triple-gcc" -print-libgcc-file-name)
fi
if command -v "$LLD" >/dev/null 2>&1; then
    "$LLD" -m "$emulation" -shared -soname libc.so --hash-style=both -z text -z max-page-size=4096 -z noexecstack \
        -o "$OUT/lib/libc.so" $objects "$work/setjmp.o" ${LIBGCC:+"$LIBGCC"}
fi
for crt in crt0 crti crtn; do
    $CC $flags -I"$HERE/crt" -c "$HERE/crt/$crt-$ARCH.S" -o "$OUT/lib/$crt.o"
done
cp -R "$HERE"/include/. "$OUT/include/"
rm -rf "$work"
echo "Built DOS-XC C library for $ARCH in $OUT"
