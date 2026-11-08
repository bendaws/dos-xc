/*
 * DOS-XC C library - sockets
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
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
    (void)domain; (void)type; (void)protocol; (void)fds;
    errno = EOPNOTSUPP;
    return -1;
}
int connect(int fd, const struct sockaddr *address, socklen_t length) { return (int)CALL3(DOSXC_SYS_CONNECT, fd, address, length); }
int bind(int fd, const struct sockaddr *address, socklen_t length) { return (int)CALL3(DOSXC_SYS_BIND, fd, address, length); }
int listen(int fd, int backlog) { return (int)CALL2(DOSXC_SYS_LISTEN, fd, backlog); }
int accept(int fd, struct sockaddr *address, socklen_t *length) { return (int)CALL3(DOSXC_SYS_ACCEPT, fd, address, length); }
int shutdown(int fd, int how) { return (int)CALL2(DOSXC_SYS_SHUTDOWN, fd, how); }

ssize_t sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length) {
    struct dosxc_message m = {(void *)buffer, length, flags, (void *)address, &address_length};
    return CALL2(DOSXC_SYS_SENDTO, fd, &m);
}
ssize_t recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length) {
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
ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    if (message->msg_iovlen == 1)
        return sendto(fd, message->msg_iov[0].iov_base, message->msg_iov[0].iov_len, flags, message->msg_name, message->msg_namelen);
    size_t size = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) size += message->msg_iov[i].iov_len;
    char *joined = malloc(size ? size : 1);
    if (!joined) { errno = ENOMEM; return -1; }
    size_t at = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) {
        memcpy(joined + at, message->msg_iov[i].iov_base, message->msg_iov[i].iov_len);
        at += message->msg_iov[i].iov_len;
    }
    ssize_t r = sendto(fd, joined, size, flags, message->msg_name, message->msg_namelen);
    free(joined);
    return r;
}
ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    if (message->msg_iovlen != 1) { errno = EINVAL; return -1; }
    message->msg_flags = 0;
    message->msg_controllen = 0;
    return recvfrom(fd, message->msg_iov[0].iov_base, message->msg_iov[0].iov_len, flags, message->msg_name, message->msg_name ? &message->msg_namelen : 0);
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *length) {
    struct dosxc_sockopt o = {value, length};
    return (int)__result(__dosxc_syscall5(DOSXC_SYS_SOCKOPT, fd, 0, level, name, (__dosxc_word)&o));
}
int setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
    struct dosxc_sockopt o = {(void *)value, &length};
    return (int)__result(__dosxc_syscall5(DOSXC_SYS_SOCKOPT, fd, 1, level, name, (__dosxc_word)&o));
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
int inet_pton(int family, const char *text, void *out) {
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
