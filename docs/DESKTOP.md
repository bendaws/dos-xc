# The desktop

DOS-XC starts on the graphical desktop when the `DOS-XC Desktop` package is
installed, and at the command prompt when it is not.  Tap or hold **Esc** while
the system starts to stay at the prompt for that boot; `DESKTOP` starts it from
the prompt.  It looks like SerenityOS: the same theme colors and bitmap fonts, gradient title bars,
beveled controls, and a taskbar along the bottom of the main monitor.  The
fonts are used under the BSD 2-Clause license (`desktop/LICENSE.SerenityOS`).
**F12** leaves the desktop at any time, even without a mouse, and ends the
programs running on it.

The desktop needs a mouse: a USB one on a BIOS boot (there is no PS/2 driver),
or whatever the firmware provides under UEFI.

## Programs

The desktop is the package `DOS-XC Desktop`, in `\Programs\DOS-XC Desktop`.  Each
program runs as its own process, from the package's `Apps` folder, and can be left
running beside the others.  `DESKTOP.EXE PROGRAM [ARGS]` starts a program on the
desktop as it opens.

The Start menu, from the top:

1. The default web browser, when one is chosen and installed: DOSfox (the
   `dosfox` package, `\Programs\dosfox\DOSFOX.EXE`, with the logo it ships as
   `DOSFOX.PNG`; its windows use the logo too).
2. The default text editor: File Editor, or Vim in a terminal window when the
   `Vim` package is installed and chosen.
3. System (with its Administrators folder: Package Manager, Event Logs,
   Firewall and other programs that need an administrator), Utilities and
   Games, then a folder for each category that
   installed packages use (Internet, Office, ...; see
   [PACKAGES.md](PACKAGES.md#start-menu-entries)).
4. Control Panel and About DOS-XC.
5. Exit to DOS, Restart and Shut Down.  Restart and Shut Down ask first.

The menu is built each time it opens, so installing or removing a package shows
at once.

## User accounts

User accounts are optional: Setup asks whether to use them and creates the
first administrator.  Without accounts everyone is the one public user, with
`C:\Users\Public` as home.  With them, the computer starts at a login screen
(on the desktop when the `DOS-XC Desktop` package is installed, as text
otherwise): a row of users with their nicknames and pictures.  Click one, or move
with Tab or the arrow keys and press Enter, to type the password.  After logging
in the user gets the desktop or the command prompt, as they chose.  The Start
menu shows the user's picture and nickname at the top and **Log Out** beside
Restart and Shut Down; at the prompt, `LOGOUT` at the first command prompt does
the same.  Logging out ends the user's programs.

Each user has a user name (letters and digits), a password, a nickname (any
printable ASCII), a square picture (nine are in the package's `Pictures` folder,
or any picture file), a home folder (`C:\Users\<name>` unless an administrator
chose another) and their own desktop settings there.  Control Panel > User
Accounts changes one's own picture, nickname, password and start mode;
administrators also add and remove users, make or unmake administrators, set
home folders, and turn accounts on or off.  There is always at least one
administrator.

Only administrators may write `\SYSTEM` (and `\SYSTEM\Drivers`), `\Programs`,
`\Packages`, `\EFI` and `\AUTOEXEC.BAT`, open other users' home folders, read the
account database (`\SYSTEM\USERS.DB`: salted, iterated SHA-256 password hashes),
use raw disk access, load drivers, or run 16-bit programs directly on the hardware
(other users can run them in a terminal window, through `DOSVM.EXE`).  So only
administrators can install packages.  The rules are enforced by the kernel on the
system drive; they hold against programs only where programs are isolated
(64-bit BIOS boots, see `docs/PROGRAMS.md`).

## Control Panel

Every setting of the desktop in one window: Display (screens, resolution,
scale), Wallpaper, Accent Color, Sound, Network (the adapter and its address),
Default Programs (the web browser, or none, and the text editor) and Packages
(the Package Manager).  The settings are kept in `%HOME%\DESKTOP.CFG`.

## Icons and shortcuts on the desktop

The desktop shows Computer (File Explorer) and everything in `%HOME%\Desktop`,
in columns from the top left of the main screen.  Click to select, double-click
or Enter to open; F2 renames and Delete deletes (after asking).  Files open in
the program for their type: pictures in Paint, `.EXE` files in a terminal window,
web pages in the default browser, everything else in the default editor.  The
folder is looked at every two seconds, so files put there by other programs show
up by themselves.

Right-click the desktop for New File..., New Folder..., New Shortcut...,
Refresh, File Explorer, Wallpaper..., Display Settings... and Control Panel;
right-click an icon to open, rename or delete it.  New Shortcut... offers the
desktop's programs (and DOSfox and Vim when installed) or, with Browse..., any
file, folder or program.  In File Explorer, Organize > Add shortcut to desktop
does the same for the selected item.

A shortcut is a text file `NAME.LNK` of `key=value` lines:

| Key | Meaning |
| --- | --- |
| `target` | The file, folder or program, with its drive |
| `args` | Arguments for a program (optional) |
| `icon` | A built-in icon (`folder`, `calc`, `editor`, `paint`, `dosfox`, ...) or a 16 x 16 PNG (optional; else `TARGET.PNG` beside a program, if there is one) |
| `terminal` | `1`: run the program in a terminal window |

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
| Event Logs | `EVENTS.EXE` | The system's event log (administrators): programs starting, ending and stopping on errors, drivers, devices, logins, account changes, power, firewall blocks, and what programs send with `syslog`.  Filter by level, save as text, clear.  The last 512 events are kept in memory until restart |
| Firewall | `FIREWALL.EXE` | Rules in `\SYSTEM\FIREWALL.CFG`, checked from the top, the first match deciding; unmatched traffic is blocked.  Click a rule's Action, Direction or Protocol to change it, double-click Ports or Program to edit them.  On by default, allowing DOSfox, curl, Python and PING out |
| Fonts | `FONTS.EXE` | System fonts in `\SYSTEM\FONTS` (used by DOSfox) and the desktop's built-in fonts, with previews; adds and removes font files.  Opening a `.ttf`, `.otf` or `.ttc` file shows it with Install This Font |
| Install DOS-XC | `SETUPGUI.EXE` | The same steps as `SETUP`; in the Start menu on the live system only |

Double-clicking a file in File Explorer opens it in the matching program.  A
program that is not one of these opens in a terminal window.

## 16-bit programs

A 16-bit program started in a terminal window runs in `DOSVM.EXE`, a virtual PC
(`desktop/vdm`): a 386 real-mode interpreter with a VGA, the BIOS and DOS services.
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

Control Panel > Accent Color picks the color of title bars, selections and menus from
twelve presets or with red, green and blue sliders.  Every running program follows
at once.  A light color is darkened where white text is drawn on it.  The choice
is kept in `%HOME%\DESKTOP.CFG`.

## Wallpaper

The Wallpaper window lists the pictures in the package's `Wallpapers` folder and
`%HOME%\Wallpapers`.  **Add...** copies a picture into the second folder.  BMP,
PNG (not interlaced) and JPEG (baseline and progressive) can be used.  The choice
is kept in `%HOME%\DESKTOP.CFG`.  The package's pictures are in
`desktop/wallpapers`; `.avif` ones there are turned into JPEG when the package is
built (with Python's Pillow, or `sips` on macOS), since DOS-XC cannot read AVIF.

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
one-way rings of events and requests through it (`desktop/gui.c`).  All
programs share one address space, so a window's pixels are read straight from
the program that draws them.  The fonts in `desktop/fonts.h` are generated
by `tools/make_fonts.py`.  See "Graphics" and "Multitasking" in
[PROGRAMS.md](PROGRAMS.md).
