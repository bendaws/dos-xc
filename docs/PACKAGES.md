# Packages

DOS-XC installs programs as packages: `.dsp` files managed by `PKG.EXE`,
the DOS-XC Package Manager (`\SYSTEM\PKG.EXE`).  Each package installs to
its own folder, `\Programs\<Name>` on the system drive.

## Using PKG

```
PKG LIST                         installed packages
PKG INFO file.dsp | name         package details
PKG FILES file.dsp | name        files in a package
PKG INSTALL file.dsp [/Y]        install
PKG UPDATE file.dsp [/Y] [/F]    replace an installed package with a newer version
PKG REMOVE name [/Y]             uninstall (deletes \Programs\<Name>)
PKG PACK folder out.dsp [/N:name] [/V:version] [/A:author] [/D:description]
         [/B:16|32|64] [/P:folders] [/S:program] [/E:NAME=value] [/R:A.DRV;B.DRV]
```

`/Y` answers every question with yes; `/F` lets `UPDATE` install an older
or equal version.  A package for the other edition (32-bit on x64 or the
reverse) is refused; 16-bit DOS packages install on both.

The shell builds `PATH` at start from the installed packages (the system
folder first, then each package's `Path` folders), and `PKG` updates
`PATH` and the package's environment variables at once when it installs or
removes something.

The installation media carry every package of their edition in
`\Packages`; Setup offers them, and `PKG INSTALL X:\Packages\Vim.dsp`
installs one into the live system (in memory until restart).

## Bundled packages

| Package | Folder | Contents |
|---------|--------|----------|
| `coreutils` | `\Programs\coreutils` | DIR, TYPE, MORE, COPY, MOVE, REN, DEL, MD, RD, EDIT, WRITE, CLS, COLOR, VER, BEEP, SPEED, EJECT, SHUTDOWN, RESTART (REBOOT), HELP |
| `batch` | `\Programs\batch` | `BATCH.EXE`, which runs `.BAT` files |
| `DOS-XC Desktop` | `\Programs\DOS-XC Desktop` | The graphical desktop and its programs ([DESKTOP.md](DESKTOP.md)) |
| `dosfox` | `\Programs\dosfox` | DOSfox web browser (built from Firefox source), 64-bit only ([firefox/README.md](../firefox/README.md)) |
| `dosoffice` | `\Programs\dosoffice` | DOS Office: DOS Writer (.docx, .odt, .rtf, .html, .txt, .md) and DOS Sheets (.xlsx, .ods, .csv, .tsv); `.gdoc`/`.gsheet` shortcuts open in the browser |
| `GCC` | `\Programs\GCC` | GCC 15.2 (C, C++) and binutils 2.45 ([TOOLCHAIN.md](TOOLCHAIN.md)) |
| `ethernet` | `\SYSTEM\DRIVERS` | Ethernet drivers: Intel, Broadcom, virtio ([NETWORK.md](NETWORK.md)) |
| `sound` | `\SYSTEM\DRIVERS` | Sound drivers: Intel High Definition Audio ([DRIVERS.md](DRIVERS.md#sound)) |
| `hyperv` | `\SYSTEM\DRIVERS` | Hyper-V network, SCSI disks, keyboard and mouse ([DRIVERS.md](DRIVERS.md#hyper-v)) |
| `curl` | `\Programs\curl` | curl 8.11 (`CURL`, HTTP and HTTPS) |
| `Vim` | `\Programs\Vim` | Vim 9.1 (`VIM`, `VI`, `VIEW`, `EX`, `VIMDIFF`) |
| `xz` | `\Programs\xz` | XZ Utils 5.8 (`XZ`, `UNXZ`, `XZCAT`) |
| `python3` | `\Programs\python3` | Python 3.14 (`PYTHON`, `PYTHON3`); see [TOOLCHAIN.md](TOOLCHAIN.md#python) |
| `Oregon` | `\Programs\Oregon` | The Oregon Trail (16-bit; from `OREGON.ZIP`) |

The live system comes with `coreutils`, `batch` and `DOS-XC Desktop` installed;
Setup always installs `coreutils` and selects `batch`, `ethernet`, `hyperv` and
`DOS-XC Desktop` by default.  A name with spaces needs quotes:
`PKG REMOVE "DOS-XC Desktop"`.

## The .dsp format

A package is a ZIP archive with a header in front, so the whole file is
also a valid (self-extracting style) ZIP:

| Offset | Size | Contents |
|-------:|-----:|----------|
| 0 | 4 | magic `"DSPK"` |
| 4 | 2 | format version, 1 |
| 6 | 2 | flags, 0 |
| 8 | 4 | metadata length *N* |
| 12 | *N* | metadata: `Key=value` lines (CR LF or LF) |
| 12 + *N* | | ZIP archive, stored or deflated entries; offsets count from the start of the `.dsp` file |

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
id=writer
name=DOS Writer
program=WRITER.EXE
icon=WRITER.PNG
category=Office
default=document
types=docx odt rtf doc htm html md gdoc
```

The live system installs `coreutils`, `batch`, the desktop, `dosfox` and
`dosoffice`; the others are in `\Packages`.

An installed package keeps its metadata in `\Programs\<Name>\PACKAGE.INF`.
`PKG PACK` reads the metadata from `PACKAGE.INF` in the folder being
packed when it exists (options override it).

The format is implemented in [`os/pkg/dsp.c`](../os/pkg/dsp.c) (with
its own inflate and deflate) and, for the build machine, in
[`tools/dsp.py`](../tools/dsp.py).

## Building the packages

Each package has a folder at the top of the repository with a `package.sh`, and `scripts/build.sh` runs them all, making `build/packages/<Name>-<bits>.dsp`.  `FOLDER/package.sh [32] [64]` builds one package by itself.

Programs in this repository are compiled by their `package.sh`.  GCC and the ported programs take much longer, so they have a separate `build.sh`, and `package.sh` packs its output if it exists: `toolchain/dist/ARCH/GCC` from `gcc/build.sh ARCH`, and `build/ports/ARCH` from `scripts/build-ports.sh ARCH`.  `oregon` uses `oregon/OREGON.ZIP`.

Missing inputs are skipped, so a plain checkout builds without the larger packages.

`scripts/build.sh --select` chooses packages and editions on a checklist, and `--last` repeats that choice.  `--only a,b,c` and `--editions 64` do the same without one.  The core system and `coreutils` are always built.  Only the chosen packages go on the media, and a package left out of the live system's defaults (`batch`, the desktop) is simply absent from it.
