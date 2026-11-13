/*
 * DOS-XC /os/libc/include/signal.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SIGNAL_H
#define _SIGNAL_H
#define __NEED_pid_t
#define __NEED_uid_t
#define __NEED_size_t
#define __NEED_sigset_t
#define __NEED_struct_timespec
#include <sys/_types.h>

typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGIOT SIGABRT
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL SIGIO
#define SIGPWR 30
#define SIGSYS 31
#define NSIG 32
#define _NSIG NSIG
#define SIGRTMIN 34
#define SIGRTMAX 64
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SI_USER 0
#define SI_KERNEL 0x80
#define SI_QUEUE (-1)
#define SI_TIMER (-2)
#define ILL_ILLOPC 1
#define ILL_ILLOPN 2
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8
#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define BUS_OBJERR 3
#define TRAP_BRKPT 1
#define TRAP_TRACE 2
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

union sigval { int sival_int; void *sival_ptr; };
/* si_addr stays where Linux has it: Rust reads the struct by that layout. */
typedef struct { int si_signo, si_errno, si_code; void *si_addr; pid_t si_pid; uid_t si_uid; int si_status; union sigval si_value; } siginfo_t;
struct sigaction {
    union { void (*sa_handler)(int); void (*sa_sigaction)(int, siginfo_t *, void *); } __handler;
    sigset_t sa_mask;
    int sa_flags;
};
#define sa_handler __handler.sa_handler
#define sa_sigaction __handler.sa_sigaction

typedef struct { void *ss_sp; int ss_flags; size_t ss_size; } stack_t;
#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192
#include <sys/ucontext.h>

__BEGIN_DECLS
int sigaltstack(const stack_t *__restrict, stack_t *__restrict);
void (*signal(int, void (*)(int)))(int);
int raise(int);
int kill(pid_t, int);
int killpg(pid_t, int);
int sigaction(int, const struct sigaction *__restrict, struct sigaction *__restrict);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
int sigprocmask(int, const sigset_t *__restrict, sigset_t *__restrict);
int pthread_sigmask(int, const sigset_t *__restrict, sigset_t *__restrict);
int sigpending(sigset_t *);
int sigsuspend(const sigset_t *);
int sigwait(const sigset_t *, int *);
void psignal(int, const char *);
extern const char *const sys_siglist[];
__END_DECLS
#endif
