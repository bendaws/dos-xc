#!/bin/sh
#
# DOS-XC /sqlite/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port SQLite Version=3.50.4 Author="D. Richard Hipp and the SQLite developers" Description="SQLite database shell (sqlite3)" Path=BIN
done
