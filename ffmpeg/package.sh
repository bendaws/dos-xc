#!/bin/sh
#
# DOS-XC /ffmpeg/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port ffmpeg Version=6.1.2 Author="The FFmpeg developers" \
        Description="FFmpeg: convert and inspect audio and video (ffmpeg, ffprobe)" Path=BIN Firewall="FFMPEG.EXE;FFPROBE.EXE"
done
