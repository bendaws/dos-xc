#!/bin/sh
#
# DOS-XC /ethernet/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    mkdir -p "$out/ethernet"
    for driver in e1000 tg3 virtio-net; do
        name=$(echo "$driver" | tr a-z A-Z | tr -d -)
        spawn "$cc" $driver_flags "$HERE/$driver.c" -o "$out/ethernet/$name.DRV"
    done
    finish_jobs
    pack "$out/ethernet" "ethernet-$bits.dsp" Name=ethernet Version=1.0 \
        Author="Ben Daws" Description="Ethernet drivers (Intel, Broadcom, virtio)" Arch="$bits" \
        Driver="$(cd "$out/ethernet" && ls *.DRV | tr '\n' ';' | sed 's/;$//')"
done
