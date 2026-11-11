/*
 * DOS-XC /os/libc/include/unistd.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _UNISTD_H
#define _UNISTD_H
#define __NEED_size_t
#define __NEED_ssize_t
#define __NEED_off_t
#define __NEED_pid_t
#define __NEED_uid_t
#define __NEED_suseconds_t
#define __NEED_intptr_t
#include <sys/_types.h>

#define _POSIX_VERSION 200809L
#define _POSIX_THREADS 200809L
#define _POSIX_THREAD_ATTR_STACKSIZE 200809L
#define _POSIX_TIMEOUTS 200809L
#define _POSIX_SEMAPHORES 200809L
#define _POSIX_READER_WRITER_LOCKS 200809L
#define _POSIX_SPIN_LOCKS 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L
#define _POSIX_BARRIERS 200809L
#define _POSIX2_VERSION 200809L
#define _XOPEN_VERSION 700
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_NGROUPS_MAX 3
#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _SC_AVPHYS_PAGES 86
#define _SC_LINE_MAX 43
#define _SC_HOST_NAME_MAX 180
#define _SC_IOV_MAX 60
#define _SC_GETPW_R_SIZE_MAX 70
#define _SC_TTY_NAME_MAX 72
#define _PC_LINK_MAX 0
#define _PC_MAX_CANON 1
#define _PC_MAX_INPUT 2
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5

__BEGIN_DECLS
int access(const char *, int);
unsigned alarm(unsigned);
int chdir(const char *);
int fchdir(int);
int chown(const char *, uid_t, gid_t);
int fchown(int, uid_t, gid_t);
int lchown(const char *, uid_t, gid_t);
int close(int);
int dup(int);
int dup2(int, int);
int dup3(int, int, int);
int execl(const char *, const char *, ...);
int execle(const char *, const char *, ...);
int execlp(const char *, const char *, ...);
int execv(const char *, char *const[]);
int execve(const char *, char *const[], char *const[]);
int execvp(const char *, char *const[]);
int execvpe(const char *, char *const[], char *const[]);
void _exit(int) __noreturn;
pid_t fork(void);
pid_t vfork(void);
long fpathconf(int, int);
long pathconf(const char *, int);
char *getcwd(char *, size_t);
char *get_current_dir_name(void);
pid_t gettid(void);
gid_t getegid(void);
uid_t geteuid(void);
gid_t getgid(void);
int getgroups(int, gid_t[]);
int gethostname(char *, size_t);
int getentropy(void *, size_t);
char *getlogin(void);
int getlogin_r(char *, size_t);
int getopt(int, char *const[], const char *);
extern char *optarg;
extern int optind, opterr, optopt;
extern char **environ;
int getpagesize(void);
long syscall(long, ...);
pid_t getpgrp(void);
pid_t getpid(void);
pid_t getppid(void);
pid_t getsid(pid_t);
uid_t getuid(void);
int isatty(int);
int link(const char *, const char *);
off_t lseek(int, off_t, int);
int nice(int);
int pause(void);
int pipe(int[2]);
int pipe2(int[2], int);
ssize_t pread(int, void *, size_t, off_t);
ssize_t pwrite(int, const void *, size_t, off_t);
ssize_t read(int, void *, size_t);
ssize_t readlink(const char *__restrict, char *__restrict, size_t);
int rmdir(const char *);
void *sbrk(intptr_t);
int setgid(gid_t);
int setpgid(pid_t, pid_t);
pid_t setsid(void);
int setuid(uid_t);
unsigned sleep(unsigned);
int symlink(const char *, const char *);
void sync(void);
int fsync(int);
int fdatasync(int);
long sysconf(int);
pid_t tcgetpgrp(int);
int truncate(const char *, off_t);
int ftruncate(int, off_t);
char *ttyname(int);
int ttyname_r(int, char *, size_t);
int unlink(const char *);
int usleep(useconds_t);
ssize_t write(int, const void *, size_t);
int lockf(int, int, off_t);
char *getpass(const char *);
void swab(const void *from, void *to, ssize_t n);
__END_DECLS
#endif
