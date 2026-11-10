# DOS-XC TODO: Firefox support
**Goal:** a `firefox` package ("Firefox web browser") running real, current Firefox on the 64-bit edition, because the user needs modern websites. The user chose this knowing it is a very large project, after considering NetSurf and Ladybird. Do not reopen that choice. System changes are explicitly allowed. A `vlc` package ("VLC media player") is also wanted, later; it shares most prerequisites.

Older hardware checks that are still open: `git show 97064ed:TODO.md`.

## Before starting on the desktop
**Build in WSL. Test the OS in Hyper-V.** These are the user's choices: Hyper-V is where DOS-XC runs for testing, and it performs better than emulation.

- [ ] Set up WSL (Ubuntu); see "Building on Windows" in `README.md`.
- [ ] Test the PowerShell scripts. `scripts/boot{32,64}.ps1`, `scripts/build.ps1`, `toolchain/build-toolchain.ps1`, `ports/build-ports.ps1` and `scripts/windows-common.ps1` **have never been run**: no PowerShell on the Mac.
- [ ] Rebuild the toolchain in WSL. `toolchain/cross` holds Mac binaries; `toolchain/{cross,work,downloads,dist}` and `build/` are git-ignored. The shell scripts have only ever run on macOS, so expect Linux fixes. Keep the tree on WSL's ext4, not `/mnt/c`: the toolchain and Firefox builds will be far too slow there.
- [ ] Install `python3.14` in WSL (the Python port needs that exact minor version on the host).

### Testing in Hyper-V
DOS-XC has never run in Hyper-V, so everything here is untested. All testing so far used QEMU's software emulation, so expect problems with ring 0 programs, timer preemption and `rdtsc` timing under a real hypervisor.

- [ ] Use a **Generation 1** VM (BIOS, emulated IDE, PS/2 and VGA), which suits what DOS-XC already does on a BIOS boot: disk access through INT 13h (`src/runtime/cache.c`), graphics through VBE (`src/runtime/display.c`), and timer preemption, which Generation 2's UEFI path lacks. Generation 2 cannot use the Hyper-V drivers while DOS-XC keeps UEFI boot services running, because the firmware already holds the VMBus connection.
- [ ] Boot the ISO from the VM's DVD drive and get to the shell. Consider adding a `scripts\hyperv.ps1` that creates the VM and attaches the current ISO; it needs an elevated prompt or the Hyper-V Administrators group.
- [ ] **Bring up `HYPERV.DRV`, which has only been compiled and has never run on Hyper-V.** It is the `hyperv` package (`src/hyperv`: VMBus, netvsc, storvsc, synthetic keyboard and mouse), written from the Linux and FreeBSD protocol definitions, and QEMU cannot exercise it. On the live system, `PKG INSTALL X:\Packages\hyperv.dsp`, then read `DRIVERS /LOG`, which logs each connection stage. Use a synthetic network adapter (not Legacy) and a disk on a SCSI controller. Expect bugs: check the VMBus connection first, then each device. Details and limits are in `docs/DRIVERS.md`.
- The mouse and network parts of `HYPERV.DRV` replace the need for a PS/2 mouse driver and a DEC 21140 driver. If the synthetic mouse cannot be made to work, the desktop needs a PS/2 driver instead, since Generation 1 has no USB.
- [ ] Automation. `tests/qemu_session.py` depends on QEMU's debug port (0xE9) and monitor, neither of which exists in Hyper-V. Keep using it with QEMU for scripted tests; for Hyper-V, a serial console over a Hyper-V COM named pipe would be the equivalent.
- Check RAM above 4 GiB here too (roadmap item 2): Hyper-V is where Firefox-sized memory will first be tried.

## What already works (QEMU only, never on real hardware)
- **Threads:** kernel tasks sharing a process (`src/runtime/sched.c`), futexes, `src/libc/src/pthread.c`, thread-safe `errno`, `malloc`, stdio, emulated `__thread`. Passed a stress test on BIOS 32/64-bit and UEFI 64-bit. Documented in `docs/PROGRAMS.md`.
- **TLS:** OpenSSL 3.5.9 linked statically into curl and Python; Mozilla roots as `CACERT.PEM` in each package. Valid sites connect; expired, self-signed and wrong-host certificates are refused.
- **Randomness:** `DOSXC_SYS_RANDOM`, `src/runtime/random.c`. Not saved across restarts, which matters on CPUs without RDRAND. A saved seed is a good small improvement.
- **Python:** CPython 3.14.6 package, all extension modules static, real threads. Threads get 6 MiB stacks (`-DTHREAD_STACK_SIZE` in `ports/build-ports.sh`) because CPython assumes about 4 MB when `pthread_getattr_np` is missing. Passed locks, queues, thread pools, `RecursionError` in a thread, and TLS from four threads at once on both editions.
- **GCC:** thread model `posix`. C `__thread`, C++ `thread_local`, `std::thread`, `std::mutex` and exceptions in threads work from the cross compilers and from the native package on both editions (native compiles only tested from a FAT32 disk with the package at `\Programs\GCC`).

## Firefox roadmap
Facts checked against the source on 2026-11-09 (`github.com/mozilla-firefox/firefox`, 162.0a1): Rust 1.95+ is required; toolkits are only windows, cocoa, uikit, android, gtk3 (`toolkit/moz.configure`), with a `widget/headless` backend; `fission.autostart` defaults to true; the repository is about 5.7 GB. **Not yet checked:** whether GCC 15 is still accepted (see `build/moz.configure/toolchain.configure`). A Clang targeting DOS-XC may be needed; decide this early, since it affects everything below. Also check whether single-process mode still exists.

1. **Memory.** `mmap`, `munmap`, `mprotect`, `madvise`, anonymous shared memory. Only `DOSXC_SYS_ALLOC` exists now. There is no `<sys/mman.h>`.
2. **Address space.** BIOS 64-bit boots map only the first 4 GiB (`src/runtime/cpu.c`), and QEMU gets 1 GiB by default. Firefox needs several GiB, so plan on at least 4 GiB of RAM and mapping above 4 GiB. UEFI memory is a block reserved by the loader; check its limits too.
3. **Native TLS.** The loader rejects `PT_TLS`. Firefox and Rust use thread-locals constantly, so set FS base per task and support `PT_TLS` instead of relying on emutls. This also replaces `sched_thread_slot` for libc.
4. **`dlopen`.** No `<dlfcn.h>`. The loader in `src/runtime/dynamic.c` handles startup dependencies only and a limited set of relocations (see `docs/PROGRAMS.md`).
5. **Processes and isolation.** The largest change. Programs run in ring 0 in one shared address space; `spawn` is synchronous; there is no `fork`; `DOSXC_TASK_START` starts background programs. Firefox's content processes need asynchronous spawning, and running untrusted web content safely needs per-process page tables and ring 3. Per-process address spaces would also change the position-independent loading design, the library data swapping in `libraries_activate`, and the assumption that syscalls run on the program stack.
6. **IPC.** `AF_UNIX` sockets with descriptor passing; `socketpair` currently returns `EOPNOTSUPP`. When it exists, remove the asyncio self-pipe patch in `ports/python-dosxc.py`.
7. **Limits to raise.** `MAX_FDS` and `FD_SETSIZE` are 64, `MAX_DEPTH` is 32 processes, 64 task slots system-wide (`MAX_TASKS`, 32 KiB kernel stack each), 2 MiB default thread stacks with no guard pages. Also missing: `sys/un.h`, `sys/shm.h`, `ifaddrs.h`, `pthread_getattr_np`, per-thread signals. Cancellation only happens at `pthread_testcancel`, condition waits and joins. `fork` returns `ENOSYS`.
8. **Rust.** A `x86_64-unknown-dosxc` target with std, plus the `libc` crate and low-level crates that switch on the OS. Must match Firefox's required version.
9. **Firefox platform layers.** NSPR, NSS, mozjemalloc, IPC (`ipc/chromium`), sandbox disabled at first, `xpcom`, `cubeb` stubbed.
10. **Widget backend.** New `widget/dosxc` and toolkit entry, starting from `widget/headless`. The desktop is `src/desktop` (apps are processes on `gui.c`, sharing state with DOSXC_TASK_SHARE). Render in software first. Start with the JIT off; the JIT then needs `mprotect` or W^X handling.
11. **Later.** IPv6, a fuller TCP stack (see limits below), fonts, a sound driver and cubeb backend, video. Hardware acceleration in QEMU on Windows (`$env:ACCEL = 'whpx'`) is untested and worth trying early: a browser under pure emulation will be painfully slow.

Networking limits today: IPv4 only, no fragment reassembly, 64 KiB TCP windows, NICs polled from system calls (`net_tick`) with no interrupts.

## How to test
```sh
python3 tests/qemu_session.py --iso build/iso/DOSXC_0.2_x64.iso --timeout 240 \
  --qemu-arg="-netdev user,id=n0 -device e1000,netdev=n0" \
  -c 'PKG INSTALL X:\Packages\ethernet.dsp /Y' -c 'PKG INSTALL X:\Packages\python3.dsp /Y' -c '~10' -c 'PYTHON3 X:\T\TEST.PY'
```
- `-c '~N'` waits, `-c '&TEXT'` types without waiting for a prompt, `-c '!cmd'` is a QEMU monitor command. `--uefi` takes a firmware file, `--screendump` writes a PPM.
- Put test programs on the media with `DOSXC_EXTRA_TREES=dir scripts/build.sh`; they appear at the root of `X:`.
- Build test media in a separate folder with `BUILD=/some/dir`. **`scripts/build.sh` deletes `build/iso`, so check the user has no QEMU running from it.**
- Cross-compile a test: `x86_64-dosxc-gcc --sysroot=toolchain/work/ports-x86_64/sysroot -O1 t.c -o T.EXE`.

## Things that cost time
- The live system's RAM disk is small: Python and Vim do not both fit, and a failed install reports "is the disk full?". Test big packages one at a time or on a real disk. Nothing has been tested from a hard-disk install yet.
- Ports link `libc.so` dynamically, so old installed systems (including any existing `build/disk.img`) need reinstalling from a current ISO.
- Autotools ports need help: CPython's link tests silently all fail without `--with-suffix=.exe`, because the toolchain writes `conftest.exe`. Set `PKG_CONFIG=false` or host libraries leak in.
- Paths are `C:/...` with `;` search paths. Anything assuming `/`-rooted paths or `:` separators breaks; expect this in Firefox's build output and at run time.
- Native GCC looks for headers in `/Programs/GCC/include` on the **current drive**. Run it from the drive GCC is installed on or it reports `wchar.h` as missing.
- A header that defines different things depending on include order can silently change struct layouts. CPython's `CONDATTR_MONOTONIC` needs `CLOCK_MONOTONIC`, so `<pthread.h>` must include `<time.h>`; without it `_hashlib` saw `_PyRuntime` 8 bytes larger than the core did and crashed. A jump to an address containing `F000FF53` means a call through a null object: low memory holds the BIOS vector table. Expect more of this with Firefox, whose headers assume glibc includes.
- For a test crash use `__asm__("ud2")`. GCC folds `1 / z` to a conditional with no division, and low addresses are mapped, so a null write does not fault.
- Preemption only happens on BIOS boots (100 Hz, program code only). Under UEFI, threads switch only at system calls, so spinning code stalls others. Firefox on UEFI will need a real timer.
- Update `docs/` alongside system changes; the project keeps them accurate.
