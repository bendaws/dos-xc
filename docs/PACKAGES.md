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
| `GCC` | `\Programs\GCC` | GCC 15.2 (C, C++) and binutils 2.45 ([TOOLCHAIN.md](TOOLCHAIN.md)) |
| `ethernet` | `\SYSTEM\DRIVERS` | Ethernet drivers: Intel, Broadcom, virtio ([NETWORK.md](NETWORK.md)) |
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

An installed package keeps its metadata in `\Programs\<Name>\PACKAGE.INF`.
`PKG PACK` reads the metadata from `PACKAGE.INF` in the folder being
packed when it exists (options override it).

The format is implemented in [`src/pkg/dsp.c`](../src/pkg/dsp.c) (with
its own inflate and deflate) and, for the build machine, in
[`tools/dsp.py`](../tools/dsp.py).

## Building the packages

`scripts/build.sh` makes `build/packages/<Name>-<bits>.dsp`:

* `coreutils` and `batch` from `src/coreutils` and `src/batch` (every build);
* `DOS-XC Desktop`, as `desktop-<bits>.dsp`, from `src/desktop` and `src/vdm` (every build);
* `GCC` from `toolchain/dist/ARCH/GCC` (`toolchain/build-toolchain.sh ARCH`);
* `ethernet` from `src/netdrivers` and `hyperv` from `src/hyperv` (every build);
* `Vim`, `xz`, `curl` and `python3` from `build/ports/ARCH` (`ports/build-ports.sh ARCH`);
* `Oregon` from `OREGON.ZIP` in the repository root.

Missing inputs are skipped, so a plain checkout builds without the larger
packages.
