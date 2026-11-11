#!/bin/sh
#
# DOS-XC /vim/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
VIM_VERSION=9.1.1000

fetch vim-$VIM_VERSION.tar.gz https://github.com/vim/vim/archive/refs/tags/v$VIM_VERSION.tar.gz
unpack vim-$VIM_VERSION.tar.gz vim
cd "$WORK/vim/src"
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
printf '@VIM %%*\r\n' > "$OUT/Vim/BIN/VI.BAT"
printf '@VIM -R %%*\r\n' > "$OUT/Vim/BIN/VIEW.BAT"
printf '@VIM -e %%*\r\n' > "$OUT/Vim/BIN/EX.BAT"
printf '@VIM -d %%*\r\n' > "$OUT/Vim/BIN/VIMDIFF.BAT"
mkdir -p "$OUT/Vim/share"
cp -R "$WORK/vim-staging/Programs/Vim/share/vim" "$OUT/Vim/share/vim"
rm -rf "$OUT/Vim/share/vim/vim91/lang" "$OUT/Vim/share/vim/vim91/tutor" "$OUT/Vim/share/vim/vim91/print" \
       "$OUT/Vim/share/vim/vim91/macros" "$OUT/Vim/share/vim/vim91/tools" "$OUT/Vim/share/vim/vim91/doc/"*.??x
cp "$WORK/vim/LICENSE" "$OUT/Vim/LICENSE.TXT"
echo "vim $VIM_VERSION -> $OUT/Vim"
