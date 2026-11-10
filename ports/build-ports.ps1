# Build the third-party ports from Windows: runs ports/build-ports.sh inside WSL.
#   ports\build-ports.ps1 [x86_64|i686] [PORT...]
. (Join-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts') 'windows-common.ps1')
Invoke-PosixScript 'ports/build-ports.sh' $args
