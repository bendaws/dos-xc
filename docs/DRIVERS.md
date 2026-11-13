# Device drivers

Hardware support that is not built into DOS-XC comes as drivers: small
ELF shared objects named `*.DRV` in `\SYSTEM\DRIVERS`.  They load at
startup, and `PKG` starts and stops them as it installs and removes them.

```
DRIVERS                   drivers currently loaded
DRIVERS /LOG              what they reported (start here when something fails)
DRIVERS /DEVICES          PCI devices and the driver running each
DRIVERS /LOAD file.drv    load one now
DRIVERS /UNLOAD name.drv  stop one
```

A driver that hangs the machine at startup can be skipped by holding Shift
during a BIOS boot.  A processor exception inside a driver stops only that
driver: it shows as `FAILED` and its device loses bus mastering.

## Writing one

Every service a driver can use is in [`<dosxc/driver.h>`](../os/libc/include/dosxc/driver.h).
A driver exports one symbol, `dosxc_driver`, and receives a `struct dk_api`
table when it loads.  There are no interrupts: DOS-XC calls `poll` often,
so a driver checks its hardware there and uses timeouts everywhere else.

```c
#include <dosxc/driver.h>

static const struct dk_api *dk;
static const struct dk_pci_id ids[] = {{0x1234, 0x5678}, {0, 0}};

static int load(const struct dk_api *api) { dk = api; return 0; }

static int probe(const struct dk_pci_device *device, void **instance) {
    dk->log("HELLO: found %04x:%04x", device->vendor, device->id);
    *instance = 0;
    return 0;
}

static void poll(void *instance) { (void)instance; }
static void remove_device(void *instance) { (void)instance; }

DK_DRIVER = {DK_ABI_VERSION, sizeof(struct dk_driver), "Hello", "1.0",
             "An example", ids, load, probe, poll, remove_device, 0};
```

Build it with the cross compiler (`i686-dosxc-gcc` for the x86 edition):

```
x86_64-dosxc-gcc -shared -fPIC -nostdlib -ffreestanding -mno-red-zone \
    -mno-sse -mno-mmx -O2 -s hello.c -o HELLO.DRV
```

Rules that follow from running inside the system:

* Do not use the C library or floating point; the loader only supplies
  `memcpy`, `memset`, `memmove` and `memcmp`, which the compiler emits.
* Allocate device-visible memory with `dma_alloc`.  It is below 4 GiB and
  physically contiguous.
* Stop all DMA in `remove`.  It runs at unload, restart and power off.
* Never wait without a time limit: nothing else runs while you wait.

`pci_ids` lists vendor and device IDs; `{DK_ANY}` as the device matches every
device of a vendor, and `{DK_CLASS, 0x0403}` matches every function of a PCI
class and subclass (here HD Audio), whoever made it.  Prefer exact IDs or a
class: a driver whose table names a device is started for it automatically.

## Finding drivers automatically

`\SYSTEM\DRIVERS\STORE` holds every driver DOS-XC ships (the live media builds
it from the packages; Setup copies it to the disk).  At startup, after the
drivers in `\SYSTEM\DRIVERS`, the system reads the ID table of each store
driver that is not running, without running any of its code
(`DOSXC_DRIVER_MATCH`), and starts only the ones naming a device no driver has
taken.  A store driver that then takes no device is unloaded again.  Drivers
without a table (`pci_ids` 0, such as `HYPERV.DRV`) are always tried.

The desktop goes further at each login of an administrator (`DEVICES.EXE /AUTO`):

1. store drivers matching a device without a driver are started;
2. every package in `\Packages` on any drive (the live media, a USB stick)
   whose `Driver=` files match such a device is installed or updated with
   `PKG`, so it is kept and updated like any package;
3. on an installed system, a package is also installed when one of its drivers
   is running only from the store.

It opens a New Hardware window when something was installed and stays out of
sight otherwise.  An empty file `\SYSTEM\DRIVERS\MANUAL` turns this off; Control
Panel > Device Manager > Find Drivers still does it on request.

## Network adapters

`probe` registers an adapter with `net_register`, reports the carrier with
`net_link`, and hands each received Ethernet frame to `net_receive` from
`poll`.  The system does the rest (DHCP, ARP, IP, TCP, sockets).  The
drivers in [`ethernet`](../ethernet) are complete examples;
`e1000.c` is the simplest.

## Devices that are not PCI functions

A driver whose `pci_ids` is 0 has `probe` called once, with a null device,
and `poll` called like any other.  `probe` decides whether its hardware exists.

## Disks

`disk_register` adds a disk and mounts its FAT volumes.  `io` transfers at most
64 sectors of 512 bytes and waits for the result; `disk_unregister` removes it.

IDE (ATA and ATAPI CD drives), AHCI SATA and NVMe controllers are driven by the
runtime itself on a BIOS boot (`os/runtime/storage.c`, `ata.c`, `ahci.c`,
`nvme.c`), with DMA and polling.  A disk with the same size and first sector as
one the BIOS listed replaces it, including the disk or CD DOS-XC booted from,
and INT 13h for that drive is passed to the native driver, so 16-bit programs
keep working.  Disks the BIOS did not list become new drive letters.  An empty
file `\SYSTEM\BIOSDISK` on the system drive keeps every disk on INT 13h.  UEFI
boots still use the firmware's block I/O.  Tested in QEMU only (IDE, AHCI,
NVMe; reads about three times faster than through the BIOS).

## Sound

`probe` calls `audio_register` with a `struct dk_audio_ops`: `open` (rate and channels, 16-bit little-endian PCM), `close`, `write` (takes what fits and never waits) and `position` (frames played since `open`). One device is used. Programs reach it through `DOSXC_SYS_AUDIO`, one program at a time; `sound/hda.c` is the example, and it only supports 48 kHz stereo.

## Keyboards and mice

From `poll`, `input_key` takes a HID keyboard usage with whether it is pressed
(a device that repeats keys just sends the press again).  `input_mouse` takes
relative motion and the button state, and its first call makes the system
report that a mouse is present.

## Hyper-V

`HYPERV.DRV`, in the `hyperv` package and built from [`hyperv`](../hyperv),
drives VMBus and the synthetic network adapter, SCSI controllers, keyboard and
mouse.  They share one driver because they share one VMBus connection.

It needs a **Generation 1** VM.  On Generation 2, firmware services keep running
under DOS-XC and the firmware holds the VMBus connection, so the driver reports
that in `DRIVERS /LOG` and stays idle; outside Hyper-V it also stays idle.
Disks on the VM's IDE controller keep using the BIOS, and only SCSI disks of 512-byte
sectors appear through the driver.  Disks added while the VM runs are not noticed.
The VM's mouse is absolute and DOS-XC's is relative, so the guest pointer may
sit apart from the host's until it reaches a screen edge.

**This driver has only been compiled.**  It has not run on Hyper-V.  Start with
`DRIVERS /LOG` when something fails: each stage of the connection logs there.

## Graphics

DOS-XC has no native driver for Intel, AMD or NVIDIA graphics: the desktop draws
into the framebuffer the firmware set up (VESA BIOS on a BIOS boot, GOP on UEFI),
so it gets the modes the card's firmware offers and one monitor.  On a BIOS boot
the monitor's EDID is read through VBE, so the desktop starts at the monitor's
native resolution when the card's firmware lists it, and otherwise the largest
mode that fits.  The 64-bit edition maps the framebuffer write-combining (PAT),
which makes drawing on real cards many times faster than uncached access.  The
card's HDMI/DisplayPort audio is an HD Audio function and `HDA.DRV` can drive
it, but only one sound device is used, normally the board's own.

A modern GPU's display engine (NVIDIA since Turing, AMD since RDNA) is set up
through signed firmware and very large drivers; nothing smaller than those
drivers can change its modes, so the firmware path is the one DOS-XC supports.

## Packaging

A package lists its drivers in the `Driver` key.  `PKG` copies those
files to `\SYSTEM\DRIVERS` and loads them; everything else in the package
goes to `\Programs\<Name>` as usual.

```
PKG PACK C:\Work\hello C:\hello.dsp /N:hello /V:1.0 /B:64 /R:HELLO.DRV
PKG INSTALL C:\hello.dsp
```

`PKG` refuses to overwrite a driver file another package installed, and
`/B:` must match the edition, since a driver is 32-bit or 64-bit code.
