# Boot the 32-bit edition in QEMU on Windows.
#   scripts\boot32.ps1 [-hd disk.img] [QEMU options...]
# Environment: ISO, QEMU, MEMORY, AUDIO, DISPLAYS, ACCEL.
. (Join-Path $PSScriptRoot 'windows-common.ps1')
Start-DosXc 'qemu-system-i386' 'x86' $args
