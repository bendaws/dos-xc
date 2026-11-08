/*
 * DOS-XC C library - poll.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _POLL_H
#define _POLL_H
#include <sys/cdefs.h>
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100
typedef unsigned long nfds_t;
struct pollfd { int fd; short events, revents; };
__BEGIN_DECLS
int poll(struct pollfd *, nfds_t, int);
__END_DECLS
#endif
