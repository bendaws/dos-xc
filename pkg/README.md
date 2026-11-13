# Package Manager

The `pkg` package: `PKG.EXE`, the command-line package manager, and
`PKGMGR.EXE`, its window on the desktop (Start, System, Administrators).
The sources are [`os/pkg`](../os/pkg) and [`desktop/pkgmgr.c`](../desktop/pkgmgr.c).

It is an ordinary package with `Required=1`: it is always installed, `PKG
REMOVE pkg` is refused, and `PKG UPDATE pkg-64.dsp` replaces it, PKG
included.  It also stages updates of the `system` package (the kernel,
runtime, C library and command shell), which are put in place at the next
start.  See [docs/PACKAGES.md](../docs/PACKAGES.md).

Online commands use `https://dos-packages.bendaws.net`: `PKG SEARCH`, `PKG SHOW`,
`PKG FETCH`, and `PKG INSTALL` / `UPDATE` with a package name. The standard image
includes curl; minimal builds can install it from local media. Downloads are checked against the catalog's size,
SHA-256 and metadata before the existing installer runs. PKGMGR's Online tab
browses the same catalog. The self-hosted server and Ubuntu/Apache deployment
package are in [`catalog`](../catalog/README.md).
