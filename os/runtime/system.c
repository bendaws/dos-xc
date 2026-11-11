/*
 * DOS-XC /os/runtime/system.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"
#include "usb.h"
#include "net.h"
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

static char env_text[16384];          /* "NAME=value\0" entries */
static uptr env_used;
static char *env_vector[512];

static char *env_find(const char *name) {
    uptr length = rt_strlen(name);
    for (uptr at = 0; at < env_used; at += rt_strlen(env_text + at) + 1) {
        char *entry = env_text + at;
        uptr i = 0;
        while (i < length && entry[i] && rt_upper((u8)entry[i]) == rt_upper((u8)name[i])) ++i;
        if (i == length && entry[i] == '=') return entry;
    }
    return 0;
}

static int env_set(const char *name, const char *value) {
    uptr length = rt_strlen(name);
    if (!length || length > 255) return -DOSXC_EINVAL;
    for (uptr i = 0; i < length; ++i)
        if (name[i] == '=') return -DOSXC_EINVAL;
    char *old = env_find(name);
    if (old) {
        uptr size = rt_strlen(old) + 1, at = (uptr)(old - env_text);
        memmove(old, old + size, env_used - at - size);
        env_used -= size;
    }
    if (!value) return 0;
    uptr size = length + rt_strlen(value) + 2;
    if (env_used + size + 1 > sizeof(env_text)) return -DOSXC_ENOMEM;
    char *entry = env_text + env_used;
    for (uptr i = 0; i < length; ++i) entry[i] = (char)rt_upper((u8)name[i]);
    entry[length] = '=';
    rt_strcpy(entry + length + 1, value);
    env_used += size;
    return 0;
}

char **environment_master(void) {
    unsigned count = 0;
    for (uptr at = 0; at < env_used && count + 1 < sizeof(env_vector) / sizeof(*env_vector);
         at += rt_strlen(env_text + at) + 1)
        env_vector[count++] = env_text + at;
    env_vector[count] = 0;
    return env_vector;
}

static int api_environ(int operation, const char *name, char *buffer, dosxc_size size) {
    switch (operation) {
    case DOSXC_ENV_GET: {
        char *entry = name ? env_find(name) : 0;
        if (!entry) return -DOSXC_ENOENT;
        const char *value = entry + rt_strlen(name) + 1;
        uptr n = rt_strlen(value);
        if (n + 1 > size) return -DOSXC_ERANGE;
        memcpy(buffer, value, n + 1);
        return (int)n;
    }
    case DOSXC_ENV_SET: return name ? env_set(name, buffer) : -DOSXC_EINVAL;
    case DOSXC_ENV_LIST:
        if (env_used + 1 <= size) {
            memcpy(buffer, env_text, env_used);
            buffer[env_used] = 0;
        }
        return (int)(env_used + 1);
    }
    return -DOSXC_EINVAL;
}

static void env_default(const char *name, const char *suffix) {
    char value[PATH_MAX_RT];
    value[0] = (char)rt_upper(ctx->drive);
    value[1] = ':';
    rt_strcpy(value + 2, suffix);
    env_set(name, value);
}

static void env_init(void) {
    env_used = 0;
    env_default("COMSPEC", "\\SYSTEM\\COMMAND.EXE");
    env_default("PATH", "\\SYSTEM");
    env_default("TMPDIR", "\\TEMP");
    env_default("TEMP", "\\TEMP");
    env_default("HOME", "\\Home");
    env_set("TERM", "ansi");
    env_set("PROMPT", "$P$G");
}

/* ---- processes -------------------------------------------------------- */

static int api_system(const char *line) {
    if (!line) return 1;
    char shell[PATH_MAX_RT];
    shell[0] = (char)rt_upper(ctx->drive);
    shell[1] = ':';
    rt_strcpy(shell + 2, "\\SYSTEM\\COMMAND.EXE");
    char *argv[4] = {"COMMAND.EXE", "/C", (char *)line, 0};
    int status = process_spawn(current, shell, argv, 0, 0, 0);
    return status < 0 ? DOSXC_EXITED(127) : status;
}

static struct saved_context dos_resume;
static int dos_pending;

static int api_rundos(const char *path, const char *arguments) {
    if (!ctx->run_dos) return -DOSXC_ENOSYS;
    char program[PATH_MAX_RT], directory[PATH_MAX_RT];
    int r = path_resolve(current, path, program);
    if (r) return r;
    struct fat32_info info;
    if (fs_stat(program, &info)) return -DOSXC_ENOENT;
    if (info.attributes & DOSXC_ATTR_DIRECTORY) return -DOSXC_EACCES;
    char system = (char)rt_upper(ctx->drive);
    if (rt_upper(program[0]) != system) return -DOSXC_EXDEV;
    if (current && rt_upper(current->cwd[0]) == system) rt_strcpy(directory, current->cwd);
    else {
        rt_strcpy(directory, program);
        uptr n = rt_strlen(directory);
        while (n > 3 && directory[n - 1] != '\\') --n;
        directory[n > 3 ? n - 1 : 3] = 0;
    }
    if (!arguments) arguments = "";
    if (ctx->short_path) {
        char shorter[PATH_MAX_RT];
        struct fat32_volume *volume = volume_of(program);
        if (volume && !ctx->short_path(volume, program + 2, shorter + 2, sizeof(shorter) - 2)) {
            shorter[0] = program[0];
            shorter[1] = ':';
            rt_strcpy(program, shorter);
        }
        if (volume && !ctx->short_path(volume, directory + 2, shorter + 2, sizeof(shorter) - 2)) {
            shorter[0] = directory[0];
            shorter[1] = ':';
            rt_strcpy(directory, shorter);
        }
    }
    if (rt_strlen(program) >= 128 || rt_strlen(arguments) > 126 || rt_strlen(directory) >= 128) return -DOSXC_E2BIG;
    files_flush_all();
    display_text();
    terminal_mouse_hide();
    sched_pause();
    int value = runtime_save(&dos_resume);
    if (!value) {
        dos_pending = 1;
        cpu_kernel_idt();
        bridge_runtime_active(0);
        ctx->run_dos(program + 2, arguments, directory + 2);
        bridge_runtime_active(1);
        cpu_resume();
        sched_resume();
        dos_pending = 0;
        return -DOSXC_EINVAL;
    }
    return value - 1;
}

static int api_shell(int operation, const char *a, long b) {
    (void)b;
    if (operation == DOSXC_SHELL_SPEED) {
        if (a) {
            uptr speed = (uptr)a;
            if (speed < 1 || speed > 100) return -DOSXC_EINVAL;
            ctx->speed = (unsigned)speed;
        }
        return (int)ctx->speed;
    }
    if (operation == DOSXC_SHELL_CHDIR) {
        char canonical[PATH_MAX_RT];
        int r = path_resolve(current, a, canonical);
        if (r) return r;
        struct fat32_info info;
        if (fs_stat(canonical, &info)) return -DOSXC_ENOENT;
        if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
        struct process *shell = process_root();
        if (shell) rt_strcpy(shell->cwd, canonical);
        if (current) rt_strcpy(current->cwd, canonical);
        return 0;
    }
    return -DOSXC_EINVAL;
}

static int api_disk(int operation, unsigned index, uptr lba, void *buffer, unsigned count) {
    switch (operation) {
    case DOSXC_DISK_INFO: {
        int r = disk_info(index, buffer);
        return r ? r : (int)disk_count();
    }
    case DOSXC_DISK_READ: return disk_io(index, lba, count, buffer, 0);
    case DOSXC_DISK_WRITE: return disk_io(index, lba, count, buffer, 1);
    case DOSXC_DISK_RESCAN: volumes_scan(); return 0;
    }
    return -DOSXC_EINVAL;
}

static int api_reboot(void) {
    files_flush_all();
    drivers_shutdown();
    usb_shutdown();
    ctx->reboot();
    return -DOSXC_ENOSYS;
}

static dosxc_i64 api_time(void) { return cpu_rtc_time(); }
static dosxc_u64 api_nanoseconds(void) { return cpu_nanoseconds(); }
static void api_sleep(dosxc_u64 nanoseconds) {
    if (!net_active()) { sched_sleep(nanoseconds); return; }
    u64 end = cpu_nanoseconds() + nanoseconds;
    for (;;) {
        net_poll();
        u64 now = cpu_nanoseconds();
        if (now >= end) return;
        sched_sleep(end - now < 1000000 ? end - now : 1000000);
    }
}

static int sound_volume = 100, speaker_owner;

static void speaker_set(unsigned divisor) {
    u8 state = rt_inb(0x61);
    if (!divisor || !sound_volume) { rt_outb(0x61, (u8)(state & 0xfc)); return; }
    if (divisor > 65535) divisor = 65535;
    rt_outb(0x43, 0xb6);
    rt_outb(0x42, (u8)divisor);
    rt_outb(0x42, (u8)(divisor >> 8));
    rt_outb(0x61, (u8)(state | 3));
}

void speaker_process_finished(int pid) {
    if (speaker_owner != pid) return;
    speaker_owner = 0;
    speaker_set(0);
}

/* A tone, timed with the calibrated time stamp counter. */
static void speaker_beep(unsigned hertz, unsigned milliseconds) {
    if (hertz < 19) hertz = 19;
    if (milliseconds > 60000) milliseconds = 60000;
    speaker_set(1193182u / hertz);
    cpu_sleep((u64)milliseconds * 1000000ull);
    speaker_set(0);
}

static int api_console(int operation, long a, long b) {
    if (current && current->vc) {
        struct vconsole *v = current->vc;
        switch (operation) {
        case DOSXC_CONSOLE_CLEAR: return vc_clear(v);
        case DOSXC_CONSOLE_GETCOLOR: return vc_get_color(v);
        case DOSXC_CONSOLE_COLOR: return vc_color(v, (unsigned)a & 255, (a & 0x100) != 0);
        case DOSXC_CONSOLE_CURSOR: return vc_cursor(v, (unsigned)a, (unsigned)b);
        case DOSXC_CONSOLE_VIRTUAL: return 1;
        }
    }
    switch (operation) {
    case DOSXC_CONSOLE_CLEAR: ctx->clear(); return 0;
    case DOSXC_CONSOLE_POLL: return terminal_poll();
    case DOSXC_CONSOLE_SIZE: return 25 << 16 | 80;
    case DOSXC_CONSOLE_GETCOLOR: return ctx->get_color ? ctx->get_color() : 7;
    case DOSXC_CONSOLE_COLOR:
        if (a & 0x100) ctx->color((unsigned char)a);
        else ctx->text_color((unsigned char)a);
        return 0;
    case DOSXC_CONSOLE_CURSOR: ctx->cursor((unsigned)a, (unsigned)b); return 0;
    case DOSXC_CONSOLE_GETKEY: return terminal_get_key((int)a);
    case DOSXC_CONSOLE_MODIFIERS: return keyboard_modifiers();
    case DOSXC_CONSOLE_BEEP: speaker_beep((unsigned)a, (unsigned)b); return 0;
    case DOSXC_CONSOLE_VIRTUAL: return 0;
    case DOSXC_CONSOLE_SPEAKER:
        speaker_owner = a ? (current ? current->pid : 0) : 0;
        speaker_set((unsigned)a);
        return 0;
    case DOSXC_CONSOLE_VOLUME:
        if (a >= 0) {
            sound_volume = a > 100 ? 100 : (int)a;
            if (!sound_volume) speaker_set(0);
        }
        return sound_volume;
    case DOSXC_CONSOLE_MODE: return files_console_mode((int)a);
    case DOSXC_CONSOLE_MOUSE: return terminal_mouse((struct dosxc_mouse *)a);
    case DOSXC_CONSOLE_POINTER: return terminal_pointer((struct dosxc_pointer *)a);
    }
    return -DOSXC_EINVAL;
}

/* power(op): switch off.  Only returns when the computer cannot do it,
   after saying it is safe to switch off by hand. */
static int api_power(int operation) {
    if (operation != DOSXC_POWER_OFF) return -DOSXC_EINVAL;
    display_text();
    terminal_mouse_hide();
    power_off();
    rt_print("\r\nIt is now safe to turn off your computer.\r\n");
    for (;;) __asm__ volatile("cli; hlt");
}

static int api_drive(int operation, int letter, char *buffer, dosxc_size size) {
    switch (operation) {
    case DOSXC_DRIVE_EJECT: return volume_eject((char)letter);
    case DOSXC_DRIVE_NOTICE: return buffer ? volume_notice(buffer, size) : -DOSXC_EFAULT;
    }
    return -DOSXC_EINVAL;
}

static int api_sysinfo(struct dosxc_sysinfo *info) {
    memset(info, 0, sizeof(*info));
    info->abi_version = DOSXC_ABI_VERSION;
    info->bits = (dosxc_u32)runtime_bits;
    info->memory_total = memory_total() + vm_total();
    info->memory_free = memory_available() + vm_free();
    rt_strcpy(info->version, DOSXC_LABEL);
    info->drive = ctx->drive;
    info->firmware = ctx->bios_available ? 'B' : 'U';
    info->live = (char)(ctx->live != 0);
    return 0;
}

#if __SIZEOF_POINTER__ == 8
#define ARG(i) (i == 0 ? f->di : i == 1 ? f->si : i == 2 ? f->dx : i == 3 ? f->r10 : i == 4 ? f->r8 : f->r9)
#define WIDE(i) ((i64)ARG(i))
#define AFTER_WIDE(i) ARG((i) + 1)
#else
#define ARG(i) (i == 0 ? f->bx : i == 1 ? f->cx : i == 2 ? f->dx : i == 3 ? f->si : i == 4 ? f->di : f->bp)
#define WIDE(i) ((i64)((u64)ARG(i) | (u64)ARG((i) + 1) << 32))
#define AFTER_WIDE(i) ARG((i) + 2)
#endif
#define P(i) ((void *)ARG(i))
#define I(i) ((int)ARG(i))

#define BUF(p, n, w) do { if ((p) && !paging_user_ok((uptr)(p), (uptr)(n), (w))) return -DOSXC_EFAULT; } while (0)
#define NEED(p, n, w) do { if (!paging_user_ok((uptr)(p), (uptr)(n), (w))) return -DOSXC_EFAULT; } while (0)
#define STR(p) do { if ((p) && !paging_user_string((const char *)(p))) return -DOSXC_EFAULT; } while (0)

static int strings_ok(char *const *list) {
    if (!list) return 1;
    for (int i = 0; i < 4096; ++i) {
        if (!paging_user_ok((uptr)&list[i], sizeof(*list), 0)) return 0;
        if (!list[i]) return 1;
        if (!paging_user_string(list[i])) return 0;
    }
    return 0;
}

static int message_ok(const struct dosxc_message *m, int receive) {
    if (!paging_user_ok((uptr)m, sizeof(*m), 0)) return 0;
    if (m->length && !paging_user_ok((uptr)m->buffer, m->length, receive)) return 0;
    if (m->address) {
        if (!paging_user_ok((uptr)m->address_length, sizeof(dosxc_u32), receive)) return 0;
        if (!paging_user_ok((uptr)m->address, *m->address_length, receive)) return 0;
    }
    return 1;
}

/* Ring 3 programs may only hand the kernel their own memory. */
static i64 check_arguments(struct syscall_frame *f) {
    uptr op = ARG(0);
    switch (f->ax) {
    case DOSXC_SYS_SPAWN:
        STR(P(0));
        if (!strings_ok(P(1)) || !strings_ok(P(2))) return -DOSXC_EFAULT;
        break;
    case DOSXC_SYS_SYSTEM: case DOSXC_SYS_OPEN: case DOSXC_SYS_UNLINK: case DOSXC_SYS_MKDIR:
    case DOSXC_SYS_RMDIR: case DOSXC_SYS_CHDIR:
        STR(P(0));
        break;
    case DOSXC_SYS_RENAME: case DOSXC_SYS_RUNDOS: STR(P(0)); STR(P(1)); break;
    case DOSXC_SYS_READ: NEED(P(1), ARG(2), 1); break;
    case DOSXC_SYS_WRITE: NEED(P(1), ARG(2), 0); break;
    case DOSXC_SYS_PIPE: NEED(P(0), 2 * sizeof(int), 1); break;
    case DOSXC_SYS_FSTAT: NEED(P(1), sizeof(struct dosxc_stat), 1); break;
    case DOSXC_SYS_STAT: STR(P(0)); NEED(P(1), sizeof(struct dosxc_stat), 1); break;
    case DOSXC_SYS_GETCWD: NEED(P(0), ARG(1), 1); break;
    case DOSXC_SYS_LISTDIR: STR(P(0)); NEED(P(1), ARG(2) * sizeof(struct dosxc_dirent), 1); break;
    case DOSXC_SYS_RESOLVE: case DOSXC_SYS_FULLPATH: STR(P(0)); NEED(P(1), ARG(2), 1); break;
    case DOSXC_SYS_SYSINFO: NEED(P(0), sizeof(struct dosxc_sysinfo), 1); break;
    case DOSXC_SYS_ENVIRON:
        STR(P(1));
        if (op == DOSXC_ENV_SET) STR(P(2));
        else BUF(P(2), ARG(3), 1);
        break;
    case DOSXC_SYS_SHELL: if (op == DOSXC_SHELL_CHDIR) STR(P(1)); break;
    case DOSXC_SYS_VOLUMES: NEED(P(0), ARG(1) * sizeof(struct dosxc_volume), 1); break;
    case DOSXC_SYS_DISK:
        if (op == DOSXC_DISK_INFO) BUF(P(3), sizeof(struct dosxc_disk), 1);
        else NEED(P(3), (uptr)ARG(4) * 512, op == DOSXC_DISK_READ);
        break;
    case DOSXC_SYS_DRIVE: BUF(P(2), ARG(3), 1); break;
    case DOSXC_SYS_NET:
        if (op == DOSXC_NET_INFO) BUF(P(2), sizeof(struct dosxc_netinfo), 1);
        else if (op == DOSXC_NET_INFO6) BUF(P(2), sizeof(struct dosxc_netinfo6), 1);
        else if (op == DOSXC_NET_CONFIG) BUF(P(2), sizeof(struct dosxc_netconfig), 0);
        else if (op == DOSXC_NET_PING6) BUF(P(2), 16, 0);
        break;
    case DOSXC_SYS_DRIVER: BUF(P(2), ARG(3) ? ARG(3) : 1, 0); break;
    case DOSXC_SYS_DISPLAY:
        if (op == DOSXC_DISPLAY_INFO) BUF(P(2), sizeof(struct dosxc_display), 1);
        else if (op == DOSXC_DISPLAY_MODES) BUF(P(2), ARG(3) * sizeof(struct dosxc_display_mode), 1);
        else if (op == DOSXC_DISPLAY_SET) BUF(P(2), sizeof(struct dosxc_display_mode), 0);
        break;
    case DOSXC_SYS_TASK:
        switch (op) {
        case DOSXC_TASK_START: STR(P(1)); if (!strings_ok((char *const *)ARG(2))) return -DOSXC_EFAULT; break;
        case DOSXC_TASK_LIST: NEED(P(1), ARG(2) * sizeof(struct dosxc_task_info), 1); break;
        case DOSXC_TASK_STATS: NEED(P(1), sizeof(struct dosxc_task_stats), 1); break;
        case DOSXC_TASK_SHARE_SET: BUF(P(1), ARG(2) ? ARG(2) : 1, 1); break;
        case DOSXC_TASK_CONSOLE_READ: NEED(P(1), ARG(3), 1); break;
        case DOSXC_TASK_CONSOLE_KEYS: NEED(P(1), ARG(3) * sizeof(int), 0); break;
        case DOSXC_TASK_THREAD_EXIT: BUF(P(1), sizeof(int), 1); break;
        case DOSXC_TASK_FUTEX_WAIT: case DOSXC_TASK_FUTEX_WAKE: NEED(P(1), sizeof(int), 0); break;
        }
        break;
    case DOSXC_SYS_LIBRARY:
        switch (op) {
        case DOSXC_LIB_OPEN: STR(P(1)); NEED(P(2), sizeof(struct dosxc_library_opened), 1); break;
        case DOSXC_LIB_SYMBOL: case DOSXC_LIB_NEXT: STR(P(2)); break;
        case DOSXC_LIB_ADDRESS: case DOSXC_LIB_OBJECT: NEED(P(2), sizeof(struct dosxc_library_where), 1); break;
        case DOSXC_LIB_TLS: NEED(P(2), sizeof(struct dosxc_tls_module), 1); break;
        }
        break;
    case DOSXC_SYS_AUDIO:
        if (op == DOSXC_AUDIO_WRITE) NEED(ARG(1), ARG(2) * 4, 0);
        else if (op == DOSXC_AUDIO_POSITION) NEED(ARG(1), sizeof(dosxc_u64), 1);
        break;
    case DOSXC_SYS_RANDOM: NEED(P(0), ARG(1), 1); break;
    case DOSXC_SYS_SHM: STR(P(1)); break;
    case DOSXC_SYS_UNIX:
        if (op == DOSXC_UNIX_PAIR) NEED(ARG(1), 2 * sizeof(int), 1);
        else {
            struct dosxc_unix_message *m = (struct dosxc_unix_message *)ARG(2);
            NEED(m, sizeof(*m), op == DOSXC_UNIX_RECEIVE);
            if (m->length) NEED(m->buffer, m->length, op == DOSXC_UNIX_RECEIVE);
            if (m->fd_count > 0) NEED(m->fds, (uptr)m->fd_count * sizeof(int), op == DOSXC_UNIX_RECEIVE);
        }
        break;
    case DOSXC_SYS_CONSOLE:
        if (op == DOSXC_CONSOLE_MOUSE) NEED(ARG(1), sizeof(struct dosxc_mouse), 1);
        else if (op == DOSXC_CONSOLE_POINTER) NEED(ARG(1), sizeof(struct dosxc_pointer), 1);
        break;
    case DOSXC_SYS_CONNECT: case DOSXC_SYS_BIND: NEED(ARG(1), ARG(2), 0); break;
    case DOSXC_SYS_ACCEPT:
        if (ARG(2)) {
            NEED(ARG(2), sizeof(dosxc_u32), 1);
            BUF(ARG(1), *(dosxc_u32 *)ARG(2), 1);
        }
        break;
    case DOSXC_SYS_SENDTO: if (!message_ok(P(1), 0)) return -DOSXC_EFAULT; break;
    case DOSXC_SYS_RECVFROM: if (!message_ok(P(1), 1)) return -DOSXC_EFAULT; break;
    case DOSXC_SYS_SOCKOPT: {
        const struct dosxc_sockopt *o = (const struct dosxc_sockopt *)ARG(4);
        NEED(o, sizeof(*o), 0);
        NEED(o->length, sizeof(dosxc_u32), 1);
        NEED(o->value, *o->length, !ARG(1));
        break;
    }
    case DOSXC_SYS_SOCKNAME:
        NEED(ARG(3), sizeof(dosxc_u32), 1);
        NEED(ARG(2), *(dosxc_u32 *)ARG(3), 1);
        break;
    }
    return 0;
}

void runtime_syscall(struct syscall_frame *f) {
    const struct dosxc_api *a = &runtime_api;
    i64 r;
    int wide = 0;
    random_stir();
    usb_tick();
    volumes_poll();
    power_poll();
    net_tick();
    if (paging_isolated && (((uptr *)(f + 1))[1] & 3)) {
        i64 bad = check_arguments(f);
        if (bad) {
            f->ax = (uptr)bad;
            sched_restore_tp();
            sched_syscall_end((uptr *)(f + 1));
            return;
        }
    }
    switch (f->ax) {
    case DOSXC_SYS_EXIT: a->exit(I(0));
    case DOSXC_SYS_SPAWN: r = a->spawn(P(0), P(1), P(2)); break;
    case DOSXC_SYS_GETPID: r = a->getpid(); break;
    case DOSXC_SYS_GETPPID: r = a->getppid(); break;
    case DOSXC_SYS_SYSTEM: r = a->system(P(0)); break;
    case DOSXC_SYS_OPEN: r = a->open(P(0), I(1), I(2)); break;
    case DOSXC_SYS_CLOSE: r = a->close(I(0)); break;
    case DOSXC_SYS_READ: r = a->read(I(0), P(1), (dosxc_size)ARG(2)); break;
    case DOSXC_SYS_WRITE: r = a->write(I(0), P(1), (dosxc_size)ARG(2)); break;
    case DOSXC_SYS_SEEK: r = a->seek(I(0), WIDE(1), (int)AFTER_WIDE(1)); wide = 1; break;
    case DOSXC_SYS_DUP: r = a->dup(I(0), I(1)); break;
    case DOSXC_SYS_DUP2: r = a->dup2(I(0), I(1)); break;
    case DOSXC_SYS_FDFLAGS: r = a->fdflags(I(0), I(1), I(2)); break;
    case DOSXC_SYS_STATUS_FLAGS: r = a->status_flags(I(0)); break;
    case DOSXC_SYS_PIPE: r = a->pipe(P(0)); break;
    case DOSXC_SYS_ISATTY: r = a->isatty(I(0)); break;
    case DOSXC_SYS_FTRUNCATE: r = a->ftruncate(I(0), WIDE(1)); break;
    case DOSXC_SYS_FSYNC: r = a->fsync(I(0)); break;
    case DOSXC_SYS_FSTAT: r = a->fstat(I(0), P(1)); break;
    case DOSXC_SYS_STAT: r = a->stat(P(0), P(1)); break;
    case DOSXC_SYS_UNLINK: r = a->unlink(P(0)); break;
    case DOSXC_SYS_RENAME: r = a->rename(P(0), P(1)); break;
    case DOSXC_SYS_MKDIR: r = a->mkdir(P(0)); break;
    case DOSXC_SYS_RMDIR: r = a->rmdir(P(0)); break;
    case DOSXC_SYS_CHDIR: r = a->chdir(P(0)); break;
    case DOSXC_SYS_GETCWD: r = a->getcwd(P(0), (dosxc_size)ARG(1)); break;
    case DOSXC_SYS_LISTDIR: r = a->listdir(P(0), P(1), (dosxc_size)ARG(2)); break;
    case DOSXC_SYS_RESOLVE: r = a->resolve(P(0), P(1), (dosxc_size)ARG(2)); break;
    case DOSXC_SYS_ALLOC: r = (i64)(uptr)a->alloc((dosxc_size)ARG(0)); break;
    case DOSXC_SYS_RELEASE: r = a->release(P(0)); break;
    case DOSXC_SYS_TIME: r = a->time(); wide = 1; break;
    case DOSXC_SYS_NANOSECONDS: r = (i64)a->nanoseconds(); wide = 1; break;
    case DOSXC_SYS_SLEEP: a->sleep((u64)WIDE(0)); r = 0; break;
    case DOSXC_SYS_CONSOLE: r = a->console(I(0), (long)ARG(1), (long)ARG(2)); break;
    case DOSXC_SYS_SYSINFO: r = a->sysinfo(P(0)); break;
    case DOSXC_SYS_RUNDOS: r = api_rundos(P(0), P(1)); break;
    case DOSXC_SYS_REBOOT: r = api_reboot(); break;
    case DOSXC_SYS_ENVIRON: r = api_environ(I(0), P(1), P(2), (dosxc_size)ARG(3)); break;
    case DOSXC_SYS_SHELL: r = api_shell(I(0), P(1), (long)ARG(2)); break;
    case DOSXC_SYS_VOLUMES: r = volumes_list(P(0), (unsigned)ARG(1)); break;
    case DOSXC_SYS_DISK: r = api_disk(I(0), (unsigned)ARG(1), (uptr)ARG(2), P(3), (unsigned)ARG(4)); break;
    case DOSXC_SYS_FULLPATH: r = files_fullpath(P(0), P(1), (dosxc_size)ARG(2)); break;
    case DOSXC_SYS_DRIVE: r = api_drive(I(0), I(1), P(2), (dosxc_size)ARG(3)); break;
    case DOSXC_SYS_POWER: r = api_power(I(0)); break;
    case DOSXC_SYS_NET: r = net_syscall_net(I(0), (long)ARG(1), P(2)); break;
    case DOSXC_SYS_DRIVER: r = driver_syscall(I(0), (long)ARG(1), P(2), (dosxc_size)ARG(3)); break;
    case DOSXC_SYS_DISPLAY: r = display_syscall(I(0), (long)ARG(1), P(2), (dosxc_size)ARG(3)); break;
    case DOSXC_SYS_TASK: r = sched_syscall(I(0), P(1), (long)ARG(2), (long)ARG(3)); break;
    case DOSXC_SYS_LIBRARY: r = library_syscall(I(0), P(1), P(2)); break;
    case DOSXC_SYS_MEMORY: r = vm_syscall(I(0), (uptr)ARG(1), (uptr)ARG(2), (long)ARG(3), I(4), (u64)ARG(5)); break;
    case DOSXC_SYS_SHM: r = shm_syscall(I(0), P(1), I(2)); break;
    case DOSXC_SYS_AUDIO: r = audio_syscall(I(0), (uptr)ARG(1), (uptr)ARG(2)); break;
    case DOSXC_SYS_RANDOM: r = random_syscall(P(0), (dosxc_size)ARG(1)); break;
    case DOSXC_SYS_SETFL: r = files_set_flags(I(0), I(1)); break;
    case DOSXC_SYS_UNIX: r = unix_syscall(I(0), (uptr)ARG(1), (uptr)ARG(2)); break;
    case DOSXC_SYS_SOCKET: case DOSXC_SYS_CONNECT: case DOSXC_SYS_BIND: case DOSXC_SYS_LISTEN: case DOSXC_SYS_ACCEPT:
    case DOSXC_SYS_SENDTO: case DOSXC_SYS_RECVFROM: case DOSXC_SYS_SHUTDOWN: case DOSXC_SYS_SOCKOPT: case DOSXC_SYS_SOCKNAME:
    case DOSXC_SYS_FDPOLL:
        if (f->ax == DOSXC_SYS_FDPOLL && (r = files_poll(I(0), I(1))) != -DOSXC_ENOTSOCK) break;
        r = socket_syscall(f->ax, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
        break;
    default: r = -DOSXC_ENOSYS; break;
    }
    f->ax = (uptr)r;
#if __SIZEOF_POINTER__ == 4
    if (wide) f->dx = (uptr)((u64)r >> 32);
#else
    (void)wide;
#endif
    sched_restore_tp();
    sched_syscall_end((uptr *)(f + 1));
}

int runtime_init(struct command_context *context) {
    ctx = context;
    if (memory_init()) {
        rt_print("ELFAPI: too little memory for 32/64-bit programs (at least 96 MiB is needed).\r\n");
        return -1;
    }
    cpu_init();
    sched_init();
    cache_init();
    volumes_init();
    usb_init();
    if (ctx->bios_available && ctx->live) bridge_install();
    power_init();
    files_init();
    env_init();
    net_init();
    drivers_init();
    memset(&runtime_api, 0, sizeof(runtime_api));
    runtime_api.version = DOSXC_ABI_TABLE;
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

static char argument_text[8192];
static char *argument_vector[1024];

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

/* A wait status as a DOS errorlevel. */
static int errorlevel(int status) {
    if ((status & 0x7f) == 6) rt_print("Abnormal program termination.\r\n");
    if ((status & 0x7f) == 2) rt_print("Program interrupted.\r\n");
    if (status & 0x7f) return 128 + (status & 0x7f);
    return (status >> 8) & 0xff;
}

/* Called by init: run a program with the system environment.  Returns its
   errorlevel, or -1 when it could not be started. */
/* The shell's program runs on a kernel stack of its own (its system calls
   run there once programs are in ring 3): the kernel's is small. */
static uptr spawn_root(uptr path) { return (uptr)(long)process_spawn(0, (const char *)path, argument_vector, 0, 0, 0); }

static int run_root(const char *path) {
    static u8 *stack;
    enum { SIZE = 256 * 1024 };
    if (!stack) stack = memory_alloc(SIZE, PAGE, OWNER_KERNEL, 0);
    if (!stack) return process_spawn(0, path, argument_vector, 0, 0, 0);
    return (int)(long)runtime_on_stack(spawn_root, (uptr)path, stack + SIZE);
}

int runtime_run(const char *path, const char *arguments) {
    char posix[PATH_MAX_RT];
    path_posix(path, posix, sizeof(posix));
    if (split_arguments(posix, arguments ? arguments : "") < 0) {
        rt_print("Command line is too long.\r\n");
        return -1;
    }
    int status = run_root(path);
    if (status < 0) {
        rt_print(posix);
        rt_print(": ");
        rt_print(error_text(status));
        rt_print(".\r\n");
        return -1;
    }
    return errorlevel(status);
}

/* Called by init after a 16-bit program: continue the program that ran it. */
int runtime_resume(struct command_context *context, int code) {
    struct process *shell = process_root();
    if (!dos_pending || !shell) return -1;
    ctx = context;
    dos_pending = 0;
    bridge_runtime_active(1);
    cpu_resume();
    cache_resume();
    volumes_resume();
    sched_resume();
    int status = runtime_switch(&shell->saved, &dos_resume, (code & 0xff) + 1);
    cpu_restore_idt();
    current = 0;
    process_finish(shell);
    return errorlevel(status);
}
