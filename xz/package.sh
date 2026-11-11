#!/bin/sh
#
# DOS-XC /xz/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port xz Version=5.8.1 Author="The Tukaani Project" \
        Description="Compression utility" Path=BIN
done
