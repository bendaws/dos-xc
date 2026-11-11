#
# DOS-XC /scripts/ports-common.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")
ARCH=${1:-x86_64}
case "$ARCH" in
x86_64) TARGET=x86_64-dosxc ;;
i686) TARGET=i686-dosxc ;;
*) echo "error: ARCH must be x86_64 or i686" >&2; exit 1 ;;
esac
ZLIB_VERSION=1.3.1
OPENSSL_VERSION=3.5.9
DOWNLOADS=$ROOT/toolchain/downloads
WORK=$ROOT/toolchain/work/ports-$ARCH
OUT=${BUILD:-"$ROOT/build"}/ports/$ARCH
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
export PATH="$ROOT/toolchain/cross/bin:$PATH"
command -v "$TARGET-gcc" >/dev/null || { echo "error: $TARGET-gcc not found; run gcc/build.sh $ARCH" >&2; exit 1; }
# Ports link against the C library in this tree (built here, so the ports
# always see its current headers).
SYSROOT=$WORK/sysroot
mkdir -p "$WORK" "$OUT"
"$ROOT/os/libc/build.sh" "$( [ "$ARCH" = i686 ] && echo i386 || echo x86_64 )" "$SYSROOT" clang >/dev/null
CC_PORT="$TARGET-gcc --sysroot=$SYSROOT"
BUILD_TRIPLE=$(sh "$ROOT/toolchain/work/src/gcc-15.2.0/config.guess" 2>/dev/null || echo "$(uname -m)-apple-darwin")

fetch() {
    mkdir -p "$DOWNLOADS"
    [ -f "$DOWNLOADS/$1" ] || curl -fL --retry 3 -o "$DOWNLOADS/$1" "$2"
}

unpack() {
    rm -rf "$WORK/$2"
    mkdir -p "$WORK/$2"
    tar -C "$WORK/$2" --strip-components=1 -xf "$DOWNLOADS/$1"
    find "$WORK/$2" -name config.sub | while read -r sub; do
        grep -q 'dosxc' "$sub" || sed -i.orig 's/| rtems\* /| rtems* | dosxc* /' "$sub"
    done
}

run_logged() {
    log=$WORK/$1.log
    shift
    if ! "$@" > "$log" 2>&1; then
        echo "error: command failed; last lines of $log:" >&2
        tail -30 "$log" >&2
        exit 1
    fi
}

upper_exe() {
    for f in "$1"/*.exe; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .exe | tr a-z A-Z)
        mv "$f" "$1/$base.tmp" && mv "$1/$base.tmp" "$1/$base.EXE"
    done
}

build_openssl() {
    stamp=$SYSROOT/lib/.openssl-$OPENSSL_VERSION-threads
    [ -f "$stamp" ] && return 0
    fetch openssl-$OPENSSL_VERSION.tar.gz https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz
    unpack openssl-$OPENSSL_VERSION.tar.gz openssl
    cd "$WORK/openssl"
    case "$ARCH" in x86_64) openssl_target=linux-x86_64 ;; *) openssl_target=linux-elf ;; esac
    run_logged openssl-configure env CC="$CC_PORT" AR="$TARGET-ar" RANLIB="$TARGET-ranlib" \
        perl ./Configure "$openssl_target" --prefix="$SYSROOT" --libdir=lib --openssldir=/SYSTEM/ssl \
        --with-rand-seed=getrandom no-shared no-dso no-async no-engine no-module no-legacy no-tests \
        no-apps no-docs no-afalgeng no-ui-console no-secure-memory no-quic -DNO_SYS_UN_H -DNO_SYSLOG
    run_logged openssl-make make -j"$JOBS" build_libs
    run_logged openssl-install make install_dev
    touch "$stamp"
}

fetch_ca_bundle() {
    fetch cacert.pem https://curl.se/ca/cacert.pem
}
