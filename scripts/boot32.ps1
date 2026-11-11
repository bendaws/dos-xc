#
# DOS-XC /scripts/boot32.ps1
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

. (Join-Path $PSScriptRoot 'windows-common.ps1')
Start-DosXc 'qemu-system-i386' 'x86' $args
