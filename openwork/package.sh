#!/bin/sh
#
# DOS-XC /openwork/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    office=$HERE
    desktop=$ROOT/desktop
    tree=$BUILD/ports/$arch/openwork
    rm -rf "$tree"
    mkdir -p "$tree"
    app_flags="$flags -Wno-missing-field-initializers -I$desktop -I$office -I$ROOT/os/pkg"
    gui="$desktop/gfx.c $desktop/gui.c $desktop/widgets.c $desktop/dialogs.c $desktop/image.c $ROOT/os/pkg/inflate.c"
    common="$office/suite.c $office/zip.c $office/xml.c $office/office.c $ROOT/os/pkg/dsp.c $ROOT/os/pkg/unxz.c $ROOT/os/pkg/deflate.c"
    # OpenMail uses the OpenSSL that the ports build (scripts/ports-common.sh); without it, mail is unencrypted only.
    ports=$ROOT/toolchain/work/ports-$arch/sysroot
    tls=
    if [ -f "$ports/lib/libssl.a" ] && [ -f "$ports/lib/libcrypto.a" ]; then
        tls="-DHAVE_TLS -idirafter $ports/include $ports/lib/libssl.a $ports/lib/libcrypto.a -lpthread"
    else
        echo "warning: no OpenSSL in $ports; OpenMail is built without TLS" >&2
    fi
    version="-DOPENWORK_VERSION=\"$VERSION\""
    spawn "$cc" $app_flags "$version" "$office/papers.c" "$office/doc.c" $common $gui -lm -o "$tree/PAPERS.EXE"
    spawn "$cc" $app_flags "$version" "$office/numbers.c" "$office/sheet.c" $common $gui -lm -o "$tree/NUMBERS.EXE"
    spawn "$cc" $app_flags "$version" "$office/show.c" "$office/deck.c" $common $gui -lm -o "$tree/SHOW.EXE"
    spawn "$cc" $app_flags "$version" "$office/mail.c" "$office/mailnet.c" "$office/mime.c" $common $gui -lm $tls -o "$tree/MAIL.EXE"
    finish_jobs
    python3 "$office/icons.py" "$tree"
    [ ! -f "$ROOT/toolchain/downloads/cacert.pem" ] || cp "$ROOT/toolchain/downloads/cacert.pem" "$tree/CACERT.PEM"
    printf '%s\n' "id=openpapers" "name=OpenPapers" "program=PAPERS.EXE" "icon=PAPERS.PNG" "category=Office" "default=document" \
        "types=docx odt rtf doc htm html md gdoc" "" \
        "id=opennumbers" "name=OpenNumbers" "program=NUMBERS.EXE" "icon=NUMBERS.PNG" "category=Office" "default=spreadsheet" \
        "types=xlsx xlsm ods csv tsv xls gsheet" "" \
        "id=openshow" "name=OpenShow" "program=SHOW.EXE" "icon=SHOW.PNG" "category=Office" "default=presentation" \
        "types=pptx ppsx odp gslides" "" \
        "id=openmail" "name=OpenMail" "program=MAIL.EXE" "icon=MAIL.PNG" "category=Office" > "$tree/MENU.CFG"
    package_port openwork Version="$VERSION" Author="Ben Daws" Description="OpenWork: OpenPapers, OpenNumbers, OpenShow and OpenMail" Path=. Requires=desktop
done
