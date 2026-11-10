<h1 align="center" style="display:grid;place-items:center;">
    <img width=115 src=".github/dosxc.png"/>
    DOS-XC
</h1>

DOS-XC (DOS eXtended Core) is a full-fledged modern DOS operating system. It can run your 16-bit DOS programs through kernel-level emulation, or new modern 32-bit and 64-bit programs. In summary, it's like DOS but awesome.

DOS-XC is beta software and should not be used as a daily machine unless you **llloovveee** DOS.

## Screenshots
![The GUI Setup program showing a list of installable packages](/.github/screenshots/setup-gui.png)
![GCC compiling and running a Hello world program on DOS-XC](/.github/screenshots/gcc.png)
![The 16-bit MS-DOS game "The Oregon Trail" running on DOS-XC among a task manager and a file manager](/.github/screenshots/oregon.png)
![The Displays settings menu showing multi-monitor support and scaling support](/.github/screenshots/displays.png)

## Goals
DOS-XC wants to be a(n):
- viable-to-use operating system you can actually use, work, and play on with proper 32-bit and 64-bit support
- easy-to-use operating system that people from your dad to your kids can use
- fun-to-use operating system that you can run your favorite DOS games with

## Get the system
Every version has an accompanying GitHub release, so you can start there: https://github.com/bendaws/dos-xc/releases/latest

- Download `DOSXC_<version>_x64.iso` for 64-bit systems, or `DOSXC_<version>_x86.iso` for 32-bit systems.

## Using the system
By default, DOS-XC starts with its graphical environment, the DOS-XC Desktop. Here, you get graphical tools, games, and configuration utilities, plus wallpapers and greater customizability.

For hardcore DOS command line fans, you can uninstall the desktop by running:
```bat
pkg.exe remove "DOS-XC Desktop"
```

Your 16-bit MS-DOS programs run in a kernel-level emulator and you can run them just like regular programs.

## Building
```sh
# Builds GCC and ported programs
toolchain/build-toolchain.sh x86_64
toolchain/build-toolchain.sh i686
ports/build-ports.sh x86_64

# Builds the ISOs to /build/iso/
scripts/build.sh

# Boots the ISO with QEMU, sub "64" for "32" to run the 32-bit system instead
scripts/boot64.sh

# Attach a virtual hard disk
scripts/boot64.sh -hd disk.img
```

### Building on Windows
The same commands exist as PowerShell scripts. The build runs inside WSL, because GCC and the ports need a POSIX shell, and booting uses the Windows build of [QEMU](https://www.qemu.org/download/#windows).

```powershell
# Once, then restart. Inside WSL (Ubuntu), install the build tools:
wsl --install
sudo apt install build-essential clang lld llvm nasm xorriso python3 curl xz-utils bison flex gawk zlib1g-dev perl

toolchain\build-toolchain.ps1 x86_64
toolchain\build-toolchain.ps1 i686
ports\build-ports.ps1 x86_64
scripts\build.ps1
scripts\boot64.ps1
scripts\boot64.ps1 -hd disk.img
```

If PowerShell refuses to run the scripts, use `Set-ExecutionPolicy -Scope Process Bypass` first. Building the Python port also needs `python3.14` inside WSL. The boot scripts read `ISO`, `QEMU`, `MEMORY`, `AUDIO` and `DISPLAYS` like their shell versions, and `$env:ACCEL = 'whpx'` uses the Windows Hypervisor Platform where it is enabled.

To build the system, you need the cross-compiler with our shell script:
```sh
.\toolchain\build-toolchain.sh
```

## Documentation
- [docs/PROGRAMS.md](docs/PROGRAMS.md): the 32/64-bit program format, system calls, drives, the console, shared libraries and the C library
- [docs/DESKTOP.md](docs/DESKTOP.md): the graphical desktop and multiple monitors
- [docs/PACKAGES.md](docs/PACKAGES.md): packages, PKG and the `.dsp` format
- [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md): using and building GCC on DOS-XC, and the ports
- [docs/USB.md](docs/USB.md): USB keyboards, mice and drives
- [docs/NETWORK.md](docs/NETWORK.md): Ethernet, IPCONFIG, PING and sockets
- [docs/DRIVERS.md](docs/DRIVERS.md): writing and packaging device drivers
