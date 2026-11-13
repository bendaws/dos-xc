#!/bin/sh
#
# DOS-XC /qt/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/ports-common.sh"
QT_VERSION=5.15.17
QT_URL=https://download.qt.io/archive/qt/5.15/$QT_VERSION/submodules
[ "$ARCH" = x86_64 ] || { echo "Qt is built for the x64 edition only; skipping $ARCH"; exit 0; }

# Static Qt (Core, Gui, Widgets, Svg) in the port sysroot for VLC's interface,
# with the host tools (qmake, moc, rcc, uic) in $WORK/qt5-host.
QT_PREFIX=$SYSROOT/qt5
QT_HOST=$WORK/qt5-host
fetch qtbase-everywhere-opensource-src-$QT_VERSION.tar.xz $QT_URL/qtbase-everywhere-opensource-src-$QT_VERSION.tar.xz
fetch qtsvg-everywhere-opensource-src-$QT_VERSION.tar.xz $QT_URL/qtsvg-everywhere-opensource-src-$QT_VERSION.tar.xz
unpack qtbase-everywhere-opensource-src-$QT_VERSION.tar.xz qtbase
patch -d "$WORK/qtbase" -p1 < "$HERE/dosxc.patch"
cp -R "$HERE/mkspecs/dosxc-g++" "$WORK/qtbase/mkspecs/"
sed -i.orig 's/^win32 {/win32|dosxc {/' "$WORK/qtbase/config.tests/arch/write_info.pri"
if [ -d "$HERE/qpa" ]; then
    mkdir -p "$WORK/qtbase/src/plugins/platforms/dosxc"
    cp "$HERE"/qpa/* "$WORK/qtbase/src/plugins/platforms/dosxc/"
    cp "$ROOT"/desktop/gfx.c "$ROOT"/desktop/gui.c "$ROOT"/desktop/widgets.c "$ROOT"/desktop/dialogs.c "$ROOT"/desktop/image.c \
       "$ROOT"/desktop/*.h "$ROOT"/os/pkg/inflate.c "$ROOT"/os/pkg/*.h "$WORK/qtbase/src/plugins/platforms/dosxc/"
    printf 'SUBDIRS += dosxc\n' >> "$WORK/qtbase/src/plugins/platforms/platforms.pro"
    platform=dosxc
else
    platform=minimal
fi
cd "$WORK/qtbase"
run_logged qt-configure ./configure -prefix /Programs/Qt -extprefix "$QT_PREFIX" -hostprefix "$QT_HOST" \
    -xplatform dosxc-g++ -device-option CROSS_COMPILE="$ROOT/toolchain/cross/bin/$TARGET-" -sysroot "$SYSROOT" \
    -static -release -opensource -confirm-license -nomake examples -nomake tests -no-pch -no-opengl -no-dbus \
    -no-icu -no-glib -no-fontconfig -qt-freetype -qt-zlib -qt-libpng -qt-libjpeg -qt-pcre -qt-harfbuzz \
    -no-openssl -no-cups -no-iconv -no-evdev -no-libinput -no-mtdev -no-tslib -no-xcb -no-eglfs -no-gbm -no-kms \
    -no-linuxfb -no-directfb -no-vulkan -no-feature-sharedmemory -no-feature-systemsemaphore \
    -no-feature-dnslookup -no-feature-bearermanagement -no-feature-vnc -no-feature-tuiotouch -qpa $platform
run_logged qt-make make -j"$JOBS"
run_logged qt-install make install
unpack qtsvg-everywhere-opensource-src-$QT_VERSION.tar.xz qtsvg
cd "$WORK/qtsvg"
run_logged qtsvg-qmake "$QT_HOST/bin/qmake"
run_logged qtsvg-make make -j"$JOBS"
run_logged qtsvg-install make install
touch "$SYSROOT/lib/.qt-$QT_VERSION"
echo "Qt $QT_VERSION -> $QT_PREFIX"
