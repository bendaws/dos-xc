# USB

USB keyboards, mice and drives (flash drives, card readers, external
disks) work in DOS-XC and can be plugged in and pulled out at any time.

## Using it

* **Drives** get the next free letter when plugged in, one per FAT
  partition, and the shell says so before its next prompt (or right away
  while you are typing):

  ```
  USB drive ready as E:
  ```

  Pulling a drive out unmounts it (`USB drive removed: E:`).  If you wrote
  to it, type `EJECT E:` first: it writes back everything still waiting,
  unmounts the drive (and any other drive on the same disk) and tells the
  device it is about to be removed.  The drive comes back the next time it
  is plugged in.  Card readers are checked every two seconds for a card
  going in or out.
* **Keyboards** work alongside the PS/2 keyboard, with Num/Caps/Scroll Lock
  lights and key repeat.
* **Mice** move a block cursor over the text screen.  Programs that ask for
  mouse reports get them (see [PROGRAMS.md](PROGRAMS.md#the-console); in
  Vim, `:set mouse=a ttymouse=sgr`).
* **16-bit DOS programs** see USB keyboards through INT 16h, USB mice
  through an INT 33h mouse driver (version 8.05, text-mode cursor), and a
  USB disk the BIOS knew at boot, such as the stick DOS-XC started from,
  through INT 13h.

## How it works

### BIOS boot

The runtime (`\SYSTEM\ELFAPI.DLL`) drives the USB host controllers itself
([`src/runtime/usb.h`](../src/runtime/usb.h)):

| File | Contents |
|------|----------|
| `pci.c` | PCI configuration space, the scan for USB controllers, DMA memory |
| `xhci.c` | xHCI (USB 3) |
| `ehci.c` | EHCI (USB 2); full- and low-speed devices go to the companion controllers, or use split transactions behind a high-speed hub |
| `ohci.c`, `uhci.c` | OHCI and UHCI (USB 1.1) |
| `usb.c` | enumeration, hubs and hot plug, boot-protocol keyboards and mice, bulk-only mass storage (SCSI) |
| `bridge.c` | the 16-bit side, below |

Everything is polled; DOS-XC uses no interrupts.  The USB code runs from
the keyboard wait loops and from system calls (at most every 4 ms); root
hub ports are looked at every 50 ms and external hubs report changes
through their status endpoint.  At start the runtime takes each controller
from the BIOS (the legacy-support handoff), resets it and enumerates what
is connected.  Disks are 512-byte-sector SCSI block devices (LUN 0).

Once the runtime owns the controllers the BIOS's own USB support is gone,
so a small real-mode **bridge** ([`src/dos/bridge.asm`](../src/dos/bridge.asm))
is copied to `0x1000` and hooks:

* INT 13h for BIOS drives that turned out to be USB disks: a disk is
  recognised by its size and first sector, keeps its BIOS drive number and
  letters, and its sectors now come from the runtime's driver.  This is
  how DOS-XC keeps running from a USB stick, including the kernel restart
  after every 16-bit program.
* INT 13h for the live CD while a 16-bit program runs (or the kernel
  restarts after one): the BIOS reads the CD, then the runtime copies in
  the sectors its RAM overlay holds, so programs installed or files changed
  on the live system are seen; 16-bit writes to existing files go to the
  overlay.  (The bridge is installed on every live BIOS boot for this.)
* INT 16h: USB keystrokes are moved into the BIOS keyboard buffer.
* INT 33h: a mouse driver for USB mice.
* INT 08h: a plain timer tick (some BIOSes, SeaBIOS among them, poll their
  own USB drivers from it and would write to controllers the runtime has
  reset).

The bridge switches the CPU to protected or long mode (with its own GDT and
page tables, since 16-bit programs overwrite the boot loader's), calls the
runtime's `bridge_entry` with interrupts off, and switches back.

### UEFI boot

The firmware keeps its USB drivers: keyboards already reach DOS-XC through
the firmware console, the runtime looks at the firmware's disks (Block I/O)
again every second to mount and unmount drives, and reads mice through the
Simple Pointer protocol.

## Testing in QEMU

`tests/qemu_session.py` adds devices with `--qemu-arg` and plugs them in
and out with monitor commands (`!...`):

```sh
tests/qemu_session.py --iso build/iso/DOSXC_0.2_x64.iso \
  --qemu-arg="-device qemu-xhci,id=xhci" --qemu-arg="-device usb-kbd,bus=xhci.0" \
  --qemu-arg="-drive if=none,id=stick,format=raw,file=stick.img" \
  -c "!device_add usb-storage,bus=xhci.0,drive=stick,id=s1" -c "~4" -c "DIR C:" \
  -c "EJECT C:" -c "!device_del s1"
```

Other controllers: `-device piix3-usb-uhci,id=uhci`, `-device pci-ohci,id=ohci`,
and EHCI with UHCI companions:
`-device ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on -device ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on`
(and `uhci2`, `uhci3` with `firstport=2` and `4`).  `-device usb-hub` is a
full-speed hub.  QEMU drops a `-drive` together with its device, so plug a
drive in again with `!drive_add 0 if=none,id=stick2,format=raw,file=stick.img`
and a new `device_add`.  Set `USB_DEBUG` in `src/runtime/usb.c` to trace
enumeration.

Not covered by QEMU: split transactions (QEMU has no high-speed hub), USB 3
hubs, and the UEFI mouse (OVMF has no USB mouse driver).
