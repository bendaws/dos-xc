# Ben's DOS
Ben's DOS is an x86 and x64 BIOS/UEFI compliant DOS operating system
made for running the Oregon Trail.

It's a small, simple, and manageable operating system with the following:

- MS-DOS binary support
- Full shell
- Simple installer that looks like early installers for Windows
- Batch command processor
- Game support

It's also free and open-source.
## Build and boot

Requires NASM, Clang, lld, llvm-objcopy, xorriso, and Python 3. QEMU is
required for the boot scripts and integration tests. Use at least 128 MiB RAM.

```sh
./build.sh
./boot64.sh       # BIOS CD setup; boot32.sh is also available
```

The build produces BIOS/UEFI `BenDOS-x64.iso`, BIOS `BenDOS-x32.iso`,
and raw USB setup images `BenDOS-Setup-x64.img` and `BenDOS-Setup-x32.img`
in `build/`. No game or other third-party program is bundled by default.
The version source is `src/kernel/bootinfo.h`; Setup, VER, the shell, and
`SYSTEM/VERSION.TXT` are generated or compiled from that definition.

## Installation

Setup starts when booting the external media. ENTER continues, F1 shows
help, and F3 exits to the external-drive shell. Select a destination and
type ERASE to confirm. **Installation replaces the selected disk's partition
table and removes its existing partitions.** The setup source is excluded.

Setup creates a FAT32 system partition using the whole destination disk
from the standard 1 MiB alignment boundary to the last sector. Filesystem
metadata and a final partial cluster consume a small amount of space.
The source media's 64 MiB size does not limit the installed system.
Both BIOS and x64 UEFI use the same formatter and verify every written sector.
The present disk format supports 512-byte logical sectors and disks from
64 MiB to below 2 TiB; larger disks need GPT support and are not selected.

After installation, remove the setup media and boot the internal drive.
The default prompt is `C:\>` when installed and `X:\>` on external media.
System files live in `\SYSTEM`: boot/kernel `.BIN` files, shared `.DLL`
modules, command `.EXE` programs in `\SYSTEM\CMD`, and the batch processor
in `\SYSTEM\BATCH\BATCH.EXE`. These are native BenDOS modules; their `.EXE`
extension does not make them Windows PE applications.

## Commands and programs

Only CD/CHDIR and executable dispatch are built into the shell. Other commands
are loaded from the filesystem. The default PATH is the current system drive's
`\SYSTEM\CMD`. Commands include HELP, DIR, TYPE, ECHO, CLS, COLOR, VER, BEEP,
WRITE, COPY, MOVE, REN, DEL, MD, RD, EDIT, SET, PATH, SPEED, and EXIT.
Use `command /?` for its supported arguments; this is a subset of Windows/DOS
command syntax, not a complete CMD.EXE implementation.

```text
CD \SYSTEM
DIR /B
CD \
MD WORK
CD WORK
WRITE NOTE.TXT Hello
EDIT NOTE.TXT
COPY /Y NOTE.TXT COPY.TXT
REN COPY.TXT SAVED.TXT
DEL SAVED.TXT
```

EDIT supports text up to 32 KiB, arrow keys, Home/End, Delete/Backspace,
F2 or Ctrl+S to save, and ESC to exit. It saves DOS CR/LF line endings.
ECHO/TYPE and other native command output support `>` and `>>` redirection.

Batch scripts support positional arguments, `%VAR%`, `%ERRORLEVEL%`, `%CD%`,
ECHO ON/OFF, REM, labels, GOTO, CALL, IF EXIST, IF ERRORLEVEL, string comparison,
and EXIT /B. Nesting is bounded to four scripts. Native commands, scripts,
and BIOS DOS programs can be launched by filename without a special game command.

The BIOS DOS services load ordinary 16-bit MZ `.EXE` and `.COM` files from
FAT16/FAT32 and preserve the shell directory, environment, and batch execution
when a program exits. DOS compatibility is incomplete: DOS extenders, general
EXEC, long DOS filenames, and allocating new file clusters through INT 21h
are not implemented. Native command programs provide general file creation;
16-bit programs can read files and update existing allocated file chains.
Direct UEFI boot runs native commands but currently cannot execute 16-bit programs.

`SPEED 5` selects the default 5% CPU duty for 16-bit programs; `SPEED 100`
disables throttling. Values 1–100 are supported. This is a host-dependent
execution throttle, not a cycle-accurate 8088 emulator, and BIOS timekeeping
continues while the program is paused.

## Validation

All destructive integration tests use temporary disk-image files:

```sh
python3 tests/smoke.py
python3 tests/install.py --bits 32 --size-mib 5120
python3 tests/install.py --bits 64 --size-mib 512
python3 tests/install.py --uefi --size-mib 5120
```

UEFI tests need OVMF (`UEFI_CODE` and `UEFI_VARS` can override the paths).
Installation tests check that the partition and filesystem span the selected
disk, the source disk is not written, the installed disk boots alone, and
files can be allocated beyond the former 64 MiB boundary. BIOS tests also
exercise DOS execution, large MZ relocation headers, persistent file updates,
and batch continuation.
