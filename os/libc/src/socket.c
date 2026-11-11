/*
 * DOS-XC /os/libc/src/socket.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <stdio.h>
#include "libc.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#define CALL2(n, a, b) __result(__dosxc_syscall2(n, (__dosxc_word)(a), (__dosxc_word)(b)))
#define CALL3(n, a, b, c) __result(__dosxc_syscall3(n, (__dosxc_word)(a), (__dosxc_word)(b), (__dosxc_word)(c)))

int socket(int domain, int type, int protocol) {
    int fd = (int)CALL3(DOSXC_SYS_SOCKET, domain, type & 15, protocol);
    if (fd >= 0 && (type & SOCK_NONBLOCK)) dosxc_sys_setfl(fd, O_NONBLOCK);
    if (fd >= 0 && (type & SOCK_CLOEXEC)) dosxc_sys_fdflags(fd, 1, DOSXC_FD_CLOEXEC);
    return fd;
}
int socketpair(int domain, int type, int protocol, int fds[2]) {
    (void)protocol;
    if (domain != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    if ((type & 15) != SOCK_STREAM) { errno = EPROTONOSUPPORT; return -1; }
    if (__result(dosxc_sys_unix(DOSXC_UNIX_PAIR, (__dosxc_word)fds, 0)) < 0) return -1;
    for (int i = 0; i < 2; ++i) {
        if (type & SOCK_NONBLOCK) dosxc_sys_setfl(fds[i], O_NONBLOCK);
        if (type & SOCK_CLOEXEC) dosxc_sys_fdflags(fds[i], 1, DOSXC_FD_CLOEXEC);
    }
    return 0;
}

/* A local socket's message: -2 when FD is not one. */
static ssize_t unix_message(int fd, int operation, void *buffer, size_t length, int flags, int *fds, int *fd_count) {
    struct dosxc_unix_message m = {buffer, length, flags & MSG_DONTWAIT ? DOSXC_MSG_DONTWAIT : 0, fd_count ? *fd_count : 0, fds};
    long r = dosxc_sys_unix(operation, (__dosxc_word)fd, (__dosxc_word)&m);
    if (r == -DOSXC_ENOTSOCK) return -2;
    if (fd_count) *fd_count = m.fd_count;
    if (r == -DOSXC_EPIPE && !(flags & MSG_NOSIGNAL)) raise(SIGPIPE);
    return __result(r);
}

int connect(int fd, const struct sockaddr *address, socklen_t length) { return (int)CALL3(DOSXC_SYS_CONNECT, fd, address, length); }
int bind(int fd, const struct sockaddr *address, socklen_t length) { return (int)CALL3(DOSXC_SYS_BIND, fd, address, length); }
int listen(int fd, int backlog) { return (int)CALL2(DOSXC_SYS_LISTEN, fd, backlog); }
int accept(int fd, struct sockaddr *address, socklen_t *length) { return (int)CALL3(DOSXC_SYS_ACCEPT, fd, address, length); }
int shutdown(int fd, int how) {
    long r = __dosxc_syscall2(DOSXC_SYS_SHUTDOWN, (__dosxc_word)fd, (__dosxc_word)how);
    if (r == -DOSXC_ENOTSOCK && dosxc_sys_fdpoll(fd, 0) >= 0) return 0;
    return (int)__result(r);
}

ssize_t sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length) {
    ssize_t local = unix_message(fd, DOSXC_UNIX_SEND, (void *)buffer, length, flags, 0, 0);
    if (local != -2) return local;
    struct dosxc_message m = {(void *)buffer, length, flags, (void *)address, &address_length};
    return CALL2(DOSXC_SYS_SENDTO, fd, &m);
}
ssize_t recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length) {
    ssize_t local = unix_message(fd, DOSXC_UNIX_RECEIVE, buffer, length, flags, 0, 0);
    if (local != -2) {
        if (local >= 0 && address_length) *address_length = 0;
        return local;
    }
    struct dosxc_message m = {buffer, length, flags, address, address_length};
    return CALL2(DOSXC_SYS_RECVFROM, fd, &m);
}
ssize_t send(int fd, const void *buffer, size_t length, int flags) { return sendto(fd, buffer, length, flags, 0, 0); }
ssize_t recv(int fd, void *buffer, size_t length, int flags) { return recvfrom(fd, buffer, length, flags, 0, 0); }

ssize_t writev(int fd, const struct iovec *vector, int count) {
    ssize_t total = 0;
    for (int i = 0; i < count; ++i) {
        ssize_t n = write(fd, vector[i].iov_base, vector[i].iov_len);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < vector[i].iov_len) break;
    }
    return total;
}
ssize_t readv(int fd, const struct iovec *vector, int count) {
    ssize_t total = 0;
    for (int i = 0; i < count; ++i) {
        ssize_t n = read(fd, vector[i].iov_base, vector[i].iov_len);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < vector[i].iov_len) break;
    }
    return total;
}
static void *join(const struct msghdr *message, size_t *size) {
    *size = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) *size += message->msg_iov[i].iov_len;
    char *joined = malloc(*size ? *size : 1);
    if (!joined) return 0;
    size_t at = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) {
        memcpy(joined + at, message->msg_iov[i].iov_base, message->msg_iov[i].iov_len);
        at += message->msg_iov[i].iov_len;
    }
    return joined;
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    int fds[64], fd_count = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(message); c; c = CMSG_NXTHDR((struct msghdr *)message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < n && fd_count < 64; ++i) fds[fd_count++] = ((int *)CMSG_DATA(c))[i];
    }
    size_t size;
    void *joined = message->msg_iovlen == 1 ? 0 : join(message, &size);
    void *buffer = message->msg_iovlen == 1 ? message->msg_iov[0].iov_base : joined;
    if (message->msg_iovlen == 1) size = message->msg_iov[0].iov_len;
    if (!buffer && message->msg_iovlen > 1) { errno = ENOMEM; return -1; }
    ssize_t r = unix_message(fd, DOSXC_UNIX_SEND, buffer, size, flags, fds, &fd_count);
    if (r == -2) {
        if (fd_count) { free(joined); errno = EOPNOTSUPP; return -1; }
        r = sendto(fd, buffer, size, flags, message->msg_name, message->msg_namelen);
    }
    free(joined);
    return r;
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    message->msg_flags = 0;
    size_t size = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) size += message->msg_iov[i].iov_len;
    char *buffer = message->msg_iovlen == 1 ? message->msg_iov[0].iov_base : malloc(size ? size : 1);
    if (!buffer) { errno = ENOMEM; return -1; }
    int fds[64], room = message->msg_control ? (int)((message->msg_controllen - CMSG_LEN(0)) / sizeof(int)) : 0;
    if (room > 64) room = 64;
    if (room < 0) room = 0;
    int fd_count = room;
    ssize_t r = unix_message(fd, DOSXC_UNIX_RECEIVE, buffer, size, flags, fds, &fd_count);
    if (r == -2) {
        fd_count = 0;
        r = recvfrom(fd, buffer, size, flags, message->msg_name, message->msg_name ? &message->msg_namelen : 0);
    } else if (message->msg_name) {
        message->msg_namelen = 0;
    }
    if (r > 0 && message->msg_iovlen > 1) {
        size_t at = 0;
        for (size_t i = 0; i < message->msg_iovlen && at < (size_t)r; ++i) {
            size_t n = message->msg_iov[i].iov_len;
            if (n > (size_t)r - at) n = (size_t)r - at;
            memcpy(message->msg_iov[i].iov_base, buffer + at, n);
            at += n;
        }
    }
    if (message->msg_iovlen != 1) free(buffer);
    if (fd_count > 0) {
        struct cmsghdr *c = (struct cmsghdr *)message->msg_control;
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN((size_t)fd_count * sizeof(int));
        memcpy(CMSG_DATA(c), fds, (size_t)fd_count * sizeof(int));
        message->msg_controllen = CMSG_SPACE((size_t)fd_count * sizeof(int));
    } else {
        message->msg_controllen = 0;
    }
    return r;
}

/* Socket options of a local socket, which the kernel does not keep: -2 when FD is not one. */
static int unix_option(int fd, int set, int level, int name, void *value, socklen_t *length) {
    if (dosxc_sys_fdpoll(fd, 0) < 0) return -2;
    if (set || level != SOL_SOCKET) return 0;
    int answer;
    switch (name) {
    case SO_SNDBUF: case SO_RCVBUF: answer = 4 << 20; break;
    case SO_TYPE: answer = SOCK_STREAM; break;
    case SO_ERROR: answer = 0; break;
    default: errno = ENOPROTOOPT; return -1;
    }
    if (!value || !length || *length < sizeof(int)) { errno = EINVAL; return -1; }
    *(int *)value = answer;
    *length = sizeof(int);
    return 0;
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *length) {
    struct dosxc_sockopt o = {value, length};
    long r = __dosxc_syscall5(DOSXC_SYS_SOCKOPT, fd, 0, level, name, (__dosxc_word)&o);
    if (r == -DOSXC_ENOTSOCK) { int local = unix_option(fd, 0, level, name, value, length); if (local != -2) return local; }
    return (int)__result(r);
}
int setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
    struct dosxc_sockopt o = {(void *)value, &length};
    long r = __dosxc_syscall5(DOSXC_SYS_SOCKOPT, fd, 1, level, name, (__dosxc_word)&o);
    if (r == -DOSXC_ENOTSOCK) { int local = unix_option(fd, 1, level, name, (void *)value, &length); if (local != -2) return local; }
    return (int)__result(r);
}
int getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    return (int)__result(__dosxc_syscall4(DOSXC_SYS_SOCKNAME, fd, 0, (__dosxc_word)address, (__dosxc_word)length));
}
int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
    return (int)__result(__dosxc_syscall4(DOSXC_SYS_SOCKNAME, fd, 1, (__dosxc_word)address, (__dosxc_word)length));
}

/* ---- addresses as text ------------------------------------------------------ */

int inet_aton(const char *text, struct in_addr *out) {
    unsigned long parts[4];
    int count = 0;
    while (count < 4) {
        char *end;
        if (*text < '0' || *text > '9') return 0;
        parts[count++] = strtoul(text, &end, 0);
        text = end;
        if (*text == '.' && count < 4) ++text;
        else break;
    }
    if (*text) return 0;
    in_addr_t v;
    switch (count) {
    case 1: v = (in_addr_t)parts[0]; break;
    case 2: if (parts[0] > 255 || parts[1] > 0xffffff) return 0; v = (in_addr_t)(parts[0] << 24 | parts[1]); break;
    case 3: if (parts[0] > 255 || parts[1] > 255 || parts[2] > 0xffff) return 0; v = (in_addr_t)(parts[0] << 24 | parts[1] << 16 | parts[2]); break;
    case 4: if (parts[0] > 255 || parts[1] > 255 || parts[2] > 255 || parts[3] > 255) return 0; v = (in_addr_t)(parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]); break;
    default: return 0;
    }
    if (out) out->s_addr = htonl(v);
    return 1;
}
in_addr_t inet_addr(const char *text) {
    struct in_addr a;
    return inet_aton(text, &a) ? a.s_addr : INADDR_NONE;
}
char *inet_ntoa(struct in_addr address) {
    static char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}
const struct in6_addr in6addr_any = {{0}};
const struct in6_addr in6addr_loopback = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_ipv6(const char *s, unsigned char *out) {
    unsigned words[8];
    unsigned char tail[4];
    int count = 0, gap = -1, has_tail = 0;
    if (s[0] == ':') {
        if (s[1] != ':') return 0;
        s += 2;
        gap = 0;
    }
    while (*s) {
        const char *e = s;
        while (*e && *e != ':' && *e != '.') ++e;
        if (*e == '.') {
            if (count > 6 || inet_pton(AF_INET, s, tail) != 1) return 0;
            has_tail = 1;
            break;
        }
        unsigned value = 0;
        int digits = 0;
        for (; hex_digit(*s) >= 0; ++s) {
            if (++digits > 4) return 0;
            value = value * 16 + (unsigned)hex_digit(*s);
        }
        if (!digits || count == 8) return 0;
        words[count++] = value;
        if (!*s) break;
        if (*s++ != ':') return 0;
        if (*s == ':') {
            if (gap >= 0) return 0;
            gap = count;
            ++s;
        } else if (!*s) {
            return 0;
        }
    }
    int total = count + (has_tail ? 2 : 0);
    if (gap < 0 ? total != 8 : total > 7) return 0;
    memset(out, 0, 16);
    int front = gap < 0 ? count : gap;
    for (int i = 0; i < count; ++i) {
        int at = i < front ? i : 8 - (has_tail ? 2 : 0) - (count - i);
        out[at * 2] = (unsigned char)(words[i] >> 8);
        out[at * 2 + 1] = (unsigned char)words[i];
    }
    if (has_tail) memcpy(out + 12, tail, 4);
    return 1;
}

static const char *format_ipv6(const unsigned char *b, char *out, socklen_t size) {
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
    char text[INET6_ADDRSTRLEN];
    int n = 0;
    if (!memcmp(b, mapped, 12)) {
        n = snprintf(text, sizeof(text), "::ffff:%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    } else {
        int best = -1, best_length = 1;
        for (int i = 0; i < 8; ++i) {
            int run = 0;
            while (i + run < 8 && !b[(i + run) * 2] && !b[(i + run) * 2 + 1]) ++run;
            if (run > best_length) { best = i; best_length = run; }
            if (run) i += run - 1;
        }
        for (int i = 0; i < 8; ++i) {
            if (i == best) {
                text[n++] = ':';
                text[n++] = ':';
                i += best_length - 1;
                continue;
            }
            if (n && text[n - 1] != ':') text[n++] = ':';
            n += snprintf(text + n, sizeof(text) - (size_t)n, "%x", (unsigned)b[i * 2] << 8 | b[i * 2 + 1]);
        }
        text[n] = 0;
    }
    if ((socklen_t)n + 1 > size) { errno = ENOSPC; return 0; }
    memcpy(out, text, (size_t)n + 1);
    return out;
}

int inet_pton(int family, const char *text, void *out) {
    if (family == AF_INET6) return parse_ipv6(text, out);
    if (family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    unsigned char bytes[4];
    for (int i = 0; i < 4; ++i) {
        unsigned v = 0, digits = 0;
        while (*text >= '0' && *text <= '9' && digits < 3) { v = v * 10 + (unsigned)(*text++ - '0'); ++digits; }
        if (!digits || v > 255) return 0;
        bytes[i] = (unsigned char)v;
        if (i < 3 && *text++ != '.') return 0;
    }
    if (*text) return 0;
    memcpy(out, bytes, 4);
    return 1;
}
const char *inet_ntop(int family, const void *address, char *out, socklen_t size) {
    if (family == AF_INET6) return format_ipv6(address, out, size);
    if (family != AF_INET) { errno = EAFNOSUPPORT; return 0; }
    const unsigned char *b = address;
    char text[INET_ADDRSTRLEN];
    size_t n = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned v = b[i];
        if (v >= 100) text[n++] = (char)('0' + v / 100);
        if (v >= 10) text[n++] = (char)('0' + v / 10 % 10);
        text[n++] = (char)('0' + v % 10);
        if (i < 3) text[n++] = '.';
    }
    text[n] = 0;
    if (n + 1 > size) { errno = ENOSPC; return 0; }
    memcpy(out, text, n + 1);
    return out;
}
