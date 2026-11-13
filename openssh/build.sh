#!/bin/sh
#
# DOS-XC /openssh/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
OPENSSH_VERSION=10.0p1

build_openssl
fetch openssh-$OPENSSH_VERSION.tar.gz https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$OPENSSH_VERSION.tar.gz
unpack openssh-$OPENSSH_VERSION.tar.gz openssh
cd "$WORK/openssh"
patch -p1 < "$HERE/dosxc.patch" > /dev/null
run_logged openssh-configure env ac_cv_func_getpgid=no \
    ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/OpenSSH --sysconfdir=/Programs/OpenSSH/ETC \
    --with-ssl-dir="$SYSROOT" --without-zlib --without-pam --without-selinux --without-kerberos5 --without-libedit \
    --without-security-key-builtin --disable-security-key --disable-lastlog --disable-utmp --disable-utmpx \
    --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline --without-shadow --without-stackprotect \
    --without-hardening --disable-strip --with-sandbox=no --with-privsep-path=/TEMP \
    CC="$CC_PORT" CFLAGS="-O2"
run_logged openssh-libs make -j"$JOBS" SSH_PROGRAM=ssh libssh.a openbsd-compat/libopenbsd-compat.a
run_logged openssh-make make -j"$JOBS" SSH_PROGRAM=ssh ssh.exe scp.exe sftp.exe ssh-keygen.exe ssh-add.exe ssh-agent.exe ssh-keyscan.exe
rm -rf "$OUT/OpenSSH"
mkdir -p "$OUT/OpenSSH/BIN" "$OUT/OpenSSH/ETC"
for program in ssh scp sftp ssh-keygen ssh-add ssh-agent ssh-keyscan; do
    target=$(echo "$program" | tr a-z A-Z)
    cp "$program.exe" "$OUT/OpenSSH/BIN/$target.EXE"
    "$TARGET-strip" "$OUT/OpenSSH/BIN/$target.EXE"
done
cp ssh_config "$OUT/OpenSSH/ETC/SSH_CONFIG"
cp LICENCE "$OUT/OpenSSH/LICENCE.TXT"
echo "OpenSSH $OPENSSH_VERSION -> $OUT/OpenSSH"
