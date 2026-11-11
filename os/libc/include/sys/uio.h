/*
 * DOS-XC /os/libc/include/sys/uio.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_UIO_H
#define _SYS_UIO_H
#define __NEED_size_t
#define __NEED_ssize_t
#include <sys/_types.h>
#include <sys/cdefs.h>
struct iovec { void *iov_base; size_t iov_len; };
__BEGIN_DECLS
ssize_t readv(int fd, const struct iovec *vector, int count);
ssize_t writev(int fd, const struct iovec *vector, int count);
__END_DECLS
#endif
