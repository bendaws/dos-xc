# Build the GNU toolchain from Windows: runs toolchain/build-toolchain.sh inside WSL.
#   toolchain\build-toolchain.ps1 [x86_64|i686] [STAGE...]
. (Join-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts') 'windows-common.ps1')
Invoke-PosixScript 'toolchain/build-toolchain.sh' $args
