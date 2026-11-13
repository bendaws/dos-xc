# GNU tools for DOS-XC

Builds the everyday Unix command-line tools as the `gnu` package:

| Project | Version | Programs |
|---------|---------|----------|
| coreutils | 9.7 | `ls`, `cp`, `mv`, `rm`, `cat`, `head`, `tail`, `sort`, `wc`, `chmod`, `date`, `env`, `sha256sum` and the rest |
| grep | 3.12 | `grep` |
| sed | 4.9 | `sed` |
| gawk | 5.3.2 | `gawk`, `awk` |
| findutils | 4.10.0 | `find`, `xargs` |
| diffutils | 3.12 | `diff`, `cmp`, `diff3`, `sdiff` |
| tar | 1.35 | `tar` |
| gzip | 1.14 | `gzip`, `gunzip`, `zcat` |
| make | 4.4.1 | `make` |
| patch | 2.8 | `patch` |
| less | 679 | `less` |

- `build.sh [x86_64|i686]` compiles them into `build/ports` (`TOOLS="sed grep"` builds only some).
- `package.sh [32] [64]` packs that as a `.dsp`.

The package folder is on `PATH` after the system folder, so DOS commands keep their names in the command shell (`DIR`, `MKDIR` ...). Bash puts the GNU tools first (`GNU_TOOLS`). `dir`, `vdir` and the programs about logins, `chroot` or process priorities are left out. Shell scripts such as `gunzip` need the `Bash` package.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). The tools are GPL-3.0-or-later (less: GPL or the Less Licence); their licences are in the package's `LICENSES` folder.
