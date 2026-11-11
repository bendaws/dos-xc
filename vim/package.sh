#!/bin/sh
#
# DOS-XC /vim/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port Vim Version=9.1.1000 Author="Bram Moolenaar and the Vim community" \
        Description="Fast, keyboard-only terminal text editor" Path=BIN Env=VIM=%PACKAGE%/share/vim
done
