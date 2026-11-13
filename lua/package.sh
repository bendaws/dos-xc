#!/bin/sh
#
# DOS-XC /lua/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port Lua Version=5.4.7 Author="Lua.org, PUC-Rio" Description="Lua scripting language (lua, luac)" Path=BIN
done
