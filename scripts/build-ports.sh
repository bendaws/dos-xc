#!/bin/sh
#
# DOS-XC /scripts/build-ports.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCH=${1:-x86_64}
[ $# -gt 0 ] && shift
for port in ${*:-xz vim curl python}; do
    [ -f "$ROOT/$port/build.sh" ] || { echo "error: unknown port $port" >&2; exit 1; }
    "$ROOT/$port/build.sh" "$ARCH"
done
