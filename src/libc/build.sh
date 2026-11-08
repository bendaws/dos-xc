#!/bin/sh
# Build the DOS-XC C library.
#   src/libc/build.sh ARCH OUTPUT [CC]
# ARCH is x86_64 or i386.  OUTPUT receives include/ and lib/ (a sysroot):
# crt0.o, crti.o, crtn.o, libc.a, libm.a and the shared libc.so.
# CC defaults to clang; a DOS-XC cross gcc works as well.  libc.so is
# linked with ld.lld against the cross compiler's libgcc.a (LIBGCC=path to
# override) when one is found.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
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
    $CC $flags -c "$source" -o "$object"
    objects="$objects $object"
done
$CC $flags -c "$HERE/crt/setjmp-$ARCH.S" -o "$work/setjmp.o"
rm -f "$OUT/lib/libc.a" "$OUT/lib/libm.a"
"$AR" rcs "$OUT/lib/libc.a" $objects "$work/setjmp.o"
# The math functions live in libc.a; libm.a exists so -lm links.  It holds
# one tiny object because GNU ld rejects archives without a symbol index.
echo 'char __dosxc_libm;' | $CC $flags -x c -c - -o "$work/libm.o"
"$AR" rcs "$OUT/lib/libm.a" "$work/libm.o"
# The shared C library (\\SYSTEM\\LIB\\libc.so on DOS-XC).
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
