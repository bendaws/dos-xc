#
# DOS-XC /gcc/build.ps1
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

. (Join-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts') 'windows-common.ps1')
Invoke-PosixScript 'gcc/build.sh' $args
