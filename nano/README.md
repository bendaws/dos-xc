# nano for DOS-XC

Builds GNU nano 8.6 as the `nano` package (`NANO`), linked statically against ncursesw 6.5 with the `ansi`, `vt100`, `vt220`, `xterm`, `xterm-256color` and `linux` terminals compiled in (DOS-XC has no terminfo database).

- `build.sh [x86_64|i686]` compiles it into `build/ports`.
- `package.sh [32] [64]` packs that as a `.dsp`.

`ETC/NANORC` (read from `\Programs\nano` on the current drive) turns on line numbers, the mouse and the bundled syntax colouring.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). nano is GPL-3.0-or-later and ncurses is MIT-style; nano's licence is copied into the package.
