# Packages

DOS-XC installs programs as packages: `.dsp` files managed by `PKG.EXE`,
the DOS-XC Package Manager.  Each package installs to its own folder,
`\Programs\<Name>` on the system drive.

The package manager is itself a package, `pkg` (`\Programs\pkg`: `PKG.EXE`
and its window, `PKGMGR.EXE`, on the Start menu under System >
Administrators).  It is marked `Required=1`, so it is always installed and
cannot be removed, and `PKG UPDATE pkg.dsp` replaces it, PKG included.

## Using PKG

```
PKG LIST                         installed packages
PKG INFO file.dsp | name         package details
PKG FILES file.dsp | name        files in a package
PKG INSTALL file.dsp [/Y] [/F]   install
PKG UPDATE file.dsp [/Y] [/F]    replace an installed package with a newer version
PKG REMOVE name [/Y] [/F]        uninstall (deletes \Programs\<Name>)
PKG PACK folder out.dsp [/N:name] [/V:version] [/A:author] [/D:description]
         [/B:16|32|64] [/P:folders] [/S:program] [/E:NAME=value] [/R:A.DRV;B.DRV]
         [/O:roles] [/U:requirements] [/Q:1] [/Z:1]
```

`/Y` answers every question with yes; `/F` lets `UPDATE` install an older
or equal version, and lets any command go ahead when it breaks a
requirement (see [Requirements](#requirements)).  `UPDATE` unpacks the new version into `<Name>.new` beside
the old folder and swaps the folders only once every file is out, so a
failed update leaves the installed version as it was.  `REMOVE` refuses
packages with `Required=1`.  A package for the other edition (32-bit on x64 or the
reverse) is refused; 16-bit DOS packages install on both.

The shell builds `PATH` at start from the installed packages (the system
folder first, then each package's `Path` folders), and `PKG` updates
`PATH` and the package's environment variables at once when it installs or
removes something.

The installation media carry every package of their edition in
`\Packages`; Setup offers them, and `PKG INSTALL X:\Packages\Vim.dsp`
installs one into the live system (in memory until restart).

## Online catalog

The default catalog is `https://dos-packages.bendaws.net`. The standard image
includes curl; minimal builds without it must install curl from local media first.
Use:

```
PKG SEARCH [text]                 find compatible packages
PKG SHOW name                     online package details
PKG FETCH name [out.dsp]           download without installing
PKG INSTALL name [/Y] [/F]         download, verify and install
PKG UPDATE name [/Y] [/F]          download, verify and update
```

Quote names containing spaces. Paths and names ending in `.dsp` retain their
existing local-file behavior. Online commands verify HTTPS certificates using
curl's `CACERT.PEM`, then verify the size, SHA-256 and package metadata. Existing
architecture, dependency and update checks still apply; dependencies are not
downloaded automatically. Deleted packages and deleted publishers' packages
cannot be fetched; cached catalog entries are resolved again before installation.

PKGMGR opens its **Online** tab by default and loads the catalog automatically.
Its Local tab shows packages on local drives. The Refresh button reloads the
online catalog. Retrieval runs in a background
PKG process so the desktop remains responsive. Lists show up to 128 package names.

`SET PKG_REPOSITORY=https://another-host` selects another catalog. An explicit
`SET PKG_ALLOW_HTTP=1` permits an HTTP repository for local testing; do not set it
for normal use. The server's API and Ubuntu/Apache installation are documented
in [`catalog/README.md`](../catalog/README.md).

## Bundled packages

| Package | Folder | Contents |
|---------|--------|----------|
| `system` | `\SYSTEM` | The boot core: kernel, runtime, C library and `COMMAND.EXE`; required ([below](#the-system-package)) |
| `pkg` | `\Programs\pkg` | `PKG` and the Package Manager window; required |
| `coreutils` | `\Programs\coreutils` | DIR, TYPE, MORE, COPY, MOVE, REN, DEL, MD, RD, EDIT, WRITE, CLS, COLOR, VER, BEEP, SPEED, EJECT, MOUNT, SHUTDOWN, RESTART (REBOOT), HELP |
| `batch` | `\Programs\batch` | `BATCH.EXE`, which runs `.BAT` files |
| `DOS-XC Desktop` | `\Programs\DOS-XC Desktop` | The graphical desktop and its programs ([DESKTOP.md](DESKTOP.md)); provides `desktop` |
| `dosfox` | `\Programs\dosfox` | DOSfox web browser (built from Firefox source), 64-bit only ([firefox/README.md](../firefox/README.md)) |
| `openwork` | `\Programs\openwork` | OpenWork office suite ([OPENWORK.md](OPENWORK.md)): OpenPapers (.docx, .odt, .rtf, .html, .txt, .md), OpenNumbers (.xlsx, .ods, .csv, .tsv), OpenShow (.pptx, .odp) and OpenMail (IMAP, POP3, SMTP); `.gdoc`/`.gsheet`/`.gslides` shortcuts open in the browser |
| `GCC` | `\Programs\GCC` | GCC 15.2 (C, C++) and binutils 2.45 ([TOOLCHAIN.md](TOOLCHAIN.md)) |
| `ethernet` | `\SYSTEM\DRIVERS` | Ethernet drivers: Intel, Broadcom, virtio ([NETWORK.md](NETWORK.md)) |
| `sound` | `\SYSTEM\DRIVERS` | Sound drivers: Intel High Definition Audio ([DRIVERS.md](DRIVERS.md#sound)) |
| `hyperv` | `\SYSTEM\DRIVERS` | Hyper-V network, SCSI disks, keyboard and mouse ([DRIVERS.md](DRIVERS.md#hyper-v)) |
| `curl` | `\Programs\curl` | curl 8.11 (`CURL`, HTTP and HTTPS) |
| `Vim` | `\Programs\Vim` | Vim 9.1 (`VIM`, `VI`, `VIEW`, `EX`, `VIMDIFF`) |
| `xz` | `\Programs\xz` | XZ Utils 5.8 (`XZ`, `UNXZ`, `XZCAT`) |
| `python3` | `\Programs\python3` | Python 3.14 (`PYTHON`, `PYTHON3`); see [TOOLCHAIN.md](TOOLCHAIN.md#python) |
| `Oregon` | `\Programs\Oregon` | The Oregon Trail (16-bit; from `OREGON.ZIP`) |

The live system comes with `system`, `DOS-XC Desktop`, `batch`, `coreutils`,
`dosfox`, `openwork`, `ethernet`, `curl`, `nano`, `pkg`, `VLC`, `Vim`, `xz` and `zip`
preinstalled when their edition's packages are included in the build. Setup
selects the same built-in packages by default; `system` is copied directly,
and `pkg` and `coreutils` are always installed. A name with spaces needs quotes:
`PKG REMOVE "DOS-XC Desktop"`.

## The .dsp format

A package is a header, its metadata, a list of its files and one `.xz`
stream holding every file's data, one after another (a "solid" archive,
much smaller than compressing each file alone):

| Offset | Size | Contents |
|-------:|-----:|----------|
| 0 | 4 | magic `"DSPK"` |
| 4 | 2 | format version, 2 |
| 6 | 2 | flags, 0 |
| 8 | 4 | metadata length *N* |
| 12 | *N* | metadata: `Key=value` lines (CR LF or LF) |
| 12 + *N* | 4 | number of files *C* |
| | | *C* times: u16 name length, u32 size, u32 CRC-32, the name (UTF-8, `/` separators) |
| | | an `.xz` stream: the files' data in list order |

Numbers are little-endian.  The stream uses LZMA2 (preset 6e, an 8 MiB
dictionary) with CRC-32 checks, after the x86 filter except in 16-bit
packages; several blocks are fine.  Reading a file unpacks the stream up to
it, so files read fastest in list order, as PKG and Setup do.  The decoder
is XZ Embedded ([`os/pkg/xz`](../os/pkg/xz), 0BSD), built by
[`os/pkg/unxz.c`](../os/pkg/unxz.c).  Compared with version 1 the packages
are 30 to 60% smaller (DOSfox 150 MB to 106 MB, GCC 52 MB to 31 MB).

Version 1, a ZIP archive with stored or deflated entries after the
metadata (offsets counting from the start of the file), is still read.
`PKG PACK` writes version 2 with `XZ.EXE` from the `xz` package (the same
settings as the build machine; the files' data goes through a temporary
`out.dsp.tmp` beside the package).  Without the `xz` package, when XZ fails
(it needs about 100 MB of memory), or with `/Z:1`, it writes version 1.
`/Q:1` sets `Required=1`.

| Key | Meaning |
|-----|---------|
| `Name` | required; letters, digits, inner spaces, `-`, `_`, `.`; the folder name |
| `Version` | required; numeric parts compare as numbers (1.10 > 1.9) |
| `Author`, `Description` | shown by `PKG` and Setup |
| `Arch` | required: `16` (DOS), `32` or `64` |
| `Path` | `;`-separated folders inside the package for `PATH` (`.` is the package folder) |
| `StartIn` | `program`: 16-bit programs of the package start in their own folder |
| `Env` | `NAME=value`, repeatable; `%PACKAGE%` is the package folder as `C:/Programs/<Name>` |
| `Driver` | `;`-separated `.DRV` files that go to `\\SYSTEM\\DRIVERS` and start at once ([DRIVERS.md](DRIVERS.md)) |
| `Required` | `1`: part of the system; always installed, updated but never removed |
| `Provides` | `;`-separated roles the package fills, such as `desktop` |
| `Requires` | `;`-separated packages or roles it needs: `name` or `name>=version` |
| `Firewall` | `;`-separated programs of the package the firewall lets send anywhere ([NETWORK.md](NETWORK.md#firewall)) |

### Requirements

`Requires=` names what a package needs, by package name or by a role that
another package `Provides=`: `Requires=desktop` (any desktop),
`Requires=python3>=3.14` (that package, at least that version), or
`Requires=system>=0.3` (at least that version of the boot core).  A version
can only be asked of a package name; any package that provides a role
meets it.

`PKG INSTALL` and `UPDATE` refuse a package whose requirements are not
installed.  `REMOVE`, and an `UPDATE` to an older version, refuse when
another installed package would lose something it needs, unless a second
package also provides it.  PKG names the packages involved, and `/F` goes
ahead anyway.  Setup selects what the chosen packages need.

DOSfox and DOS Office have `Requires=desktop`.

### Choosing the desktop

The desktop is a role: the command shell starts the `DESKTOP.EXE` of the
package that provides `desktop`, so another desktop can replace DOS-XC
Desktop.  When several are installed, it starts the one that the `DESKTOP`
environment variable names (`SET DESKTOP=Other Desktop` in
`AUTOEXEC.BAT`), else DOS-XC Desktop, else the first other one.  A
desktop is also started with `/LOGON` to show its login screen when user
accounts are on; it returns once someone has logged in.

### Start menu entries

A package puts programs on the Start menu with a `MENU.CFG` in its folder:
one entry of `key=value` lines per program, entries separated by a blank
line.  Removing the package removes its entries; nothing else is registered.

| Key | Meaning |
|-----|---------|
| `id` | required; a short name, used by `DESKTOP.CFG` for default programs |
| `name` | required; shown on the Start menu |
| `program` | required; the executable, relative to the package folder |
| `icon` | a 16 x 16 PNG, relative to the package folder |
| `category` | the Start menu folder: `Internet`, `Office`, ...; `System/Administrators` for programs that need an administrator |
| `default` | roles it can be the default for: `browser`, `editor`, `media`, `document`, `spreadsheet`, `presentation` |
| `types` | file extensions it opens, separated by spaces |
| `terminal` | `1` runs it in a terminal window |
| `url` | `1` passes files as `file://` URLs |

```
id=openpapers
name=OpenPapers
program=PAPERS.EXE
icon=PAPERS.PNG
category=Office
default=document
types=docx odt rtf doc htm html md gdoc
```

The live system preinstalls the built-in packages listed above. Other packages
included in the build remain available in `\Packages` for optional installation.

An installed package keeps its metadata in `\Programs\<Name>\PACKAGE.INF`.
`PKG PACK` reads the metadata from `PACKAGE.INF` in the folder being
packed when it exists (options override it).

The format is implemented in [`os/pkg/dsp.c`](../os/pkg/dsp.c) (with
its own inflate and deflate, and XZ Embedded) and, for the build machine, in
[`tools/dsp.py`](../tools/dsp.py), which compresses with the `xz` program when
it is installed (all cores) and Python's `lzma` otherwise.

## The system package

The kernel modules, the runtime (`ELFAPI.DLL`), `libc.so` and
`COMMAND.EXE` cannot be replaced while DOS-XC runs, so they ship together
as the `system` package (`system-64.dsp`, `Required=1`), and the installed
version is in `\SYSTEM\PACKAGE.INF`.  It holds every file of `\SYSTEM`
and `\SYSTEM\LIB` that the build makes, except `LOADER.BIN`, `SETUP.CFG`
and `DOSDATA.BIN`: the boot sector finds those by their place on the disk,
so only Setup writes them.  The settings in `\SYSTEM` (`USERS.DB`,
`NET.CFG`, `FIREWALL.CFG`) and the `DRIVERS` and `FONTS` folders are never
part of it.

`PKG UPDATE system-64.dsp` checks the package: it has to bring all the boot
files (`KERNEL.BIN`, `CORE.DLL`, `SHELL.DLL`, `DOSAPI.DLL`, `ELFAPI.DLL`,
`COMMAND.EXE`, `LIB/libc.so`), each a module of the right edition and small
enough for its loader.  PKG then unpacks it into `\SYSTEM.NEW` and nothing
else changes until DOS-XC restarts.  The live system cannot be updated.

At the next start, `SHELL.DLL` does the work before the runtime loads:

1. It moves each file of `\SYSTEM.NEW` into `\SYSTEM`, one at a time, and
   moves the file it replaces into `\SYSTEM.OLD`.  Files that are new are
   listed in `\SYSTEM.OLD\ADDED.LST`.  Each step can be done again, so
   a power cut part-way through is finished at the following start.  Then
   it restarts, so the new kernel loads (this step is the old version's
   `SHELL.DLL`; the rest is the new one's).
2. `\SYSTEM.OLD\TRIAL` counts the starts of the new version.
   `COMMAND.EXE` deletes it as soon as it runs, which confirms the update.
3. If the new runtime does not load, if `COMMAND.EXE` fails three times, or
   if three starts go by without the confirmation (a hang or a crash, then
   a reset), the new `SHELL.DLL` puts the files in `\SYSTEM.OLD` back, deletes the
   added ones and restarts.  The command shell says why under the welcome
   text (`\SYSTEM\UPDATE.MSG`).

After a confirmed update, `\SYSTEM.OLD` keeps the previous version's files
until the next system update.  Only one update is staged at a time, and PKG
refuses a new one until the last one has been confirmed.

What the old version cannot protect against is a new `KERNEL.BIN`,
`CORE.DLL` or `SHELL.DLL` that loads but fails before `SHELL.DLL` counts
the start; PKG's checks keep out damaged or wrong-edition modules, but not
a working module with a bug in it.  Setup can repair such a system.

The build makes `build/packages/system-<bits>.dsp` with the media
(`tools/make_disk.py`), and the media carry it as `\Packages\system.dsp`,
so a newer installation medium can update an installed system:
`PKG UPDATE D:\Packages\system.dsp`.  Setup copies `\SYSTEM` instead
and does not list it.

## Building the packages

Each package has a folder at the top of the repository with a `package.sh`, and `scripts/build.sh` runs them all, making `build/packages/<Name>-<bits>.dsp`.  `FOLDER/package.sh [32] [64]` builds one package by itself.

Programs in this repository are compiled by their `package.sh`.  GCC and the ported programs take much longer, so they have a separate `build.sh`, and `package.sh` packs its output if it exists: `toolchain/dist/ARCH/GCC` from `gcc/build.sh ARCH`, and `build/ports/ARCH` from `scripts/build-ports.sh ARCH`.  `oregon` uses `oregon/OREGON.ZIP`.

Missing inputs are skipped, so a plain checkout builds without the larger packages.

`scripts/build.sh --select` chooses packages and editions on a checklist, and `--last` repeats that choice.  `--only a,b,c` and `--editions 64` do the same without one.  The core system, `pkg` and `coreutils` are always built.  Only the chosen packages go on the media, and a package left out of the live system's defaults (`batch`, the desktop) is simply absent from it.
