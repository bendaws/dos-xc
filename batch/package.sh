#!/bin/sh
#
# DOS-XC /batch/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    mkdir -p "$out/batch"
    "$cc" $flags "$HERE/batch.c" -o "$out/batch/BATCH.EXE"
    pack "$out/batch" "batch-$bits.dsp" Name=batch Version="$VERSION" \
        Author="Ben Daws" Description="Batch file processor" Arch="$bits" Path=.
done
