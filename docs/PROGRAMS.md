# 32-bit and 64-bit programs

Besides 16-bit MS-DOS programs, DOS-XC runs native protected-mode
(x86, 32-bit) and long-mode (x64, 64-bit) programs.  They are ordinary
`.EXE` files: the shell looks at the first bytes of the file, and an ELF
header (`7F 'E' 'L' 'F'`) selects the native runtime instead of the DOS
emulation.  The x86 edition runs 32-bit programs and the x64 edition runs
64-bit programs.

Everything the user runs is such a program, the command shell included:
the kernel's init module loads the runtime and keeps
`\SYSTEM\COMMAND.EXE` running.

## Executable format

A DOS-XC executable is a position-independent ELF image, dynamically
linked against `libc.so` unless it was built with `gcc -static`:

| Field | Requirement |
|-------|-------------|
| Class / machine | `ELFCLASS32` + `EM_386`, or `ELFCLASS64` + `EM_X86_64` |
| Type | `ET_DYN` (PIE); static, or dynamic with `PT_INTERP` `/SYSTEM/ELFAPI.DLL` (see [Shared libraries](#shared-libraries)) |
| ABI note | a `PT_NOTE` containing name `"DOS-XC"`, type 1, descriptor `{u32 abi_version = 2, u32 flags = 0}` (section `.note.dosxc`); see [ABI versions](#abi-versions) |
| Relocations | static programs: `R_*_RELATIVE` and `R_*_IRELATIVE` (`DT_RELA`, `DT_REL` or `DT_RELR`) |
| TLS | `PT_TLS` is rejected (GCC emulates TLS) |
| Stack | `PT_GNU_STACK` `p_memsz`, if non-zero, is the stack size (default 16 MiB, or 4 MiB with under 512 MiB of RAM) |

The loader ([`src/runtime/loader.c`](../src/runtime/loader.c)) copies the image to any free,
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
`AT_ENTRY`, `AT_RANDOM`, `AT_EXECFN` and DOS-XC entries:

| Tag | Value |
|-----|-------|
| `DOSXC_AT_BASE` (`0x42440002`) | load address of the image |
| `DOSXC_AT_API` (`0x42440001`) | pointer to `struct dosxc_api`; ABI 1 programs only |
| `DOSXC_AT_INITS` (`0x42440003`) | dynamic programs: 0-terminated list of shared library constructors |
| `DOSXC_AT_FINIS` (`0x42440004`) | dynamic programs: 0-terminated list of shared library destructors |

`crt0` passes `__libc_start` a table of the program's own constructor and
destructor arrays, because a shared `libc.so` cannot see them.

## System calls

Programs call the system with interrupt `0x80`, much like 32-bit Linux.
The call number (`DOSXC_SYS_*` in
[`src/libc/include/dosxc/abi.h`](../src/libc/include/dosxc/abi.h)) goes
in `eax`/`rax`:

| | Number | Arguments 1-6 | Result |
|-|--------|---------------|--------|
| x86-64 | `rax` | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` | `rax` |
| i386 | `eax` | `ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp` | `eax` (`edx:eax` for 64-bit results) |

On i386, a 64-bit argument (the offset of `seek`, the length of
`ftruncate`, the time of `sleep`) takes two consecutive registers, low half
first, and `seek`, `time` and `nanoseconds` return 64 bits in `edx:eax`.
Every other register is preserved, including the flags and the whole x87
and SSE state, so a call is safe anywhere an inline `int $0x80` can go.
Calls that can fail return a negative errno value; an unknown number
returns `-ENOSYS`.

| Number | Call | Number | Call | Number | Call |
|-------:|------|-------:|------|-------:|------|
| 1 | `exit(status)` | 16 | `dup2(fd, target)` | 34 | `rmdir(path)` |
| 2 | `spawn(path, argv, envp)` | 17 | `fdflags(fd, set, flags)` | 35 | `chdir(path)` |
| 3 | `getpid()` | 18 | `status_flags(fd)` | 36 | `getcwd(buffer, size)` |
| 4 | `getppid()` | 19 | `pipe(fds)` | 37 | `listdir(path, entries, capacity)` |
| 5 | `system(command)` | 20 | `isatty(fd)` | 38 | `resolve(path, buffer, size)` |
| 6 | `rundos(path, arguments)` | 21 | `ftruncate(fd, length64)` | 40 | `alloc(size)` |
| 7 | `reboot()` | 22 | `fsync(fd)` | 41 | `release(block)` |
| 10 | `open(path, flags, mode)` | 23 | `fstat(fd, stat)` | 50 | `time()` → 64-bit |
| 11 | `close(fd)` | 30 | `stat(path, stat)` | 51 | `nanoseconds()` → 64-bit |
| 12 | `read(fd, buffer, size)` | 31 | `unlink(path)` | 52 | `sleep(nanoseconds64)` |
| 13 | `write(fd, buffer, size)` | 32 | `rename(from, to)` | 53 | `console(op, a, b)` |
| 14 | `seek(fd, offset64, whence)` → 64-bit | 33 | `mkdir(path)` | 54 | `sysinfo(info)` |
| 15 | `dup(fd, minimum)` | | | 55 | `environ(op, name, buffer, size)` |
| | | | | 56 | `shell(op, a, b)` |
| | | | | 57 | `volumes(entries, capacity)` |
| | | | | 58 | `disk(op, index, lba, buffer, count)` |
| | | | | 59 | `fullpath(path, buffer, size)` |
| | | | | 61 | `drive(op, letter, buffer, size)` |
| | | | | 62 | `power(op)` |

* `system` runs `COMMAND.EXE /C command`.
* `rundos` runs a 16-bit program from the system drive and returns its exit
  code (BIOS boot only; `-ENOSYS` under UEFI).  The 16-bit program replaces
  the protected-mode system while it runs; afterwards the boot loader
  restarts the kernel and the runtime resumes the caller, so a batch file
  or any other program simply continues.
* `environ` reads and changes the system environment (the DOS "master
  environment").  The shell passes it to every program it starts, so `SET`
  in a batch file stays in effect.  A `spawn` with a null `envp` uses it.
* `shell(DOSXC_SHELL_CHDIR, path)` changes the command shell's folder (how
  `CD` works in batch files); `DOSXC_SHELL_SPEED` reads or sets the 16-bit
  CPU duty.  While a 16-bit program uses the PC speaker the duty limit is
  lifted (music in old games is timed with CPU loops calibrated at full
  speed), and it applies again 200 ms after the speaker goes quiet.
* `power(DOSXC_POWER_OFF)` writes back open files, stops the USB
  controllers and switches the computer off (ACPI S5, or the UEFI
  firmware); when that is impossible it says it is safe to switch off and
  halts.  The power button (the ACPI fixed power button, polled) runs
  `\Programs\coreutils\SHUTDOWN.EXE /B` as a child of the running program,
  or switches off directly without coreutils.
* `volumes` lists the drive letters; `disk` reads and writes whole disks
  (used by Setup; the boot disk is read-only); `fullpath` gives the DOS
  form of a path, `C:\DIR\FILE`.

[`<dosxc/syscall.h>`](../src/libc/include/dosxc/syscall.h) defines a
typed inline wrapper for each call (`dosxc_sys_open`, `dosxc_sys_write` ...)
and generic `__dosxc_syscall0` ... `__dosxc_syscall3` helpers.  The C library
uses nothing else to reach the system.

```c
#include <dosxc/syscall.h>
int main(void) {
    dosxc_sys_write(1, "hi\n", 3);
    dosxc_sys_exit(DOSXC_EXITED(0));
}
```

Because programs run in ring 0, the CPU pushes the interrupt frame on the
program's own stack, which is why programs must not use a red zone.  The
gate is a trap gate, so the interrupt flag is left as the caller had it.

### ABI versions

The `abi_version` in the ABI note selects the system call interface:

| Version | Interface |
|---------|-----------|
| 1 | calls through the `struct dosxc_api` function table passed in `DOSXC_AT_API` (programs built before interrupt system calls) |
| 2 | `int $0x80` system calls; no `DOSXC_AT_API` entry is passed |

The loader still runs version 1 programs, and refuses versions newer than
it knows.  The table entries and the system calls share one implementation,
so both behave identically.

`spawn` works like DOS EXEC: it runs the child to completion and returns its
POSIX wait status.  The child inherits the parent's file descriptors
(except close-on-exec ones), so pipes and redirections work as on Unix.
Pipes are buffered in memory, which is what lets `gcc -pipe` work without
concurrency.

## Drives and paths

The system volume keeps the letter it booted with (`C:` once installed,
`X:` on the installation media).  Every other FAT partition found on the
disks (MBR, GPT or unpartitioned) gets the next free letter from `C:`.

Paths accept `/` or `\`, a drive letter and `..`; `/DIR` is on the
current drive.  Programs see paths with the drive, as on DJGPP:
`getcwd()` returns `C:/DIR/SUB`.  A drive spec after a separator starts
again at that drive (`C:/HOME/D:/X` is `D:/X`), which keeps Unix programs
that join the current folder with a path working.  Long file names are
supported.

Drives come and go: a USB disk gets its letters when it is plugged in and
loses them when it is pulled out or ejected (see [USB.md](USB.md)).  The
`drive` system call (`DOSXC_SYS_DRIVE`) has `EJECT` (write back and unmount
a drive's disk) and `NOTICE`, which returns the next message about drives
("USB drive ready as E:") for the shell to show.  Files still open on a
drive that went away fail with `EIO`.

## The console

Console output understands the usual ANSI/VT100 sequences: cursor movement
and positioning (`CUU` ... `CUP`, `HVP`, `CHA`, `VPA`), erase (`ED`,
`EL`, `ECH`), colors (`SGR` 0, 1, 7, 22, 27, 30-37, 39, 40-47, 49, 90-97,
100-107), scrolling regions (`DECSTBM`), insert/delete line (`IL`, `DL`),
`SU`/`SD`, save/restore cursor and the cursor position report (`DSR 6`).
The last column wraps lazily, as on a VT100.  `TERM` is `ansi`.

## Multitasking

`spawn` still suspends the parent until the child exits.  `DOSXC_SYS_TASK`
(`dosxc_sys_task(op, a, b, c)`) also runs programs side by side:

| Operation | Effect |
| --- | --- |
| `DOSXC_TASK_START` | Starts program `a` with argv `b` beside the caller and returns its pid; `c` = console + 1 runs it in a terminal window |
| `DOSXC_TASK_YIELD` | Gives other tasks a turn |
| `DOSXC_TASK_LIST` | Fills `struct dosxc_task_info` `a[b]`, returns the process count |
| `DOSXC_TASK_KILL` | Ends process `b` (not the first shell or the desktop) |
| `DOSXC_TASK_ALIVE` | 1 if process `b` is running |
| `DOSXC_TASK_STATS` | Fills `struct dosxc_task_stats` `a` |
| `DOSXC_TASK_CONSOLE_OPEN` | Creates a virtual console and returns its number |
| `DOSXC_TASK_CONSOLE_READ` | Copies up to `c` bytes that programs wrote on console `b` to `a` |
| `DOSXC_TASK_CONSOLE_KEYS` | Queues the `c` keys in int array `a` on console `b` |
| `DOSXC_TASK_CONSOLE_CLOSE` | Closes console `b` and ends what runs in it |

Tasks switch at system calls and while one sleeps or waits for input.  On a BIOS
boot, a 100 Hz timer also preempts program code, so a program stuck in a loop
cannot hold up the others.  Under UEFI the firmware owns the timer, so a program
that never makes a system call keeps the processor until it does.  The kernel
itself is not preempted, and programs share one address space without protection
from each other.

## Threads

`<pthread.h>` provides POSIX threads: creation, join and detach, mutexes,
condition variables, read-write locks, barriers, spin locks, `pthread_once`,
thread-specific data and, in `<semaphore.h>`, unnamed semaphores.  They are in
`libc`; `-pthread` and `-lpthread` are accepted but add nothing.  `errno`, `malloc`, stdio streams
and GCC's emulated `__thread` variables are per-thread or thread-safe.

A thread is a kernel task that shares its process.  All programs and threads
in the system share 64 task slots, and creating a thread beyond that reports
`EAGAIN`.  The system runs on one processor.  Threads are preempted under the same rules as programs:
by a 100 Hz timer on a BIOS boot, but only at system calls under UEFI, so a
thread spinning without system calls holds up the others there.

| Operation | Effect |
| --- | --- |
| `DOSXC_TASK_THREAD_CREATE` | Starts a thread at `a` with stack pointer `b`; `c` is its data pointer |
| `DOSXC_TASK_THREAD_EXIT` | Ends the thread; zeroes and wakes the int at `a` once its stack is unused |
| `DOSXC_TASK_THREAD_SLOT` | Returns the address of a word holding the running thread's data (0 for the main thread) |
| `DOSXC_TASK_FUTEX_WAIT` | Sleeps while the int at `a` equals `b`, for up to `c` ns (forever if `c` <= 0) |
| `DOSXC_TASK_FUTEX_WAKE` | Wakes up to `b` threads sleeping on `a`; returns how many |

`exit` from any thread, a crash in any thread, or the main thread returning
ends the whole process and all its threads.

Limits: cancellation is deferred and only takes effect at
`pthread_testcancel` and in condition waits and `pthread_join`.  Signals are
not per-thread (`pthread_kill` only accepts signal 0).  Stacks default to 2 MiB,
have no guard page, and are not extended.

## Graphics

`DOSXC_SYS_DISPLAY` (`dosxc_sys_display(op, index, buffer, size)`) gives a
program the displays.  Each is a linear 32-bit XRGB framebuffer that the program
writes to directly.

| Operation | Effect |
| --- | --- |
| `DOSXC_DISPLAY_INFO` | Returns the number of displays; fills `struct dosxc_display` for `index` if `buffer` is set |
| `DOSXC_DISPLAY_MODES` | Returns the mode count; copies up to `size` `struct dosxc_display_mode` |
| `DOSXC_DISPLAY_SET` | Sets the mode in `buffer` on display `index` |
| `DOSXC_DISPLAY_TEXT` | Returns every display to the text console |

The console is restored when the program that set a mode exits.  While a
graphics mode is set the text console is not visible, so call `TEXT` before
printing or running a text program.

`DOSXC_CONSOLE_VIRTUAL` returns 1 in a terminal window, `DOSXC_CONSOLE_SPEAKER`
sets the PC speaker's PIT divisor (0 silences it, and it is silenced when the
caller ends), and `DOSXC_CONSOLE_VOLUME` sets the sound volume, or reads it when
`a` is negative.

`dosxc_sys_console(DOSXC_CONSOLE_POINTER, (long)&pointer, 0)` reads the mouse
movement since the last call into `struct dosxc_pointer` and returns 1, or 0 if
there is no mouse.  See [DESKTOP.md](DESKTOP.md).

Input is cooked (a line editor with echo) until a program clears `ICANON`
with `tcsetattr`; then reads return one byte per key, Enter as CR, and the
cursor and function keys as VT220 sequences (`ESC [ A`, `ESC [ 3 ~`,
`ESC O P` ...).  `select` and `poll` wait for console input; files and
pipes are always ready.  The `console` system call also offers direct
operations (`CLEAR`, `COLOR`, `CURSOR`, `GETKEY`, `POLL`, `SIZE`,
`GETCOLOR`, `MOUSE`).

A mouse moves a block cursor over the screen (BIOS boot).  In raw mode a
program can ask for mouse reports as from xterm: `CSI ? 1000 h` (button
presses and releases), `1002 h` (also motion while a button is held),
`1003 h` (all motion), with `CSI ? 1006 h` for the SGR encoding
(`CSI < b ; column ; row M`, `m` on release); wheel turns are buttons 64
and 65.  In Vim: `:set mouse=a ttymouse=sgr`.  `console(MOUSE, &m)` reads
the cursor cell, the buttons and the wheel directly.

## Shared libraries

`gcc -shared -fPIC` makes a shared library (`.so`).  Programs are linked
dynamically by default, against `libc.so` and any other libraries they
use; `gcc -static` makes a static program instead.  Dynamic programs name
`/SYSTEM/ELFAPI.DLL` as their interpreter: the runtime is the dynamic
linker.

* Libraries are searched in the `DT_RUNPATH`/`DT_RPATH` folders of the
  object that needs them, the program's folder and `\SYSTEM\LIB` (which
  holds `libc.so`).
* A library is loaded once.  Its code is shared by every process using it;
  its writable pages (data, bss, GOT) are private: every process gets a
  copy, relocated for that process, mapped at the library's address with
  paging while the process runs.  Processes nest, so mappings only change
  when a process starts or ends.  (The x86 edition turns paging on when the
  first library is loaded; the x64 edition clones the page tables it needs
  to change.)
* All symbols are bound at start (`BIND_NOW`); lookups search the program,
  then its libraries breadth-first, so programs may interpose symbols and
  use copy relocations.  Supported relocations: `RELATIVE`, `IRELATIVE`,
  `GLOB_DAT`, `JUMP_SLOT`, `COPY`, absolute and PC-relative word.  Text
  relocations and thread-local storage are rejected.
* Library constructors run before the program's (`DOSXC_AT_INITS`),
  destructors after the program's (`DOSXC_AT_FINIS`).

## The runtime

`\SYSTEM\ELFAPI.DLL` is loaded by the kernel's init module at boot at a
fixed address (`0x4400000`).  It owns all memory above `0x5000000` (sized
from the E820 map on BIOS, or a block reserved by the UEFI loader), a
sector cache for the system volume (with a RAM overlay when booted from
read-only media), the drive letters, the open-file table, the terminal,
the system environment and the shared libraries.  On 64-bit BIOS boots it
maps the first 4 GiB.  Files opened for reading are read from disk on
demand; files opened for writing are kept in memory and written back when
closed.

## The C library

`src/libc` is a C library written for DOS-XC: stdio (with exact
floating-point formatting), stdlib, string, math, time, locale stubs,
wide characters, `setjmp`, `getopt`, directories, `posix_spawn`, `waitpid`,
`pipe`, `select` on pipes, POSIX threads, `fork`-less process creation and the POSIX
pieces GCC, binutils and libstdc++ need.  Build it with

```sh
src/libc/build.sh x86_64 OUT [CC]     # or i386
```

which produces `OUT/lib/{crt0,crti,crtn}.o`, `libc.a`, `libm.a` (empty, the
math functions are in `libc.a`) and `OUT/include`.

`src/libc/build.sh` also links `libc.so`.

`fork` and `exec*` without a following exit are not available; use
`posix_spawn`, `system` or `popen` (`popen(..., "r")` runs the command at
once; `"w"` runs it at `pclose`).  `termios`, `ioctl` (`TIOCGWINSZ`),
`poll`, `select`, `pwd.h` (one user) and a database-less `termcap` are
provided for terminal programs.  Signals are limited to `raise`,
`signal` and `kill` on the calling process; Ctrl+C delivers SIGINT.

## Testing

`tests/qemu_session.py` boots an image in QEMU, types commands and prints
the console transcript (mirrored to port 0xE9).  `HELLO16.COM`-style
16-bit programs can be added the same way; their output goes through the
BIOS and does not appear in the transcript.  `scripts/build.sh`
empties `build/part` and `build/iso`, so keep test trees elsewhere:

```sh
mkdir -p /tmp/t64/TESTS
for t in hello libctest syscalltest; do
    toolchain/cross/bin/x86_64-dosxc-gcc -O2 tests/programs/$t.c \
        -o /tmp/t64/TESTS/$(echo $t | cut -c1-8 | tr a-z A-Z).EXE
done
DOSXC_EXTRA_TREES_64=/tmp/t64 scripts/build.sh
tests/qemu_session.py --iso build/iso/DOSXC_0.2_x64.iso -c "CD \\TESTS" -c "SYSCALLT.EXE" -c "LIBCTEST.EXE"
```

Use `i686-dosxc-gcc`, `DOSXC_EXTRA_TREES_32` and `build/iso/DOSXC_0.2_x86.iso`
for the x86 edition.  `SYSCALLT.EXE` checks the system call interface
(register preservation, 64-bit results, `spawn` keeping the FPU state).
`LIBCTEST.EXE` deliberately runs a child that crashes with an invalid
opcode, so the exception message in its output is expected.
