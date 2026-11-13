# FFmpeg for DOS-XC

Builds FFmpeg 6.1.2 as the `ffmpeg` package (`ffmpeg` and `ffprobe`), with zlib and OpenSSL for compressed formats and `https://` inputs. Its static libraries also go into the port sysroot for VLC.

- `build.sh [x86_64|i686]` compiles it into `build/ports` and the port sysroot.
- `package.sh [64|32]` packs that as a `.dsp`.

Hardware decoders (VA-API, VDPAU) and the capture devices (Video4Linux, framebuffer) are left out, as is `ffplay`; play files in VLC. The x64 build keeps FFmpeg's SIMD assembly; the 32-bit build uses plain C.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). FFmpeg is built as LGPL-3.0 (`--enable-version3`, without the GPL parts); its licence is copied into the package.
