/*
 * DOS-XC C library - termcap.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * There is no terminal database: tgetent() reports that no entry exists,
 * so programs fall back to their built-in descriptions (the console
 * understands ANSI escape sequences; TERM is "ansi").  tgoto() and tputs()
 * work for capability strings a program has itself.
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
