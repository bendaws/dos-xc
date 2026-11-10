# Boot the 64-bit edition in QEMU on Windows.
#   scripts\boot64.ps1 [-hd disk.img] [QEMU options...]
# Environment: ISO, QEMU, MEMORY, AUDIO, DISPLAYS, ACCEL.
. (Join-Path $PSScriptRoot 'windows-common.ps1')
Start-DosXc 'qemu-system-x86_64' 'x64' $args
