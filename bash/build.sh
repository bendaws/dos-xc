#!/bin/sh
#
# DOS-XC /bash/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
BASH_VERSION_PORT=5.3
[ "$ARCH" = x86_64 ] || { echo "bash needs fork(), which only the x64 edition has; skipping $ARCH"; exit 0; }

fetch bash-$BASH_VERSION_PORT.tar.gz https://ftp.gnu.org/gnu/bash/bash-$BASH_VERSION_PORT.tar.gz
unpack bash-$BASH_VERSION_PORT.tar.gz bash
cd "$WORK/bash"
patch -p1 < "$HERE/dosxc.patch" > /dev/null
# Answers for the configure tests that cannot run on the build machine.  The
# kernel does not start "#!" scripts, so bash does (ac_cv_sys_interpreter).
run_logged bash-configure env \
    bash_cv_dev_fd=absent bash_cv_dev_stdin=absent bash_cv_sys_named_pipes=missing \
    bash_cv_job_control_missing=missing bash_cv_func_ctype_nonascii=no bash_cv_must_reinstall_sighandlers=no \
    bash_cv_func_sigsetjmp=present bash_cv_getcwd_malloc=yes bash_cv_printf_a_format=yes \
    bash_cv_unusable_rtsigs=no bash_cv_wcwidth_broken=no bash_cv_termcap_lib=libc bash_cv_func_strcoll_broken=no \
    bash_cv_ulimit_maxfds=yes bash_cv_type_rlimit=rlim_t bash_cv_wexitstatus_offset=8 bash_cv_fnmatch_equiv_fallback=no \
    ac_cv_sys_interpreter=no ac_cv_func_mmap_fixed_mapped=no ac_cv_func_fork=yes ac_cv_func_fork_works=yes \
    ac_cv_func_vfork=yes ac_cv_func_vfork_works=yes ac_cv_func_setvbuf_reversed=no \
    ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/Bash \
    --without-bash-malloc --disable-nls --disable-job-control --enable-readline --enable-history \
    --without-installed-readline --enable-static-link=no \
    CC="$CC_PORT" CFLAGS="-O2" CC_FOR_BUILD=cc
run_logged bash-make make -j"$JOBS"
rm -rf "$OUT/Bash"
mkdir -p "$OUT/Bash/BIN" "$OUT/Bash/ETC"
cp bash.exe "$OUT/Bash/BIN/BASH.EXE" 2>/dev/null || cp bash "$OUT/Bash/BIN/BASH.EXE"
"$TARGET-strip" "$OUT/Bash/BIN/BASH.EXE"
cp "$OUT/Bash/BIN/BASH.EXE" "$OUT/Bash/BIN/SH.EXE"
cp "$HERE/bashrc" "$OUT/Bash/ETC/BASHRC"
cp COPYING "$OUT/Bash/COPYING.TXT"
echo "bash $BASH_VERSION_PORT -> $OUT/Bash"
