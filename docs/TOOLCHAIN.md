# The GCC toolchain

DOS-XC ships GNU binutils 2.45 and GCC 15.2 (C and C++) that run on
DOS-XC itself and build DOS-XC programs.  The same scripts produce a
cross toolchain for building those programs from macOS or Linux.

## Using GCC on DOS-XC

The toolchain is the `GCC` package (see [PACKAGES.md](PACKAGES.md)),
installed in `\Programs\GCC`; its `BIN` folder is on `PATH` and `TMPDIR`
points at `\TEMP`.  Setup can install it, or `PKG INSTALL X:\Packages\GCC.dsp`
on the live system.

```
C:\> gcc hello.c -o hello
C:\> hello
Hello, world!
C:\> g++ -O2 prog.cpp -o prog
```

Programs are always written with the `.EXE` extension: `-o hello` writes
`HELLO.EXE` and the default output is `A.EXE`.  `gcc -c`, `-S`, `-E`, `as`,
`ld`, `ar`, `nm`, `objdump`, `readelf`, `strip` and the other binutils work
as on any Unix-like system.  GCC needs at least 512 MiB of RAM; give QEMU
`-m 1024` (the boot scripts do).  Under QEMU without hardware
virtualization (for example on an Apple silicon Mac) a hello-world C
program takes about a minute to build and a small C++ program using the
STL about eight minutes; with KVM or real hardware it is far faster.

File names are case-insensitive, so source suffixes are too: `.C` and `.c`
are C; `.CPP`, `.CC` and `.CXX` are C++; `.S` and `.s` are plain assembly
(use `.sx` or `-x assembler-with-cpp` for assembly that needs the
preprocessor).

The target name is `x86_64-dosxc` on the x64 edition and `i686-dosxc` on
the x86 edition; both editions have a native GCC package.  Code generation
defaults to PIE with `-mno-red-zone` (64-bit); the predefined macros
`__dosxc__` and `__DOSXC__` identify the system.  Paths may use drive
letters and `\` (GCC and binutils are built as for a DOS file system, like
DJGPP), and search paths such as `LIBRARY_PATH` use `;`.

Programs are linked dynamically by default, against `libc.so` in
`\SYSTEM\LIB` (libgcc and libstdc++ are still linked in statically);
`-static` makes a self-contained program.  `-shared` makes a shared library
(compile it with `-fPIC`); see [PROGRAMS.md](PROGRAMS.md#shared-libraries).
The compiler and binutils themselves are dynamic programs too.  `-no-pie`
is rejected.
Thread-local storage is emulated (`--disable-tls`) and the thread model is
`single`.

## Building the toolchain

Requirements: a C/C++ compiler (clang), make, curl, Python 3, xz, and about
15 GB of disk space.  Then:

```sh
toolchain/build-toolchain.sh x86_64        # or i686
scripts/build.sh                            # images now include \GCC
```

`build-toolchain.sh ARCH [STAGE...]` runs these stages (all of them when
none are named):

| Stage | Result |
|-------|--------|
| `fetch` | downloads and unpacks binutils, GCC, GMP, MPFR, MPC into `toolchain/work/src`, prunes unused front ends, and applies `patch-sources.sh` |
| `libc` | the C library, built with clang, in `toolchain/cross/ARCH-dosxc/sysroot` |
| `binutils` | cross binutils in `toolchain/cross/bin` |
| `gcc` | cross GCC, libgcc and libstdc++ |
| `libc-gcc` | rebuilds the C library with the cross GCC |
| `native` | Canadian cross: GMP/MPFR/MPC, binutils and GCC that *run on* DOS-XC (`native-binutils` and `native-gcc` run the last two parts alone) |
| `package` | assembles `toolchain/dist/ARCH/GCC`, the tree of the GCC package |

`scripts/build.sh` packs `toolchain/dist/ARCH/GCC` (with the current C
library) into the GCC package of each edition when it exists.  The native
compiler is configured with the prefix `/Programs/GCC`.

### Porting notes

* `toolchain/gcc-config/dosxc.h` is the target description (specs,
  startfiles, link options).  `xm-dosxc.h` sets the host executable suffix.
* `toolchain/patch-sources.sh` adds `*-dosxc` to `config.sub`, BFD, ld,
  gas, `config.gcc`, `config.host`, libgcc and libstdc++, and turns on the
  DOS file-system path handling of libiberty (`filenames.h`,
  `make-relative-prefix.c`) and ld.  Every edit is idempotent.
* GCC's driver runs `cc1`, `as` and `collect2`/`ld` through libiberty's
  `pex-unix`, which uses `posix_spawn`.  DOS-XC runs the child to
  completion before `posix_spawn` returns; `waitpid` then reports the saved
  status.  Pipes (`-pipe`) are in-memory buffers, so this works without
  concurrency.

## Cross compiling from macOS or Linux

```sh
export PATH=$PWD/toolchain/cross/bin:$PATH
x86_64-dosxc-gcc -O2 prog.c -o PROG.EXE
```

Put the program on a disk image with `DOSXC_EXTRA_TREES=dir scripts/build.sh`
(every file and folder in `dir` is copied to the root of the system drive).

## Ports

`ports/build-ports.sh ARCH [xz] [vim] [curl] [python]` cross-compiles the
ports into `build/ports/ARCH`, which `scripts/build.sh` packs.  Vim uses its
built-in `ansi` terminal description (there is no terminfo database) and
`system()` for shell commands, since DOS-XC has no `fork()`.

### Python

The `python3` package is CPython 3.14.  Building it needs a host `python3.14`.
Every extension module is linked into the interpreter, because DOS-XC has no
`dlopen`, and [`ports/python-dosxc.py`](../ports/python-dosxc.py) adapts the
standard library to drive letters, `;` search paths and `posix_spawn`.

* Not available: threads (`threading.Thread.start` raises `RuntimeError`),
  `ctypes`, `mmap`, `multiprocessing`, `ssl` and so HTTPS, `sqlite3`, `bz2`,
  `lzma`, IPv6, and C extension modules that are not built in.
* `subprocess` runs the child to completion inside `Popen`, so a child cannot
  talk back and forth with its parent.  `shell=True` splits the command into
  words and runs it directly; shell syntax such as pipes is not interpreted.
* `asyncio` runs on one thread, and `os.urandom` is only cryptographically
  strong on processors with RDRAND.
