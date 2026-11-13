#!/bin/sh
#
# DOS-XC /bash/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    [ "$edition" = 64 ] || continue
    use_edition "$edition"
    tree=$BUILD/ports/$arch/Bash
    [ -d "$tree" ] && printf '%s\n' "id=bash" "name=Bash" "program=BIN/BASH.EXE" "icon=prompt" "category=Utilities" "terminal=1" \
        > "$tree/MENU.CFG"
    package_port Bash Version=5.3 Author="Chet Ramey and the Free Software Foundation" \
        Description="GNU Bourne-Again Shell (64-bit)" Path=BIN Env=BASH_SYSTEM_RC=%PACKAGE%/ETC/BASHRC
done
