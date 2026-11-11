# GCC for DOS-XC

Builds GCC 15.2 and binutils 2.45 as cross compilers (`x86_64-dosxc`, `i686-dosxc`) and as the native `GCC` package. See [docs/TOOLCHAIN.md](../docs/TOOLCHAIN.md).

| File | Purpose |
|------|---------|
| `build.sh` | Downloads and builds everything: `gcc/build.sh [x86_64\|i686] [STAGE...]` |
| `build.ps1` | The same from Windows, through WSL |
| `patch-sources.sh` | Applies the DOS-XC changes to the GCC and binutils sources |
| [`config`](config) | The DOS-XC target description for GCC |
| `package.sh` | Packs the result as a `.dsp` |

Output goes to the git-ignored `toolchain/` folder at the top of the repository.

## Licence

The scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). The files in [`config`](config) are part of GCC and are under the GPL version 3 or later ([COPYING3.txt](COPYING3.txt)), as are GCC and binutils themselves.
