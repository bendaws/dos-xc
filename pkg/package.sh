#!/bin/sh
#
# DOS-XC /pkg/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    desktop=$ROOT/desktop
    tree=$out/pkg
    rm -rf "$tree"
    mkdir -p "$tree"
    reader="$ROOT/os/pkg/dsp.c $ROOT/os/pkg/unxz.c $ROOT/os/pkg/inflate.c $ROOT/os/pkg/deflate.c $ROOT/os/pkg/repo.c $ROOT/os/pkg/sha256.c"
    spawn "$cc" $flags -I"$ROOT/os/pkg" "$ROOT/os/pkg/pkg.c" $reader -o "$tree/PKG.EXE"
    gui="$desktop/gfx.c $desktop/gui.c $desktop/widgets.c $desktop/dialogs.c $desktop/image.c"
    spawn "$cc" $flags -Wno-missing-field-initializers -I"$desktop" -I"$ROOT/os" -I"$ROOT/os/pkg" "$desktop/pkgmgr.c" $gui $reader -lm \
        -o "$tree/PKGMGR.EXE"
    finish_jobs
    printf '%s\n' "id=pkgmgr" "name=Package Manager" "program=PKGMGR.EXE" "icon=package" "category=System/Administrators" \
        > "$tree/MENU.CFG"
    pack "$tree" "pkg-$bits.dsp" Name=pkg Version="$VERSION" Author="Ben Daws" \
        Description="DOS-XC Package Manager (PKG and its window)" Arch="$bits" Path=. Required=1
done
