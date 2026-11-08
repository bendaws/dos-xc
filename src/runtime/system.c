/*
 * DOS-XC /runtime/system.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Module entry points and the dosxc_api system table.
 */

#include "runtime.h"
#include "../kernel/bootinfo.h"

struct command_context *ctx;
int runtime_bits = sizeof(void *) * 8;
struct dosxc_api runtime_api;

static void *api_alloc(dosxc_size size) {
    if (!current || !size || size > (dosxc_size)memory_total()) return 0;
    return memory_alloc(size, PAGE, current->pid, 1);
}

static int api_release(void *block) {
    return current && !memory_free(block, current->pid) ? 0 : -DOSXC_EINVAL;
}

static __attribute__((noreturn)) void api_exit(int status) { process_exit(status); }

static int api_spawn(const char *path, char *const argv[], char *const envp[]) {
    return process_spawn(current, path, argv, envp, 0, 0);
}

static int api_getpid(void) { return current ? current->pid : 0; }
static int api_getppid(void) { return current && current->parent ? current->parent->pid : 0; }

static int api_system(const char *line) {
    if (!line) return 1;
    return DOSXC_EXITED(ctx->execute(line));
}

static dosxc_i64 api_time(void) { return cpu_rtc_time(); }
static dosxc_u64 api_nanoseconds(void) { return cpu_nanoseconds(); }
static void api_sleep(dosxc_u64 nanoseconds) { cpu_sleep(nanoseconds); }

static int api_console(int operation, long a, long b) {
    switch (operation) {
    case DOSXC_CONSOLE_CLEAR: ctx->clear(); return 0;
    case DOSXC_CONSOLE_COLOR: ctx->text_color((unsigned char)a); return 0;
    case DOSXC_CONSOLE_CURSOR: ctx->cursor((unsigned)a, (unsigned)b); return 0;
    case DOSXC_CONSOLE_GETKEY: {
        int key;
        do key = ctx->read_key();
        while (a && key < 0);
        return key;
    }
    case DOSXC_CONSOLE_BEEP: ctx->beep((unsigned)a, (unsigned)b); return 0;
    case DOSXC_CONSOLE_MODE: return files_console_mode((int)a);
    }
    return -DOSXC_EINVAL;
}

static int api_sysinfo(struct dosxc_sysinfo *info) {
    memset(info, 0, sizeof(*info));
    info->abi_version = DOSXC_ABI_VERSION;
    info->bits = (dosxc_u32)runtime_bits;
    info->memory_total = memory_total();
    info->memory_free = memory_available();
    rt_strcpy(info->version, DOSXC_LABEL);
    info->drive = ctx->drive;
    info->firmware = ctx->bios_available ? 'B' : 'U';
    return 0;
}

int runtime_init(struct command_context *context) {
    ctx = context;
    if (memory_init()) {
        rt_print("ELFAPI: too little memory for 32/64-bit programs (at least 96 MiB is needed).\r\n");
        return -1;
    }
    cpu_init();
    cache_init();
    files_init();
    memset(&runtime_api, 0, sizeof(runtime_api));
    runtime_api.version = DOSXC_ABI_VERSION;
    runtime_api.size = sizeof(runtime_api);
    runtime_api.exit = api_exit;
    runtime_api.spawn = api_spawn;
    runtime_api.getpid = api_getpid;
    runtime_api.getppid = api_getppid;
    runtime_api.system = api_system;
    runtime_api.alloc = api_alloc;
    runtime_api.release = api_release;
    runtime_api.time = api_time;
    runtime_api.nanoseconds = api_nanoseconds;
    runtime_api.sleep = api_sleep;
    runtime_api.console = api_console;
    runtime_api.sysinfo = api_sysinfo;
    files_fill_api(&runtime_api);
    return 0;
}

/* DOS command tail -> argv: blanks separate words, double quotes group,
   \" is a literal quote; other backslashes are ordinary path characters. */
static char argument_text[8192];
static char *argument_vector[1024];
static char environment_text[16 * 162];
static char *environment_vector[17];

static int split_arguments(const char *program, const char *tail) {
    uptr used = 0;
    int count = 0;
    uptr n = rt_strlen(program) + 1;
    memcpy(argument_text, program, n);
    argument_vector[count++] = argument_text;
    used = n;
    while (*tail) {
        while (*tail == ' ' || *tail == '\t') ++tail;
        if (!*tail) break;
        if (count + 1 >= (int)(sizeof(argument_vector) / sizeof(*argument_vector))) return -1;
        argument_vector[count++] = argument_text + used;
        int quoted = 0;
        while (*tail && (quoted || (*tail != ' ' && *tail != '\t'))) {
            char c = *tail++;
            if (c == '"') { quoted = !quoted; continue; }
            if (c == '\\' && *tail == '"') c = *tail++;
            if (used + 2 >= sizeof(argument_text)) return -1;
            argument_text[used++] = c;
        }
        argument_text[used++] = 0;
    }
    argument_vector[count] = 0;
    return count;
}

static void build_environment(void) {
    uptr used = 0;
    int count = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const struct environment_variable *v = &ctx->environment[i];
        if (!v->name[0]) continue;
        uptr name = rt_strlen(v->name), value = rt_strlen(v->value);
        environment_vector[count++] = environment_text + used;
        memcpy(environment_text + used, v->name, name);
        environment_text[used + name] = '=';
        memcpy(environment_text + used + name + 1, v->value, value + 1);
        used += name + value + 2;
    }
    environment_vector[count] = 0;
}

static const char *error_text(int error) {
    switch (-error) {
    case DOSXC_ENOENT: return "file not found";
    case DOSXC_ENOEXEC: return "not a valid DOS-XC program";
    case DOSXC_ENOMEM: return "not enough memory";
    case DOSXC_EAGAIN: return "too many nested programs";
    case DOSXC_E2BIG: return "argument list too long";
    case DOSXC_EIO: return "disk read error";
    case DOSXC_EBUSY: return "the program file is still open for writing";
    case DOSXC_EMFILE: case DOSXC_ENFILE: return "too many open files";
    }
    return "cannot start program";
}

int runtime_run(const char *path, const char *arguments, const char *output, int append) {
    char posix[PATH_MAX_RT];
    path_posix(path, posix, sizeof(posix));
    if (split_arguments(posix, arguments ? arguments : "") < 0) {
        rt_print("Command line is too long.\r\n");
        return 1;
    }
    build_environment();
    int status = process_spawn(current, path, argument_vector, environment_vector, output, append);
    if (status < 0) {
        rt_print(posix);
        rt_print(": ");
        rt_print(error_text(status));
        rt_print(".\r\n");
        return 1;
    }
    if ((status & 0x7f) == 6) rt_print("Abnormal program termination.\r\n");
    if ((status & 0x7f) == 2) rt_print("Program interrupted.\r\n");
    if (status & 0x7f) return 128 + (status & 0x7f);
    return (status >> 8) & 0xff;
}
