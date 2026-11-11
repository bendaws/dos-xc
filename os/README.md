# DOS-XC core system

Everything that boots and runs DOS-XC, and the parts of `\SYSTEM` that every installation has.

| Folder | Contents |
|--------|----------|
| [`boot`](boot) | BIOS and UEFI boot loaders |
| [`kernel`](kernel) | The kernel |
| [`runtime`](runtime) | `ELFAPI.DLL`: loader, memory, files, networking, scheduling and system calls for 32/64-bit programs |
| [`libc`](libc) | The C library |
| [`dos`](dos) | The original 16-bit DOS runtime |
| [`drivers`](drivers) | Built-in file system, keyboard, storage and speaker drivers |
| [`shell`](shell) | `COMMAND.EXE` |
| [`programs`](programs) | The kernel's shell driver and the program API header |
| [`pkg`](pkg) | `PKG.EXE` and the `.dsp` package format |
| [`setup`](setup) | The text-mode Setup program |

## Building

`scripts/build.sh` runs `build.sh` here to compile the system, builds every package, then runs `image.sh` to make the ISOs. See [docs/PROGRAMS.md](../docs/PROGRAMS.md) for the program interface.

## Licence

BSD 3-Clause; see [LICENSE.txt](LICENSE.txt).
