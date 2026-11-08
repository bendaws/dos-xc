### Remaining checks (implemented, not yet verified)
- x86 (32-bit) edition: Setup, PKG, the native 32-bit GCC package, Vim/xz (the shell, USB and shared libraries with the 16-bit round trip, `TESTS32\D.BAT`, work)
- UEFI: Setup under UEFI and the logo glyphs (boot, drive hot plug and EJECT work)
- USB paths QEMU cannot exercise: EHCI split transactions (low/full-speed devices behind a high-speed hub), USB 3 hubs, the UEFI mouse (OVMF has no USB mouse driver), Vim with `:set mouse=a ttymouse=sgr`, real hardware (BIOS handoff, SMM legacy emulation)
- The Oregon Trail's music on real hardware: note lengths now use a timer-based `Delay` (src/dos/crtdelay.asm); confirm it on the Z400
- Power button and SHUTDOWN on real hardware (ACPI mode is enabled through SMI_CMD on BIOS boot; QEMU's power button works)
- HP Z400 onboard Ethernet (BCM5764, `TG3.DRV`): written from the documented programming model, never run on hardware; QEMU cannot emulate it. Try `DRIVERS /LOG` and `IPCONFIG` first
