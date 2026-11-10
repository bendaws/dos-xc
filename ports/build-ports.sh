#!/bin/sh
# Build third-party programs for DOS-XC with the cross compiler.
#
#   ports/build-ports.sh ARCH [PORT...]
#
# ARCH is x86_64 or i686; PORTs are xz, vim, curl and python (all when none are named).
# Sources are downloaded to toolchain/downloads.  Each port is installed
# into build/ports/ARCH/<Name>, the folder layout of its package
# (\Programs\<Name> on DOS-XC); scripts/build.sh packs those folders.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")
ARCH=${1:-x86_64}
[ $# -gt 0 ] && shift
PORTS=${*:-xz vim curl python}
case "$ARCH" in
x86_64) TARGET=x86_64-dosxc ;;
i686) TARGET=i686-dosxc ;;
*) echo "error: ARCH must be x86_64 or i686" >&2; exit 1 ;;
esac
XZ_VERSION=5.8.1
VIM_VERSION=9.1.1000
CURL_VERSION=8.11.1
PYTHON_VERSION=3.14.6
ZLIB_VERSION=1.3.1
OPENSSL_VERSION=3.5.9
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

# OpenSSL (TLS) goes into the sysroot once, for the ports that link it.
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

# The Mozilla root certificates, as distributed by the curl project.
fetch_ca_bundle() {
    fetch cacert.pem https://curl.se/ca/cacert.pem
}

port_curl() {
    build_openssl
    fetch_ca_bundle
    fetch curl-$CURL_VERSION.tar.xz https://curl.se/download/curl-$CURL_VERSION.tar.xz
    unpack curl-$CURL_VERSION.tar.xz curl
    cd "$WORK/curl"
    # IPv6 is not implemented.  The certificate bundle is found through CURL_CA_BUNDLE.
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
}

port_python() {
    build_openssl
    fetch_ca_bundle
    minor=${PYTHON_VERSION%.*}
    # A cross build runs a host Python of the same minor version.
    build_python=$(command -v "python$minor") || { echo "error: python$minor is needed on the build machine" >&2; exit 1; }
    fetch zlib-$ZLIB_VERSION.tar.gz https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz
    fetch Python-$PYTHON_VERSION.tar.xz https://www.python.org/ftp/python/$PYTHON_VERSION/Python-$PYTHON_VERSION.tar.xz

    # zlib, linked statically into the interpreter, goes into the sysroot.
    unpack zlib-$ZLIB_VERSION.tar.gz zlib
    cd "$WORK/zlib"
    run_logged zlib-configure env CC="$CC_PORT" CHOST=$TARGET CFLAGS="-O2" ./configure --static --prefix="$SYSROOT"
    run_logged zlib-make make -j"$JOBS" libz.a
    cp libz.a "$SYSROOT/lib/"
    cp zlib.h zconf.h "$SYSROOT/include/"

    unpack Python-$PYTHON_VERSION.tar.xz python
    cd "$WORK/python"
    # Teach configure the platform.
    perl -0pi -e 's/(\t\*-\*-wasi\*\)\n\s+ac_sys_system=WASI\n\s+;;\n)/$1\t*-*-dosxc*)\n\t    ac_sys_system=DOSXC\n\t    ;;\n/;
                  s/(\t\*-\*-vxworks\*\)\n\t\t_host_ident=\$host_cpu\n\t\t;;\n)/$1\t*-*-dosxc*)\n\t\t_host_ident=\$host_cpu\n\t\t;;\n/' configure
    # Search paths are separated by ";" because paths contain "C:".
    perl -0pi -e 's/#ifndef DELIM\n/#ifdef __dosxc__\n#  define DELIM L\x27;\x27\n#endif\n#ifndef DELIM\n/' Include/osdefs.h
    "$build_python" "$HERE/python-dosxc.py" .
    site=$WORK/python.site
    cat > "$site" <<'EOF'
ac_cv_file__dev_ptmx=no
ac_cv_file__dev_ptc=no
ac_cv_buggy_getaddrinfo=no
ax_cv_c_float_words_bigendian=no
ac_cv_enable_visibility=no
ac_cv_pthread_is_default=yes
ac_cv_pthread=no
ac_cv_kthread=no
ac_cv_kpthread=no
ac_cv_cxx_thread=no
ac_cv_posix_semaphores_enabled=yes
ac_cv_broken_sem_getvalue=no
py_cv_module__ctypes=n/a
py_cv_module__ctypes_test=n/a
py_cv_module__remote_debugging=n/a
EOF
    # The 32-bit edition computes doubles on the x87.
    [ "$ARCH" = i686 ] && printf 'ac_cv_gcc_asm_for_x87=yes\nac_cv_x87_double_rounding=yes\n' >> "$site"
    # No dlopen: every extension module is linked into the interpreter.
    run_logged python-configure env CONFIG_SITE="$site" PKG_CONFIG=false MODULE_BUILDTYPE=static \
        ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/python3 \
        --with-build-python="$build_python" --with-suffix=.exe --disable-shared --disable-ipv6 \
        --without-ensurepip --without-mimalloc --disable-test-modules \
        --with-openssl="$SYSROOT" --with-openssl-rpath=no \
        CC="$CC_PORT" CFLAGS="-O2 -DTHREAD_STACK_SIZE=0x600000"
    run_logged python-make make -j"$JOBS"
    rm -rf "$WORK/python-staging"
    run_logged python-install make install DESTDIR="$WORK/python-staging"

    rm -rf "$OUT/python3"
    mkdir -p "$OUT/python3/BIN" "$OUT/python3/lib"
    "$TARGET-strip" -o "$OUT/python3/BIN/PYTHON3.EXE" "$WORK/python/python.exe"
    cp "$OUT/python3/BIN/PYTHON3.EXE" "$OUT/python3/BIN/PYTHON.EXE"
    cp -R "$WORK/python-staging/Programs/python3/lib/python$minor" "$OUT/python3/lib/python$minor"
    # Trim what cannot run here (GUI toolkits, the C test suite) and the
    # optimised bytecode that is never used by default.
    lib=$OUT/python3/lib/python$minor
    rm -rf "$lib/test" "$lib/idlelib" "$lib/tkinter" "$lib/turtledemo" "$lib/ensurepip" "$lib/turtle.py" \
           "$lib"/config-*
    find "$lib" -name '*.opt-[12].pyc' -delete
    cp "$WORK/python/LICENSE" "$OUT/python3/LICENSE.TXT"
    cp "$DOWNLOADS/cacert.pem" "$OUT/python3/CACERT.PEM"
    echo "python $PYTHON_VERSION -> $OUT/python3"
}

mkdir -p "$WORK" "$OUT"
for port in $PORTS; do
    case "$port" in
    xz) port_xz ;;
    vim) port_vim ;;
    curl) port_curl ;;
    python) port_python ;;
    *) echo "error: unknown port $port" >&2; exit 1 ;;
    esac
done
