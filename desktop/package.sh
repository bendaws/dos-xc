#!/bin/sh
#
# DOS-XC /desktop/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    desktop=$HERE
    desktop_out=$out/desktop
    rm -rf "$desktop_out"
    mkdir -p "$desktop_out/Apps" "$desktop_out/Wallpapers" "$desktop_out/Pictures"
    cp "$desktop"/pictures/*.jpg "$desktop_out/Pictures/"
    cp "$desktop"/wallpapers/*.jpg "$desktop_out/Wallpapers/"
    for picture in "$desktop"/wallpapers/*.avif; do
        [ -f "$picture" ] || continue
        name=$(basename "$picture" .avif)
        python3 -c 'import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert("RGB")
image.thumbnail((2560, 2560))
image.save(sys.argv[2], quality=90)' "$picture" "$desktop_out/Wallpapers/$name.jpg" 2>/dev/null ||
            sips -s format jpeg "$picture" --out "$desktop_out/Wallpapers/$name.jpg" >/dev/null 2>&1 ||
            echo "warning: $picture could not be converted (needs Pillow with AVIF, or sips)"
    done
    app_flags="$flags -Wno-missing-field-initializers"
    spawn "$cc" $app_flags -I"$desktop" "$desktop/desktop.c" "$desktop/gfx.c" "$desktop/apps.c" "$desktop/widgets.c" "$desktop/dialogs.c" \
        "$desktop/icons.c" "$desktop/control.c" "$desktop/netpanel.c" "$desktop/datetime.c" "$desktop/logon.c" "$desktop/users.c" \
        "$desktop/image.c" "$ROOT/os/pkg/inflate.c" -I"$ROOT/os/pkg" -o "$desktop_out/DESKTOP.EXE"
    gui="$desktop/gfx.c $desktop/gui.c $desktop/widgets.c $desktop/dialogs.c $desktop/image.c $ROOT/os/pkg/inflate.c"
    archive="$desktop/archive/common.c $desktop/archive/inflate.c $desktop/archive/deflate.c $desktop/archive/lzma.c $desktop/archive/xz.c
             $desktop/archive/tar.c $desktop/archive/zip.c $desktop/archive/sevenz.c $desktop/archive/rar.c"
    for app in explorer:EXPLORER taskmgr:TASKMGR calc:CALC editor:EDITOR paint:PAINT solitaire:SOLITAIRE mines:MINES chess:CHESS terminal:TERMINAL events:EVENTS firewall:FIREWALL; do
        [ -f "$desktop/${app%%:*}.c" ] || continue
        extra=
        [ "${app%%:*}" = explorer ] && extra=$archive
        spawn "$cc" $app_flags -I"$desktop" "$desktop/${app%%:*}.c" $gui $extra -I"$ROOT/os/pkg" -lm -o "$desktop_out/Apps/${app#*:}.EXE"
    done
    spawn "$cc" $app_flags -I"$desktop" "$desktop/devices.c" $gui "$ROOT/os/pkg/dsp.c" "$ROOT/os/pkg/unxz.c" "$ROOT/os/pkg/deflate.c" \
        -I"$ROOT/os/pkg" -lm -o "$desktop_out/Apps/DEVICES.EXE"
    spawn "$cc" $app_flags -I"$desktop" "$desktop/fonts.c" "$desktop/ttf.c" $gui -I"$ROOT/os/pkg" -lm -o "$desktop_out/Apps/FONTS.EXE"
    spawn "$cc" $app_flags -I"$desktop" -I"$ROOT/os" "$desktop/setupgui.c" $gui "$ROOT/os/setup/install.c" "$ROOT/os/runtime/sha256.c" "$ROOT/os/setup/target.c" \
        "$ROOT/os/drivers/fat32.c" "$ROOT/os/pkg/dsp.c" "$ROOT/os/pkg/unxz.c" "$ROOT/os/pkg/deflate.c" -I"$ROOT/os/pkg" -lm -o "$desktop_out/Apps/SETUPGUI.EXE"
    spawn "$cc" $app_flags -I"$ROOT/os" "$HERE/vdm/cpu.c" "$HERE/vdm/bios.c" "$HERE/vdm/video.c" "$HERE/vdm/dos.c" "$HERE/vdm/host.c" \
        -o "$desktop_out/Apps/DOSVM.EXE"
    finish_jobs
    pack "$desktop_out" "desktop-$bits.dsp" Name="DOS-XC Desktop" Version="$VERSION" \
        Author="Ben Daws" Description="Graphical desktop and its programs" Arch="$bits" Path=. Provides=desktop
done
