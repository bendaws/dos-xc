#!/bin/sh
#
# DOS-XC /hyperv/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    mkdir -p "$out/hyperv"
    "$cc" $driver_flags -fvisibility=hidden -I"$HERE" "$HERE"/*.c -o "$out/hyperv/HYPERV.DRV"
    pack "$out/hyperv" "hyperv-$bits.dsp" Name=hyperv Version=1.0 \
        Author="Ben Daws" Description="Hyper-V network, disks, keyboard and mouse" Arch="$bits" Driver=HYPERV.DRV
done
