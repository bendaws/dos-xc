/*
 * DOS-XC /vdm/host.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * DOSVM.EXE program [arguments]: runs a 16-bit program in a virtual machine
 * on its terminal window.  COMMAND.EXE starts it for 16-bit programs typed
 * or opened inside a terminal window.
 *
 * The machine keeps its own clock, ten million instructions a second, and
 * is held back to real time (a slow computer just runs it slower: timing
 * loops still come out right because the timer shares the same clock).
 */
#include "vdm.h"
#include "screen.h"
#include <dosxc/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLICE 2000
#define PRESENT_NS 16000000ull

static struct vdm_screen *screen_shared;
static char pending[4096];
static size_t pending_size;
static int announced;

static void flush_output(void) {
    if (pending_size) write(1, pending, pending_size);
    pending_size = 0;
}

u64 host_ns(void) { return dosxc_sys_nanoseconds(); }
void host_sleep(u64 nanoseconds) { dosxc_sys_sleep(nanoseconds); }
void host_yield(void) { dosxc_sys_task(DOSXC_TASK_YIELD, 0, 0, 0); }
i64 host_time(void) { return dosxc_sys_time(); }
int host_key(void) { return dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 0, 0); }
void host_speaker(unsigned divisor) { dosxc_sys_console(DOSXC_CONSOLE_SPEAKER, (long)divisor, 0); }

void host_mouse(int *x, int *y, int *buttons) {
    *x = screen_shared->mouse_x;
    *y = screen_shared->mouse_y;
    *buttons = screen_shared->mouse_buttons;
}

void host_output(const char *text, size_t size) {
    while (size) {
        size_t n = sizeof(pending) - pending_size;
        if (n > size) n = size;
        memcpy(pending + pending_size, text, n);
        pending_size += n;
        text += n;
        size -= n;
        if (pending_size == sizeof(pending)) flush_output();
    }
}

void host_publish(int graphics, int width, int height, const u32 *pixels, const u8 *text, int row, int col, int on) {
    struct vdm_screen *s = screen_shared;
    s->graphics = graphics;
    s->width = width;
    s->height = height;
    s->pixels = pixels;
    s->text = text;
    s->cursor_row = row;
    s->cursor_col = col;
    s->cursor_on = on;
    if (!announced) {
        char line[40];
        int n = snprintf(line, sizeof(line), "\033]8086;%lx\a", (unsigned long)(uintptr_t)s);
        host_output(line, (size_t)n);
        flush_output();
        announced = 1;
    }
}

void host_frame(void) { ++screen_shared->frame; }

int host_listdir(const char *path, struct vdm_dirent *entries, int capacity) {
    static struct dosxc_dirent found[512];
    int n = (int)dosxc_sys_listdir(path, found, 512);
    if (n > capacity) n = capacity;
    for (int i = 0; i < n; ++i) {
        snprintf(entries[i].name, sizeof(entries[i].name), "%s", found[i].name);
        entries[i].attributes = found[i].attributes;
        entries[i].size = (u32)found[i].size;
        entries[i].mtime = found[i].mtime;
    }
    return n < 0 ? 0 : n;
}

int host_fullpath(const char *path, char *out, size_t size) { return dosxc_sys_fullpath(path, out, size); }
void host_native_path(const char *path, char *out, size_t size) { snprintf(out, size, "%s", path); }

const char *host_environment(void) {
    static char text[8192];
    if (dosxc_sys_environ(DOSXC_ENV_LIST, 0, text, sizeof(text)) < 0) text[0] = text[1] = 0;
    return text;
}

unsigned long long host_disk_free(int drive) {
    char where[8];
    if (!drive) {
        if (dosxc_sys_fullpath(".", where, sizeof(where)) < 0) return 0;
    } else where[0] = (char)('A' + drive - 1);
    struct dosxc_volume volumes[26];
    int n = dosxc_sys_volumes(volumes, 26);
    for (int i = 0; i < n && i < 26; ++i)
        if (volumes[i].letter == (where[0] & 0xdf)) return volumes[i].free_bytes;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("DOSVM program [arguments]   Runs a 16-bit program in a terminal window.");
        return 1;
    }
    const char *tail = argc > 2 ? argv[2] : "";
    screen_shared = calloc(1, sizeof(*screen_shared));
    mem = calloc(1, 0x100000);
    if (!mem || !screen_shared) { puts("Not enough memory for the DOS virtual machine."); return 1; }
    screen_shared->magic = VDM_MAGIC;
    screen_shared->pid = dosxc_sys_getpid();
    cpu_reset();
    video_init();
    bios_init();
    int r = dos_start(argv[1], tail);
    if (r) {
        printf("%s: this program cannot be started (DOS error %d).\n", argv[1], r);
        return 1;
    }
    u64 base = host_ns(), last_count = 0, last_present = 0;
    while (!vdm_finished) {
        cpu_run(SLICE);
        virtual_ns += (cpu.count - last_count) * VDM_INSTRUCTION_NS;
        last_count = cpu.count;
        u64 real = host_ns() - base;
        if (cpu.idle) {
            u64 next = bios_next_event();
            if (next > real) {
                flush_output();
                u64 wait = next - real;
                dosxc_sys_sleep(wait < 5000000ull ? wait : 5000000ull);
                real = host_ns() - base;
            }
            u64 target = next < real ? next : real;
            if (target > virtual_ns) virtual_ns = target;
        } else if (virtual_ns > real + 2000000ull) {
            flush_output();
            dosxc_sys_sleep(virtual_ns - real);
        } else {
            if (real > virtual_ns + 250000000ull) base += real - virtual_ns - 250000000ull;
            host_yield();
        }
        bios_tick();
        if (real - last_present >= PRESENT_NS) {
            last_present = real;
            video_present();
            flush_output();
        }
    }
    host_speaker(0);
    flush_output();
    if (announced) write(1, "\033]8086;0\a", 9);
    return vdm_exit_code;
}
