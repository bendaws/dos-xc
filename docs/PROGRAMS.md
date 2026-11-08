# 32-bit and 64-bit programs

Besides 16-bit MS-DOS programs, DOS-XC runs native protected-mode
(x86, 32-bit) and long-mode (x64, 64-bit) programs.  They are ordinary
`.EXE` files: the shell looks at the first bytes of the file, and an ELF
header (`7F 'E' 'L' 'F'`) selects the native runtime instead of the DOS
emulation.  The x32 edition runs 32-bit programs and the x64 edition runs
64-bit programs.

## Executable format

A DOS-XC executable is a static, position-independent ELF image:

| Field | Requirement |
|-------|-------------|
| Class / machine | `ELFCLASS32` + `EM_386`, or `ELFCLASS64` + `EM_X86_64` |
| Type | `ET_DYN` (PIE); no `PT_INTERP`, no `DT_NEEDED` |
| ABI note | a `PT_NOTE` containing name `"DOS-XC"`, type 1, descriptor `{u32 abi_version = 1, u32 flags = 0}` (section `.note.dosxc`) |
| Relocations | only `R_*_RELATIVE` and `R_*_IRELATIVE` (`DT_RELA`, `DT_REL` or `DT_RELR`) |
| TLS | `PT_TLS` is rejected (GCC emulates TLS) |
| Stack | `PT_GNU_STACK` `p_memsz`, if non-zero, is the stack size (default 16 MiB, or 4 MiB with under 512 MiB of RAM) |

The loader (`src/runtime/loader.c`) copies the image to any free,
page-aligned block, zeroes `.bss`, applies relocations and calls the entry
point.  Because images never assume a fixed address, nested programs
(`gcc` → `cc1` → `as` → `collect2` → `ld`) share one address space without
paging.  The ABI note keeps Linux binaries from being started by mistake.

Programs run in ring 0 with SSE enabled.  CPU exceptions inside a program
are caught: the runtime prints the exception name and image offset, and the
program terminates with the matching signal status (SIGSEGV, SIGILL,
SIGFPE ...).

## Process start

The entry point receives the System V initial stack:

```
argc, argv[0..argc-1], NULL, envp[...], NULL, auxv pairs..., AT_NULL
```

The auxiliary vector carries `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`,
`AT_ENTRY`, `AT_RANDOM`, `AT_EXECFN` and two DOS-XC entries:

| Tag | Value |
|-----|-------|
| `DOSXC_AT_API` (`0x42440001`) | pointer to `struct dosxc_api` |
| `0x42440002` | load address of the image |

## System calls

There are no interrupts or `syscall` instructions.  The API is a table of
function pointers (`struct dosxc_api` in
[`src/libc/include/dosxc/abi.h`](../src/libc/include/dosxc/abi.h)) using
the normal C calling convention of the architecture.  Calls return a
negative errno value on failure.  The table covers processes (`exit`,
`spawn`, `system`), file descriptors (`open` ... `fstat`, `pipe`, `dup2`),
paths (`stat`, `rename`, `mkdir`, `listdir` ...), memory (`alloc`,
`release`), time and the console.

`spawn` works like DOS EXEC: it runs the child to completion and returns its
POSIX wait status.  The child inherits the parent's file descriptors
(except close-on-exec ones), so pipes and redirections work as on Unix.
Pipes are buffered in memory, which is what lets `gcc -pipe` work without
concurrency.

Paths accept `/` or `\`, an optional drive letter and `..`; programs see the
current directory as `/DIR/SUB`.  Long file names are supported.

## The runtime

`\SYSTEM\ELFAPI.DLL` is loaded by the shell at boot at a fixed address
(`0x4400000`).  It owns all memory above `0x5000000` (sized from the E820
map on BIOS, or a block reserved by the UEFI loader), a sector cache for
the system volume (with a RAM overlay when booted from read-only media), the
open-file table and the console line editor.  On 64-bit BIOS boots it maps
the first 4 GiB.

## The C library

`src/libc` is a C library written for DOS-XC: stdio (with exact
floating-point formatting), stdlib, string, math, time, locale stubs,
wide characters, `setjmp`, `getopt`, directories, `posix_spawn`, `waitpid`,
`pipe`, `select` on pipes, `fork`-less process creation and the POSIX
pieces GCC, binutils and libstdc++ need.  Build it with

```sh
src/libc/build.sh x86_64 OUT [CC]     # or i386
```

which produces `OUT/lib/{crt0,crti,crtn}.o`, `libc.a`, `libm.a` (empty, the
math functions are in `libc.a`) and `OUT/include`.

`fork` and `exec*` without a following exit are not available; use
`posix_spawn`, `system` or `popen`.  Signals are limited to `raise`,
`signal` and `kill` on the calling process; Ctrl+C delivers SIGINT.

## Testing

`tests/qemu_session.py` boots an image in QEMU, types commands and prints
the console transcript (mirrored to port 0xE9):

```sh
mkdir -p build/tests/TESTS
toolchain/cross/bin/x86_64-dosxc-gcc -O2 tests/programs/libctest.c -o build/tests/TESTS/LIBCTEST.EXE
DOSXC_EXTRA_TREES=build/tests ./build.sh
tests/qemu_session.py --iso build/DOS-XC-x64.iso -c "CD \\TESTS" -c "LIBCTEST.EXE"
```
