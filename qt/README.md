# Qt for DOS-XC

Builds Qt 5.15.17 (qtbase and qtsvg) as static libraries in the port sysroot, for programs such as VLC. It is not a package of its own: a Qt program links what it uses.

- `build.sh [x86_64]` builds it into `toolchain/work/ports-x86_64/sysroot/qt5`, with the host tools (`qmake`, `moc`, `rcc`, `uic`) in `toolchain/work/ports-x86_64/qt5-host`.

`mkspecs/dosxc-g++` is the qmake target, `dosxc.patch` teaches Qt the `Q_OS_DOSXC` system, and `qpa/` is the `dosxc` platform plugin, which puts Qt's windows on the DOS-XC desktop:

- Each top-level window is a desktop window; menus, tooltips and other frameless windows are borderless ones placed by the program, and full screen is a borderless window over the whole screen.
- Showing or raising a popup changes its stacking without taking keyboard focus. Hidden popup windows are reused and raised again on every show.
- Native child windows (`winId()`) are areas of their top-level that another thread may draw into (`desktop/embed.h`); VLC's video uses this.
- Fonts come from a `FONTS` folder beside the program and from `\SYSTEM\FONTS`; the default is DejaVu Sans 9 pt. Text on the clipboard is shared with other programs.

A qmake project picks the plugin with `QTPLUGIN.platforms = qdosxc`; other programs link `plugins/platforms/libqdosxc.a`, use `Q_IMPORT_PLUGIN(QDosxcIntegrationPlugin)` and set `QT_QPA_PLATFORM=dosxc`. OpenGL, D-Bus, printing to CUPS, ICU and fontconfig are left out.

## Licence

The build scripts, mkspec and platform plugin in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). Qt is LGPL-3.0; a program that links it statically must let its users relink it with another Qt.
