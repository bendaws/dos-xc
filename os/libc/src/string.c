/*
 * DOS-XC /os/libc/src/string.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void *memcpy(void *__restrict d, const void *__restrict s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    if ((uintptr_t)d - (uintptr_t)s >= n) return memcpy(d, s, n);
    void *r = d;
    char *dd = (char *)d + n - 1;
    const char *ss = (const char *)s + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dd), "+S"(ss), "+c"(n) : : "memory");
    return r;
}

void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; --n, ++p)
        if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--)
        if (*--p == (unsigned char)c) return (void *)p;
    return 0;
}

void *rawmemchr(const void *s, int c) {
    const unsigned char *p = s;
    while (*p != (unsigned char)c) ++p;
    return (void *)p;
}

void *memccpy(void *__restrict d, const void *__restrict s, int c, size_t n) {
    unsigned char *dd = d;
    const unsigned char *ss = s;
    for (; n; --n) {
        if ((*dd++ = *ss++) == (unsigned char)c) return dd;
    }
    return 0;
}

void *mempcpy(void *__restrict d, const void *__restrict s, size_t n) { return (char *)memcpy(d, s, n) + n; }

void *memmem(const void *haystack, size_t hn, const void *needle, size_t nn) {
    const char *h = haystack, *n = needle;
    if (!nn) return (void *)h;
    for (; hn >= nn; ++h, --hn)
        if (*h == *n && !memcmp(h, n, nn)) return (void *)h;
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) ++i;
    return i;
}

char *strcpy(char *__restrict d, const char *__restrict s) {
    char *r = d;
    while ((*d++ = *s++)) {
    }
    return r;
}

char *stpcpy(char *__restrict d, const char *__restrict s) {
    while ((*d = *s++)) ++d;
    return d;
}

char *strncpy(char *__restrict d, const char *__restrict s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}

char *stpncpy(char *__restrict d, const char *__restrict s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    char *end = d + i;
    for (; i < n; ++i) d[i] = 0;
    return end;
}

size_t strlcpy(char *__restrict d, const char *__restrict s, size_t n) {
    size_t length = strlen(s);
    if (n) {
        size_t copy = length < n - 1 ? length : n - 1;
        memcpy(d, s, copy);
        d[copy] = 0;
    }
    return length;
}

size_t strlcat(char *__restrict d, const char *__restrict s, size_t n) {
    size_t used = strnlen(d, n);
    if (used == n) return n + strlen(s);
    return used + strlcpy(d + used, s, n - used);
}

char *strcat(char *__restrict d, const char *__restrict s) {
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *__restrict d, const char *__restrict s, size_t n) {
    char *end = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; ++i) end[i] = s[i];
    end[i] = 0;
    return d;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) ++a, ++b;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; --n, ++a, ++b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; --n, ++a, ++b) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }
int strcoll_l(const char *a, const char *b, locale_t l) { (void)l; return strcmp(a, b); }

size_t strxfrm(char *__restrict d, const char *__restrict s, size_t n) {
    size_t length = strlen(s);
    if (length < n) strcpy(d, s);
    return length;
}
size_t strxfrm_l(char *__restrict d, const char *__restrict s, size_t n, locale_t l) { (void)l; return strxfrm(d, s, n); }

char *strchr(const char *s, int c) {
    for (;; ++s) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

char *strchrnul(const char *s, int c) {
    while (*s && *s != (char)c) ++s;
    return (char *)s;
}

char *strrchr(const char *s, int c) {
    const char *found = 0;
    for (;; ++s) {
        if (*s == (char)c) found = s;
        if (!*s) return (char *)found;
    }
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) ++n;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) ++n;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : 0;
}

char *strstr(const char *h, const char *n) {
    size_t length = strlen(n);
    if (!length) return (char *)h;
    for (; *h; ++h)
        if (*h == *n && !strncmp(h, n, length)) return (char *)h;
    return 0;
}

char *strcasestr(const char *h, const char *n) {
    size_t length = strlen(n);
    if (!length) return (char *)h;
    for (; *h; ++h)
        if (!strncasecmp(h, n, length)) return (char *)h;
    return 0;
}

char *strtok_r(char *__restrict s, const char *__restrict delimiters, char **__restrict state) {
    if (!s) s = *state;
    if (!s) return 0;
    s += strspn(s, delimiters);
    if (!*s) {
        *state = 0;
        return 0;
    }
    char *end = s + strcspn(s, delimiters);
    if (*end) *end++ = 0;
    else end = 0;
    *state = end;
    return s;
}

char *strtok(char *__restrict s, const char *__restrict delimiters) {
    static char *state;
    return strtok_r(s, delimiters, &state);
}

char *strsep(char **__restrict string, const char *__restrict delimiters) {
    char *s = *string;
    if (!s) return 0;
    char *end = s + strcspn(s, delimiters);
    if (*end) *end++ = 0;
    else end = 0;
    *string = end;
    return s;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    return d ? memcpy(d, s, n) : 0;
}

char *strndup(const char *s, size_t n) {
    size_t length = strnlen(s, n);
    char *d = malloc(length + 1);
    if (!d) return 0;
    memcpy(d, s, length);
    d[length] = 0;
    return d;
}

static const char *const messages[] = {
    [0] = "Success", [EPERM] = "Operation not permitted", [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process", [EINTR] = "Interrupted system call", [EIO] = "Input/output error",
    [ENXIO] = "No such device or address", [E2BIG] = "Argument list too long", [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor", [ECHILD] = "No child processes", [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Cannot allocate memory", [EACCES] = "Permission denied", [EFAULT] = "Bad address",
    [ENOTBLK] = "Block device required", [EBUSY] = "Device or resource busy", [EEXIST] = "File exists",
    [EXDEV] = "Invalid cross-device link", [ENODEV] = "No such device", [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory", [EINVAL] = "Invalid argument", [ENFILE] = "Too many open files in system",
    [EMFILE] = "Too many open files", [ENOTTY] = "Inappropriate ioctl for device", [ETXTBSY] = "Text file busy",
    [EFBIG] = "File too large", [ENOSPC] = "No space left on device", [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system", [EMLINK] = "Too many links", [EPIPE] = "Broken pipe",
    [EDOM] = "Numerical argument out of domain", [ERANGE] = "Numerical result out of range",
    [EDEADLK] = "Resource deadlock avoided", [ENAMETOOLONG] = "File name too long", [ENOLCK] = "No locks available",
    [ENOSYS] = "Function not implemented", [ENOTEMPTY] = "Directory not empty",
    [ELOOP] = "Too many levels of symbolic links", [ENOMSG] = "No message of desired type",
    [EIDRM] = "Identifier removed", [ENOSTR] = "Device not a stream", [ENODATA] = "No data available",
    [ETIME] = "Timer expired", [ENOSR] = "Out of streams resources", [ENOLINK] = "Link has been severed",
    [EPROTO] = "Protocol error", [EMULTIHOP] = "Multihop attempted", [EBADMSG] = "Bad message",
    [EOVERFLOW] = "Value too large for defined data type", [EILSEQ] = "Invalid or incomplete multibyte or wide character",
    [ENOTSOCK] = "Socket operation on non-socket", [EDESTADDRREQ] = "Destination address required",
    [EMSGSIZE] = "Message too long", [EPROTOTYPE] = "Protocol wrong type for socket",
    [ENOPROTOOPT] = "Protocol not available", [EPROTONOSUPPORT] = "Protocol not supported",
    [EOPNOTSUPP] = "Operation not supported", [EAFNOSUPPORT] = "Address family not supported by protocol",
    [EADDRINUSE] = "Address already in use", [EADDRNOTAVAIL] = "Cannot assign requested address",
    [ENETDOWN] = "Network is down", [ENETUNREACH] = "Network is unreachable",
    [ENETRESET] = "Network dropped connection on reset", [ECONNABORTED] = "Software caused connection abort",
    [ECONNRESET] = "Connection reset by peer", [ENOBUFS] = "No buffer space available",
    [EISCONN] = "Transport endpoint is already connected", [ENOTCONN] = "Transport endpoint is not connected",
    [ETIMEDOUT] = "Connection timed out", [ECONNREFUSED] = "Connection refused", [EHOSTUNREACH] = "No route to host",
    [EALREADY] = "Operation already in progress", [EINPROGRESS] = "Operation now in progress",
    [ESTALE] = "Stale file handle", [EDQUOT] = "Disk quota exceeded", [ECANCELED] = "Operation canceled",
    [EOWNERDEAD] = "Owner died", [ENOTRECOVERABLE] = "State not recoverable",
};

char *strerror(int error) {
    static char unknown[32];
    if (error >= 0 && (size_t)error < sizeof(messages) / sizeof(*messages) && messages[error])
        return (char *)messages[error];
    char *p = unknown + sizeof(unknown) - 1;
    unsigned value = error < 0 ? -(unsigned)error : (unsigned)error;
    *p = 0;
    do *--p = (char)('0' + value % 10);
    while (value /= 10);
    if (error < 0) *--p = '-';
    static const char prefix[] = "Unknown error ";
    p -= sizeof(prefix) - 1;
    memcpy(p, prefix, sizeof(prefix) - 1);
    return p;
}

char *strerror_l(int error, locale_t l) { (void)l; return strerror(error); }

int strerror_r(int error, char *buffer, size_t size) {
    const char *text = strerror(error);
    if (strlen(text) >= size) {
        if (size) {
            memcpy(buffer, text, size - 1);
            buffer[size - 1] = 0;
        }
        return ERANGE;
    }
    strcpy(buffer, text);
    return 0;
}

int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
void bcopy(const void *s, void *d, size_t n) { memmove(d, s, n); }
void bzero(void *d, size_t n) { memset(d, 0, n); }
void explicit_bzero(void *d, size_t n) {
    memset(d, 0, n);
    __asm__ volatile("" : : "r"(d) : "memory");
}
char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }
int ffs(int v) { return __builtin_ffs(v); }
int ffsl(long v) { return __builtin_ffsl(v); }
int ffsll(long long v) { return __builtin_ffsll(v); }
