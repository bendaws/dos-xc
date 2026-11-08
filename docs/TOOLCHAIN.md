# The GCC toolchain

DOS-XC ships GNU binutils 2.45 and GCC 15.2 (C and C++) that run on
DOS-XC itself and build DOS-XC programs.  The same scripts produce a
cross toolchain for building those programs from macOS or Linux.

## Using GCC on DOS-XC

The toolchain lives in `\GCC`; `\GCC\BIN` is on the default `PATH` and
`TMPDIR` points at `\TEMP`.

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
the x32 edition.  (The i686 cross compiler is tested; a native `\GCC` for
the x32 edition is built the same way with `build-toolchain.sh i686` but
has not been tested yet.)  Code generation defaults to PIE with `-mno-red-zone`
(64-bit); the predefined macros `__dosxc__` and `__DOSXC__` identify the
system.  Linking is always static: `-shared` and `-no-pie` are rejected.
Thread-local storage is emulated (`--disable-tls`) and the thread model is
`single`.

## Building the toolchain

Requirements: a C/C++ compiler (clang), make, curl, Python 3, xz, and about
15 GB of disk space.  Then:

```sh
toolchain/build-toolchain.sh x86_64        # or i686
./build.sh                                  # images now include \GCC
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
| `package` | assembles `toolchain/dist/ARCH/GCC`, the `\GCC` tree |

`build.sh` adds `toolchain/dist/x86_64` to the x64 image and
`toolchain/dist/i686` to the x32 image when they exist.

### Porting notes

* `toolchain/gcc-config/dosxc.h` is the target description (specs,
  startfiles, link options).  `xm-dosxc.h` sets the host executable suffix.
* `toolchain/patch-sources.sh` adds `*-dosxc` to `config.sub`, BFD, ld,
  gas, `config.gcc`, `config.host`, libgcc and libstdc++.  Every edit is
  idempotent.
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

Put the program on a disk image with `DOSXC_EXTRA_TREES=dir ./build.sh`
(every file and folder in `dir` is copied to the root of the system drive).
