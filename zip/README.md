# Info-ZIP for DOS-XC

Builds Info-ZIP zip 3.0 and unzip 6.0 as the `zip` package (`ZIP`, `UNZIP`, `ZIPNOTE`, `ZIPSPLIT`, `ZIPCLOAK`, `FUNZIP`, `UNZIPSFX`).

- `build.sh [x86_64|i686]` compiles them into `build/ports`.
- `package.sh [32] [64]` packs that as a `.dsp`.

The Unix makefiles' `configure` steps are skipped (their probes do not compile with GCC 15); `build.sh` gives the answers instead. bzip2 compression is not included.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). Info-ZIP's licence (BSD-style) is copied into the package.
