#!/bin/sh
#
# DOS-XC /git/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
GIT_VERSION=2.51.0
[ "$ARCH" = x86_64 ] || { echo "git needs fork(), which only the x64 edition has; skipping $ARCH"; exit 0; }

build_libcurl
fetch_ca_bundle
fetch git-$GIT_VERSION.tar.xz https://mirrors.edge.kernel.org/pub/software/scm/git/git-$GIT_VERSION.tar.xz
unpack git-$GIT_VERSION.tar.xz git
cd "$WORK/git"
cp "$HERE/dosxc-paths.h" compat/
# The C library answers readlink("/proc/self/exe"); there is no /proc to resolve.
sed -i.orig 's|strbuf_realpath(buf, PROCFS_EXECUTABLE_PATH, 0)|!strbuf_readlink(buf, PROCFS_EXECUTABLE_PATH, 0)|' exec-cmd.c
# PATH is ';'-separated, and running "DIR/NAME" runs DIR/NAME.EXE.
sed -i.orig "s|strchrnul(p, ':')|strchrnul(p, PATH_SEP)|; s|if (is_executable(buf.buf))|if (is_executable(buf.buf) \|\| (strbuf_addstr(\&buf, \".exe\"), is_executable(buf.buf)))|" run-command.c
# Paths may start with a drive ("C:/DIR"): git's Windows path helpers are
# plain string code.  The libc has no <regex.h>, so git uses its own.
cat > config.mak <<MAK
CC = $CC_PORT
AR = $TARGET-ar
STRIP = $TARGET-strip
CFLAGS = -O2
prefix = /Programs/git
gitexecdir = libexec/git-core
sysconfdir = etc
template_dir = share/git-core/templates
RUNTIME_PREFIX = YesPlease
X = .exe
PROCFS_EXECUTABLE_PATH = /proc/self/exe
SKIP_DASHED_BUILT_INS = YesPlease
NO_INSTALL_HARDLINKS = YesPlease
INSTALL_SYMLINKS =
NO_REGEX = YesPlease
NO_MMAP = YesPlease
CSPRNG_METHOD = getrandom
NO_PERL = YesPlease
NO_PYTHON = YesPlease
NO_TCLTK = YesPlease
NO_GETTEXT = YesPlease
NO_EXPAT = YesPlease
NO_ICONV = YesPlease
NO_OPENSSL = YesPlease
NO_SYS_SELECT_H =
NO_NSEC = YesPlease
HAVE_PATHS_H = YesPlease
HAVE_DEV_TTY = YesPlease
HAVE_CLOCK_GETTIME = YesPlease
HAVE_GETDELIM = YesPlease
HAVE_ALLOCA_H = YesPlease
CURLDIR = $SYSROOT
CURL_LDFLAGS = -lcurl -lssl -lcrypto -lz
ZLIB_PATH = $SYSROOT
COMPAT_CFLAGS += -include compat/dosxc-paths.h
COMPAT_OBJS += compat/win32/path-utils.o
MAK
UNAME="uname_S=DOSXC uname_O=DOSXC uname_M=$ARCH uname_R=0.2 uname_P=unknown uname_V=0"
run_logged git-make make -j"$JOBS" $UNAME all
rm -rf "$WORK/git-staging"
run_logged git-install make $UNAME install DESTDIR="$WORK/git-staging"
rm -rf "$OUT/git"
mkdir -p "$OUT/git"
cp -R "$WORK/git-staging/Programs/git/"* "$OUT/git/"
rm -rf "$OUT/git/share/man" "$OUT/git/share/perl5" "$OUT/git/share/gitweb" "$OUT/git/share/git-gui" "$OUT/git/share/gitk"
# FAT has no symbolic links: https gets a copy of the http helper, and the
# server-side programs and the Perl and Python commands are left out.
core=$OUT/git/libexec/git-core
find "$OUT/git" -type l -delete
cp "$core/git-remote-http.exe" "$core/git-remote-https.exe"
for unused in git-daemon git-http-backend git-imap-send git-shell scalar git-cvsserver git-archimport \
              git-cvsexportcommit git-cvsimport git-p4 git-instaweb git-send-email git-svn; do
    rm -f "$core/$unused" "$core/$unused.exe" "$OUT/git/bin/$unused" "$OUT/git/bin/$unused.exe"
done
find "$OUT/git" -type f -name '*.exe' | while read -r program; do
    upper=$(dirname "$program")/$(basename "$program" .exe | tr a-z A-Z).EXE
    mv "$program" "$upper.tmp" && mv "$upper.tmp" "$upper"
    "$TARGET-strip" "$upper"
done
cp "$DOWNLOADS/cacert.pem" "$OUT/git/CACERT.PEM"
cp COPYING "$OUT/git/COPYING.TXT"
echo "git $GIT_VERSION -> $OUT/git"
