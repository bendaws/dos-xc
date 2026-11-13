#!/bin/sh
#
# DOS-XC /gcc/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")
ARCH=${1:-x86_64}
[ $# -gt 0 ] && shift
STAGES=${*:-fetch libc binutils gcc libc-gcc native package}
case "$ARCH" in
x86_64) TARGET=x86_64-dosxc LIBC_ARCH=x86_64 ;;
i686) TARGET=i686-dosxc LIBC_ARCH=i386 ;;
*) echo "error: ARCH must be x86_64 or i686" >&2; exit 1 ;;
esac

BINUTILS_VERSION=2.45
GCC_VERSION=15.2.0
GMP_VERSION=6.3.0
MPFR_VERSION=4.2.2
MPC_VERSION=1.3.1
WORK=$ROOT/toolchain/work
SRC=$WORK/src
CROSS=$ROOT/toolchain/cross
SYSROOT=$CROSS/$TARGET/sysroot
STAGING=$WORK/staging-$ARCH          # DESTDIR for the native toolchain
DIST=$ROOT/toolchain/dist/$ARCH
PREFIX=/Programs/GCC                 # C:\Programs\GCC (the GCC package)
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
MAKE=${MAKE:-make}
export PATH="$CROSS/bin:$PATH"

log() { printf '\n=== %s\n' "$*"; }
remove_tree() {
    case "${1:?remove_tree needs a path}" in
    "$WORK"/?* | "$ROOT"/toolchain/dist/?*) rm -rf -- "$1" ;;
    *) echo "error: refusing to delete $1" >&2; exit 1 ;;
    esac
}
fresh_directory() { remove_tree "$1"; mkdir -p "$1"; }
run_logged() {
    run_log=$WORK/$1-$ARCH.log; shift
    if ! "$@" > "$run_log" 2>&1; then
        echo "error: command failed; last lines of $run_log:" >&2
        tail -30 "$run_log" >&2
        exit 1
    fi
}

fetch() {
    mkdir -p "$ROOT/toolchain/downloads" "$SRC"
    for url in https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz \
               https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz \
               https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VERSION.tar.xz \
               https://ftp.gnu.org/gnu/mpfr/mpfr-$MPFR_VERSION.tar.xz \
               https://ftp.gnu.org/gnu/mpc/mpc-$MPC_VERSION.tar.gz; do
        file=$ROOT/toolchain/downloads/${url##*/}
        [ -f "$file" ] || curl -fL --retry 3 -o "$file" "$url"
        dir=$SRC/$(basename "$file" | sed 's/\.tar\..*//')
        [ -d "$dir" ] || tar -C "$SRC" -xf "$file"
    done
    # Front ends and libraries DOS-XC does not build (saves ~400 MB).
    # (gcc/d and gcc/rust keep their *-target.def hook lists, which genhooks reads.)
    find "$SRC/gcc-$GCC_VERSION/gcc/d" "$SRC/gcc-$GCC_VERSION/gcc/rust" -type f ! -name '*.def' ! -name '*.h' -delete 2>/dev/null || true
    find "$SRC/gcc-$GCC_VERSION/gcc/testsuite" -mindepth 1 -maxdepth 1 ! -name selftests -exec rm -rf {} + 2>/dev/null || true
    for d in gcc/ada gcc/go gcc/fortran gcc/m2 gcc/cobol libgo libphobos \
             libgfortran libgm2 libgcobol libada gotools libobjc gcc/objc gcc/objcp libsanitizer libitm \
             libquadmath libvtv libffi gnattools libgrust; do
        remove_tree "$SRC/gcc-$GCC_VERSION/$d"
    done
    find "$SRC/gcc-$GCC_VERSION/libstdc++-v3/testsuite" -mindepth 1 -maxdepth 1 ! -name 'Makefile.*' -exec rm -rf {} + 2>/dev/null || true
    remove_tree "$SRC/binutils-$BINUTILS_VERSION/gprofng"
    "$HERE/patch-sources.sh" "$SRC"
}

libc() {
    "$ROOT/os/libc/build.sh" "$LIBC_ARCH" "$SYSROOT" "${LIBC_CC:-clang}"
}

binutils() {
    b=$WORK/build/binutils-cross-$ARCH
    fresh_directory "$b"
    cd "$b"
    run_logged binutils-cross-configure "$SRC/binutils-$BINUTILS_VERSION/configure" \
        --target=$TARGET --prefix="$CROSS" --with-sysroot="$SYSROOT" --with-system-zlib \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng --disable-sim --disable-gold \
        --disable-libctf MAKEINFO=true
    run_logged binutils-cross-make $MAKE -j"$JOBS" MAKEINFO=true
    run_logged binutils-cross-install $MAKE install MAKEINFO=true
    cd "$HERE" && remove_tree "$b"
}

GCC_OPTIONS="--disable-nls --disable-shared --enable-static --disable-multilib --enable-threads=posix \
 --disable-tls --disable-libssp --disable-libgomp --disable-libatomic --disable-libquadmath --disable-libvtv \
 --disable-libsanitizer --disable-libitm --disable-libcc1 --disable-lto --disable-plugin --enable-default-pie \
 --enable-initfini-array --without-isl --disable-bootstrap --disable-werror --disable-gcov \
 --disable-libstdcxx-pch --disable-libstdcxx-backtrace --enable-languages=c,c++ --with-gnu-as --with-gnu-ld"

# The target description lives in gcc/config; copy it (and the
# other idempotent source edits) again so a rebuild sees the current one.
refresh_sources() { "$HERE/patch-sources.sh" "$SRC" >/dev/null; }

gcc() {
    refresh_sources
    for lib in gmp mpfr mpc; do
        version=$(eval echo \$$(echo $lib | tr a-z A-Z)_VERSION)
        [ -e "$SRC/gcc-$GCC_VERSION/$lib" ] || ln -s "../$lib-$version" "$SRC/gcc-$GCC_VERSION/$lib"
    done
    b=$WORK/build/gcc-cross-$ARCH
    fresh_directory "$b"
    cd "$b"
    run_logged gcc-cross-configure "$SRC/gcc-$GCC_VERSION/configure" --target=$TARGET --prefix="$CROSS" \
        --with-sysroot="$SYSROOT" --with-native-system-header-dir=/include --with-system-zlib \
        $GCC_OPTIONS CFLAGS_FOR_TARGET="-O2 -fPIC" CXXFLAGS_FOR_TARGET="-O2 -fPIC" MAKEINFO=true
    mkdir -p gcc/d gcc/rust     # genhooks writes their hook headers even when disabled
    run_logged gcc-cross-make $MAKE -j"$JOBS" all-gcc all-target-libgcc MAKEINFO=true
    run_logged gcc-cross-install $MAKE install-strip-gcc install-strip-target-libgcc MAKEINFO=true
    # fixincludes copies libc's stdio.h (to add <stdarg.h>), which would then
    # hide every later change to the real one.
    rm -f "$CROSS/lib/gcc/$TARGET/$GCC_VERSION/include-fixed/stdio.h"
    # libstdc++'s configure links test programs against libc.so.  On i686 that
    # needs libgcc's 64-bit division, which did not exist at the libc stage.
    ( libc )
    run_logged gcc-cross-make-libstdc++ $MAKE -j"$JOBS" all-target-libstdc++-v3 MAKEINFO=true
    run_logged gcc-cross-install-libstdc++ $MAKE install-strip-target-libstdc++-v3 MAKEINFO=true
    cd "$HERE" && remove_tree "$b"
}

libc_gcc() {
    LIBC_CC=$CROSS/bin/$TARGET-gcc libc
}

DEPS=$WORK/native-deps-$ARCH
HOST_TOOLS="CC=$TARGET-gcc CXX=$TARGET-g++ AR=$TARGET-ar RANLIB=$TARGET-ranlib NM=$TARGET-nm STRIP=$TARGET-strip"
BUILD_TOOLS="CC_FOR_BUILD=clang CXX_FOR_BUILD=clang++"

native_library() {
    name=$1; shift
    [ -f "$DEPS/lib/lib$name.a" ] && return 0     # already built
    b=$WORK/build/$name-native-$ARCH
    fresh_directory "$b"
    cd "$b"
    version=$(eval echo \$$(echo $name | tr a-z A-Z)_VERSION)
    run_logged $name-native-configure env $HOST_TOOLS CC="$TARGET-gcc -std=gnu17" \
        "$SRC/$name-$version/configure" --build="$BUILD_TRIPLE" \
        --host=$TARGET --prefix="$DEPS" --disable-shared --enable-static "$@"
    run_logged $name-native-make $MAKE -j"$JOBS"
    run_logged $name-native-install $MAKE install
    cd "$HERE" && remove_tree "$b"
}

native() {
    native_library gmp --disable-assembly
    native_library mpfr --with-gmp="$DEPS"
    native_library mpc --with-gmp="$DEPS" --with-mpfr="$DEPS"

    native_binutils
    native_gcc
}

native_binutils() {
    remove_tree "$STAGING"
    b=$WORK/build/binutils-native-$ARCH
    fresh_directory "$b"
    cd "$b"
    run_logged binutils-native-configure env $HOST_TOOLS $BUILD_TOOLS "$SRC/binutils-$BINUTILS_VERSION/configure" \
        --build="$BUILD_TRIPLE" --host=$TARGET --target=$TARGET --prefix=$PREFIX --disable-nls --disable-werror \
        --disable-gdb --disable-gprofng --disable-sim --disable-gold --disable-libctf --disable-plugins \
        --disable-shared --enable-static --without-zstd --without-debuginfod MAKEINFO=true
    run_logged binutils-native-make $MAKE -j"$JOBS" MAKEINFO=true
    run_logged binutils-native-install $MAKE install-strip DESTDIR="$STAGING" MAKEINFO=true
    cd "$HERE" && remove_tree "$b"
}

native_gcc() {
    refresh_sources
    for lib in gmp mpfr mpc; do
        [ -L "$SRC/gcc-$GCC_VERSION/$lib" ] && rm "$SRC/gcc-$GCC_VERSION/$lib"
    done
    build_sysroot=$WORK/build-sysroot-$ARCH
    fresh_directory "$build_sysroot$PREFIX"
    ln -s "$SYSROOT/include" "$build_sysroot$PREFIX/include"
    b=$WORK/build/gcc-native-$ARCH
    fresh_directory "$b"
    cd "$b"
    run_logged gcc-native-configure env $HOST_TOOLS $BUILD_TOOLS "$SRC/gcc-$GCC_VERSION/configure" \
        --build="$BUILD_TRIPLE" --host=$TARGET --target=$TARGET --prefix=$PREFIX \
        --with-gmp="$DEPS" --with-mpfr="$DEPS" --with-mpc="$DEPS" \
        --with-native-system-header-dir=$PREFIX/include --with-local-prefix=$PREFIX \
        --with-build-sysroot="$build_sysroot" --enable-host-pie $GCC_OPTIONS CFLAGS_FOR_TARGET="-O2 -fPIC" CXXFLAGS_FOR_TARGET="-O2 -fPIC" MAKEINFO=true
    mkdir -p gcc/d gcc/rust
    run_logged gcc-native-make $MAKE -j"$JOBS" all-gcc MAKEINFO=true
    run_logged gcc-native-install $MAKE install-strip-gcc DESTDIR="$STAGING" MAKEINFO=true
    rm -f "$STAGING$PREFIX/lib/gcc/$TARGET/$GCC_VERSION/include-fixed/stdio.h"
    cd "$HERE" && remove_tree "$b"
}

package() {
    remove_tree "$DIST"
    mkdir -p "$DIST"
    cp -R "$STAGING$PREFIX" "$DIST/GCC"
    g=$DIST/GCC
    lib=$g/lib/gcc/$TARGET/$GCC_VERSION
    cp "$CROSS/lib/gcc/$TARGET/$GCC_VERSION"/*.a "$CROSS/lib/gcc/$TARGET/$GCC_VERSION"/*.o "$lib/"
    mkdir -p "$g/include/c++"
    cp -R "$CROSS/$TARGET/include/c++/$GCC_VERSION" "$g/include/c++/$GCC_VERSION"
    cp "$CROSS/$TARGET/lib/libstdc++.a" "$CROSS/$TARGET/lib/libsupc++.a" "$g/lib/"
    cp -R "$SYSROOT/include/." "$g/include/"
    cp "$SYSROOT/lib/"* "$g/lib/"
    for d in share lib/bfd-plugins "libexec/gcc/$TARGET/$GCC_VERSION/install-tools" \
             "lib/gcc/$TARGET/$GCC_VERSION/install-tools" "lib/gcc/$TARGET/$GCC_VERSION/plugin"; do
        remove_tree "$g/$d"
    done
    find "$g" -name '*.la' -type f -delete
    for f in lib/libbfd.a lib/libopcodes.a lib/libsframe.a include/ansidecl.h include/bfd.h include/bfdlink.h \
             include/diagnostics.h include/dis-asm.h include/plugin-api.h include/sframe.h include/sframe-api.h \
             include/symcat.h bin/gprof.exe bin/elfedit.exe; do
        rm -f "$g/$f"
    done
    find "$g/bin" -name "$TARGET-*" -type f -delete
    for f in bin/ld.bfd bin/gcc-ar bin/gcc-nm bin/gcc-ranlib $TARGET/bin/ld.bfd $TARGET/bin/ar $TARGET/bin/nm \
             $TARGET/bin/objcopy $TARGET/bin/objdump $TARGET/bin/ranlib $TARGET/bin/readelf $TARGET/bin/strip; do
        rm -f "$g/$f.exe"
    done
    find "$g" -type f -name '*.exe' | while read -r f; do mv "$f" "${f%.exe}.EXE"; done
    python3 - "$g" <<'PY'
import collections, os, sys
seen = collections.defaultdict(list)
for root, dirs, files in os.walk(sys.argv[1]):
    for name in dirs + files:
        seen[(root, name.upper())].append(name)
clashes = [v for v in seen.values() if len(v) > 1]
if clashes:
    sys.exit(f'error: names that differ only in case: {clashes[:5]}')
PY
    du -sh "$DIST"
}

mkdir -p "$WORK"
for stage in $STAGES; do
    log "$ARCH: $stage"
    BUILD_TRIPLE=$(sh "$SRC/gcc-$GCC_VERSION/config.guess" 2>/dev/null || echo unknown)
    case "$stage" in
    fetch) fetch ;;
    libc) libc ;;
    binutils) binutils ;;
    gcc) gcc ;;
    libc-gcc) libc_gcc ;;
    native) native ;;
    native-binutils) native_binutils ;;
    native-gcc) native_gcc ;;
    package) package ;;
    *) echo "error: unknown stage $stage" >&2; exit 1 ;;
    esac
done
