#!/bin/sh
# Build third-party programs for DOS-XC with the cross compiler.
#
#   ports/build-ports.sh ARCH [PORT...]
#
# ARCH is x86_64 or i686; PORTs are xz, vim and curl (all when none are named).
# Sources are downloaded to toolchain/downloads.  Each port is installed
# into build/ports/ARCH/<Name>, the folder layout of its package
# (\Programs\<Name> on DOS-XC); scripts/build.sh packs those folders.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")
ARCH=${1:-x86_64}
[ $# -gt 0 ] && shift
PORTS=${*:-xz vim curl}
case "$ARCH" in
x86_64) TARGET=x86_64-dosxc ;;
i686) TARGET=i686-dosxc ;;
*) echo "error: ARCH must be x86_64 or i686" >&2; exit 1 ;;
esac
XZ_VERSION=5.8.1
VIM_VERSION=9.1.1000
CURL_VERSION=8.11.1
DOWNLOADS=$ROOT/toolchain/downloads
WORK=$ROOT/toolchain/work/ports-$ARCH
OUT=${BUILD:-"$ROOT/build"}/ports/$ARCH
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
export PATH="$ROOT/toolchain/cross/bin:$PATH"
command -v "$TARGET-gcc" >/dev/null || { echo "error: $TARGET-gcc not found; run toolchain/build-toolchain.sh $ARCH" >&2; exit 1; }
# Ports link against the C library in this tree (built here, so the ports
# always see its current headers).
SYSROOT=$WORK/sysroot
mkdir -p "$WORK"
"$ROOT/src/libc/build.sh" "$( [ "$ARCH" = i686 ] && echo i386 || echo x86_64 )" "$SYSROOT" clang >/dev/null
CC_PORT="$TARGET-gcc --sysroot=$SYSROOT"
BUILD_TRIPLE=$(sh "$ROOT/toolchain/work/src/gcc-15.2.0/config.guess" 2>/dev/null || echo "$(uname -m)-apple-darwin")

fetch() {
    mkdir -p "$DOWNLOADS"
    [ -f "$DOWNLOADS/$1" ] || curl -fL --retry 3 -o "$DOWNLOADS/$1" "$2"
}

# Unpack $1 into a fresh $WORK/$2 and teach its config.sub about DOS-XC.
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

# Programs are named *.EXE on DOS-XC.
upper_exe() {
    for f in "$1"/*.exe; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .exe | tr a-z A-Z)
        mv "$f" "$1/$base.tmp" && mv "$1/$base.tmp" "$1/$base.EXE"
    done
}

port_xz() {
    fetch xz-$XZ_VERSION.tar.gz https://github.com/tukaani-project/xz/releases/download/v$XZ_VERSION/xz-$XZ_VERSION.tar.gz
    unpack xz-$XZ_VERSION.tar.gz xz
    cd "$WORK/xz"
    # argv[0] is "C:/PATH/XZCAT.EXE": strip the path and suffix and lower
    # the case as on other DOS-like systems, so XZCAT and UNXZ work.
    sed -i.orig 's/^#ifdef TUKLIB_DOSLIKE$/#if defined(TUKLIB_DOSLIKE) || defined(__dosxc__)/' src/common/tuklib_progname.c
    run_logged xz-configure ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/XZ \
        --disable-shared --enable-static --disable-threads --disable-nls --disable-doc --disable-scripts \
        --disable-lzmadec --disable-lzmainfo --disable-lzma-links --disable-xzdec \
        --disable-sandbox CC="$CC_PORT" CFLAGS="-O2"
    run_logged xz-make make -j"$JOBS"
    rm -rf "$WORK/xz-staging"
    run_logged xz-install make install-strip DESTDIR="$WORK/xz-staging"
    rm -rf "$OUT/xz"
    mkdir -p "$OUT/xz/BIN"
    cp "$WORK/xz-staging/Programs/XZ/bin/xz.exe" "$OUT/xz/BIN/XZ.EXE"
    # xz picks its mode from its own name.
    cp "$OUT/xz/BIN/XZ.EXE" "$OUT/xz/BIN/UNXZ.EXE"
    cp "$OUT/xz/BIN/XZ.EXE" "$OUT/xz/BIN/XZCAT.EXE"
    cp "$WORK/xz/COPYING" "$OUT/xz/COPYING.TXT"
    echo "xz $XZ_VERSION -> $OUT/xz"
}

port_vim() {
    fetch vim-$VIM_VERSION.tar.gz https://github.com/vim/vim/archive/refs/tags/v$VIM_VERSION.tar.gz
    unpack vim-$VIM_VERSION.tar.gz vim
    cd "$WORK/vim/src"
    # cscope talks to a forked child process, which DOS-XC cannot provide.
    sed -i.orig 's/#if defined(UNIX) \&\& defined(FEAT_HUGE) \&\& !defined(FEAT_CSCOPE)/#if defined(UNIX) \&\& defined(FEAT_HUGE) \&\& !defined(FEAT_CSCOPE) \&\& !defined(__dosxc__)/' feature.h
    # Answers for the configure tests that cannot run on the build machine.
    # The console has no terminfo database: Vim uses its built-in "ansi"
    # entry, and runs shell commands with system() (there is no fork()).
    run_logged vim-configure env \
        vim_cv_uname_output=DOS-XC vim_cv_uname_r_output=0.2 vim_cv_uname_m_output=x86 \
        vim_cv_toupper_broken=no vim_cv_terminfo=no vim_cv_tgetent=zero vim_cv_getcwd_broken=no \
        vim_cv_stat_ignores_slash=no vim_cv_memmove_handles_overlap=yes vim_cv_bcopy_handles_overlap=yes \
        vim_cv_memcpy_handles_overlap=no vim_cv_timer_create=no vim_cv_timer_create_with_lrt=no \
        vim_cv_tty_group=world vim_cv_tty_mode=0620 ac_cv_func_fork=no ac_cv_func_vfork=no \
        ac_cv_sizeof_int=4 ac_cv_func_sigaltstack=no ac_cv_func_setpgid=no ac_cv_func_setsid=no \
        ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/Vim \
        --with-features=huge --enable-gui=no --without-x --disable-gpm --disable-sysmouse --disable-nls \
        --disable-netbeans --disable-channel --disable-terminal --disable-xsmp --disable-xsmp-interact \
        --disable-libsodium --disable-acl --disable-selinux --disable-canberra --disable-smack \
        --disable-darwin --enable-multibyte --with-tlib=c --with-compiledby="DOS-XC" \
        CC="$CC_PORT" CFLAGS="-O2 -DUSE_SYSTEM -include sys/wait.h" STRIP="$TARGET-strip"
    run_logged vim-make make -j"$JOBS"
    rm -rf "$WORK/vim-staging"
    run_logged vim-install make install DESTDIR="$WORK/vim-staging" STRIP="$TARGET-strip"
    rm -rf "$OUT/Vim"
    mkdir -p "$OUT/Vim/BIN"
    cp "$WORK/vim-staging/Programs/Vim/bin/vim.exe" "$OUT/Vim/BIN/VIM.EXE"
    "$TARGET-strip" "$OUT/Vim/BIN/VIM.EXE"
    # The other names Vim answers to, as batch files (they need the batch package).
    printf '@VIM %%*\r\n' > "$OUT/Vim/BIN/VI.BAT"
    printf '@VIM -R %%*\r\n' > "$OUT/Vim/BIN/VIEW.BAT"
    printf '@VIM -e %%*\r\n' > "$OUT/Vim/BIN/EX.BAT"
    printf '@VIM -d %%*\r\n' > "$OUT/Vim/BIN/VIMDIFF.BAT"
    mkdir -p "$OUT/Vim/share"
    cp -R "$WORK/vim-staging/Programs/Vim/share/vim" "$OUT/Vim/share/vim"
    # Trim what a text console cannot use (translations, GUI and tutor files).
    rm -rf "$OUT/Vim/share/vim/vim91/lang" "$OUT/Vim/share/vim/vim91/tutor" "$OUT/Vim/share/vim/vim91/print" \
           "$OUT/Vim/share/vim/vim91/macros" "$OUT/Vim/share/vim/vim91/tools" "$OUT/Vim/share/vim/vim91/doc/"*.??x
    cp "$WORK/vim/LICENSE" "$OUT/Vim/LICENSE.TXT"
    echo "vim $VIM_VERSION -> $OUT/Vim"
}

port_curl() {
    fetch curl-$CURL_VERSION.tar.xz https://curl.se/download/curl-$CURL_VERSION.tar.xz
    unpack curl-$CURL_VERSION.tar.xz curl
    cd "$WORK/curl"
    # HTTP only: no TLS library has been ported, and IPv6 is not implemented.
    run_logged curl-configure ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/curl \
        --disable-shared --enable-static --without-ssl --disable-ipv6 --disable-threaded-resolver \
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
    echo "curl $CURL_VERSION -> $OUT/curl"
}

mkdir -p "$WORK" "$OUT"
for port in $PORTS; do
    case "$port" in
    xz) port_xz ;;
    vim) port_vim ;;
    curl) port_curl ;;
    *) echo "error: unknown port $port" >&2; exit 1 ;;
    esac
done
