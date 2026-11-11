#
# DOS-XC /scripts/build-ports.ps1
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

. (Join-Path $PSScriptRoot 'windows-common.ps1')
Invoke-PosixScript 'scripts/build-ports.sh' $args
