#!/bin/sh
#
# DOS-XC /vlc/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    [ "$edition" = 64 ] || continue
    use_edition "$edition"
    tree=$BUILD/ports/$arch/VLC
    [ -d "$tree" ] && printf '%s\n' "id=vlc" "name=VLC media player" "program=VLC.EXE" "icon=ICON.PNG" "category=Multimedia" \
        "default=media" "types=mp4 m4v mkv webm avi mov mpg mpeg ts flv wmv mp3 m4a aac wav flac ogg opus wma" > "$tree/MENU.CFG"
    package_port VLC Version=3.0.21 Author="VideoLAN" Description="VLC media player (64-bit)" Path=. \
        Requires=desktop Firewall=VLC.EXE
done
