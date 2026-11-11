/*
 * DOS-XC /os/libc/include/termcap.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _TERMCAP_H
#define _TERMCAP_H
#include <sys/cdefs.h>
__BEGIN_DECLS
extern char PC, *UP, *BC;
extern short ospeed;
int tgetent(char *, const char *);
int tgetflag(const char *);
int tgetnum(const char *);
char *tgetstr(const char *, char **);
char *tgoto(const char *, int, int);
int tputs(const char *, int, int (*)(int));
__END_DECLS
#endif
