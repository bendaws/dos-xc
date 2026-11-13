#!/bin/sh
#
# DOS-XC /firefox/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
DIST=$ROOT/toolchain/work/firefox-obj/dist/firefox
FONTS=$ROOT/toolchain/work/dejavu-fonts-ttf-2.37/ttf
CJK=$ROOT/toolchain/work/noto-cjk

for edition in $EDITIONS; do
    [ "$edition" = 64 ] || continue
    use_edition "$edition"
    if [ ! -f "$DIST/libxul.so" ]; then
        echo "DOSfox has not been built (firefox/build.sh, then firefox/build.sh package): skipping"
        on_media dosfox-64.dsp
        continue
    fi
    if [ ! -d "$FONTS" ]; then
        curl -fsSL https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2 |
            tar -xjf - -C "$ROOT/toolchain/work"
    fi

    if [ ! -f "$CJK/NotoSansCJK-Regular.ttc" ]; then
        mkdir -p "$CJK"
        curl -fsSL -o "$CJK/NotoSansCJK-Regular.ttc" https://github.com/notofonts/noto-cjk/raw/main/Sans/OTC/NotoSansCJK-Regular.ttc
    fi

    rm -rf "$BUILD/ports/$arch/firefox"           # the package's name before DOSfox
    tree=$BUILD/ports/$arch/dosfox
    rm -rf "$tree"
    mkdir -p "$tree/fonts" "$tree/home" "$tree/browser/defaults/preferences"
    (cd "$DIST" && tar -cf - --exclude=firefox-bin --exclude=pingsender --exclude=precomplete \
        --exclude=removed-files --exclude=gmp-clearkey .) | tar -xf - -C "$tree"
    mv "$tree/firefox" "$tree/DOSFOX.EXE"
    "$CROSS/$triple-strip" --strip-unneeded "$tree/DOSFOX.EXE" "$tree"/*.so
    cp "$(dirname "$0")/files/dosxc-prefs.js" "$tree/browser/defaults/preferences/dosxc.js"
    for font in DejaVuSans DejaVuSans-Bold DejaVuSans-Oblique DejaVuSans-BoldOblique \
                DejaVuSerif DejaVuSerif-Bold DejaVuSerif-Italic DejaVuSerif-BoldItalic \
                DejaVuSansMono DejaVuSansMono-Bold DejaVuSansMono-Oblique DejaVuSansMono-BoldOblique; do
        cp "$FONTS/$font.ttf" "$tree/fonts/"
    done
    cp "$CJK/NotoSansCJK-Regular.ttc" "$tree/fonts/"
    cp "$ROOT/toolchain/work/firefox/browser/branding/dosfox/default16.png" "$tree/DOSFOX.PNG"
    echo "DOSfox's profile lives here." > "$tree/home/README.TXT"
    printf '%s\n' "id=dosfox" "name=DOSfox" "program=DOSFOX.EXE" "icon=DOSFOX.PNG" "category=Internet" \
        "default=browser" "types=htm html xhtml svg" "url=1" > "$tree/MENU.CFG"
    package_port dosfox Version=162.0a1 Author="Ben Daws" \
        Description="Firefox for DOS-XC (ported)" \
        Path=. Requires=desktop
done
