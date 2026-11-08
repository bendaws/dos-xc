/*
 * DOS-XC C library - sys/wait.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#define __NEED_pid_t
#include <sys/_types.h>
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#define WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define WTERMSIG(s) ((s) & 0x7f)
#define WSTOPSIG(s) WEXITSTATUS(s)
#define WIFEXITED(s) (!WTERMSIG(s))
#define WIFSIGNALED(s) (((s) & 0xffff) - 1U < 0xffu)
#define WIFSTOPPED(s) ((short)((((s) & 0xffff) * 0x10001) >> 8) > 0x7f00)
#define WIFCONTINUED(s) ((s) == 0xffff)
#define WCOREDUMP(s) ((s) & 0x80)
struct rusage;
__BEGIN_DECLS
pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);
pid_t wait3(int *, int, struct rusage *);
pid_t wait4(pid_t, int *, int, struct rusage *);
__END_DECLS
#endif
