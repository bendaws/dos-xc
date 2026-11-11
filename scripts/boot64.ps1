#
# DOS-XC /scripts/boot64.ps1
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

. (Join-Path $PSScriptRoot 'windows-common.ps1')
Start-DosXc 'qemu-system-x86_64' 'x64' $args
