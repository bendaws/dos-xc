#!/bin/sh
#
# DOS-XC /vlc/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
VLC_VERSION=3.0.21
[ "$ARCH" = x86_64 ] || { echo "VLC is built for the x64 edition only; skipping $ARCH"; exit 0; }

[ -f "$SYSROOT/lib/.ffmpeg-6.1.2" ] || "$ROOT/ffmpeg/build.sh" "$ARCH"
[ -f "$SYSROOT/lib/.qt-5.15.17" ] || "$ROOT/qt/build.sh" "$ARCH"
QT=$SYSROOT/qt5
desktop=$ROOT/desktop
fetch vlc-$VLC_VERSION.tar.xz https://get.videolan.org/vlc/$VLC_VERSION/vlc-$VLC_VERSION.tar.xz
# VLC_RELINK=1 only links VLC.EXE again, against the libraries already built.
[ -n "${VLC_RELINK:-}" ] || { unpack vlc-$VLC_VERSION.tar.xz vlc; patch -d "$WORK/vlc" -p1 < "$HERE/dosxc.patch"; }
cd "$WORK/vlc"
[ -n "${VLC_RELINK:-}" ] || {
# libvlc with its modules linked in (no dynamic plugins), decoding through
# FFmpeg, with the Qt interface; dosxc_vout.c and dosxc_aout.c are the video
# and sound outputs.
run_logged vlc-configure env PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$QT/lib/pkgconfig" PKG_CONFIG_PATH= \
    ac_cv_func_fork=yes ac_cv_lib_dl_dlopen=no ac_cv_header_dlfcn_h=no \
    ./configure --build="$BUILD_TRIPLE" --host=$TARGET --prefix=/Programs/VLC \
    --disable-shared --enable-static --disable-vlc --disable-nls --disable-rpath --disable-update-check \
    --disable-lua --enable-qt --disable-skins2 --disable-xcb --disable-xvideo --disable-wayland --disable-dbus \
    --disable-alsa --disable-pulse --disable-jack --disable-oss --disable-sndio --disable-a52 --disable-dca \
    --disable-libgcrypt --disable-gnutls --disable-mad --disable-mkv --disable-taglib --disable-vlm \
    --disable-sout --disable-addonmanagermodules --disable-srt --disable-dvdread --disable-dvdnav --disable-bluray \
    --disable-smbclient --disable-dsm --disable-sftp --disable-nfs --disable-v4l2 --disable-decklink --disable-vcd \
    --disable-libcddb --disable-screen --disable-vnc --disable-freerdp --disable-asdcp --disable-dc1394 \
    --disable-dv1394 --disable-linsys --disable-live555 --disable-gme --disable-sid --disable-ogg --disable-shout \
    --disable-mod --disable-mpc --disable-shine --disable-omxil --disable-crystalhd --disable-libva --disable-vdpau \
    --disable-mmal --disable-avahi --disable-udev --disable-mtp --disable-upnp --disable-microdns --disable-notify \
    --disable-kwallet --disable-secret --disable-libxml2 --disable-freetype --disable-fribidi --disable-harfbuzz \
    --disable-fontconfig --disable-svg --disable-svgdec --disable-aribsub --disable-telx --disable-zvbi --disable-kate \
    --disable-tiger --disable-libass --disable-chromecast --disable-goom --disable-projectm --disable-vsxu \
    --disable-gst-decode --disable-x264 --disable-x26410b --disable-x265 --disable-fluidsynth --disable-fluidlite \
    --disable-flac --disable-vorbis --disable-opus --disable-speex --disable-theora --disable-daala \
    --disable-schroedinger --disable-png --disable-jpeg --disable-bpg --disable-twolame --disable-fdkaac \
    --disable-vpx --disable-aom --disable-dav1d --disable-spatialaudio --disable-samplerate --disable-soxr \
    --disable-libplacebo --disable-gles2 --disable-glspectrum --disable-caca --disable-postproc --disable-libmpeg2 \
    --disable-mpg123 --disable-faad --disable-vpl --disable-archive --disable-libtar --disable-dvbpsi --disable-sparkle \
    --disable-lirc --disable-ncurses --disable-macosx --disable-gles1 --disable-egl --disable-opencv --disable-orc \
    --disable-mfx --disable-medialibrary --disable-wasapi --disable-dsound --disable-kai --disable-evas \
    --disable-sdl-image --disable-libsecret --without-x --enable-avcodec --enable-avformat --enable-swscale \
    CC="$CC_PORT" CXX="$TARGET-g++ --sysroot=$SYSROOT" CFLAGS="-O2" CXXFLAGS="-O2 -fPIC"
run_logged vlc-make make -j"$JOBS"
}

mods=$WORK/vlc-dosxc
rm -rf "$mods"
mkdir -p "$mods"
for m in vout aout; do
    $CC_PORT -O2 -c -DHAVE_CONFIG_H -I. -Iinclude -I"$desktop" -DMODULE_STRING=\"dosxc_$m\" -DMODULE_NAME=dosxc_$m \
        "$HERE/dosxc_$m.c" -o "$mods/dosxc_$m.o"
    $TARGET-ar rcs "$mods/libdosxc_${m}_plugin.a" "$mods/dosxc_$m.o"
done

# Every module is linked in: libvlccore finds them through vlc_static_modules.
# Their entry points come from the archives (puzzle and visual clash with the
# desktop library's symbols).
modules=
entries=$WORK/vlc-modules.c
printf '#include <stddef.h>\n' > "$entries"
for archive in modules/.libs/lib*_plugin.a "$mods"/lib*_plugin.a; do
    case "$archive" in *puzzle*|*visual*) continue ;; esac
    entry=$($TARGET-nm "$archive" | awk '$2 == "T" && $3 ~ /^vlc_entry__/ {print $3; exit}')
    [ -n "$entry" ] || continue
    printf 'int %s(int (*)(void *, void *, int, ...), void *);\n' "$entry" >> "$entries"
    modules="$modules $archive"
    names="${names:-} $entry"
done
printf 'void *vlc_static_modules[] = {\n' >> "$entries"
for entry in $names; do printf '    (void *)%s,\n' "$entry" >> "$entries"; done
printf '    NULL\n};\n' >> "$entries"
rm -rf "$OUT/VLC"
mkdir -p "$OUT/VLC"
run_logged vlc-link $CC_PORT -O2 -I"$WORK/vlc/include" "$HERE/main.c" "$entries" -o "$OUT/VLC/VLC.EXE" \
    -L"$QT/lib" -L"$QT/plugins/platforms" -L"$QT/plugins/iconengines" -L"$QT/plugins/imageformats" \
    -Wl,--start-group lib/.libs/libvlc.a src/.libs/libvlccore.a $modules modules/.libs/libavcodec_common.a \
    $(ls modules/.libs/lib*.a | grep -v '_plugin.a' | grep -v libavcodec_common) compat/.libs/libcompat.a \
    -lqdosxc -lqsvgicon -lqsvg -lQt5Svg -lQt5Widgets -lQt5Gui -lQt5EventDispatcherSupport -lQt5FontDatabaseSupport \
    -lQt5Core -lqtfreetype -lqtharfbuzz -lqtlibpng -lqtpcre2 -Wl,--end-group \
    -L"$SYSROOT/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil -lssl -lcrypto -lz -lstdc++ -lpthread -lm
"$TARGET-strip" "$OUT/VLC/VLC.EXE"
cp COPYING "$OUT/VLC/COPYING.TXT"
cp share/icons/16x16/vlc.png "$OUT/VLC/ICON.PNG"
mkdir -p "$OUT/VLC/FONTS"
for font in DejaVuSans DejaVuSans-Bold DejaVuSans-Oblique DejaVuSansMono; do
    cp "$ROOT/toolchain/work/dejavu-fonts-ttf-2.37/ttf/$font.ttf" "$OUT/VLC/FONTS/"
done
echo "VLC $VLC_VERSION -> $OUT/VLC"
