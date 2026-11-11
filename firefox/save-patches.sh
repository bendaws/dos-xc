#!/bin/sh
#
# DOS-XC /firefox/save-patches.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC=$(dirname "$HERE")/toolchain/work/firefox
git -C "$SRC" add -N -A
git -C "$SRC" diff --binary > "$HERE/patches/0001-dosxc-target.patch"
git -C "$SRC" reset -q
cat "$HERE"/patches/*.patch | cksum > "$SRC/.git/dosxc-patches"
wc -l "$HERE/patches/0001-dosxc-target.patch"
