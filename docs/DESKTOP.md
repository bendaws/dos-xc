# The desktop

DOS-XC starts on the graphical desktop when the `DOS-XC Desktop` package is
installed, and at the command prompt when it is not.  Tap or hold **Esc** while
the system starts to stay at the prompt for that boot; `DESKTOP` starts it from
the prompt.  It looks like SerenityOS: the same theme colors and bitmap fonts, gradient title bars,
beveled controls, and a taskbar along the bottom of the main monitor.  The
fonts are used under the BSD 2-Clause license (`src/desktop/LICENSE.SerenityOS`).
**F12** leaves the desktop at any time, even without a mouse, and ends the
programs running on it.

The desktop needs a mouse: a USB one on a BIOS boot (there is no PS/2 driver),
or whatever the firmware provides under UEFI.

## Programs

The desktop is the package `DOS-XC Desktop`, in `\Programs\DOS-XC Desktop`.  Each
program runs as its own process, from the package's `Apps` folder, and can be left
running beside the others.  The Start menu sorts them into System, Utilities,
Games and Settings.

| Program | File | Notes |
| --- | --- | --- |
| File Explorer | `EXPLORER.EXE` | Navigation pane, address bar, search, details view and pane. Right-click for copy, paste, rename, delete and Set as wallpaper |
| File Editor | `EDITOR.EXE` | Plain text. Ctrl+S, Ctrl+O, Ctrl+N, Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V |
| Calculator | `CALC.EXE` | Mouse or keyboard |
| Paint | `PAINT.EXE` | Opens BMP, PNG and JPEG; saves BMP |
| Task Manager | `TASKMGR.EXE` | Applications, Processes and Performance tabs |
| DOS Prompt | `TERMINAL.EXE` | `COMMAND.EXE` in a window: 80 x 25, 500 lines of scrollback |
| Solitaire | `SOLITAIRE.EXE` | Klondike, draw one |
| Minesweeper | `MINES.EXE` | Three sizes; right-click flags |
| Chess | `CHESS.EXE` | Against the computer, three levels |
| Package Manager | `PKGMGR.EXE` | Installed and available packages; runs `PKG` for every change and shows its output |
| Install DOS-XC | `SETUPGUI.EXE` | The same steps as `SETUP`; in the Start menu on the live system only |

Double-clicking a file in File Explorer opens it in the matching program.  A
program that is not one of these opens in a terminal window.

## 16-bit programs

A 16-bit program started in a terminal window runs in `DOSVM.EXE`, a virtual PC
(`src/vdm`): a 386 real-mode interpreter with a VGA, the BIOS and DOS services.
The desktop keeps running around it.  A program that only prints stays part of
the terminal's text; one that sets a video mode or writes to the screen takes over
the window, which resizes to fit, until it exits.  CGA, EGA, VGA and mode 13h
graphics, the PC speaker, the keyboard and the mouse work.  It is built to run
under UEFI too, where real-mode programs cannot, but that has not been tested.
There is no floating-point unit and no protected mode, so programs that need a DOS extender do not run.  Outside the
desktop, at the prompt, 16-bit programs still run directly on the hardware.

## Taskbar

On the right are the sound volume, the network status and the date and time.
Click the volume to change it, or the network icon for the adapter and address.
The only sound device is the PC speaker, which has no volume of its own: 0 mutes
it and any other level plays at full volume.

## Accent color

Settings > Accent Color picks the color of title bars, selections and menus from
twelve presets or with red, green and blue sliders.  Every running program follows
at once.  A light color is darkened where white text is drawn on it.  The choice
is kept in `%HOME%\DESKTOP.CFG`.

## Wallpaper

The Wallpaper window lists the pictures in the package's `Wallpapers` folder and
`%HOME%\Wallpapers`.  **Add...** copies a picture into the second folder.  BMP,
PNG (not interlaced) and JPEG (baseline and progressive) can be used.  The choice
is kept in `%HOME%\DESKTOP.CFG`.

## Monitors

Display Settings shows each monitor on a map.  Drag them to arrange them, step
through resolutions with `<` and `>`, choose where the taskbar goes with
**Make main**, and press **Apply**.  **Identify** shows each monitor's number.
The arrangement is kept in `%HOME%\DISPLAYS.CFG`.

**Scale** enlarges everything to 125% or 150%, as long as at least 640 x 480 of
desktop remains.  The whole desktop is drawn at the smaller size and enlarged when
it reaches the screen, so every program scales without knowing about it.

| Hardware | Monitors |
| --- | --- |
| QEMU/Bochs adapters (`VGA`, `secondary-vga`, `bochs-display`) | One per adapter, up to 8 |
| Other graphics card, BIOS boot | One, through VESA BIOS Extensions (32-bit modes with a linear framebuffer) |
| Other graphics card, UEFI boot | One, through the firmware's GOP (BGRX modes only) |

A real graphics card shows only one monitor because the firmware interfaces
drive only one.  More would need a native driver for each card family.

`DISPLAYS=2 scripts/boot64.sh` starts QEMU with two monitors (up to 4); switch
between them in QEMU's View menu.

## How it fits together

`DESKTOP.EXE` is the compositor and window manager.  It publishes a board in
shared memory (`DOSXC_TASK_SHARE_SET`); each program claims a slot and exchanges
one-way rings of events and requests through it (`src/desktop/gui.c`).  All
programs share one address space, so a window's pixels are read straight from
the program that draws them.  The fonts in `src/desktop/fonts.h` are generated
by `tools/make_fonts.py`.  See "Graphics" and "Multitasking" in
[PROGRAMS.md](PROGRAMS.md).
