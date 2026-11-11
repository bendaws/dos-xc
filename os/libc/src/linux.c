/*
 * DOS-XC /os/libc/src/linux.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <errno.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sched.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/random.h>
#include <dosxc/abi.h>
#include <dosxc/syscall.h>

int __xpg_strerror_r(int error, char *buffer, size_t size);
int __xpg_strerror_r(int error, char *buffer, size_t size) { return strerror_r(error, buffer, size); }

int sigaltstack(const stack_t *stack, stack_t *old) {
    (void)stack;
    if (old) {
        memset(old, 0, sizeof(*old));
        old->ss_flags = SS_DISABLE;
    }
    return 0;
}

#ifdef __x86_64__
enum { SYS_getpid = 39, SYS_gettid = 186, SYS_futex = 202, SYS_getrandom = 318 };
enum { FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_WAIT_BITSET = 9, FUTEX_WAKE_BITSET = 10, FUTEX_CLOCK_REALTIME = 256 };

static long futex(int *address, int op, int value, const struct timespec *timeout) {
    int command = op & 127;
    if (command == FUTEX_WAKE || command == FUTEX_WAKE_BITSET)
        return dosxc_sys_task(DOSXC_TASK_FUTEX_WAKE, address, value, 0);
    if (command != FUTEX_WAIT && command != FUTEX_WAIT_BITSET) return -ENOSYS;
    long nanoseconds = 0;
    if (timeout) {
        long long total = timeout->tv_sec * 1000000000LL + timeout->tv_nsec;
        if (command == FUTEX_WAIT_BITSET) {
            struct timespec now;
            clock_gettime(op & FUTEX_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
            total -= now.tv_sec * 1000000000LL + now.tv_nsec;
        }
        if (total <= 0) return -ETIMEDOUT;
        nanoseconds = (long)total;
    }
    long r = dosxc_sys_task(DOSXC_TASK_FUTEX_WAIT, address, value, nanoseconds);
    return r;
}

long syscall(long number, ...) {
    va_list list;
    va_start(list, number);
    long a = va_arg(list, long), b = va_arg(list, long), c = va_arg(list, long), d = va_arg(list, long);
    va_end(list);
    long r;
    switch (number) {
    case SYS_futex: r = futex((int *)a, (int)b, (int)c, (const struct timespec *)d); break;
    case SYS_getrandom: return getrandom((void *)a, (size_t)b, (unsigned)c);
    case 213: return epoll_create((int)a);
    case 291: return epoll_create1((int)a);
    case 233: return epoll_ctl((int)a, (int)b, (int)c, (struct epoll_event *)d);
    case 232: return epoll_wait((int)a, (struct epoll_event *)b, (int)c, (int)d);
    case 281: return epoll_wait((int)a, (struct epoll_event *)b, (int)c, (int)d);
    case SYS_getpid: return getpid();
    case SYS_gettid: return (long)(((uintptr_t)pthread_self() >> 4) & 0x7fffffff);
    default: r = -ENOSYS; break;
    }
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}
#endif

#include <fenv.h>
#include <ifaddrs.h>
#include <poll.h>
#include <resolv.h>
#include <stdlib.h>
#include <syslog.h>
#include <ucontext.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/sendfile.h>
#include <sys/sysinfo.h>
#include "libc.h"

int prctl(int option, ...) {
    switch (option) {
    case PR_SET_NAME: case PR_SET_DUMPABLE: case PR_SET_TIMERSLACK: case PR_SET_VMA: case PR_SET_PTRACER: return 0;
    case PR_GET_NAME: {
        va_list list;
        va_start(list, option);
        char *name = va_arg(list, char *);
        va_end(list);
        name[0] = 0;
        return 0;
    }
    case PR_GET_DUMPABLE: return 0;
    default: errno = EINVAL; return -1;
    }
}

void openlog(const char *name, int options, int facility) { (void)name; (void)options; (void)facility; }
void syslog(int priority, const char *format, ...) { (void)priority; (void)format; }
void closelog(void) {}

int get_nprocs(void) { return 1; }
int get_nprocs_conf(void) { return 1; }
int sysinfo(struct sysinfo *out) {
    struct dosxc_task_stats stats;
    memset(out, 0, sizeof(*out));
    dosxc_sys_task(DOSXC_TASK_STATS, &stats, 0, 0);
    out->uptime = (long)(stats.now_ns / 1000000000ull);
    out->totalram = stats.memory_total;
    out->freeram = stats.memory_free;
    out->procs = (unsigned short)stats.tasks;
    out->mem_unit = 1;
    return 0;
}
long get_phys_pages(void) { struct sysinfo s; sysinfo(&s); return (long)(s.totalram / 4096); }
long get_avphys_pages(void) { struct sysinfo s; sysinfo(&s); return (long)(s.freeram / 4096); }

ssize_t sendfile(int out, int in, off_t *offset, size_t count) {
    char buffer[65536];
    size_t total = 0;
    off_t at = offset ? *offset : 0;
    while (total < count) {
        size_t want = count - total < sizeof(buffer) ? count - total : sizeof(buffer);
        ssize_t got = offset ? pread(in, buffer, want, at) : read(in, buffer, want);
        if (got <= 0) { if (got < 0 && !total) return -1; break; }
        ssize_t put = write(out, buffer, (size_t)got);
        if (put < 0) { if (!total) return -1; break; }
        total += (size_t)put;
        at += put;
        if (put < got) break;
    }
    if (offset) *offset = at;
    return (ssize_t)total;
}

/* Nothing here uses these; callers have fallbacks. */
int semget(key_t key, int n, int flags) { (void)key; (void)n; (void)flags; errno = ENOSYS; return -1; }
int semop(int id, struct sembuf *ops, size_t n) { (void)id; (void)ops; (void)n; errno = ENOSYS; return -1; }
int semctl(int id, int n, int op, ...) { (void)id; (void)n; (void)op; errno = ENOSYS; return -1; }
key_t ftok(const char *path, int id) { (void)path; (void)id; errno = ENOSYS; return -1; }
int eventfd(unsigned initial, int flags) { (void)initial; (void)flags; errno = ENOSYS; return -1; }
int getcontext(ucontext_t *context) { (void)context; errno = ENOSYS; return -1; }
int setcontext(const ucontext_t *context) { (void)context; errno = ENOSYS; return -1; }
int res_init(void) { return 0; }
int getifaddrs(struct ifaddrs **out) { *out = 0; return 0; }
void freeifaddrs(struct ifaddrs *list) { (void)list; }

#define MAX_EPOLLS 16
struct epoll_item { int fd; struct epoll_event event; };
static struct { int fd; struct epoll_item *items; int count; } epolls[MAX_EPOLLS];
static struct __dosxc_rlock epoll_lock;

int epoll_create1(int flags) {
    (void)flags;
    int ends[2];
    if (pipe(ends)) return -1;
    close(ends[1]);
    LOCK_GUARD(&epoll_lock);
    for (int i = 0; i < MAX_EPOLLS; ++i)
        if (epolls[i].fd == ends[0] || !epolls[i].items) {
            free(epolls[i].items);
            epolls[i].fd = ends[0];
            epolls[i].items = malloc(sizeof(struct epoll_item));
            epolls[i].count = 0;
            return ends[0];
        }
    close(ends[0]);
    errno = EMFILE;
    return -1;
}

int epoll_create(int size) { (void)size; return epoll_create1(0); }

static int find_epoll(int fd) {
    for (int i = 0; i < MAX_EPOLLS; ++i)
        if (epolls[i].items && epolls[i].fd == fd) return i;
    errno = EBADF;
    return -1;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    LOCK_GUARD(&epoll_lock);
    int e = find_epoll(epfd);
    if (e < 0) return -1;
    int at = -1;
    for (int i = 0; i < epolls[e].count; ++i)
        if (epolls[e].items[i].fd == fd) at = i;
    if (op == EPOLL_CTL_ADD) {
        if (at >= 0) { errno = EEXIST; return -1; }
        struct epoll_item *grown = realloc(epolls[e].items, (size_t)(epolls[e].count + 1) * sizeof(*grown));
        if (!grown) { errno = ENOMEM; return -1; }
        epolls[e].items = grown;
        grown[epolls[e].count].fd = fd;
        grown[epolls[e].count++].event = *event;
        return 0;
    }
    if (at < 0) { errno = ENOENT; return -1; }
    if (op == EPOLL_CTL_MOD) { epolls[e].items[at].event = *event; return 0; }
    if (op == EPOLL_CTL_DEL) { epolls[e].items[at] = epolls[e].items[--epolls[e].count]; return 0; }
    errno = EINVAL;
    return -1;
}

int epoll_wait(int epfd, struct epoll_event *out, int max, int timeout) {
    struct pollfd *polls;
    struct epoll_item *copy;
    int n;
    {
        LOCK_GUARD(&epoll_lock);
        int e = find_epoll(epfd);
        if (e < 0) return -1;
        n = epolls[e].count;
        polls = malloc((size_t)(n + 1) * sizeof(*polls));
        copy = malloc((size_t)(n + 1) * sizeof(*copy));
        if (!polls || !copy) { free(polls); free(copy); errno = ENOMEM; return -1; }
        memcpy(copy, epolls[e].items, (size_t)n * sizeof(*copy));
    }
    for (int i = 0; i < n; ++i) {
        polls[i].fd = copy[i].fd;
        polls[i].events = (short)(((copy[i].event.events & EPOLLIN) ? POLLIN : 0) | ((copy[i].event.events & EPOLLOUT) ? POLLOUT : 0));
        polls[i].revents = 0;
    }
    int r = poll(polls, (nfds_t)n, timeout), found = 0;
    for (int i = 0; r > 0 && i < n && found < max; ++i) {
        if (!polls[i].revents) continue;
        unsigned events = 0;
        if (polls[i].revents & POLLIN) events |= EPOLLIN;
        if (polls[i].revents & POLLOUT) events |= EPOLLOUT;
        if (polls[i].revents & POLLERR) events |= EPOLLERR;
        if (polls[i].revents & POLLHUP) events |= EPOLLHUP;
        out[found].events = events;
        out[found++].data = copy[i].event.data;
    }
    free(polls);
    free(copy);
    return r < 0 ? -1 : found;
}

#if defined(__x86_64__) || defined(__i386__)
static unsigned read_mxcsr(void) { unsigned v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void write_mxcsr(unsigned v) { __asm__ volatile("ldmxcsr %0" :: "m"(v)); }
static unsigned read_sw(void) { unsigned short v; __asm__ volatile("fnstsw %0" : "=a"(v)); return v; }

int fetestexcept(int mask) { return (int)(read_sw() | read_mxcsr()) & mask & FE_ALL_EXCEPT; }
int feclearexcept(int mask) {
    __asm__ volatile("fnclex");
    write_mxcsr(read_mxcsr() & ~(unsigned)(mask & FE_ALL_EXCEPT));
    return 0;
}
int feraiseexcept(int mask) { write_mxcsr(read_mxcsr() | (unsigned)(mask & FE_ALL_EXCEPT)); return 0; }
int fegetexceptflag(fexcept_t *flag, int mask) { *flag = (fexcept_t)fetestexcept(mask); return 0; }
int fesetexceptflag(const fexcept_t *flag, int mask) {
    feclearexcept(mask);
    feraiseexcept(*flag & mask);
    return 0;
}
int fegetround(void) { return (int)(read_mxcsr() >> 3) & 0xc00; }
int fesetround(int mode) {
    if (mode & ~0xc00) return -1;
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw = (unsigned short)((cw & ~0xc00) | (unsigned)mode);
    __asm__ volatile("fldcw %0" :: "m"(cw));
    write_mxcsr((read_mxcsr() & ~(unsigned)0x6000) | ((unsigned)mode << 3));
    return 0;
}
int fegetenv(fenv_t *env) {
    __asm__ volatile("fnstenv %0" : "=m"(*env));
    env->mxcsr = read_mxcsr();
    return 0;
}
int fesetenv(const fenv_t *env) {
    if (env == FE_DFL_ENV) {
        __asm__ volatile("fninit");
        write_mxcsr(0x1f80);
        return 0;
    }
    __asm__ volatile("fldenv %0" :: "m"(*env));
    write_mxcsr(env->mxcsr);
    return 0;
}
int feholdexcept(fenv_t *env) {
    fegetenv(env);
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}
int feupdateenv(const fenv_t *env) {
    int raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(env);
    feraiseexcept(raised);
    return 0;
}
#endif

#include <sys/stat.h>
#include <mntent.h>
#include <netdb.h>
#include <resolv.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/ptrace.h>
#include <sys/shm.h>
#include <sys/statfs.h>
#include <sys/timerfd.h>

static int absent(void) { errno = ENOSYS; return -1; }

static void fill_statfs(const char *path, struct statfs *out) {
    struct dosxc_volume volumes[26];
    char here[8] = "";
    char letter;
    int count;

    memset(out, 0, sizeof *out);
    out->f_bsize = out->f_frsize = 4096;
    out->f_namelen = 255;
    if (path && path[0] && path[1] == ':') letter = path[0];
    else {
        dosxc_sys_getcwd(here, sizeof here);
        letter = here[0];
    }
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    count = dosxc_sys_volumes(volumes, 26);
    for (int i = 0; i < count; i++) {
        if (volumes[i].letter != letter) continue;
        out->f_blocks = volumes[i].total_bytes / 4096;
        out->f_bfree = out->f_bavail = volumes[i].free_bytes / 4096;
    }
}
int statfs(const char *path, struct statfs *out) {
    struct stat st;
    if (stat(path, &st)) return -1;
    fill_statfs(path, out);
    return 0;
}
/* A descriptor does not say which drive it is on: this reports the current one. */
int fstatfs(int fd, struct statfs *out) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    fill_statfs(0, out);
    return 0;
}
int flock(int fd, int operation) { (void)fd; (void)operation; return 0; }
long ptrace(int request, ...) { (void)request; return absent(); }
int timerfd_create(int clock, int flags) { (void)clock; (void)flags; return absent(); }
int timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old) { (void)fd; (void)flags; (void)value; (void)old; return absent(); }
int timerfd_gettime(int fd, struct itimerspec *value) { (void)fd; (void)value; return absent(); }
int shmget(key_t key, size_t size, int flags) { (void)key; (void)size; (void)flags; return absent(); }
void *shmat(int id, const void *address, int flags) { (void)id; (void)address; (void)flags; absent(); return (void *)-1; }
int shmdt(const void *address) { (void)address; return absent(); }
int shmctl(int id, int command, struct shmid_ds *data) { (void)id; (void)command; (void)data; return absent(); }
FILE *setmntent(const char *path, const char *mode) { (void)path; (void)mode; errno = ENOENT; return 0; }
struct mntent *getmntent(FILE *f) { (void)f; return 0; }
struct mntent *getmntent_r(FILE *f, struct mntent *out, char *buffer, int size) { (void)f; (void)out; (void)buffer; (void)size; return 0; }
int endmntent(FILE *f) { (void)f; return 1; }
char *hasmntopt(const struct mntent *entry, const char *option) { (void)entry; (void)option; return 0; }
char *get_current_dir_name(void) { return getcwd(0, 0); }
pid_t gettid(void) { return (pid_t)(((uintptr_t)pthread_self() >> 4) & 0x7fffffff); }
int sched_getcpu(void) { return 0; }
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock) { (void)thread; (void)clock; return ENOENT; }
int res_query(const char *name, int klass, int type, unsigned char *answer, int size) {
    (void)name; (void)klass; (void)type; (void)answer; (void)size;
    h_errno = NO_RECOVERY;
    return -1;
}
int res_search(const char *name, int klass, int type, unsigned char *answer, int size) { return res_query(name, klass, type, answer, size); }

/* Only an exiting child can be waited for; nothing keeps it for a second look. */
int waitid(idtype_t type, unsigned id, void *info, int options) {
    siginfo_t *result = info;
    int status;
    pid_t pid;

    if (options & WNOWAIT) { errno = ECHILD; return -1; }
    pid = waitpid(type == P_PID ? (pid_t)id : -1, &status,
                  (options & WNOHANG) ? WNOHANG : 0);
    if (pid < 0) return -1;
    memset(result, 0, sizeof(*result));
    if (pid > 0) {
        result->si_signo = SIGCHLD;
        result->si_pid = pid;
        result->si_code = WIFEXITED(status) ? CLD_EXITED : CLD_KILLED;
        result->si_status = WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status);
    }
    return 0;
}

int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    int client = accept(fd, address, length);

    if (client < 0) return client;
    if (flags & SOCK_NONBLOCK) fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK);
    if (flags & SOCK_CLOEXEC) fcntl(client, F_SETFD, FD_CLOEXEC);
    return client;
}

/* Signals are not queued for a thread to collect. */
int sigwait(const sigset_t *set, int *sig) { (void)set; (void)sig; return ENOSYS; }

unsigned if_nametoindex(const char *name) { (void)name; errno = ENODEV; return 0; }
char *if_indextoname(unsigned index, char *name) { (void)index; (void)name; errno = ENXIO; return NULL; }

/* DOS-XC has no directory handles to be relative to: only the current
   directory and absolute paths work with the *at family. */
static int relative_ok(int dir, const char *path) {
    if (dir == AT_FDCWD || path[0] == '/' || path[0] == '\\') return 1;
    errno = ENOSYS;
    return 0;
}

int unlinkat(int dir, const char *path, int flags) {
    if (!relative_ok(dir, path)) return -1;
    return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int renameat(int from_dir, const char *from, int to_dir, const char *to) {
    if (!relative_ok(from_dir, from) || !relative_ok(to_dir, to)) return -1;
    return rename(from, to);
}

int linkat(int from_dir, const char *from, int to_dir, const char *to, int flags) {
    (void)flags;
    if (!relative_ok(from_dir, from) || !relative_ok(to_dir, to)) return -1;
    return link(from, to);
}

int fchmodat(int dir, const char *path, mode_t mode, int flags) {
    (void)flags;
    if (!relative_ok(dir, path)) return -1;
    return chmod(path, mode);
}

ssize_t preadv(int fd, const struct iovec *vector, int count, off_t offset) {
    ssize_t total = 0;

    for (int i = 0; i < count; i++) {
        ssize_t n = pread(fd, vector[i].iov_base, vector[i].iov_len, offset + total);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < vector[i].iov_len) break;
    }
    return total;
}

ssize_t pwritev(int fd, const struct iovec *vector, int count, off_t offset) {
    ssize_t total = 0;

    for (int i = 0; i < count; i++) {
        ssize_t n = pwrite(fd, vector[i].iov_base, vector[i].iov_len, offset + total);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < vector[i].iov_len) break;
    }
    return total;
}

int sched_getaffinity(pid_t pid, size_t size, void *mask) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned char *bits = mask;

    (void)pid;
    if (cpus < 1) cpus = 1;
    if ((size_t)cpus > size * 8) cpus = (long)size * 8;
    memset(bits, 0, size);
    for (long i = 0; i < cpus; i++) bits[i / 8] |= (unsigned char)(1 << (i % 8));
    return 0;
}

int chroot(const char *path) { (void)path; errno = ENOSYS; return -1; }
int setgroups(size_t count, const gid_t *list) { (void)count; (void)list; return 0; }
ssize_t splice(int in, off_t *in_off, int out, off_t *out_off, size_t length, unsigned flags) {
    (void)in; (void)in_off; (void)out; (void)out_off; (void)length; (void)flags;
    errno = ENOSYS;
    return -1;
}

struct dosxc_mmsghdr { struct msghdr hdr; unsigned len; };

int recvmmsg(int fd, void *vector, unsigned count, int flags, void *timeout) {
    struct dosxc_mmsghdr *msg = vector;
    unsigned done = 0;

    (void)timeout;
    while (done < count) {
        ssize_t n = recvmsg(fd, &msg[done].hdr, done ? flags | MSG_DONTWAIT : flags);
        if (n < 0) return done ? (int)done : -1;
        msg[done++].len = (unsigned)n;
    }
    return (int)done;
}

/* Files have no holes to reserve: just make sure the file is long enough. */
int fallocate(int fd, int mode, off_t offset, off_t length) {
    struct stat st;

    if (mode != 0) { errno = EOPNOTSUPP; return -1; }
    if (fstat(fd, &st) < 0) return -1;
    if (st.st_size >= offset + length) return 0;
    return ftruncate(fd, offset + length);
}

int posix_fallocate(int fd, off_t offset, off_t length) {
    return fallocate(fd, 0, offset, length) < 0 ? errno : 0;
}
