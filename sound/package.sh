#!/bin/sh
#
# DOS-XC /sound/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    mkdir -p "$out/sound"
    spawn "$cc" $driver_flags "$HERE/hda.c" -o "$out/sound/HDA.DRV"
    finish_jobs
    pack "$out/sound" "sound-$bits.dsp" Name=sound Version=1.0 \
        Author="Ben Daws" Description="Sound drivers (Intel High Definition Audio)" Arch="$bits" \
        Driver=HDA.DRV
done
