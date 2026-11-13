# VLC media player for DOS-XC

Builds VLC 3.0.21 as the `vlc` package for the x64 edition, with VLC's own Qt interface (Qt 5.15, from `qt/`) and FFmpeg 6.1.2 for decoding.

- `build.sh [x86_64]` compiles it into `build/ports` (building FFmpeg and Qt first if needed). `VLC_RELINK=1 build.sh` only links `VLC.EXE` again.
- `package.sh [64]` packs that as a `.dsp`.

The package uses VLC's upstream cone icon for its menu entry and desktop windows. When included in the system build, VLC is available on the live desktop and selected by default in Setup.

`VLC.EXE` is one static program: `main.c` starts libvlc and its interface as VLC's `vlc` program does, and every module is linked in through the `vlc_static_modules` table that `build.sh` writes. Two modules are DOS-XC's own:

- `dosxc_vout.c` draws the video in the interface's video area (the Qt platform plugin hands it over as an `embed_window`, see `desktop/embed.h`). VLC's core scales the pictures with swscale.
- `dosxc_aout.c` plays the sound through `DOSXC_SYS_AUDIO`. The sound device only runs at 48 kHz, so other rates are resampled there.

`dosxc.patch` lets VLC read drive paths (`X:\Videos\a.mp4`) and use the `dosxc` Qt platform. Lua scripts, subtitles drawn with FreeType, streaming output and disc playback are left out.

The embedded video widget uses one native child surface; its ancestors and the playback controls stay in Qt's backing store.

## Licence

The build scripts and DOS-XC modules in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). VLC is GPL-2.0 (libvlc is LGPL-2.1); its licence is copied into the package.
