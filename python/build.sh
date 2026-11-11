#!/bin/sh
#
# DOS-XC /python/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
PYTHON_VERSION=3.14.6

build_openssl
fetch_ca_bundle
minor=${PYTHON_VERSION%.*}
build_python=$(command -v "python$minor") || { echo "error: python$minor is needed on the build machine" >&2; exit 1; }
fetch zlib-$ZLIB_VERSION.tar.gz https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz
fetch Python-$PYTHON_VERSION.tar.xz https://www.python.org/ftp/python/$PYTHON_VERSION/Python-$PYTHON_VERSION.tar.xz

unpack zlib-$ZLIB_VERSION.tar.gz zlib
cd "$WORK/zlib"
run_logged zlib-configure env CC="$CC_PORT" CHOST=$TARGET CFLAGS="-O2" ./configure --static --prefix="$SYSROOT"
run_logged zlib-make make -j"$JOBS" libz.a
cp libz.a "$SYSROOT/lib/"
cp zlib.h zconf.h "$SYSROOT/include/"

unpack Python-$PYTHON_VERSION.tar.xz python
cd "$WORK/python"
perl -0pi -e 's/(\t\*-\*-wasi\*\)\n\s+ac_sys_system=WASI\n\s+;;\n)/$1\t*-*-dosxc*)\n\t    ac_sys_system=DOSXC\n\t    ;;\n/;
              s/(\t\*-\*-vxworks\*\)\n\t\t_host_ident=\$host_cpu\n\t\t;;\n)/$1\t*-*-dosxc*)\n\t\t_host_ident=\$host_cpu\n\t\t;;\n/' configure
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
[ "$ARCH" = i686 ] && printf 'ac_cv_gcc_asm_for_x87=yes\nac_cv_x87_double_rounding=yes\n' >> "$site"
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
