#!/bin/sh
#
# DOS-XC /curl/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
CURL_VERSION=8.11.1

build_openssl
fetch_ca_bundle
fetch curl-$CURL_VERSION.tar.xz https://curl.se/download/curl-$CURL_VERSION.tar.xz
unpack curl-$CURL_VERSION.tar.xz curl
cd "$WORK/curl"
run_logged curl-configure env PKG_CONFIG=false ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/curl \
    --disable-shared --enable-static --with-openssl="$SYSROOT" --without-ca-bundle --without-ca-path \
    --disable-ipv6 --disable-threaded-resolver \
    --without-zlib --without-libpsl --without-brotli --without-zstd --without-libidn2 --without-nghttp2 \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
    --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-file \
    --disable-manual --disable-docs --disable-unix-sockets --disable-socketpair --disable-netrc \
    ac_cv_func_socketpair=no CC="$CC_PORT" CFLAGS="-O2"
run_logged curl-make make -j"$JOBS"
rm -rf "$OUT/curl"
mkdir -p "$OUT/curl/BIN"
cp "$WORK/curl/src/curl.exe" "$OUT/curl/BIN/CURL.EXE" 2>/dev/null || cp "$WORK/curl/src/curl" "$OUT/curl/BIN/CURL.EXE"
"$TARGET-strip" "$OUT/curl/BIN/CURL.EXE"
cp "$WORK/curl/COPYING" "$OUT/curl/COPYING.TXT"
cp "$DOWNLOADS/cacert.pem" "$OUT/curl/CACERT.PEM"
echo "curl $CURL_VERSION -> $OUT/curl"
