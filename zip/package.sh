#!/bin/sh
#
# DOS-XC /zip/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port zip Version=3.0 Author="Info-ZIP" Description="Info-ZIP zip and unzip (.zip archives)" Path=BIN
done
