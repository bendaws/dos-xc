#!/bin/sh
#
# DOS-XC /gnu/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port gnu Version=2026.11 Author="The GNU Project and Mark Nudelman (less)" \
        Description="GNU command-line tools: ls, cp, cat, grep, sed, awk, find, diff, tar, gzip, make, patch, less" \
        Path=BIN Env=GNU_TOOLS=%PACKAGE%/BIN
done
