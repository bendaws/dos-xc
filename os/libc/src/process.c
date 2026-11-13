/*
 * DOS-XC /os/libc/src/process.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- finished children ---- */
struct child { pid_t pid; int status; };
static struct child children[64];
static int child_count;
static pid_t next_child = 1000;

hidden int __record_child(int status) {
    if (child_count == 64) memmove(children, children + 1, sizeof(children[0]) * --child_count);
    pid_t pid = next_child++;
    if (next_child > 0x7fff0000) next_child = 1000;
    children[child_count].pid = pid;
    children[child_count++].status = status;
    return pid;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    for (int i = 0; i < child_count; ++i) {
        if (pid > 0 && children[i].pid != pid) continue;
        pid_t found = children[i].pid;
        if (status) *status = children[i].status;
        memmove(children + i, children + i + 1, sizeof(children[0]) * (size_t)(--child_count - i));
        return found;
    }
    int got = 0;
    long r = dosxc_sys_task(DOSXC_TASK_WAIT, &got, pid > 0 ? pid : -1, (options & WNOHANG) ? 1 : 0);
    if (r < 0) { errno = (int)-r; return -1; }
    if (r > 0 && status) *status = got;
    return (pid_t)r;
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }
pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage) {
    if (usage) memset(usage, 0, sizeof(*usage));
    return waitpid(pid, status, options);
}
pid_t wait3(int *status, int options, struct rusage *usage) { return wait4(-1, status, options, usage); }

/* ---- posix_spawn ---- */
enum { ACTION_CLOSE = 1, ACTION_DUP2, ACTION_OPEN };
struct __dosxc_spawn_action { int type, fd, target, flags; mode_t mode; char *path; };

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *a) {
    memset(a, 0, sizeof(*a));
    return 0;
}
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *a) {
    for (int i = 0; i < a->__count; ++i) free(a->__actions[i].path);
    free(a->__actions);
    memset(a, 0, sizeof(*a));
    return 0;
}
static int add_action(posix_spawn_file_actions_t *a, struct __dosxc_spawn_action action) {
    if (action.fd < 0 || action.fd >= OPEN_MAX || action.target < 0 || action.target >= OPEN_MAX) return EBADF;
    if (a->__count == a->__capacity) {
        int capacity = a->__capacity ? a->__capacity * 2 : 8;
        struct __dosxc_spawn_action *bigger = realloc(a->__actions, sizeof(*bigger) * (size_t)capacity);
        if (!bigger) return ENOMEM;
        a->__actions = bigger;
        a->__capacity = capacity;
    }
    a->__actions[a->__count++] = action;
    return 0;
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *a, int fd) {
    return add_action(a, (struct __dosxc_spawn_action){ACTION_CLOSE, fd, 0, 0, 0, 0});
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *a, int fd, int target) {
    return add_action(a, (struct __dosxc_spawn_action){ACTION_DUP2, fd, target, 0, 0, 0});
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *a, int fd, const char *path, int flags, mode_t mode) {
    char *copy = strdup(path);
    if (!copy) return ENOMEM;
    int r = add_action(a, (struct __dosxc_spawn_action){ACTION_OPEN, fd, 0, flags, mode, copy});
    if (r) free(copy);
    return r;
}
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *a, const char *path) {
    (void)a; (void)path;
    return ENOSYS;
}

int posix_spawnattr_init(posix_spawnattr_t *a) { memset(a, 0, sizeof(*a)); return 0; }
int posix_spawnattr_destroy(posix_spawnattr_t *a) { (void)a; return 0; }
int posix_spawnattr_getflags(const posix_spawnattr_t *a, short *flags) { *flags = (short)a->__flags; return 0; }
int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags) { a->__flags = flags; return 0; }
int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t group) { a->__pgroup = group; return 0; }
int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *mask) { a->__mask = *mask; return 0; }
int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *set) { a->__default = *set; return 0; }

struct saved_fd { int fd, copy, cloexec; };

static int save(struct saved_fd *saved, int *count, int fd) {
    for (int i = 0; i < *count; ++i)
        if (saved[i].fd == fd) return 0;
    if (*count == 64) return EMFILE;
    struct saved_fd s = {fd, -1, 0};
    int flags = dosxc_sys_fdflags(fd, 0, 0);
    if (flags >= 0) {
        s.cloexec = flags;
        s.copy = dosxc_sys_dup(fd, 32);
        if (s.copy < 0) return -s.copy;
        dosxc_sys_fdflags(s.copy, 1, DOSXC_FD_CLOEXEC);
    }
    saved[(*count)++] = s;
    return 0;
}

static void restore(struct saved_fd *saved, int count) {
    while (count--) {
        struct saved_fd *s = &saved[count];
        if (s->copy >= 0) {
            dosxc_sys_dup2(s->copy, s->fd);
            dosxc_sys_close(s->copy);
            dosxc_sys_fdflags(s->fd, 1, s->cloexec);
        } else {
            dosxc_sys_close(s->fd);
        }
    }
}

static int search(const char *file, char *found);

/* "/bin/sh", "/usr/bin/env": DOS-XC has no such folders, so a program asked
   for there that does not exist is looked for on PATH by its name. */
static const char *unix_program(const char *path, char *found) {
    struct dosxc_stat s;
    if (strncmp(path, "/bin/", 5) && strncmp(path, "/usr/bin/", 9) && strncmp(path, "/usr/local/bin/", 15)) return path;
    if (!dosxc_sys_stat(path, &s)) return path;
    const char *name = strrchr(path, '/') + 1;
    return *name && !search(name, found) ? found : path;
}

/* Run PATH with the file actions applied, return 0 or an errno value. */
static int run(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
               char *const argv[], char *const envp[]) {
    struct saved_fd saved[64];
    int count = 0, error = 0;
    for (int i = 0; actions && i < actions->__count && !error; ++i) {
        const struct __dosxc_spawn_action *a = &actions->__actions[i];
        if (a->type == ACTION_CLOSE) {
            if (!(error = save(saved, &count, a->fd))) dosxc_sys_close(a->fd);
        } else if (a->type == ACTION_DUP2) {
            if (!(error = save(saved, &count, a->target))) {
                int r = a->fd == a->target ? dosxc_sys_fdflags(a->fd, 1, 0) : dosxc_sys_dup2(a->fd, a->target);
                if (r < 0) error = -r;
            }
        } else if (a->type == ACTION_OPEN) {
            if (!(error = save(saved, &count, a->fd))) {
                int fd = dosxc_sys_open(a->path, a->flags, (int)a->mode);
                if (fd < 0) error = -fd;
                else if (fd != a->fd) {
                    dosxc_sys_dup2(fd, a->fd);
                    dosxc_sys_close(fd);
                }
            }
        }
    }
    long child = 0;
    char found[PATH_MAX];
    path = unix_program(path, found);
    if (!error) {
        struct dosxc_spawn spawn = {path, argv, envp ? envp : environ};
        child = dosxc_sys_task(DOSXC_TASK_SPAWN, &spawn, 0, 0);
    }
    restore(saved, count);
    if (error) return error;
    if (child < 0) return (int)-child;
    if (pid) *pid = (pid_t)child;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]) {
    (void)attributes;
    fflush(0);
    return run(pid, path, actions, argv, envp);
}

int dosxc_spawn_fds(pid_t *pid, const char *path, char *const argv[], char *const envp[], const int *map, int count) {
    fflush(0);
    struct dosxc_spawn_fds spawn = {path, argv, envp ? envp : environ, map, count};
    long child = dosxc_sys_task(DOSXC_TASK_SPAWN_FDS, &spawn, 0, 0);
    if (child < 0) return (int)-child;
    if (pid) *pid = (pid_t)child;
    return 0;
}

int dosxc_sandbox(unsigned rights) {
    long r = dosxc_sys_task(DOSXC_TASK_SANDBOX, (void *)(unsigned long)rights, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* Find FILE in the current directory, then each PATH entry (';' separated). */
static int search(const char *file, char *found) {
    struct dosxc_stat s;
    if (strchr(file, '/') || strchr(file, '\\') || strchr(file, ':')) {
        strcpy(found, file);
        return 0;
    }
    const char *path = getenv("PATH");
    const char *cursor = "";
    char separator = path && strchr(path, ';') ? ';' : ':';
    for (int first = 1;; first = 0) {
        char directory[PATH_MAX];
        size_t n = 0;
        if (!first) {
            if (!*cursor) break;
            while (*cursor && *cursor != separator) {
                if (n + 1 < sizeof(directory)) directory[n++] = *cursor;
                ++cursor;
            }
            if (*cursor) ++cursor;
        } else {
            directory[n++] = '.';
            cursor = path ? path : "";
        }
        directory[n] = 0;
        if (!n || strlen(directory) + strlen(file) + 6 >= PATH_MAX) continue;
        snprintf(found, PATH_MAX, "%s/%s", directory, file);
        if (!dosxc_sys_stat(found, &s) && !(s.attributes & DOSXC_ATTR_DIRECTORY)) return 0;
        strcat(found, ".EXE");
        if (!dosxc_sys_stat(found, &s) && !(s.attributes & DOSXC_ATTR_DIRECTORY)) return 0;
    }
    return ENOENT;
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]) {
    char found[PATH_MAX];
    int r = search(file, found);
    if (r) return r;
    return posix_spawn(pid, found, actions, attributes, argv, envp);
}

/* ---- exec: replace a forked process; elsewhere run the program, then
   leave with its status ---- */
static int exec_common(const char *path, char *const argv[], char *const envp[], int use_path) {
    char found[PATH_MAX];
    if (use_path) {
        int missing = search(path, found);
        if (missing) {
            errno = missing;
            return -1;
        }
        path = found;
    }
    char unix_found[PATH_MAX];
    path = unix_program(path, unix_found);
    fflush(0);
    struct dosxc_spawn replacement = {path, argv, envp ? envp : environ};
    long replaced = dosxc_sys_task(DOSXC_TASK_EXEC, &replacement, 0, 0);
    if (replaced != -ENOSYS) {
        errno = (int)-replaced;
        return -1;
    }
    pid_t pid;
    int r = posix_spawn(&pid, path, 0, 0, argv, envp);
    if (r) {
        errno = r;
        return -1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    dosxc_sys_exit(status);
}
int execve(const char *path, char *const argv[], char *const envp[]) { return exec_common(path, argv, envp, 0); }
int execv(const char *path, char *const argv[]) { return exec_common(path, argv, environ, 0); }
int execvp(const char *file, char *const argv[]) { return exec_common(file, argv, environ, 1); }
int execvpe(const char *file, char *const argv[], char *const envp[]) { return exec_common(file, argv, envp, 1); }

#define COLLECT_ARGS(first, ap, argv)                                     \
    int count = 1;                                                        \
    va_list counter;                                                      \
    va_copy(counter, ap);                                                 \
    while (va_arg(counter, char *)) ++count;                              \
    va_end(counter);                                                      \
    char **argv = __builtin_alloca(sizeof(char *) * (size_t)(count + 1)); \
    argv[0] = (char *)(first);                                            \
    for (int i = 1; i <= count; ++i) argv[i] = va_arg(ap, char *);

int execl(const char *path, const char *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    COLLECT_ARGS(arg, ap, argv);
    va_end(ap);
    return execv(path, argv);
}
int execlp(const char *file, const char *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    COLLECT_ARGS(arg, ap, argv);
    va_end(ap);
    return execvp(file, argv);
}
int execle(const char *path, const char *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    COLLECT_ARGS(arg, ap, argv);
    char **envp = va_arg(ap, char **);
    va_end(ap);
    return execve(path, argv, envp);
}

#define MAX_ATFORK 32
static struct { void (*prepare)(void), (*parent)(void), (*child)(void); } atfork[MAX_ATFORK];
static int atfork_count;

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    if (atfork_count == MAX_ATFORK) return ENOMEM;
    atfork[atfork_count].prepare = prepare;
    atfork[atfork_count].parent = parent;
    atfork[atfork_count++].child = child;
    return 0;
}

hidden void __dosxc_threads_forked(void);

/* 64-bit only: the 32-bit edition has no address spaces to copy into. */
pid_t fork(void) {
    for (int i = atfork_count; i-- > 0;)
        if (atfork[i].prepare) atfork[i].prepare();
    long r = dosxc_sys_task(DOSXC_TASK_FORK, 0, 0, 0);
    if (!r) {
        child_count = 0;
        __dosxc_threads_forked();
        for (int i = 0; i < atfork_count; ++i)
            if (atfork[i].child) atfork[i].child();
        return 0;
    }
    for (int i = 0; i < atfork_count; ++i)
        if (atfork[i].parent) atfork[i].parent();
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (pid_t)r;
}
pid_t vfork(void) { return fork(); }

int system(const char *command) {
    if (!command) return 1;
    fflush(0);
    return dosxc_sys_system(command);
}

/* popen: programs run to completion, so "r" runs the command at once into a
   pipe, and "w" collects the output and runs the command at pclose. */
struct popen_stream { FILE *file; int writing, read_end, status; char *command; struct popen_stream *next; };
static struct popen_stream *streams;

static const char *command_shell(void) {
    const char *shell = getenv("COMSPEC");
    return shell && *shell ? shell : "/SYSTEM/COMMAND.EXE";
}

static int run_shell(const char *command, int fd, int target) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fd, target);
    char *argv[] = {"COMMAND.EXE", "/C", (char *)command, 0};
    pid_t pid;
    int r = posix_spawn(&pid, command_shell(), &actions, 0, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (r) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

FILE *popen(const char *command, const char *mode) {
    if (!command || !mode || (mode[0] != 'r' && mode[0] != 'w')) {
        errno = EINVAL;
        return 0;
    }
    struct popen_stream *p = calloc(1, sizeof(*p));
    int fds[2];
    if (!p || pipe(fds)) {
        free(p);
        return 0;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    if (mode[0] == 'r') {
        p->status = run_shell(command, fds[1], 1);
        close(fds[1]);
        p->file = fdopen(fds[0], "r");
        p->read_end = -1;
    } else {
        p->writing = 1;
        p->command = strdup(command);
        p->read_end = fds[0];
        p->file = fdopen(fds[1], "w");
    }
    if (!p->file || (p->writing && !p->command)) {
        close(fds[0]);
        close(fds[1]);
        free(p->command);
        free(p);
        return 0;
    }
    p->next = streams;
    streams = p;
    return p->file;
}

int pclose(FILE *stream) {
    struct popen_stream **link = &streams;
    while (*link && (*link)->file != stream) link = &(*link)->next;
    if (!*link) {
        errno = ECHILD;
        return -1;
    }
    struct popen_stream *p = *link;
    *link = p->next;
    fclose(stream);
    int status = p->status;
    if (p->writing) {
        status = run_shell(p->command, p->read_end, 0);
        close(p->read_end);
        free(p->command);
    }
    free(p);
    return status;
}

pid_t getpid(void) { return dosxc_sys_getpid(); }
pid_t getppid(void) { return dosxc_sys_getppid(); }
pid_t getpgrp(void) { return getpid(); }
pid_t getpgid(pid_t pid) { return pid ? pid : getpid(); }
pid_t getsid(pid_t pid) { (void)pid; return getpid(); }
pid_t setsid(void) { return getpid(); }
int setpgid(pid_t pid, pid_t group) { (void)pid; (void)group; return 0; }
pid_t tcgetpgrp(int fd) { (void)fd; return getpid(); }
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
int setuid(uid_t uid) { return uid ? (errno = EPERM, -1) : 0; }
int setgid(gid_t gid) { return gid ? (errno = EPERM, -1) : 0; }
int seteuid(uid_t uid) { return setuid(uid); }
int setegid(gid_t gid) { return setgid(gid); }
int setreuid(uid_t real, uid_t effective) { return (real != (uid_t)-1 && real) || (effective != (uid_t)-1 && effective) ? (errno = EPERM, -1) : 0; }
int setregid(gid_t real, gid_t effective) { return setreuid(real, effective); }
int setresuid(uid_t real, uid_t effective, uid_t saved) { return setreuid(real, effective) ? -1 : setreuid(saved, (uid_t)-1); }
int setresgid(gid_t real, gid_t effective, gid_t saved) { return setresuid(real, effective, saved); }
int getgroups(int size, gid_t list[]) { (void)size; (void)list; return 0; }
int nice(int increment) { (void)increment; return 0; }
char *getlogin(void) { return "user"; }
int getlogin_r(char *buffer, size_t size) {
    if (size < 5) return ERANGE;
    strcpy(buffer, "user");
    return 0;
}
int gethostname(char *buffer, size_t size) {
    if (size < 7) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(buffer, "dosxc");
    return 0;
}
char *ttyname(int fd) { return isatty(fd) ? "/dev/tty" : 0; }
int ttyname_r(int fd, char *buffer, size_t size) {
    if (!isatty(fd)) return ENOTTY;
    if (size < 9) return ERANGE;
    strcpy(buffer, "/dev/tty");
    return 0;
}
char *ctermid(char *buffer) {
    static char name[] = "/dev/tty";
    return buffer ? strcpy(buffer, name) : name;
}

int uname(struct utsname *u) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "DOS-XC");
    strcpy(u->nodename, "dosxc");
    const char *version = strrchr(info.version, 'v');
    strcpy(u->release, version ? version + 1 : "0");
    strcpy(u->version, info.version);
    strcpy(u->machine, info.bits == 64 ? "x86_64" : "i686");
    return 0;
}

long sysconf(int name) {
    struct dosxc_sysinfo info;
    switch (name) {
    case _SC_ARG_MAX: return ARG_MAX;
    case _SC_CHILD_MAX: return CHILD_MAX;
    case _SC_CLK_TCK: return 100;
    case _SC_NGROUPS_MAX: return 0;
    case _SC_OPEN_MAX: return OPEN_MAX;
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return 1;
    case _SC_PHYS_PAGES: dosxc_sys_sysinfo(&info); return (long)(info.memory_total / 4096);
    case _SC_AVPHYS_PAGES: dosxc_sys_sysinfo(&info); return (long)(info.memory_free / 4096);
    case _SC_LINE_MAX: return LINE_MAX;
    case _SC_HOST_NAME_MAX: return HOST_NAME_MAX;
    case _SC_IOV_MAX: return IOV_MAX;
    case _SC_GETPW_R_SIZE_MAX:
    case _SC_TTY_NAME_MAX: return 256;
    }
    errno = EINVAL;
    return -1;
}
int getpagesize(void) { return 4096; }

void *sbrk(intptr_t increment) {
    static char marker;
    if (!increment) return &marker;
    errno = ENOMEM;
    return (void *)-1;
}

int getrlimit(int resource, struct rlimit *limit) {
    limit->rlim_cur = limit->rlim_max = RLIM_INFINITY;
    if (resource == RLIMIT_NOFILE) limit->rlim_cur = limit->rlim_max = OPEN_MAX;
    if (resource == RLIMIT_STACK) limit->rlim_cur = limit->rlim_max = 16u << 20;
    return 0;
}
int setrlimit(int resource, const struct rlimit *limit) { (void)resource; (void)limit; return 0; }
int getpriority(int which, unsigned who) { (void)which; (void)who; return 0; }
int setpriority(int which, unsigned who, int value) { (void)which; (void)who; (void)value; return 0; }
int getloadavg(double *averages, int count) {
    for (int i = 0; i < count && i < 3; ++i) averages[i] = 0;
    return count < 3 ? count : 3;
}

/* ---- signals ---- */
static void (*handlers[NSIG])(int);
static sigset_t blocked;

#ifdef __x86_64__
#include <sys/ucontext.h>

/* A processor fault.  Handlers get three arguments whether or not they asked
   for SA_SIGINFO, which a one-argument handler ignores: Rust's struct
   sigaction does not match this library's, so sa_flags cannot be trusted. */
void __dosxc_fault_handle(struct dosxc_fault *f) {
    typedef void (*handler3)(int, siginfo_t *, void *);
    extern int *__dosxc_fault_nesting(void);
    void (*handler)(int) = handlers[f->signal];
    siginfo_t info;
    ucontext_t context;

    memset(&info, 0, sizeof info);
    memset(&context, 0, sizeof context);
    info.si_signo = (int)f->signal;
    info.si_code = (int)f->code;
    info.si_addr = (void *)f->address;
    memcpy(context.uc_mcontext.gregs, f->regs, sizeof f->regs);
    context.uc_mcontext.fpregs = (struct _libc_fpstate *)f->fpu;
    int *nesting = __dosxc_fault_nesting();
    ++*nesting;
    if (handler != SIG_DFL && handler != SIG_IGN)
        ((handler3)handler)((int)f->signal, &info, &context);
    --*nesting;
    memcpy(f->regs, context.uc_mcontext.gregs, sizeof f->regs);
    dosxc_sys_task(DOSXC_TASK_FAULT_DONE, 0, 0, 0);
}

/* longjmp out of fault handlers: they are over. */
void __dosxc_fault_unwind(void) {
    int *nesting = __dosxc_fault_nesting();
    for (; *nesting > 0; --*nesting) dosxc_sys_task(DOSXC_TASK_FAULT_DONE, 0, 0, 0);
}

__asm__(
    ".text\n.globl __dosxc_fault_entry\n.type __dosxc_fault_entry,@function\n"
    "__dosxc_fault_entry:\n"
    "  mov %rdi, %rbx\n"
    "  fxsave64 224(%rbx)\n"
    "  and $-16, %rsp\n"
    "  call __dosxc_fault_handle\n"
    "  fxrstor64 224(%rbx)\n"
    "  mov %ss, %eax\n  push %rax\n"
    "  push 144(%rbx)\n"               /* rsp: regs[15] */
    "  push 160(%rbx)\n"               /* eflags: regs[17] */
    "  mov %cs, %eax\n  push %rax\n"
    "  push 152(%rbx)\n"               /* rip: regs[16] */
    "  mov 24(%rbx), %r8\n  mov 32(%rbx), %r9\n  mov 40(%rbx), %r10\n  mov 48(%rbx), %r11\n"
    "  mov 56(%rbx), %r12\n  mov 64(%rbx), %r13\n  mov 72(%rbx), %r14\n  mov 80(%rbx), %r15\n"
    "  mov 88(%rbx), %rdi\n  mov 96(%rbx), %rsi\n  mov 104(%rbx), %rbp\n"
    "  mov 120(%rbx), %rdx\n  mov 128(%rbx), %rax\n  mov 136(%rbx), %rcx\n"
    "  mov 112(%rbx), %rbx\n"
    "  iretq\n");
extern void __dosxc_fault_entry(void);

static void update_fault_handlers(void) {
    static const int faults[] = {SIGSEGV, SIGILL, SIGBUS, SIGFPE};
    unsigned mask = 0;

    for (unsigned i = 0; i < sizeof faults / sizeof *faults; i++)
        if (handlers[faults[i]] != SIG_DFL && handlers[faults[i]] != SIG_IGN) mask |= 1u << faults[i];
    dosxc_sys_task(DOSXC_TASK_FAULT_ENTRY, (void *)__dosxc_fault_entry, (long)mask, 0);
}
#else
static void update_fault_handlers(void) {}
#endif

static __attribute__((noreturn)) void terminate(int signal) {
    dosxc_sys_exit(DOSXC_SIGNALED(signal));
}

int raise(int signal) {
    if (signal <= 0 || signal >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    void (*handler)(int) = handlers[signal];
    if (handler == SIG_IGN) return 0;
    if (handler == SIG_DFL) {
        if (signal == SIGCHLD || signal == SIGWINCH || signal == SIGURG || signal == SIGCONT) return 0;
        terminate(signal);
    }
    handler(signal);
    return 0;
}

void (*signal(int number, void (*handler)(int)))(int) {
    if (number <= 0 || number >= NSIG || number == SIGKILL || number == SIGSTOP) {
        errno = EINVAL;
        return SIG_ERR;
    }
    void (*old)(int) = handlers[number];
    handlers[number] = handler;
    update_fault_handlers();
    return old;
}

int sigaction(int number, const struct sigaction *action, struct sigaction *old) {
    if (number <= 0 || number >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (old) {
        memset(old, 0, sizeof(*old));
        old->sa_handler = handlers[number];
    }
    if (action) {
        if (number == SIGKILL || number == SIGSTOP) {
            errno = EINVAL;
            return -1;
        }
        handlers[number] = action->sa_handler;
        update_fault_handlers();
    }
    return 0;
}

int kill(pid_t pid, int signal) {
    if (pid == 0 || pid == getpid() || pid == -1) return signal ? raise(signal) : 0;
    if (pid < 0) pid = -pid;
    if (dosxc_sys_task(DOSXC_TASK_ALIVE, 0, pid, 0) <= 0) { errno = ESRCH; return -1; }
    if (!signal) return 0;
    long r = dosxc_sys_task(DOSXC_TASK_KILL, 0, pid, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
int killpg(pid_t group, int signal) { return kill(group, signal); }

void abort(void) {
    raise(SIGABRT);
    handlers[SIGABRT] = SIG_DFL;
    terminate(SIGABRT);
}

int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set) { *set = ~(sigset_t)0; return 0; }
int sigaddset(sigset_t *set, int n) {
    if (n <= 0 || n >= NSIG) return errno = EINVAL, -1;
    *set |= 1UL << n;
    return 0;
}
int sigdelset(sigset_t *set, int n) {
    if (n <= 0 || n >= NSIG) return errno = EINVAL, -1;
    *set &= ~(1UL << n);
    return 0;
}
int sigismember(const sigset_t *set, int n) { return n > 0 && n < NSIG && ((*set >> n) & 1); }
int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    if (old) *old = blocked;
    if (set) {
        if (how == SIG_BLOCK) blocked |= *set;
        else if (how == SIG_UNBLOCK) blocked &= ~*set;
        else blocked = *set;
    }
    return 0;
}
int sigpending(sigset_t *set) { *set = 0; return 0; }
int sigsuspend(const sigset_t *mask) { (void)mask; errno = EINTR; return -1; }
unsigned alarm(unsigned seconds) { (void)seconds; return 0; }
int pause(void) { errno = EINTR; return -1; }

const char *const sys_siglist[NSIG] = {
    "Unknown signal", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap",
    "Aborted", "Bus error", "Floating point exception", "Killed", "User defined signal 1",
    "Segmentation fault", "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated",
    "Stack fault", "Child exited", "Continued", "Stopped (signal)", "Stopped", "Stopped (tty input)",
    "Stopped (tty output)", "Urgent I/O condition", "CPU time limit exceeded", "File size limit exceeded",
    "Virtual timer expired", "Profiling timer expired", "Window changed", "I/O possible", "Power failure",
    "Bad system call"};

char *strsignal(int signal) {
    static char unknown[32];
    if (signal > 0 && signal < NSIG) return (char *)sys_siglist[signal];
    snprintf(unknown, sizeof(unknown), "Unknown signal %d", signal);
    return unknown;
}

void psignal(int signal, const char *prefix) {
    if (prefix && *prefix) fprintf(stderr, "%s: %s\n", prefix, strsignal(signal));
    else fprintf(stderr, "%s\n", strsignal(signal));
}

size_t confstr(int name, char *buffer, size_t size) {
    const char *value;
    if (name == _CS_PATH) value = "/SYSTEM";
    else { errno = EINVAL; return 0; }
    size_t n = strlen(value) + 1;
    if (buffer && size) {
        size_t copy = n < size ? n : size;
        memcpy(buffer, value, copy - 1);
        buffer[copy - 1] = 0;
    }
    return n;
}
