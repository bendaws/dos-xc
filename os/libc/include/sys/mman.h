/*
 * DOS-XC /os/libc/include/sys/mman.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_TYPE 0x0f
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_STACK 0x20000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_MERGEABLE 12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE 14
#define MADV_NOHUGEPAGE 15
#define MADV_DONTDUMP 16
#define MADV_DODUMP 17
#define POSIX_MADV_NORMAL MADV_NORMAL
#define POSIX_MADV_RANDOM MADV_RANDOM
#define POSIX_MADV_SEQUENTIAL MADV_SEQUENTIAL
#define POSIX_MADV_WILLNEED MADV_WILLNEED
#define POSIX_MADV_DONTNEED 4

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
int madvise(void *address, size_t length, int advice);
int posix_madvise(void *address, size_t length, int advice);
int msync(void *address, size_t length, int flags);
int mlock(const void *address, size_t length);
int munlock(const void *address, size_t length);
void *mremap(void *address, size_t old_length, size_t new_length, int flags, ...);
int shm_open(const char *name, int flags, mode_t mode);
int shm_unlink(const char *name);
int memfd_create(const char *name, unsigned flags);
#define MFD_CLOEXEC 1
#define MFD_ALLOW_SEALING 2

#ifdef __cplusplus
}
#endif

#endif
