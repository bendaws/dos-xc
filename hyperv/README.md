# hyperv

The `hyperv` package: `HYPERV.DRV`, one driver for Hyper-V's synthetic network adapter, SCSI disks, keyboard and mouse, which share a VMBus connection. It needs a Generation 1 VM and stays idle anywhere else.

**Status: compiled and loaded under QEMU only.** It has never run on Hyper-V. See [docs/DRIVERS.md](../docs/DRIVERS.md#hyper-v).

Build it alone with `hyperv/package.sh [32] [64]`.

## Licence

BSD 3-Clause; see [LICENSE.txt](LICENSE.txt).
