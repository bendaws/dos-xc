#!/bin/sh
#
# DOS-XC /dosoffice/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    office=$HERE
    desktop=$ROOT/desktop
    tree=$BUILD/ports/$arch/dosoffice
    rm -rf "$tree"
    mkdir -p "$tree"
    app_flags="$flags -Wno-missing-field-initializers -I$desktop -I$office -I$ROOT/os/pkg"
    gui="$desktop/gfx.c $desktop/gui.c $desktop/widgets.c $desktop/dialogs.c $desktop/image.c $ROOT/os/pkg/inflate.c"
    common="$office/zip.c $office/xml.c $office/office.c $ROOT/os/pkg/dsp.c $ROOT/os/pkg/deflate.c"
    spawn "$cc" $app_flags "$office/writer.c" "$office/doc.c" $common $gui -lm -o "$tree/WRITER.EXE"
    spawn "$cc" $app_flags "$office/sheets.c" "$office/sheet.c" $common $gui -lm -o "$tree/SHEETS.EXE"
    finish_jobs
    python3 "$office/icons.py" "$tree"
    printf '%s\n' "id=writer" "name=DOS Writer" "program=WRITER.EXE" "icon=WRITER.PNG" "category=Office" "default=document" \
        "types=docx odt rtf doc htm html md gdoc" "" \
        "id=sheets" "name=DOS Sheets" "program=SHEETS.EXE" "icon=SHEETS.PNG" "category=Office" "default=spreadsheet" \
        "types=xlsx xlsm ods csv tsv xls gsheet" > "$tree/MENU.CFG"
    package_port dosoffice Version="$VERSION" Author="Ben Daws" Description="DOS Office: Writer and Sheets" Path=.
done
