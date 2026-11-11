/*
 * DOS-XC /coreutils/help.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(void) {
    puts("Shell commands:\n"
         "  CD [path]            Show or change the current folder     D:  switch drive\n"
         "  SET [name=value]     Environment variables                 PATH [folders]\n"
         "  ECHO [ON|OFF|text]   EXIT [code]   HISTORY   REM\n"
         "\n"
         "Commands (coreutils package):\n"
         "  DIR [/B /W /P /S /A[:D|-D] /O:N|S|-N|-S] [path]   List files\n"
         "  TYPE file...         Display text files          MORE [file]    Page through text\n"
         "  COPY [/Y] src dst    Copy files (wildcards)      MOVE [/Y] src dst\n"
         "  REN src name         Rename                      DEL [/P /Q] pattern\n"
         "  MD path              Make a folder               RD path        Remove an empty folder\n"
         "  EDIT file            Text editor                 WRITE [/A] file text\n"
         "  CLS   COLOR [bf]     VER   BEEP [hz] [ms]        SPEED [1..100] 16-bit CPU duty\n"
         "  EJECT drive:         Unmount a USB drive before pulling it out\n"
         "  SHUTDOWN             Switch the computer off (also the power button)\n"
         "  RESTART              Restart the computer (REBOOT does the same)\n"
         "  DESKTOP              Start the graphical desktop (F12 leaves it)\n"
         "\n"
         "Network: IPCONFIG, PING host, DRIVERS\n"
         "Packages: PKG LIST, PKG INSTALL file.dsp, PKG REMOVE name, PKG HELP\n"
         "Type a program name to run an .EXE, .COM or .BAT file.  Most commands accept /?.\n"
         "Command lines may use > >> < 2> | && || and %VARIABLES%.");
    return 0;
}
