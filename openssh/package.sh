#!/bin/sh
#
# DOS-XC /openssh/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port OpenSSH Version=10.0p1 Author="The OpenBSD Project" \
        Description="Secure shell client: ssh, scp, sftp, ssh-keygen, ssh-agent" Path=BIN \
        Firewall="SSH.EXE;SCP.EXE;SFTP.EXE;SSH-KEYSCAN.EXE"
done
