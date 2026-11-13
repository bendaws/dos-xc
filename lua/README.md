# Lua for DOS-XC

Builds Lua 5.4.7 as the `Lua` package (`LUA` and `LUAC`), with the POSIX features on (`io.popen`, `os.tmpname` ...).

- `build.sh [x86_64|i686]` compiles it into `build/ports`.
- `package.sh [32] [64]` packs that as a `.dsp`.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). Lua is MIT; its licence is copied into the package.
