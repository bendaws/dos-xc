#!/bin/sh
#
# DOS-XC /oregon/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
zip=$HERE/OREGON.ZIP
if [ ! -f "$zip" ] || { [ -f "$PACKAGES/Oregon-16.dsp" ] && [ ! "$zip" -nt "$PACKAGES/Oregon-16.dsp" ]; }; then
    on_media Oregon-16.dsp
    exit 0
fi
rm -rf "$PART/oregon"
python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); [z.extract(i, sys.argv[2]) for i in z.infolist() if i.filename.upper() != "INSTALL.EXE"]' \
    "$zip" "$PART/oregon"
pack "$PART/oregon" Oregon-16.dsp Name=Oregon Version=1.0 Author=MECC \
    Description="The Oregon Trail" Arch=16 Path=. StartIn=program
