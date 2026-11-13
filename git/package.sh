#!/bin/sh
#
# DOS-XC /git/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    [ "$edition" = 64 ] || continue
    use_edition "$edition"
    package_port git Version=2.51.0 Author="Linus Torvalds, Junio C Hamano and the Git community" \
        Description="Git version control (64-bit; HTTPS and SSH remotes)" Path=bin \
        Env=GIT_SSL_CAINFO=%PACKAGE%/CACERT.PEM Requires=Bash \
        Firewall="GIT.EXE;GIT-REMOTE-HTTP.EXE;GIT-REMOTE-HTTPS.EXE;GIT-HTTP-FETCH.EXE"
done
