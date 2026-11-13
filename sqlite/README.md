# SQLite for DOS-XC

Builds the SQLite 3.50.4 command-line shell as the `SQLite` package (`SQLITE3`), with the database engine linked in. Loadable extensions are left out.

- `build.sh [x86_64|i686]` compiles it into `build/ports`.
- `package.sh [32] [64]` packs that as a `.dsp`.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). SQLite is in the public domain.
