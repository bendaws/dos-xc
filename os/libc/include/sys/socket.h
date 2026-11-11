/*
 * DOS-XC /os/libc/include/sys/socket.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#define __NEED_size_t
#define __NEED_ssize_t
#include <sys/_types.h>
#include <sys/cdefs.h>
#include <sys/uio.h>

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __ss_pad[126]; };

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define AF_INET 2
#define AF_INET6 10
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_LOCAL
#define PF_INET AF_INET
#define PF_INET6 AF_INET6
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER 13
#define SO_REUSEPORT 15
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_ACCEPTCONN 30
struct linger { int l_onoff, l_linger; };

#define SOMAXCONN 16
#define MSG_OOB 0x01
#define MSG_PEEK 0x02
#define MSG_DONTROUTE 0x04
#define MSG_CTRUNC 0x08
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level, cmsg_type;
};
#define SCM_RIGHTS 1
#define __CMSG_ALIGN(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_ALIGN(n) __CMSG_ALIGN(n)
#define CMSG_DATA(c) ((unsigned char *)((struct cmsghdr *)(c) + 1))
#define CMSG_SPACE(n) (__CMSG_ALIGN(n) + __CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(n) (__CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *)(m)->msg_control : (struct cmsghdr *)0)
#define CMSG_NXTHDR(m, c) \
    ((c)->cmsg_len < sizeof(struct cmsghdr) || \
     __CMSG_ALIGN((c)->cmsg_len) + sizeof(struct cmsghdr) >= (size_t)((char *)(m)->msg_control + (m)->msg_controllen - (char *)(c)) \
     ? (struct cmsghdr *)0 : (struct cmsghdr *)((char *)(c) + __CMSG_ALIGN((c)->cmsg_len)))

__BEGIN_DECLS
int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fds[2]);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int connect(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *__restrict address, socklen_t *__restrict length);
int accept4(int fd, struct sockaddr *__restrict address, socklen_t *__restrict length, int flags);
int shutdown(int fd, int how);
ssize_t send(int fd, const void *buffer, size_t length, int flags);
ssize_t recv(int fd, void *buffer, size_t length, int flags);
ssize_t sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
ssize_t recvfrom(int fd, void *__restrict buffer, size_t length, int flags, struct sockaddr *__restrict address, socklen_t *__restrict address_length);
ssize_t sendmsg(int fd, const struct msghdr *message, int flags);
ssize_t recvmsg(int fd, struct msghdr *message, int flags);
int getsockopt(int fd, int level, int name, void *__restrict value, socklen_t *__restrict length);
int setsockopt(int fd, int level, int name, const void *value, socklen_t length);
int getsockname(int fd, struct sockaddr *__restrict address, socklen_t *__restrict length);
int getpeername(int fd, struct sockaddr *__restrict address, socklen_t *__restrict length);
__END_DECLS
#endif
