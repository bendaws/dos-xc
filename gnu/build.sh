#!/bin/sh
#
# DOS-XC /gnu/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
TOOLS=${TOOLS:-"coreutils grep sed gawk findutils diffutils tar gzip make patch less"}
TREE=$OUT/gnu
STAGE=$WORK/gnu-staging
mkdir -p "$TREE/BIN" "$TREE/LICENSES"

# Answers for configure tests that cannot run on the build machine.
CACHE="gl_cv_func_getcwd_path_max=yes gl_cv_func_getcwd_null=yes gl_cv_func_working_mktime=yes \
ac_cv_func_fork=yes ac_cv_func_fork_works=yes ac_cv_func_vfork=yes ac_cv_func_vfork_works=yes \
gl_cv_func_mkdir_trailing_slash_works=yes gl_cv_func_rename_dest_works=yes gl_cv_func_rmdir_works=yes \
gt_cv_locale_fake=yes gl_cv_func_strcasecmp_works=yes"

configure_tool() {
    name=$1
    shift
    cd "$WORK/$name"
    run_logged "$name-configure" env $CACHE ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/gnu \
        --disable-nls --disable-dependency-tracking CC="$CC_PORT" CFLAGS="-O2 ${EXTRA_CFLAGS:-}" "$@"
}

install_tool() {
    name=$1
    run_logged "$name-make" make -j"$JOBS"
    rm -rf "$STAGE"
    run_logged "$name-install" make install DESTDIR="$STAGE"
    for f in "$STAGE"/gnu/bin/*; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .exe | tr a-z A-Z)
        case " $SKIP " in *" $base "*) continue ;; esac
        if head -c 4 "$f" | grep -q ELF; then
            cp "$f" "$TREE/BIN/$base.EXE"
            "$TARGET-strip" "$TREE/BIN/$base.EXE"
        else
            cp "$f" "$TREE/BIN/$base"
        fi
    done
    for licence in COPYING LICENSE; do
        [ -f "$WORK/$name/$licence" ] && cp "$WORK/$name/$licence" "$TREE/LICENSES/$name.txt" && break
    done
    echo "$name -> $TREE"
}

# Programs that clash with DOS-XC commands (DIR, MKDIR ...) or cannot work here.
SKIP="DIR VDIR CHROOT CHCON RUNCON STDBUF NICE NOHUP PINKY USERS WHO UPTIME HOSTID SU KILL"

build_coreutils() {
    fetch coreutils-9.7.tar.xz https://ftp.gnu.org/gnu/coreutils/coreutils-9.7.tar.xz
    unpack coreutils-9.7.tar.xz coreutils
    configure_tool coreutils --without-selinux --disable-acl --disable-xattr --disable-libcap --without-gmp \
        --without-openssl --disable-libsmack --enable-install-program=hostname \
        --enable-no-install-program=chroot,chcon,runcon,stdbuf,nice,nohup,pinky,users,who,uptime,hostid,kill
    install_tool coreutils
}

simple_tool() {
    name=$1 archive=$2 url=$3
    shift 3
    fetch "$archive" "$url"
    unpack "$archive" "$name"
    configure_tool "$name" "$@"
    install_tool "$name"
}

build_grep() { simple_tool grep grep-3.12.tar.xz https://ftp.gnu.org/gnu/grep/grep-3.12.tar.xz --disable-perl-regexp; }
build_sed() { simple_tool sed sed-4.9.tar.xz https://ftp.gnu.org/gnu/sed/sed-4.9.tar.xz --without-selinux --disable-acl; }
build_gawk() {
    simple_tool gawk gawk-5.3.2.tar.xz https://ftp.gnu.org/gnu/gawk/gawk-5.3.2.tar.xz --disable-extensions --without-readline \
        --disable-mpfr --disable-pma
    [ -f "$TREE/BIN/GAWK.EXE" ] && cp "$TREE/BIN/GAWK.EXE" "$TREE/BIN/AWK.EXE"
    rm -f "$TREE/BIN/GAWK-"*.EXE "$TREE/BIN/GAWKBUG"
}
build_findutils() {
    SKIP="$SKIP UPDATEDB LOCATE"
    simple_tool findutils findutils-4.10.0.tar.xz https://ftp.gnu.org/gnu/findutils/findutils-4.10.0.tar.xz --without-selinux
}
build_diffutils() { simple_tool diffutils diffutils-3.12.tar.xz https://ftp.gnu.org/gnu/diffutils/diffutils-3.12.tar.xz; }
build_tar() {
    simple_tool tar tar-1.35.tar.xz https://ftp.gnu.org/gnu/tar/tar-1.35.tar.xz --without-selinux --disable-acl \
        --without-xattrs --without-posix-acls --with-rmt=/nonexistent
}
build_gzip() {
    simple_tool gzip gzip-1.14.tar.xz https://ftp.gnu.org/gnu/gzip/gzip-1.14.tar.xz
    for script in GUNZIP ZCAT; do printf '#!/bin/sh\nexec gzip %s "$@"\n' "$( [ $script = ZCAT ] && echo -cd || echo -d)" > "$TREE/BIN/$script"; done
}
build_make() {
    fetch make-4.4.1.tar.gz https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz
    unpack make-4.4.1.tar.gz make
    # There is no /bin: recipes run in whatever "sh" PATH finds (bash's SH.EXE).
    sed -i.orig 's|^const char \*default_shell = "/bin/sh";|const char *default_shell = "sh";|' "$WORK/make/src/job.c"
    sed -i.orig 's/defined __DJGPP__$/defined __DJGPP__ || defined __dosxc__/; s/#elif defined __DJGPP__$/#elif defined __DJGPP__ || defined __dosxc__/' \
        "$WORK/make/lib/findprog-in.c"
    EXTRA_CFLAGS=-std=gnu17 configure_tool make --without-guile --disable-load
    install_tool make
}
build_patch() { simple_tool patch patch-2.8.tar.xz https://ftp.gnu.org/gnu/patch/patch-2.8.tar.xz --disable-xattr; }
build_less() {
    fetch less-679.tar.gz https://www.greenwoodsoftware.com/less/less-679.tar.gz
    build_ncurses
    unpack less-679.tar.gz less
    EXTRA_CFLAGS="-I$SYSROOT/include/ncursesw" configure_tool less --with-regex=posix LIBS=-lncursesw
    install_tool less
}

for tool in $TOOLS; do
    "build_$tool"
done
