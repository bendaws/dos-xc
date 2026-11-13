#!/bin/sh
#
# DOS-XC /nano/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    tree=$BUILD/ports/$arch/nano
    [ -d "$tree" ] && printf '%s\n' "id=nano" "name=nano" "program=BIN/NANO.EXE" "icon=editor" "category=Utilities" "terminal=1" \
        "types=txt md c h sh py" > "$tree/MENU.CFG"
    package_port nano Version=8.6 Author="The GNU Project" Description="Small, easy terminal text editor" Path=BIN
done
