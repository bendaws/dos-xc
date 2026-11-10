# Shared by the DOS-XC PowerShell scripts.  Dot-source this file.
#
# Boot scripts run the Windows build of QEMU directly.  Build scripts run the
# POSIX shell scripts inside WSL, because GCC and the ports are built with
# autotools.

$ErrorActionPreference = 'Stop'
$script:DosXcRoot = Split-Path -Parent $PSScriptRoot

function Stop-WithError([string]$Message) {
    [Console]::Error.WriteLine("error: $Message")
    exit 1
}

function Get-EnvOrDefault([string]$Name, [string]$Default) {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrEmpty($value)) { return $Default }
    return $value
}

function Find-Qemu([string]$Program) {
    $name = Get-EnvOrDefault 'QEMU' $Program
    $found = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.Source }
    foreach ($folder in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $folder) { continue }
        $candidate = Join-Path (Join-Path $folder 'qemu') "$name.exe"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    Stop-WithError "QEMU emulator not found: $name (install QEMU, or set QEMU to its full path)"
}

function Get-DiskFormat([string]$Path) {
    switch -Regex ($Path) {
        '\.qcow2$' { return 'qcow2' }
        '\.vmdk$' { return 'vmdk' }
        '\.vdi$' { return 'vdi' }
        '\.vhd$' { return 'vpc' }
        '\.vhdx$' { return 'vhdx' }
        default { return 'raw' }
    }
}

# Boot an edition in QEMU.  -hd PATH boots from a virtual disk instead of the
# ISO; every other argument goes to QEMU unchanged.
function Start-DosXc([string]$Program, [string]$IsoSuffix, [object[]]$Arguments) {
    $header = Join-Path $script:DosXcRoot (Join-Path 'src' (Join-Path 'kernel' 'bootinfo.h'))
    $match = Select-String -LiteralPath $header -Pattern '^#define DOSXC_VERSION "(.*)"$' | Select-Object -First 1
    if (-not $match) { Stop-WithError "DOSXC_VERSION not found in $header" }
    $version = $match.Matches[0].Groups[1].Value

    $hd = $null
    $passthrough = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $Arguments.Count; ++$i) {
        if ("$($Arguments[$i])" -ceq '-hd') {
            if ($i + 1 -ge $Arguments.Count) { Stop-WithError '-hd needs a disk image path' }
            ++$i
            $hd = "$($Arguments[$i])"
        } else {
            $passthrough.Add("$($Arguments[$i])")
        }
    }

    $media = New-Object System.Collections.Generic.List[string]
    if ($hd) {
        if (-not (Test-Path -LiteralPath $hd -PathType Leaf)) { Stop-WithError "disk image not found: $hd" }
        # Name the format: QEMU blocks writes to sector 0 of a raw image it had to guess.
        $escaped = $hd.Replace(',', ',,')   # -drive separates options with commas
        $media.Add('-drive')
        $media.Add("file=$escaped,format=$(Get-DiskFormat $hd),if=ide,index=0,media=disk")
        $media.Add('-boot')
        $media.Add('c')
    } else {
        $default = Join-Path $script:DosXcRoot (Join-Path 'build' (Join-Path 'iso' "DOSXC_${version}_$IsoSuffix.iso"))
        $iso = Get-EnvOrDefault 'ISO' $default
        if (-not (Test-Path -LiteralPath $iso -PathType Leaf)) { Stop-WithError "ISO not found: $iso (run scripts\build.ps1 first)" }
        $media.Add('-cdrom')
        $media.Add($iso)
    }

    $qemu = Find-Qemu $Program
    $memory = Get-EnvOrDefault 'MEMORY' '1024'   # MiB; GCC needs several hundred
    $audio = Get-EnvOrDefault 'AUDIO' 'dsound'   # the PC speaker needs a backend (AUDIO=none to do without)
    $displays = [int](Get-EnvOrDefault 'DISPLAYS' '1')   # up to 4; pick the monitor in QEMU's View menu

    $command = New-Object System.Collections.Generic.List[string]
    $command.AddRange([string[]]@('-vga', 'none', '-device', 'VGA,id=video0'))
    for ($i = 1; $i -lt $displays; ++$i) { $command.AddRange([string[]]@('-device', "secondary-vga,id=video$i")) }
    # ACCEL=whpx uses the Windows Hypervisor Platform where it is enabled.
    $accel = Get-EnvOrDefault 'ACCEL' ''
    if ($accel) { $command.AddRange([string[]]@('-accel', $accel)) }
    $command.AddRange([string[]]@('-device', 'qemu-xhci,id=xhci', '-device', 'usb-mouse,bus=xhci.0', '-m', $memory,
        '-audiodev', "$audio,id=speaker", '-machine', 'pcspk-audiodev=speaker'))
    $command.AddRange($media)
    $command.AddRange($passthrough)

    & $qemu @($command.ToArray())
    exit $LASTEXITCODE
}

# Run a POSIX script of this repository inside WSL.
function Invoke-PosixScript([string]$Script, [object[]]$Arguments) {
    $wsl = Get-EnvOrDefault 'WSL' 'wsl.exe'
    if (-not (Get-Command $wsl -ErrorAction SilentlyContinue)) {
        Stop-WithError 'WSL is required to build DOS-XC (run "wsl --install", then see the README)'
    }

    # Settings for the build, translated when they hold a Windows path.
    $environment = New-Object System.Collections.Generic.List[string]
    foreach ($name in 'BUILD', 'CROSS', 'JOBS', 'CC', 'LD', 'NASM', 'XORRISO', 'OBJCOPY',
                      'DOSXC_EXTRA_TREES', 'DOSXC_EXTRA_TREES_32', 'DOSXC_EXTRA_TREES_64', 'DOS_PROGRAMS_ZIP') {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ([string]::IsNullOrEmpty($value)) { continue }
        if ($value -match '^[A-Za-z]:[\\/]') {
            $value = (& $wsl wslpath -a -u $value | Out-String).Trim()
            if ($LASTEXITCODE) { Stop-WithError "could not translate the path in $name" }
        }
        $environment.Add("$name=$value")
    }

    $command = New-Object System.Collections.Generic.List[string]
    $command.AddRange([string[]]@('--cd', $script:DosXcRoot, '--', 'env'))
    $command.AddRange($environment)
    $command.AddRange([string[]]@('sh', "./$Script"))
    foreach ($argument in $Arguments) { $command.Add("$argument") }

    & $wsl @($command.ToArray())
    exit $LASTEXITCODE
}
