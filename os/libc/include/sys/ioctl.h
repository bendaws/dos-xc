/*
 * DOS-XC /os/libc/include/sys/ioctl.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
#include <sys/cdefs.h>
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD 0x541b
#define TIOCINQ FIONREAD
#define FIONBIO 0x5421
#define FIOCLEX 0x5451
#define FIONCLEX 0x5450
__BEGIN_DECLS
int ioctl(int, unsigned long, ...);
__END_DECLS
#endif
