# Build DOS-XC from Windows: runs scripts/build.sh inside WSL.
. (Join-Path $PSScriptRoot 'windows-common.ps1')
Invoke-PosixScript 'scripts/build.sh' $args
