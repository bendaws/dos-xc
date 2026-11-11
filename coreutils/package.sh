#!/bin/sh
#
# DOS-XC /coreutils/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
COMMANDS="HELP DIR TYPE MORE CLS COLOR VER BEEP WRITE COPY REN DEL MD RD EDIT SPEED RESTART SHUTDOWN EJECT IPCONFIG PING DRIVERS"
for edition in $EDITIONS; do
    use_edition "$edition"
    mkdir -p "$out/coreutils"
    for command in $COMMANDS; do
        source=$(echo "$command" | tr A-Z a-z)
        spawn "$cc" $flags "$HERE/$source.c" -o "$out/coreutils/$command.EXE"
    done
    spawn "$cc" $flags "$HERE/copy.c" -o "$out/coreutils/MOVE.EXE"
    finish_jobs
    for alias in MKDIR:MD RMDIR:RD ERASE:DEL RENAME:REN REBOOT:RESTART; do
        cp "$out/coreutils/${alias#*:}.EXE" "$out/coreutils/${alias%%:*}.EXE"
    done
    pack "$out/coreutils" "coreutils-$bits.dsp" Name=coreutils Version="$VERSION" \
        Author="Ben Daws" Description="Basic DOS-XC commands (DIR, COPY, EDIT ...)" Arch="$bits" Path=.
done
