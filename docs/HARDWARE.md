# Running on real hardware

DOS-XC has been developed in QEMU.  Nothing in this page has run on a real
machine yet; it says what should happen, what to set in the firmware, and what
to collect when something does not work.

## Before booting

* Use the 64-bit edition (`DOSXC_<version>_x64.iso`).  The ISO is not a hybrid
  image: burn it to a DVD.  Writing it to a USB stick with `dd` does not boot.
* Prefer a **BIOS (Legacy/CSM) boot**.  Only BIOS boots have multiple
  processors and the native IDE/AHCI/NVMe drivers; UEFI boots use the
  firmware's block I/O and pointer.  Both run programs in ring 3 with
  preemptive multitasking (UEFI switches tasks on the firmware's own timer).
* Set the SATA controller to **AHCI**.  Intel's RAID mode also works for single
  disks, which then look like AHCI ports.  RAID arrays and Optane/RST NVMe
  remapping are not supported.
* Turn Secure Boot off for a UEFI boot.

## When something fails

Holding **Shift** during a BIOS boot skips all drivers.  At the prompt (press
Esc during the welcome screen on the live system to keep it), these show what
DOS-XC found:

```
DRIVERS /DEVICES     every PCI device, its IDs and its driver
DRIVERS /LOG         what each driver reported
DRIVERS              which drivers run
IPCONFIG             network adapter state
```

Event Logs on the desktop has the same driver messages.  Files that change how
the system starts, each an empty file:

| File | Effect |
|------|--------|
| `\SYSTEM\DRIVERS\MANUAL` | the desktop no longer installs drivers by itself |
| `\SYSTEM\BIOSDISK` | disks stay on the BIOS (INT 13h) instead of the native drivers |
| `\SYSTEM\RING0` | programs run in ring 0, without isolation |
| `\SYSTEM\NOSMP` | programs run on the first processor only |

## Lenovo ThinkStation P330 (Coffee Lake, C246)

| Part | How DOS-XC drives it |
|------|----------------------|
| Core i5/i7/i9 8th-9th gen, Xeon E-2100/E-2200 | every core runs programs on a 64-bit BIOS boot; the kernel stays on the first |
| 64 GB DDR4 | programs and libraries load below 4 GiB; memory maps of the 64-bit edition use the rest |
| USB 3.1 (xHCI 8086:a36d, 26 ports) | built in; USB 3 devices appear on the controller's ports above 16, which DOS-XC now reaches |
| SATA (8086:a352 AHCI, 8086:2822 RAID) | built in |
| M.2 NVMe | built in |
| Intel I219-LM Ethernet (8086:15bb) | `I219.DRV`, starts from the driver store; the desktop installs the `ethernet` package |
| Realtek ALC662 on Cannon Lake HD Audio (8086:a348) | `HDA.DRV`, 48 kHz stereo |
| GeForce RTX 3050 | the card's VESA BIOS (BIOS boot) or GOP (UEFI); see below |
| RTX 3050 HDMI/DisplayPort audio | not used: one sound device, the board's |
| Intel Wi-Fi (CNVi, optional) | no driver |

**Graphics.**  The RTX 3050's firmware sets the modes, so the desktop gets the
resolutions the card's VESA BIOS lists (BIOS boot) or the GOP modes (UEFI), on
one monitor.  The monitor's native resolution is used when the firmware offers
it.  If a BIOS boot shows no picture, the card's firmware may lack legacy
support; boot in UEFI mode instead.  With "Above 4G decoding" or Resizable BAR
on, a UEFI framebuffer can sit above 4 GiB, where the desktop cannot reach it:
turn those off if the desktop says there is no usable display.

**Network.**  The I219 is built into the chipset and shares its PHY with Intel
AMT on the LM model.  The driver brings the PHY out of its low-power state
(which Windows or Linux may leave it in), resets the controller with the
management engine's agreement and autonegotiates.  If the link stays down,
`DRIVERS /LOG` says how far it got.
