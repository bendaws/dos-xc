# DOS-XC Desktop

The graphical environment: `DESKTOP.EXE` and, in `Apps`, one program per application (file manager, task manager, terminal, editor, paint, calculator, games, package manager and graphical Setup). [`vdm`](vdm) is the 16-bit DOS emulator behind `DOSVM.EXE`. See [docs/DESKTOP.md](../docs/DESKTOP.md).

Build it alone with `desktop/package.sh [32] [64]`.

## Licence

BSD 3-Clause; see [LICENSE.txt](LICENSE.txt). [`archive`](archive) (File Explorer's archive formats) uses XZ Embedded from `os/pkg/xz` (0BSD) to read `.xz`. The bitmap fonts in `fonts.h` come from SerenityOS; see [LICENSE.SerenityOS](LICENSE.SerenityOS).
