/*
 * DOS-XC /os/runtime/events.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#define EVENTS 512

static struct dosxc_event ring[EVENTS];
static u32 next_number = 1;

void event_log(int level, const char *source, const char *format, ...) {
    struct dosxc_event *e = &ring[next_number % EVENTS];
    e->number = next_number++;
    e->level = level;
    e->time = cpu_rtc_time();
    e->pid = current ? current->pid : 0;
    uptr i = 0;
    for (; source[i] && i + 1 < sizeof(e->source); ++i) e->source[i] = source[i];
    e->source[i] = 0;
    i = 0;
    if (current)
        for (const char *u = current->user; *u && i + 1 < sizeof(e->user); ++u) e->user[i++] = *u;
    e->user[i] = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    rt_format_v(e->text, sizeof(e->text), format, ap);
    __builtin_va_end(ap);
    {
        const char *parts[4] = {"[event] ", e->source, ": ", e->text};
        for (int p = 0; p < 4; ++p)
            for (const char *c = parts[p]; *c; ++c) rt_outb(0xe9, (u8)*c);
        rt_outb(0xe9, '\n');
    }
}

/* The program's file name, for events about it. */
const char *event_program(const struct process *p) {
    const char *name = p->path;
    for (const char *c = p->path; *c; ++c)
        if (*c == '\\' || *c == '/' || *c == ':') name = c + 1;
    return name;
}

long events_syscall(int op, void *a, void *b) {
    switch (op) {
    case DOSXC_EVENTS_ADD: {
        const struct dosxc_event_add *add = a;
        int level = add->level < DOSXC_EVENT_INFO || add->level > DOSXC_EVENT_ERROR ? DOSXC_EVENT_INFO : add->level;
        event_log(level, current ? event_program(current) : "System", "%s", add->text);
        return 0;
    }
    case DOSXC_EVENTS_READ: {
        if (!accounts_admin()) return -DOSXC_EACCES;
        u32 number = (u32)(uptr)a;
        if (!number || number >= next_number || next_number - number > EVENTS) return -DOSXC_ENOENT;
        *(struct dosxc_event *)b = ring[number % EVENTS];
        return 0;
    }
    case DOSXC_EVENTS_RANGE: {
        u32 *range = a;
        range[0] = next_number > EVENTS ? next_number - EVENTS : 1;
        range[1] = next_number;
        return 0;
    }
    case DOSXC_EVENTS_CLEAR:
        if (!accounts_admin()) return -DOSXC_EACCES;
        memset(ring, 0, sizeof(ring));
        event_log(DOSXC_EVENT_INFO, "Event Logs", "The log was cleared");
        return 0;
    }
    return -DOSXC_EINVAL;
}
