# sound

The `sound` package: the Intel High Definition Audio driver (`HDA.DRV`), which plays 48 kHz stereo through the system's audio interface described in [docs/DRIVERS.md](../docs/DRIVERS.md).

Build it alone with `sound/package.sh`. Tested in QEMU only (`-device intel-hda -device hda-output`).

## Licence

BSD 3-Clause; see [LICENSE.txt](LICENSE.txt).
