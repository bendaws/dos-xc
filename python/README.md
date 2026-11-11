# Python for DOS-XC

Builds CPython 3.14.6 as the `python3` package, with OpenSSL for `ssl` and `hashlib` and every extension module linked into the interpreter. `python-dosxc.py` adapts the standard library to drive letters and `;` search paths. See [docs/TOOLCHAIN.md](../docs/TOOLCHAIN.md#python).

- `build.sh [x86_64|i686]` compiles it into `build/ports` (needs `python3.14` on the build machine).
- `package.sh [32] [64]` packs that as a `.dsp`.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). CPython has its own licence, which is downloaded with its source and copied into the package.
