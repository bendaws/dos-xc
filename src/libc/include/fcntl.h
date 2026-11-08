/*
 * DOS-XC C library - fcntl.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _FCNTL_H
#define _FCNTL_H
#define __NEED_off_t
#define __NEED_pid_t
#define __NEED_mode_t
#include <sys/_types.h>
#include <dosxc/abi.h>

#define O_RDONLY DOSXC_O_RDONLY
#define O_WRONLY DOSXC_O_WRONLY
#define O_RDWR DOSXC_O_RDWR
#define O_ACCMODE DOSXC_O_ACCMODE
#define O_CREAT DOSXC_O_CREAT
#define O_EXCL DOSXC_O_EXCL
#define O_NOCTTY DOSXC_O_NOCTTY
#define O_TRUNC DOSXC_O_TRUNC
#define O_APPEND DOSXC_O_APPEND
#define O_NONBLOCK DOSXC_O_NONBLOCK
#define O_NDELAY O_NONBLOCK
#define O_DIRECTORY DOSXC_O_DIRECTORY
#define O_CLOEXEC DOSXC_O_CLOEXEC
#define O_NOFOLLOW 0x20000
#define O_SYNC 0x101000
#define O_DSYNC 0x1000
#define O_RSYNC O_SYNC
#define O_LARGEFILE 0
#define O_BINARY 0
#define O_TEXT 0

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EACCESS 0x200
#define AT_SYMLINK_FOLLOW 0x400

struct flock { short l_type, l_whence; off_t l_start, l_len; pid_t l_pid; };

__BEGIN_DECLS
int open(const char *, int, ...);
int openat(int, const char *, int, ...);
int creat(const char *, mode_t);
int fcntl(int, int, ...);
int posix_fadvise(int, off_t, off_t, int);
__END_DECLS
#endif
