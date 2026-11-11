/*
 * DOS-XC /os/libc/src/terminal.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/select.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static struct termios settings = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD | B38400,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = {[VINTR] = 3, [VQUIT] = 28, [VERASE] = 8, [VKILL] = 21, [VEOF] = 26, [VMIN] = 1, [VSUSP] = 0},
    .__c_ispeed = B38400,
    .__c_ospeed = B38400,
};
hidden int __console_raw;

static int console(int fd) {
    int r = dosxc_sys_isatty(fd);
    if (r < 0) { errno = -r; return 0; }
    if (!r) errno = ENOTTY;
    return r;
}

int tcgetattr(int fd, struct termios *t) {
    if (!console(fd)) return -1;
    *t = settings;
    return 0;
}

int tcsetattr(int fd, int action, const struct termios *t) {
    (void)action;
    if (!console(fd)) return -1;
    settings = *t;
    __console_raw = !(t->c_lflag & ICANON);
    dosxc_sys_console(DOSXC_CONSOLE_MODE, __console_raw ? DOSXC_CONSOLE_RAW : 0, 0);
    return 0;
}

/* Raw console bytes adjusted for the termios input flags (called by read). */
hidden ssize_t __console_input(char *buffer, ssize_t n) {
    for (ssize_t i = 0; i < n; ++i) {
        if (buffer[i] == '\r' && (settings.c_iflag & ICRNL)) buffer[i] = '\n';
        else if (buffer[i] == '\n' && (settings.c_iflag & INLCR)) buffer[i] = '\r';
        if ((settings.c_lflag & ISIG) && (unsigned char)buffer[i] == settings.c_cc[VINTR]) raise(SIGINT);
    }
    return n;
}

int tcflush(int fd, int queue) { (void)queue; return console(fd) ? 0 : -1; }
int tcdrain(int fd) { return console(fd) ? 0 : -1; }
int tcflow(int fd, int action) { (void)action; return console(fd) ? 0 : -1; }
int tcsendbreak(int fd, int duration) { (void)duration; return console(fd) ? 0 : -1; }
pid_t tcgetsid(int fd) { return console(fd) ? getpid() : -1; }

void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag = (t->c_cflag & ~(tcflag_t)(CSIZE | PARENB)) | CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}
speed_t cfgetispeed(const struct termios *t) { return t->__c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->__c_ospeed; }
int cfsetispeed(struct termios *t, speed_t s) { t->__c_ispeed = s; return 0; }
int cfsetospeed(struct termios *t, speed_t s) { t->__c_ospeed = s; return 0; }
int cfsetspeed(struct termios *t, speed_t s) { t->__c_ispeed = t->__c_ospeed = s; return 0; }

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *argument = va_arg(ap, void *);
    va_end(ap);
    switch (request) {
    case TIOCGWINSZ: {
        if (!console(fd)) return -1;
        int size = dosxc_sys_console(DOSXC_CONSOLE_SIZE, 0, 0);
        struct winsize *w = argument;
        memset(w, 0, sizeof(*w));
        w->ws_row = (unsigned short)(size >> 16);
        w->ws_col = (unsigned short)(size & 0xffff);
        return 0;
    }
    case TIOCSWINSZ: return console(fd) ? 0 : -1;
    case TCGETS: return tcgetattr(fd, argument);
    case TCSETS: return tcsetattr(fd, TCSANOW, argument);
    case FIONREAD:
        *(int *)argument = dosxc_sys_isatty(fd) > 0 ? dosxc_sys_console(DOSXC_CONSOLE_POLL, 0, 0) > 0 : 0;
        if (dosxc_sys_isatty(fd) <= 0 && (dosxc_sys_fdpoll(fd, DOSXC_POLL_IN) & DOSXC_POLL_IN) > 0) *(int *)argument = 1;
        return 0;
    case FIONBIO: {
        int flags = dosxc_sys_status_flags(fd);
        if (flags < 0) { errno = -flags; return -1; }
        return (int)__result(dosxc_sys_setfl(fd, *(int *)argument ? flags | O_NONBLOCK : flags & ~O_NONBLOCK));
    }
    case FIOCLEX: return dosxc_sys_fdflags(fd, 1, DOSXC_FD_CLOEXEC) < 0 ? (errno = EBADF, -1) : 0;
    case FIONCLEX: return dosxc_sys_fdflags(fd, 1, 0) < 0 ? (errno = EBADF, -1) : 0;
    }
    errno = ENOTTY;
    return -1;
}

static int any_socket;

static int socket_ready(int fd, int events) {
    int r = dosxc_sys_fdpoll(fd, events);
    if (r == -ENOTSOCK) return -1;
    any_socket = 1;
    return r < 0 ? 0 : r;
}

static int readable(int fd) {
    int r = socket_ready(fd, DOSXC_POLL_IN);
    if (r >= 0) return r != 0;
    if (dosxc_sys_isatty(fd) <= 0) return 1;
    return dosxc_sys_console(DOSXC_CONSOLE_POLL, 0, 0) > 0;
}

static int writable(int fd) {
    int r = socket_ready(fd, DOSXC_POLL_OUT);
    return r >= 0 ? (r & (DOSXC_POLL_OUT | DOSXC_POLL_ERR | DOSXC_POLL_HUP)) != 0 : 1;
}

static void poll_pause(void) {
    dosxc_sys_sleep(any_socket ? 100000 : 1000000);
    any_socket = 0;
}

static int poll_once(struct pollfd *fds, nfds_t count) {
    int ready = 0;
    for (nfds_t i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (dosxc_sys_fdflags(fds[i].fd, 0, 0) < 0) {
            fds[i].revents = POLLNVAL;
        } else {
            if ((fds[i].events & (POLLIN | POLLRDNORM)) && readable(fds[i].fd))
                fds[i].revents |= fds[i].events & (POLLIN | POLLRDNORM);
            if ((fds[i].events & (POLLOUT | POLLWRNORM)) && writable(fds[i].fd))
                fds[i].revents |= fds[i].events & (POLLOUT | POLLWRNORM);
            int status = socket_ready(fds[i].fd, 0);
            if (status > 0) {
                if (status & DOSXC_POLL_ERR) fds[i].revents |= POLLERR;
                if (status & DOSXC_POLL_HUP) fds[i].revents |= POLLHUP;
            }
        }
        if (fds[i].revents) ++ready;
    }
    return ready;
}

static int wait_until(unsigned long long deadline, int forever) {
    if (forever) return 1;
    return dosxc_sys_nanoseconds() < deadline;
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
    unsigned long long deadline = dosxc_sys_nanoseconds() + (unsigned long long)(timeout < 0 ? 0 : timeout) * 1000000ull;
    for (;;) {
        int ready = poll_once(fds, count);
        if (ready || !wait_until(deadline, timeout < 0)) return ready;
        poll_pause();
    }
}

int select(int count, fd_set *read_set, fd_set *write_set, fd_set *error_set, struct timeval *timeout) {
    if (count > FD_SETSIZE) count = FD_SETSIZE;
    unsigned long long deadline = dosxc_sys_nanoseconds();
    if (timeout) deadline += (unsigned long long)timeout->tv_sec * 1000000000ull + (unsigned long long)timeout->tv_usec * 1000ull;
    fd_set in, out;
    FD_ZERO(&in);
    FD_ZERO(&out);
    if (read_set) in = *read_set;
    if (write_set) out = *write_set;
    for (;;) {
        int ready = 0;
        if (read_set) FD_ZERO(read_set);
        if (write_set) FD_ZERO(write_set);
        for (int fd = 0; fd < count; ++fd) {
            if (FD_ISSET(fd, &in) && readable(fd)) { FD_SET(fd, read_set); ++ready; }
            if (FD_ISSET(fd, &out) && writable(fd)) { FD_SET(fd, write_set); ++ready; }
        }
        if (ready || !wait_until(deadline, !timeout)) {
            if (error_set) FD_ZERO(error_set);
            return ready;
        }
        poll_pause();
    }
}
