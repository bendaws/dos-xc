/*
 * DOS-XC C library - program start and termination
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>

char **environ;
const char *__progname = "";
const char *__progname_full = "";
uintptr_t __stack_chk_guard;
uintptr_t __dosxc_image_base;
void *__dosxc_phdr;
size_t __dosxc_phnum;

extern void (*__preinit_array_start[])(void) __attribute__((weak, visibility("hidden")));
extern void (*__preinit_array_end[])(void) __attribute__((weak, visibility("hidden")));
extern void (*__init_array_start[])(void) __attribute__((weak, visibility("hidden")));
extern void (*__init_array_end[])(void) __attribute__((weak, visibility("hidden")));
extern void (*__fini_array_start[])(void) __attribute__((weak, visibility("hidden")));
extern void (*__fini_array_end[])(void) __attribute__((weak, visibility("hidden")));
void _init(void) __attribute__((weak));
void _fini(void) __attribute__((weak));
int main(int, char **, char **);

/* What crt0 passes about the program (see crt0-*.S). */
struct start_info {
    void (**preinit_start)(void), (**preinit_end)(void);
    void (**init_start)(void), (**init_end)(void);
    void (**fini_start)(void), (**fini_end)(void);
    void (*init)(void), (*fini)(void);
};
static struct start_info program;
/* Shared library constructors/destructors from the system loader. */
static void (**library_inits)(void), (**library_finis)(void);

void __libc_start(uintptr_t *sp, const struct start_info *info) __attribute__((noreturn, used));
void __libc_start(uintptr_t *sp, const struct start_info *info) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char **end = envp;
    while (*end) ++end;
    for (uintptr_t *aux = (uintptr_t *)(end + 1); aux[0]; aux += 2) {
        switch (aux[0]) {
        case DOSXC_AT_BASE: __dosxc_image_base = aux[1]; break;
        case DOSXC_AT_INITS: library_inits = (void (**)(void))aux[1]; break;
        case DOSXC_AT_FINIS: library_finis = (void (**)(void))aux[1]; break;
        case 3: __dosxc_phdr = (void *)aux[1]; break;
        case 5: __dosxc_phnum = aux[1]; break;
        case 25: memcpy(&__stack_chk_guard, (void *)aux[1], sizeof(__stack_chk_guard)); break;
        }
    }
    __stack_chk_guard &= ~(uintptr_t)0xff;     /* a NUL byte stops string overruns */
    environ = envp;
    if (argc && argv[0]) {
        __progname_full = argv[0];
        const char *name = argv[0];
        for (const char *p = name; *p; ++p)
            if (*p == '/' || *p == '\\') name = p + 1;
        __progname = name;
    }
    if (info) program = *info;
    else {                                /* crt0 of an older C library */
        program.preinit_start = __preinit_array_start;
        program.preinit_end = __preinit_array_end;
        program.init_start = __init_array_start;
        program.init_end = __init_array_end;
        program.fini_start = __fini_array_start;
        program.fini_end = __fini_array_end;
        program.init = _init;
        program.fini = _fini;
    }
    for (void (**f)(void) = library_inits; f && *f; ++f) (*f)();
    for (void (**f)(void) = program.preinit_start; f < program.preinit_end; ++f) (*f)();
    if (program.init) program.init();
    for (void (**f)(void) = program.init_start; f < program.init_end; ++f) (*f)();
    exit(main(argc, argv, envp));
}

/* ---- atexit / __cxa_atexit ---- */
struct exit_handler { void (*function)(void *); void *argument; void *dso; int plain; };
static struct exit_handler builtin_handlers[64], *handlers = builtin_handlers;
static int handler_count, handler_capacity = 64;
static int exiting;

static int add_handler(void (*function)(void *), void *argument, void *dso, int plain) {
    if (handler_count == handler_capacity) {
        struct exit_handler *bigger = malloc(sizeof(*bigger) * handler_capacity * 2);
        if (!bigger) return -1;
        memcpy(bigger, handlers, sizeof(*bigger) * handler_count);
        if (handlers != builtin_handlers) free(handlers);
        handlers = bigger;
        handler_capacity *= 2;
    }
    handlers[handler_count++] = (struct exit_handler){function, argument, dso, plain};
    return 0;
}

int atexit(void (*function)(void)) { return add_handler((void (*)(void *))function, 0, 0, 1); }
int __cxa_atexit(void (*function)(void *), void *argument, void *dso) { return add_handler(function, argument, dso, 0); }
void __cxa_finalize(void *dso) {
    for (int i = handler_count; i-- > 0;) {
        struct exit_handler h = handlers[i];
        if (!h.function || (dso && h.dso != dso)) continue;
        handlers[i].function = 0;
        if (h.plain) ((void (*)(void))h.function)();
        else h.function(h.argument);
    }
}

static void (*quick_handlers[32])(void);
static int quick_count;
int at_quick_exit(void (*function)(void)) {
    if (quick_count == 32) return -1;
    quick_handlers[quick_count++] = function;
    return 0;
}

hidden void __run_exit_handlers(void) {
    __cxa_finalize(0);
    for (void (**f)(void) = program.fini_end; f > program.fini_start;) (*--f)();
    if (program.fini) program.fini();
    for (void (**f)(void) = library_finis; f && *f; ++f) (*f)();
}

void exit(int status) {
    if (!exiting) {
        exiting = 1;
        __run_exit_handlers();
    }
    __stdio_exit();
    dosxc_sys_exit(DOSXC_EXITED(status));
}

void _Exit(int status) { dosxc_sys_exit(DOSXC_EXITED(status)); }
void _exit(int status) { dosxc_sys_exit(DOSXC_EXITED(status)); }

void quick_exit(int status) {
    while (quick_count) quick_handlers[--quick_count]();
    _Exit(status);
}

void __stack_chk_fail(void) {
    static const char message[] = "*** stack smashing detected ***: terminated\n";
    dosxc_sys_write(2, message, sizeof(message) - 1);
    dosxc_sys_exit(DOSXC_SIGNALED(SIGABRT));
}
void __stack_chk_fail_local(void) { __stack_chk_fail(); }

void __assert_fail(const char *expression, const char *file, unsigned line, const char *function) {
    fflush(stdout);
    fprintf(stderr, "%s: %s:%u: %s%sAssertion `%s' failed.\n", __progname, file, line,
            function ? function : "", function ? ": " : "", expression);
    abort();
}

const char *getprogname(void) { return __progname; }
