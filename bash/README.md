# Bash for DOS-XC

Builds GNU Bash 5.3 as the `Bash` package (`BASH` and `SH`) for the x64 edition. Bash needs `fork()`, which the x86 edition does not have.

- `build.sh [x86_64]` compiles it into `build/ports`.
- `package.sh [64]` packs that as a `.dsp`.

`dosxc.patch` teaches Bash DOS-XC paths: `C:/DIR` is absolute, `PATH` may use `;` (and `:` other than a drive's), programs are found as `NAME.EXE`, batch files and 16-bit programs run through `COMMAND.EXE /C`, and `#!/bin/sh` or `#!/usr/bin/env` interpreters that do not exist are looked for on `PATH`. `bashrc` is read by every interactive shell (`BASH_SYSTEM_RC`); it puts the GNU tools first on `PATH` when the `gnu` package is installed.

Job control (`fg`, `bg`, Ctrl+Z) and process substitution are not available.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). Bash is GPL-3.0-or-later; its licence is downloaded with its source and copied into the package.
