/*
 * DOS-XC C library - errno and miscellaneous interfaces
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <cpuid.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/select.h>

int errno;
int *__errno_location(void) { return &errno; }

/* Random bytes: RDRAND where the processor has it.  Elsewhere they come from
 * the time-stamp counter and clock, which is enough for hash seeds and the
 * like but is not suitable for cryptographic keys. */
static unsigned long long entropy_word(void) {
    static unsigned long long state;
    static int have_rdrand = -1;
    unsigned int low, high, a, b, c, d;
    if (have_rdrand < 0) have_rdrand = __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 30));
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    state += 0x9e3779b97f4a7c15ULL ^ (((unsigned long long)high << 32) | low) ^ dosxc_sys_nanoseconds();
    unsigned long long z = state;
    if (have_rdrand) {
        for (int word = 0; word < 2; ++word) {
            unsigned int value = 0;
            unsigned char ok = 0;
            for (int tries = 0; tries < 10 && !ok; ++tries) __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=q"(ok));
            z ^= (unsigned long long)value << (word * 32);
        }
    }
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

ssize_t getrandom(void *buffer, size_t size, unsigned flags) {
    (void)flags;
    unsigned char *out = buffer;
    for (size_t done = 0; done < size; ++done) {
        static unsigned long long word;
        if (done % 8 == 0) word = entropy_word();
        out[done] = (unsigned char)word;
        word >>= 8;
    }
    return (ssize_t)size;
}

int getentropy(void *buffer, size_t size) {
    if (size > 256) { errno = EIO; return -1; }
    getrandom(buffer, size, 0);
    return 0;
}

int posix_fadvise(int fd, off_t offset, off_t length, int advice) {
    (void)fd;
    (void)offset;
    (void)length;
    (void)advice;
    return 0;
}
