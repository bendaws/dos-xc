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
    find "$WORK/$2" \( -name config.sub -o -name autosetup-config.sub \) | while read -r sub; do
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

# ncursesw, static, with the terminals DOS-XC programs meet compiled in (the
# console is "ansi"; there is no terminfo database to read).
build_ncurses() {
    stamp=$SYSROOT/lib/.ncurses-6.5
    [ -f "$stamp" ] && return 0
    fetch ncurses-6.5.tar.gz https://ftpmirror.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz
    host=$WORK/ncurses-host
    if [ ! -x "$host/bin/tic" ]; then           # tic and infocmp for the build machine compile the terminals in
        unpack ncurses-6.5.tar.gz ncurses-host-src
        cd "$WORK/ncurses-host-src"
        run_logged ncurses-host-configure ./configure --prefix="$host" --without-shared --without-debug --without-ada \
            --without-cxx-binding --without-tests --without-manpages --enable-widec CC=cc
        run_logged ncurses-host-make make -j"$JOBS"
        run_logged ncurses-host-install make install.progs
    fi
    unpack ncurses-6.5.tar.gz ncurses
    cd "$WORK/ncurses"
    run_logged ncurses-configure env PATH="$host/bin:$PATH" ./configure --with-tic-path="$host/bin/tic" \
        --with-infocmp-path="$host/bin/infocmp" --build="$BUILD_TRIPLE" --host=$TARGET --prefix="$SYSROOT" \
        --with-normal --without-shared --without-debug --without-ada --without-cxx-binding --without-progs \
        --without-tests --without-manpages --disable-database --disable-home-terminfo --enable-widec --disable-ext-colors \
        --with-fallbacks=ansi,vt100,vt220,xterm,xterm-256color,linux --disable-stripping \
        --with-build-cc=cc --enable-pc-files=no CC="$CC_PORT" CFLAGS="-O2"
    run_logged ncurses-make env PATH="$host/bin:$PATH" make -j"$JOBS" libs
    run_logged ncurses-install make install.libs install.includes
    touch "$stamp"
}

build_zlib() {
    stamp=$SYSROOT/lib/.zlib-$ZLIB_VERSION
    [ -f "$stamp" ] && return 0
    fetch zlib-$ZLIB_VERSION.tar.gz https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz
    unpack zlib-$ZLIB_VERSION.tar.gz zlib
    cd "$WORK/zlib"
    run_logged zlib-configure env CC="$CC_PORT" CHOST=$TARGET CFLAGS="-O2" ./configure --static --prefix="$SYSROOT"
    run_logged zlib-make make -j"$JOBS" libz.a
    run_logged zlib-install make install
    touch "$stamp"
}

# libcurl with OpenSSL, static, for programs that fetch over HTTPS (git).
build_libcurl() {
    stamp=$SYSROOT/lib/.libcurl-8.11.1
    [ -f "$stamp" ] && return 0
    build_openssl
    build_zlib
    fetch curl-8.11.1.tar.xz https://curl.se/download/curl-8.11.1.tar.xz
    unpack curl-8.11.1.tar.xz libcurl
    cd "$WORK/libcurl"
    run_logged libcurl-configure env PKG_CONFIG=false ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix="$SYSROOT" \
        --disable-shared --enable-static --with-openssl="$SYSROOT" --with-zlib="$SYSROOT" --without-ca-bundle --without-ca-path \
        --disable-threaded-resolver --without-libpsl --without-brotli --without-zstd --without-libidn2 --without-nghttp2 \
        --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
        --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-manual --disable-docs \
        --disable-socketpair ac_cv_func_socketpair=no CC="$CC_PORT" CFLAGS="-O2"
    run_logged libcurl-make make -j"$JOBS" -C lib
    run_logged libcurl-install sh -c 'make -C lib install && make -C include install && cp curl-config "$1/bin/" 2>/dev/null; mkdir -p "$1/bin"; cp curl-config "$1/bin/"' sh "$SYSROOT"
    touch "$stamp"
}

fetch_ca_bundle() {
    fetch cacert.pem https://curl.se/ca/cacert.pem
}
