/*
 * DOS-XC /os/libc/src/mman.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#include "libc.h"
#include <dosxc/abi.h>
#include <dosxc/syscall.h>

/* A 32-bit address can look negative, so only the errno range is an error. */
static int failed(long r) { return r < 0 && r >= -4095; }

static long fail(long r) {
    if (!failed(r)) return r;
    errno = (int)-r;
    return -1;
}

#define MAX_OBJECTS 2048
#define MAX_FDS 4096
#define MAX_MAPS 4096
struct shared_object {
    int references;
    int named;
    char name[64];
    char *base;
    size_t size;
};
static struct shared_object objects[MAX_OBJECTS];
static short object_of_fd[MAX_FDS];     /* index + 1, or 0 */
static struct { char *address; int object; } maps[MAX_MAPS];
static volatile int shm_lock_word;
static int maps_high;

static void shm_lock(void) { while (__sync_lock_test_and_set(&shm_lock_word, 1)) sched_yield(); }
static void shm_unlock(void) { __sync_lock_release(&shm_lock_word); }

static struct shared_object *object_at(int fd) {
    int slot = fd - SHM_FD_BASE;
    if (slot < 0 || slot >= MAX_FDS || !object_of_fd[slot]) return 0;
    return &objects[object_of_fd[slot] - 1];
}

/* With the lock held. */
static void object_release(struct shared_object *object) {
    if (--object->references) return;
    if (object->base) dosxc_sys_memory(DOSXC_VM_UNMAP, object->base, (object->size + 4095) & ~(size_t)4095, 0);
    object->base = 0;
    object->size = 0;
}

static int fd_for(struct shared_object *object) {
    for (int slot = 0; slot < MAX_FDS; slot++) {
        if (object_of_fd[slot]) continue;
        object_of_fd[slot] = (short)(object - objects + 1);
        object->references++;
        return SHM_FD_BASE + slot;
    }
    errno = EMFILE;
    return -1;
}

#if defined(__x86_64__)
int shm_open(const char *name, int flags, mode_t mode) {
    (void)mode;
    while (*name == '/') name++;
    long r = dosxc_sys_shm(DOSXC_SHM_OPEN, name, (flags & O_CREAT ? DOSXC_O_CREAT : 0) | (flags & O_EXCL ? DOSXC_O_EXCL : 0));
    if (r >= 0 && (flags & O_TRUNC)) ftruncate((int)r, 0);
    return (int)fail(r);
}

int shm_unlink(const char *name) { return (int)fail(dosxc_sys_shm(DOSXC_SHM_UNLINK, name, 0)); }

int memfd_create(const char *name, unsigned flags) {
    (void)name;
    long r = dosxc_sys_shm(DOSXC_SHM_OPEN, 0, DOSXC_O_CREAT);
    if (r >= 0 && (flags & 1)) dosxc_sys_fdflags((int)r, 1, DOSXC_FD_CLOEXEC);
    return (int)fail(r);
}
#else
int shm_open(const char *name, int flags, mode_t mode) {
    (void)mode;
    while (*name == '/') name++;
    if (strlen(name) >= sizeof objects[0].name) { errno = ENAMETOOLONG; return -1; }
    shm_lock();
    struct shared_object *object = 0, *unused = 0;
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (objects[i].named && !strcmp(objects[i].name, name)) object = &objects[i];
        else if (!objects[i].references && !unused) unused = &objects[i];
    }
    int r = -1;
    if (object && (flags & O_CREAT) && (flags & O_EXCL)) errno = EEXIST;
    else if (!object && !(flags & O_CREAT)) errno = ENOENT;
    else if (!object && !unused) errno = ENFILE;
    else {
        if (!object) {
            object = unused;
            object->named = 1;
            object->references = 1;
            strcpy(object->name, name);
        }
        r = fd_for(object);
    }
    shm_unlock();
    return r;
}

int shm_unlink(const char *name) {
    while (*name == '/') name++;
    int r = -1;
    errno = ENOENT;
    shm_lock();
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (!objects[i].named || strcmp(objects[i].name, name)) continue;
        objects[i].named = 0;
        object_release(&objects[i]);
        r = 0;
        break;
    }
    shm_unlock();
    return r;
}
#endif

int __shm_close(int fd) {
    shm_lock();
    struct shared_object *object = object_at(fd);
    if (object) {
        object_of_fd[fd - SHM_FD_BASE] = 0;
        object_release(object);
    }
    shm_unlock();
    if (!object) { errno = EBADF; return -1; }
    return 0;
}

int __shm_dup(int fd) {
    shm_lock();
    struct shared_object *object = object_at(fd);
    int r = -1;
    if (object) r = fd_for(object);
    else errno = EBADF;
    shm_unlock();
    return r;
}

int __shm_truncate(int fd, off_t size) {
    int r = -1;
    shm_lock();
    struct shared_object *object = object_at(fd);
    if (!object) errno = EBADF;
    else if (size < 0) errno = EINVAL;
    else if (object->base && (size_t)size > ((object->size + 4095) & ~(size_t)4095)) errno = EBUSY;
    else { object->size = (size_t)size; r = 0; }
    shm_unlock();
    return r;
}

int __shm_size(int fd, off_t *size) {
    shm_lock();
    struct shared_object *object = object_at(fd);
    if (object) *size = (off_t)object->size;
    else errno = EBADF;
    shm_unlock();
    return object ? 0 : -1;
}

static void *shm_map(int fd, size_t length, off_t offset) {
    void *result = MAP_FAILED;
    shm_lock();
    struct shared_object *object = object_at(fd);
    int slot = 0;
    while (slot < MAX_MAPS && maps[slot].address) slot++;
    if (!object) errno = EBADF;
    else if ((size_t)offset + length > ((object->size + 4095) & ~(size_t)4095)) errno = ENXIO;
    else if (slot == MAX_MAPS) errno = ENOMEM;
    else {
        if (!object->base) {
            long r = dosxc_sys_memory(DOSXC_VM_MAP, 0, (object->size + 4095) & ~(size_t)4095, PROT_READ | PROT_WRITE);
            if (failed(r)) errno = (int)-r;
            else object->base = (char *)r;
        }
        if (object->base) {
            result = object->base + offset;
            maps[slot].address = result;
            maps[slot].object = (int)(object - objects);
            object->references++;
            if (slot >= maps_high) maps_high = slot + 1;
        }
    }
    shm_unlock();
    return result;
}

static int shm_unmap(void *address) {
    int found = 0;
    shm_lock();
    for (int slot = 0; slot < maps_high; slot++) {
        if (maps[slot].address != address) continue;
        maps[slot].address = 0;
        object_release(&objects[maps[slot].object]);
        found = 1;
        break;
    }
    shm_unlock();
    return found;
}

#define MAX_WRITE_BACKS 256
static struct { char *p; size_t length; off_t offset; int fd; } write_backs[MAX_WRITE_BACKS];
static struct __dosxc_rlock write_back_lock;

static int remember_write_back(char *p, size_t length, int fd, off_t offset) {
    LOCK_GUARD(&write_back_lock);
    for (int i = 0; i < MAX_WRITE_BACKS; i++) {
        if (write_backs[i].p) continue;
        int own = dup(fd);
        if (own < 0) return -1;
        write_backs[i].p = p;
        write_backs[i].length = length;
        write_backs[i].offset = offset;
        write_backs[i].fd = own;
        return 0;
    }
    return -1;
}

static void flush_write_back(int i) {
    struct stat st;
    size_t length = write_backs[i].length;

    if (fstat(write_backs[i].fd, &st)) return;
    if (st.st_size <= write_backs[i].offset) return;
    if ((off_t)length > st.st_size - write_backs[i].offset) length = (size_t)(st.st_size - write_backs[i].offset);
    pwrite(write_backs[i].fd, write_backs[i].p, length, write_backs[i].offset);
}

static void write_back_range(void *address, size_t length, int forget) {
    char *from = address, *to = from + length;

    LOCK_GUARD(&write_back_lock);
    for (int i = 0; i < MAX_WRITE_BACKS; i++) {
        if (!write_backs[i].p || write_backs[i].p >= to || write_backs[i].p + write_backs[i].length <= from) continue;
        flush_write_back(i);
        if (forget && from <= write_backs[i].p && write_backs[i].p + write_backs[i].length <= to) {
            close(write_backs[i].fd);
            write_backs[i].p = 0;
        }
    }
}

void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset) {
    int kind = flags & MAP_TYPE;
    if ((kind != MAP_SHARED && kind != MAP_PRIVATE) || !length || (offset & 4095)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    int file = !(flags & MAP_ANONYMOUS);
#if defined(__x86_64__)
    if (file) {
        int vm = 0;
        if (flags & MAP_FIXED) vm |= DOSXC_VM_FIXED;
        if (flags & MAP_FIXED_NOREPLACE) vm |= DOSXC_VM_NOREPLACE;
        long r = dosxc_sys_map_object(address, length, (long)(protection | vm << 8), fd, (long long)offset);
        if (r != -19) {
            if (failed(r)) { errno = (int)-r; return MAP_FAILED; }
            return (void *)r;
        }
    }
#endif
    if (file && fd >= SHM_FD_BASE) {
        if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) { errno = EINVAL; return MAP_FAILED; }
        return shm_map(fd, length, offset);
    }
    int write_back = file && kind == MAP_SHARED && (protection & PROT_WRITE);
    int vm = 0;
    if (flags & MAP_FIXED) vm |= DOSXC_VM_FIXED;
    if (flags & MAP_FIXED_NOREPLACE) vm |= DOSXC_VM_NOREPLACE;
    if (flags & (MAP_POPULATE | MAP_STACK)) vm |= DOSXC_VM_POPULATE;
    long r = dosxc_sys_memory(DOSXC_VM_MAP, address, length, (long)((file ? PROT_READ | PROT_WRITE : protection) | vm << 8));
    if (failed(r)) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    char *p = (char *)r;
    if (file) {
        size_t done = 0;
        while (done < length) {
            ssize_t n = pread(fd, p + done, length - done, offset + (off_t)done);
            if (n < 0) {
                int saved = errno;
                munmap(p, length);
                errno = saved;
                return MAP_FAILED;
            }
            if (!n) break;
            done += (size_t)n;
        }
        if (write_back && remember_write_back(p, length, fd, offset)) {
            munmap(p, length);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        if (protection != (PROT_READ | PROT_WRITE)) mprotect(p, length, protection);
    }
    return p;
}

int munmap(void *address, size_t length) {
    if (maps_high && shm_unmap(address)) return 0;
    write_back_range(address, length, 1);
    return (int)fail(dosxc_sys_memory(DOSXC_VM_UNMAP, address, length, 0));
}

/* Growing fails even with MREMAP_MAYMOVE: a file mapping could not get its new bytes. */
void *mremap(void *address, size_t old_length, size_t new_length, int flags, ...) {
    (void)flags;
    old_length = (old_length + 4095) & ~(size_t)4095;
    new_length = (new_length + 4095) & ~(size_t)4095;
    if (new_length > old_length || !new_length) { errno = ENOMEM; return MAP_FAILED; }
    if (new_length < old_length && munmap((char *)address + new_length, old_length - new_length)) return MAP_FAILED;
    return address;
}

int mprotect(void *address, size_t length, int protection) { return (int)fail(dosxc_sys_memory(DOSXC_VM_PROTECT, address, length, protection)); }

int madvise(void *address, size_t length, int advice) {
    if (advice != MADV_DONTNEED && advice != MADV_FREE) return 0;
    return (int)fail(dosxc_sys_memory(DOSXC_VM_DISCARD, address, length, 0));
}

int posix_madvise(void *address, size_t length, int advice) {
    (void)address; (void)length; (void)advice;
    return 0;
}

int msync(void *address, size_t length, int flags) { (void)flags; write_back_range(address, length, 0); return 0; }
int mlock(const void *address, size_t length) { (void)address; (void)length; return 0; }
int munlock(const void *address, size_t length) { (void)address; (void)length; return 0; }
