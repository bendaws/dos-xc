/*
 * DOS-XC /os/libc/src/dl.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dlfcn.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <dosxc/abi.h>
#include <dosxc/syscall.h>
#include "libc.h"

extern char **environ;

static struct __dosxc_rlock lock;
static char error_text[320];
static int have_error;
static struct dosxc_library_init loaded[64];
static unsigned loaded_count;
static int registered;

typedef void (*constructor)(int, char **, char **);
typedef void (*destructor)(void);

static void set_error(const char *text) {
    strncpy(error_text, text, sizeof(error_text) - 1);
    have_error = 1;
}

static void run_finis(void) {
    while (loaded_count) {
        const struct dosxc_library_init *l = &loaded[--loaded_count];
        for (size_t at = l->fini_array_size; at >= sizeof(void *); at -= sizeof(void *)) {
            destructor f = ((destructor *)l->fini_array)[at / sizeof(void *) - 1];
            if (f && f != (destructor)-1) f();
        }
        if (l->fini) ((destructor)l->fini)();
    }
}

void *dlopen(const char *name, int flags) {
    (void)flags;
    if (!name) return (void *)1;
    LOCK_GUARD(&lock);
    struct dosxc_library_opened opened;
    long handle = dosxc_sys_library(DOSXC_LIB_OPEN, name, &opened);
    if (handle < 0) {
        set_error(opened.error[0] ? opened.error : "cannot load library");
        return 0;
    }
    if (opened.count && !registered) {
        registered = 1;
        atexit(run_finis);
    }
    for (unsigned i = 0; i < opened.count; ++i) {
        const struct dosxc_library_init *l = &opened.libraries[i];
        if (loaded_count < sizeof(loaded) / sizeof(loaded[0])) loaded[loaded_count++] = *l;
        if (l->init) ((constructor)l->init)(0, 0, environ);
        for (size_t at = 0; at + sizeof(void *) <= l->init_array_size; at += sizeof(void *)) {
            constructor f = ((constructor *)l->init_array)[at / sizeof(void *)];
            if (f && f != (constructor)-1) f(0, 0, environ);
        }
    }
    return (void *)handle;
}

void *dlsym(void *handle, const char *symbol) {
    void *address;
    if (handle == RTLD_NEXT)
        address = (void *)dosxc_sys_library(DOSXC_LIB_NEXT, __builtin_return_address(0), (void *)symbol);
    else
        address = (void *)dosxc_sys_library(DOSXC_LIB_SYMBOL, (void *)(handle == RTLD_DEFAULT ? 1 : (long)handle), (void *)symbol);
    if (!address) {
        LOCK_GUARD(&lock);
        set_error("undefined symbol");
    }
    return address;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

char *dlerror(void) {
    LOCK_GUARD(&lock);
    if (!have_error) return 0;
    have_error = 0;
    return error_text;
}

int dladdr(const void *address, Dl_info *info) {
    static struct dosxc_library_where where;
    LOCK_GUARD(&lock);
    if (dosxc_sys_library(DOSXC_LIB_ADDRESS, address, &where) <= 0) return 0;
    info->dli_fname = where.path;
    info->dli_fbase = (void *)where.base;
    info->dli_sname = 0;
    info->dli_saddr = 0;
    return 1;
}

extern void *__dosxc_phdr;
extern uintptr_t __dosxc_image_base, __dosxc_phnum;

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data) {
    struct dl_phdr_info info = {0};
    info.dlpi_addr = __dosxc_image_base;
    info.dlpi_name = "";
    info.dlpi_phdr = __dosxc_phdr;
    info.dlpi_phnum = (ElfW(Half))__dosxc_phnum;
    int r = callback(&info, sizeof(info), data);
    struct dosxc_library_where where;
    for (long i = 0; !r && dosxc_sys_library(DOSXC_LIB_OBJECT, (void *)i, &where) > 0; ++i) {
        info.dlpi_addr = where.base;
        info.dlpi_name = where.path;
        info.dlpi_phdr = (const ElfW(Phdr) *)where.phdr;
        info.dlpi_phnum = (ElfW(Half))where.phnum;
        r = callback(&info, sizeof(info), data);
    }
    return r;
}
