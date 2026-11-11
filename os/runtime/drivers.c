/*
 * DOS-XC /os/runtime/drivers.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "net.h"
#include "elf.h"

#define MAX_DRIVERS 16
#define MAX_BOUND 8
#define MAX_PCI 96
#define LOG_SIZE 8192

struct driver {
    int used, state;
    char path[PATH_MAX_RT], file[32];
    uptr image;
    const struct dk_driver *d;
    struct { struct pci_function f; void *instance; } bound[MAX_BOUND];
    int bound_count;
};

static struct driver drivers[MAX_DRIVERS];
static struct pci_function pci_functions[MAX_PCI];
static int pci_count, pci_scanned;
static u8 claimed[MAX_PCI];
static struct driver *probing;

static char driver_log[LOG_SIZE];
static uptr log_used;

/* ---- formatting ---------------------------------------------------------- */

struct sink { char *out; uptr size, used; };

static void sink_char(struct sink *s, char c) {
    if (s->used + 1 < s->size) s->out[s->used] = c;
    ++s->used;
}

static void sink_number(struct sink *s, u64 value, unsigned base, int width, char pad) {
    char digits[24];
    int n = 0;
    do digits[n++] = "0123456789abcdef"[value % base]; while ((value /= base) && n < 24);
    while (width-- > n) sink_char(s, pad);
    while (n) sink_char(s, digits[--n]);
}

static void format_v(struct sink *s, const char *f, __builtin_va_list ap) {
    for (; *f; ++f) {
        if (*f != '%') { sink_char(s, *f); continue; }
        ++f;
        char pad = ' ';
        int width = 0, wide = 0;
        if (*f == '0') { pad = '0'; ++f; }
        while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0';
        while (*f == 'l') { wide = 1; ++f; }
        switch (*f) {
        case 's': for (const char *t = __builtin_va_arg(ap, const char *); t && *t; ++t) sink_char(s, *t); break;
        case 'c': sink_char(s, (char)__builtin_va_arg(ap, int)); break;
        case 'd': {
            i64 v = wide ? __builtin_va_arg(ap, long) : __builtin_va_arg(ap, int);
            if (v < 0) { sink_char(s, '-'); v = -v; }
            sink_number(s, (u64)v, 10, width, pad);
            break;
        }
        case 'u': sink_number(s, wide ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned), 10, width, pad); break;
        case 'x': sink_number(s, wide ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned), 16, width, pad); break;
        case '%': sink_char(s, '%'); break;
        case 0: return;
        default: sink_char(s, '%'); sink_char(s, *f); break;
        }
    }
}

int rt_format_v(char *out, uptr size, const char *format, __builtin_va_list ap) {
    struct sink s = {out, size, 0};
    format_v(&s, format, ap);
    if (size) out[s.used < size ? s.used : size - 1] = 0;
    return (int)s.used;
}

static int api_format(char *out, unsigned long size, const char *format, ...) {
    struct sink s = {out, size, 0};
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    format_v(&s, format, ap);
    __builtin_va_end(ap);
    if (size) out[s.used < size ? s.used : size - 1] = 0;
    return (int)s.used;
}

static void log_line(const char *text) {
    uptr n = rt_strlen(text);
    if (n + 2 > LOG_SIZE) return;
    if (log_used + n + 2 > LOG_SIZE) {
        memmove(driver_log, driver_log + LOG_SIZE / 2, log_used - LOG_SIZE / 2);
        log_used -= LOG_SIZE / 2;
    }
    memcpy(driver_log + log_used, text, n);
    log_used += n;
    driver_log[log_used++] = '\n';
}

static void api_log(const char *format, ...) {
    char text[200];
    struct sink s = {text, sizeof(text), 0};
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    format_v(&s, format, ap);
    __builtin_va_end(ap);
    text[s.used < sizeof(text) ? s.used : sizeof(text) - 1] = 0;
    log_line(text);
    event_log(DOSXC_EVENT_INFO, "Drivers", "%s", text);
}

static void api_notice(const char *format, ...) {
    char text[80];
    struct sink s = {text, sizeof(text), 0};
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    format_v(&s, format, ap);
    __builtin_va_end(ap);
    text[s.used < sizeof(text) ? s.used : sizeof(text) - 1] = 0;
    log_line(text);
    volume_post_notice(text);
}

static dosxc_u64 api_nanoseconds(void) { return cpu_nanoseconds(); }
static void api_sleep(dosxc_u64 nanoseconds) { cpu_sleep(nanoseconds); }
static void *api_alloc(dosxc_size size) { return memory_alloc(size, PAGE, OWNER_KERNEL, 1); }
static void api_free(void *block) { if (block) memory_free(block, OWNER_KERNEL); }

static void *api_dma_alloc(dosxc_size size, dosxc_size align) {
    void *p = memory_alloc(size, align, OWNER_KERNEL, 1);
    if (p && (u64)(uptr)p + size > 0x100000000ull) { memory_free(p, OWNER_KERNEL); return 0; }
    return p;
}
static dosxc_u64 api_dma_address(const void *p) { return (u64)(uptr)p; }

#define PCI(name, type) static type api_##name(const struct dk_pci_device *d, u8 o) { return name((const struct pci_function *)d, o); }
PCI(pci_read8, u8)
PCI(pci_read16, u16)
PCI(pci_read32, u32)
#undef PCI
#define PCI(name, type) static void api_##name(const struct dk_pci_device *d, u8 o, type v) { name((const struct pci_function *)d, o, v); }
PCI(pci_write8, u8)
PCI(pci_write16, u16)
PCI(pci_write32, u32)
#undef PCI
static dosxc_u64 api_pci_bar(const struct dk_pci_device *d, int index, int *io) { return pci_bar64((const struct pci_function *)d, index, io); }
static void api_pci_enable(const struct dk_pci_device *d, int memory, int io) { pci_enable((const struct pci_function *)d, memory, io); }
static volatile void *api_mmio_map(dosxc_u64 physical, dosxc_size size) {
    (void)size;
    if (!physical || physical != (uptr)physical) return 0;
    if ((physical >> 32) && ctx->bios_available) return 0;
    return (volatile void *)(uptr)physical;
}

#define MAX_DRIVER_DISKS 16
static struct { int used; struct driver *driver; const struct dk_disk_ops *ops; void *instance; } driver_disks[MAX_DRIVER_DISKS];

static int api_disk_register(const struct dk_disk_ops *ops, void *instance, dosxc_u64 sectors, unsigned flags, const char *name) {
    for (int i = 0; i < MAX_DRIVER_DISKS; ++i) {
        if (driver_disks[i].used) continue;
        driver_disks[i].used = 1;
        driver_disks[i].driver = probing;
        driver_disks[i].ops = ops;
        driver_disks[i].instance = instance;
        if (volumes_driver_disk(i, sectors, flags & DK_DISK_READONLY, name)) {
            driver_disks[i].used = 0;
            return -DOSXC_ENOMEM;
        }
        return i;
    }
    return -DOSXC_ENOMEM;
}

static void api_disk_unregister(int handle) {
    if (handle < 0 || handle >= MAX_DRIVER_DISKS || !driver_disks[handle].used) return;
    volumes_driver_disk_gone(handle);
    driver_disks[handle].used = 0;
}

static int api_firmware_services(void) { return !ctx->bios_available; }

static struct { struct driver *driver; const struct dk_audio_ops *ops; void *instance; int owner; } audio;

static int api_audio_register(const struct dk_audio_ops *ops, void *instance, const char *name) {
    (void)name;
    if (audio.ops) return -DOSXC_EBUSY;
    audio.driver = probing;
    audio.ops = ops;
    audio.instance = instance;
    audio.owner = 0;
    return 0;
}

static void api_audio_unregister(int handle) {
    (void)handle;
    audio.ops = 0;
    audio.owner = 0;
}

static const struct dk_api driver_api = {
    DK_ABI_VERSION, sizeof(struct dk_api),
    api_log, api_notice, api_format, api_nanoseconds, api_sleep, api_alloc, api_free,
    api_dma_alloc, api_free, api_dma_address,
    api_pci_read8, api_pci_read16, api_pci_read32, api_pci_write8, api_pci_write16, api_pci_write32,
    api_pci_bar, api_pci_enable, api_mmio_map,
    net_register, net_receive, net_link, net_unregister,
    api_disk_register, api_disk_unregister, usb_input_key, usb_input_mouse, api_firmware_services,
    api_audio_register, api_audio_unregister,
};

static struct saved_context guard;
static int guard_depth;
static struct driver *guard_driver;
static uptr fault_vector, fault_ip;

static void stop_devices(struct driver *d) {
    if (!d->d->pci_ids) return;
    for (int i = 0; i < d->bound_count; ++i) {
        const struct pci_function *f = &d->bound[i].f;
        pci_write16(f, 4, (u16)(pci_read16(f, 4) & ~4u));
    }
}

/* Stop the driver that faulted (runs after unwinding to the guard). */
static void driver_failed(struct driver *d) {
    char text[120];
    guard_depth = 0;
    guard_driver = 0;
    api_format(text, sizeof(text), "Driver %s stopped: processor exception %u at %lx", d->file,
               (unsigned)fault_vector, (unsigned long)(fault_ip - d->image));
    log_line(text);
    rt_print(text);
    rt_print("\r\n");
    d->state = DOSXC_DRIVER_STATE_FAILED;
    stop_devices(d);
}

int drivers_fault(uptr vector, uptr ip) {
    if (!guard_depth) return 0;
    fault_vector = vector;
    fault_ip = ip;
    runtime_leave(&guard, 1);
}

typedef void (*guarded_fn)(void *);

/* Run FN(ARG) for D, surviving a processor exception.  Returns 0 or -EIO.
   Nested calls (a poll that transmits) share the outermost guard. */
static int guarded(struct driver *d, guarded_fn fn, void *arg) {
    if (d->state == DOSXC_DRIVER_STATE_FAILED) return -DOSXC_EIO;
    if (guard_depth) {
        ++guard_depth;
        fn(arg);
        --guard_depth;
        return 0;
    }
    guard_driver = d;
    if (runtime_save(&guard)) {
        driver_failed(guard_driver);
        return -DOSXC_EIO;
    }
    guard_depth = 1;
    fn(arg);
    guard_depth = 0;
    guard_driver = 0;
    return 0;
}

struct transmit_call { const struct dk_net_ops *ops; void *instance; const void *frame; u32 length; int result; };
static void call_transmit(void *arg) {
    struct transmit_call *c = arg;
    c->result = c->ops->transmit(c->instance, c->frame, c->length);
}

int driver_transmit(struct driver *d, const struct dk_net_ops *ops, void *instance, const void *frame, u32 length) {
    struct transmit_call c = {ops, instance, frame, length, -DOSXC_EIO};
    if (guarded(d, call_transmit, &c)) return -DOSXC_EIO;
    return c.result;
}

struct audio_call { int op; uptr a, b; i64 result; };
static void call_audio(void *arg) {
    struct audio_call *c = arg;
    switch (c->op) {
    case DOSXC_AUDIO_OPEN: c->result = audio.ops->open(audio.instance, (unsigned)c->a, (unsigned)c->b); break;
    case DOSXC_AUDIO_CLOSE: audio.ops->close(audio.instance); c->result = 0; break;
    case DOSXC_AUDIO_WRITE: c->result = audio.ops->write(audio.instance, (const void *)c->a, (unsigned)c->b); break;
    case DOSXC_AUDIO_POSITION: *(u64 *)c->a = audio.ops->position(audio.instance); c->result = 0; break;
    }
}

static i64 audio_guarded(int op, uptr a, uptr b) {
    struct audio_call c = {op, a, b, -DOSXC_EIO};
    if (guarded(audio.driver, call_audio, &c)) {
        audio.ops = 0;
        audio.owner = 0;
        return -DOSXC_EIO;
    }
    return c.result;
}

i64 audio_syscall(int operation, uptr a, uptr b) {
    if (!audio.ops) return -DOSXC_ENODEV;
    int pid = current ? current->pid : 0;
    if (operation == DOSXC_AUDIO_OPEN) {
        if (audio.owner) return -DOSXC_EBUSY;
        i64 r = audio_guarded(DOSXC_AUDIO_OPEN, a, b);
        if (!r) audio.owner = pid;
        return r;
    }
    if (audio.owner != pid) return -DOSXC_EBADF;
    if (operation < DOSXC_AUDIO_CLOSE || operation > DOSXC_AUDIO_POSITION) return -DOSXC_EINVAL;
    i64 r = audio_guarded(operation, a, b);
    if (operation == DOSXC_AUDIO_CLOSE) audio.owner = 0;
    return r;
}

void audio_process_finished(int pid) {
    if (audio.ops && audio.owner == pid) {
        audio_guarded(DOSXC_AUDIO_CLOSE, 0, 0);
        audio.owner = 0;
    }
}

struct disk_call { const struct dk_disk_ops *ops; void *instance; u64 lba; unsigned count; void *buffer; int write, result; };
static void call_disk(void *arg) {
    struct disk_call *c = arg;
    c->result = c->ops->io(c->instance, c->lba, c->count, c->buffer, c->write);
}

int driver_disk_io(int handle, u64 lba, unsigned count, void *buffer, int write) {
    if (handle < 0 || handle >= MAX_DRIVER_DISKS || !driver_disks[handle].used) return -DOSXC_EIO;
    struct disk_call c = {driver_disks[handle].ops, driver_disks[handle].instance, lba, count, buffer, write, -DOSXC_EIO};
    if (guarded(driver_disks[handle].driver, call_disk, &c)) return -DOSXC_EIO;
    return c.result;
}

struct poll_call { struct driver *d; void *instance; };
static void call_poll(void *arg) {
    struct poll_call *c = arg;
    c->d->d->poll(c->instance);
}

void drivers_poll(void) {
    for (int i = 0; i < MAX_DRIVERS; ++i) {
        struct driver *d = &drivers[i];
        if (!d->used || d->state == DOSXC_DRIVER_STATE_FAILED || !d->d->poll) continue;
        for (int b = 0; b < d->bound_count; ++b) {
            struct poll_call c = {d, d->bound[b].instance};
            guarded(d, call_poll, &c);
        }
    }
}

int drivers_active(void) {
    for (int i = 0; i < MAX_DRIVERS; ++i)
        if (drivers[i].used && drivers[i].state != DOSXC_DRIVER_STATE_FAILED) return 1;
    return 0;
}

struct driver *driver_probing(void) { return probing; }

static void scan_pci(void) {
    if (pci_scanned) return;
    pci_scanned = 1;
    for (unsigned bus = 0; bus < 256; ++bus)
        for (u8 device = 0; device < 32; ++device) {
            struct pci_function f = {(u8)bus, device, 0, 0, 0, 0};
            if ((pci_read32(&f, 0) & 0xffff) == 0xffff) continue;
            int functions = (pci_read8(&f, 0x0e) & 0x80) ? 8 : 1;
            for (u8 function = 0; function < functions; ++function) {
                f.function = function;
                u32 id = pci_read32(&f, 0);
                if ((id & 0xffff) == 0xffff || pci_count == MAX_PCI) continue;
                f.vendor = (u16)id;
                f.id = (u16)(id >> 16);
                f.prog_if = (u8)(pci_read32(&f, 8) >> 8);
                pci_functions[pci_count++] = f;
            }
        }
}

static int id_matches(const struct dk_pci_id *ids, const struct pci_function *f) {
    for (; ids && ids->vendor; ++ids)
        if (ids->vendor == f->vendor && (ids->device == DK_ANY || ids->device == f->id)) return 1;
    return 0;
}

/* ---- loading -------------------------------------------------------------- */

struct probe_call { struct driver *d; const struct pci_function *f; int result; void *instance; };
static void call_probe(void *arg) {
    struct probe_call *c = arg;
    c->result = c->d->d->probe((const struct dk_pci_device *)c->f, &c->instance);
}
struct load_call { struct driver *d; int result; };
static void call_load(void *arg) {
    struct load_call *c = arg;
    c->result = c->d->d->load(&driver_api);
}
static void call_remove(void *arg) {
    struct poll_call *c = arg;
    c->d->d->remove(c->instance);
}
static void call_unload(void *arg) { ((struct driver *)arg)->d->unload(); }

static void file_name(const char *canonical, char *out, uptr size) {
    const char *name = canonical;
    for (const char *p = canonical; *p; ++p) if (*p == '\\') name = p + 1;
    uptr n = 0;
    for (; *name && n + 1 < size; ++name) out[n++] = (char)rt_upper((u8)*name);
    out[n] = 0;
}

static struct driver *find_by_file(const char *file) {
    for (int i = 0; i < MAX_DRIVERS; ++i)
        if (drivers[i].used && rt_equal_nocase(drivers[i].file, file)) return &drivers[i];
    return 0;
}

static void bind_devices(struct driver *d) {
    if (!d->d->probe) return;
    if (!d->d->pci_ids) {
        struct probe_call c = {d, 0, -1, 0};
        probing = d;
        int r = guarded(d, call_probe, &c);
        probing = 0;
        if (!r && !c.result) d->bound[d->bound_count++].instance = c.instance;
        if (d->state != DOSXC_DRIVER_STATE_FAILED) d->state = d->bound_count ? DOSXC_DRIVER_STATE_LOADED : DOSXC_DRIVER_STATE_IDLE;
        return;
    }
    scan_pci();
    for (int i = 0; i < pci_count && d->bound_count < MAX_BOUND; ++i) {
        if (claimed[i] || !id_matches(d->d->pci_ids, &pci_functions[i])) continue;
        struct probe_call c = {d, &pci_functions[i], -1, 0};
        probing = d;
        int r = guarded(d, call_probe, &c);
        probing = 0;
        if (r) return;
        if (c.result == 0) {
            claimed[i] = 1;
            d->bound[d->bound_count].f = pci_functions[i];
            d->bound[d->bound_count++].instance = c.instance;
        } else {
            api_log("%s: device %04x:%04x at %02x:%02x.%u refused (%d)", d->file, pci_functions[i].vendor, pci_functions[i].id,
                    pci_functions[i].bus, pci_functions[i].device, pci_functions[i].function, c.result);
        }
    }
    if (d->state != DOSXC_DRIVER_STATE_FAILED) d->state = d->bound_count ? DOSXC_DRIVER_STATE_LOADED : DOSXC_DRIVER_STATE_IDLE;
}

static int driver_load(const char *canonical) {
    char file[32];
    file_name(canonical, file, sizeof(file));
    if (find_by_file(file)) return -DOSXC_EEXIST;
    struct driver *d = 0;
    for (int i = 0; i < MAX_DRIVERS && !d; ++i)
        if (!drivers[i].used) d = &drivers[i];
    if (!d) return -DOSXC_ENOMEM;
    uptr image = 0;
    const void *symbol = 0;
    int r = driver_image_load(canonical, &image, &symbol);
    if (r) return r;
    const struct dk_driver *descriptor = symbol;
    if (descriptor->abi != DK_ABI_VERSION || descriptor->size < sizeof(struct dk_driver)) {
        api_log("%s: built for driver ABI %u; this system has %u", file, descriptor->abi, DK_ABI_VERSION);
        memory_free((void *)image, OWNER_KERNEL);
        return -DOSXC_ENOEXEC;
    }
    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->state = DOSXC_DRIVER_STATE_IDLE;
    d->image = image;
    d->d = descriptor;
    rt_strcpy(d->path, canonical);
    memcpy(d->file, file, sizeof(file));
    if (descriptor->load) {
        struct load_call c = {d, -1};
        if (guarded(d, call_load, &c) || c.result) {
            api_log("%s: refused to load (%d)", file, c.result);
            if (d->state != DOSXC_DRIVER_STATE_FAILED) {
                memory_free((void *)image, OWNER_KERNEL);
                d->used = 0;
            }
            return -DOSXC_EIO;
        }
    }
    bind_devices(d);
    api_log("%s: %s %s, %d device(s)", file, descriptor->name, descriptor->version, d->bound_count);
    return 0;
}

static int driver_unload(const char *file) {
    struct driver *d = find_by_file(file);
    if (!d) return -DOSXC_ENOENT;
    if (d->state == DOSXC_DRIVER_STATE_FAILED) {
        d->used = 0;
        return 0;
    }
    for (int i = 0; i < d->bound_count; ++i) {
        if (d->d->remove) {
            struct poll_call c = {d, d->bound[i].instance};
            guarded(d, call_remove, &c);
        }
        for (int p = 0; d->d->pci_ids && p < pci_count; ++p)
            if (pci_functions[p].bus == d->bound[i].f.bus && pci_functions[p].device == d->bound[i].f.device &&
                pci_functions[p].function == d->bound[i].f.function) claimed[p] = 0;
    }
    if (d->d->unload) guarded(d, call_unload, d);
    if (d->state != DOSXC_DRIVER_STATE_FAILED) memory_free((void *)d->image, OWNER_KERNEL);
    memset(d, 0, sizeof(*d));
    return 0;
}

void drivers_shutdown(void) {
    for (int i = 0; i < MAX_DRIVERS; ++i) {
        struct driver *d = &drivers[i];
        if (!d->used || d->state == DOSXC_DRIVER_STATE_FAILED || !d->d->remove) continue;
        for (int b = 0; b < d->bound_count; ++b) {
            struct poll_call c = {d, d->bound[b].instance};
            guarded(d, call_remove, &c);
        }
        d->bound_count = 0;
    }
}

#define MAX_FOUND 32
static char found[MAX_FOUND][64];
static int found_count;

static int collect_driver(void *context, const struct fat32_info *info) {
    (void)context;
    uptr n = rt_strlen(info->name);
    if (n < 5 || n >= sizeof(found[0]) || found_count == MAX_FOUND || (info->attributes & DOSXC_ATTR_DIRECTORY)) return 0;
    if (!rt_equal_nocase(info->name + n - 4, ".DRV")) return 0;
    rt_strcpy(found[found_count++], info->name);
    return 0;
}

static void folder_path(char *out) {
    out[0] = (char)rt_upper(ctx->drive);
    out[1] = ':';
    rt_strcpy(out + 2, "\\SYSTEM\\DRIVERS");
}

/* \SYSTEM\DRIVERS\STORE holds every driver DOS-XC ships: each one not
   installed is tried, and kept when it finds a device. */
static void autodetect(const char *folder) {
    char store[PATH_MAX_RT], path[PATH_MAX_RT];
    rt_strcpy(store, folder);
    rt_strcpy(store + rt_strlen(store), "\\STORE");
    found_count = 0;
    if (fs_list(store, collect_driver, 0)) return;
    for (int i = 0; i < found_count; ++i) {
        if (find_by_file(found[i])) continue;
        rt_strcpy(path, store);
        uptr n = rt_strlen(path);
        path[n++] = '\\';
        rt_strcpy(path + n, found[i]);
        if (driver_load(path)) continue;
        struct driver *d = find_by_file(found[i]);
        if (d && d->state == DOSXC_DRIVER_STATE_IDLE) driver_unload(found[i]);
        else if (d) api_log("%s: found a device for it, loaded automatically", found[i]);
    }
}

void drivers_init(void) {
    if (ctx->bios_available && (*(volatile u8 *)0x417 & 3)) {
        log_line("Drivers skipped: Shift was held during startup.");
        return;
    }
    char folder[PATH_MAX_RT], path[PATH_MAX_RT];
    folder_path(folder);
    found_count = 0;
    if (fs_list(folder, collect_driver, 0)) return;
    cpu_install_idt();
    for (int i = 0; i < found_count; ++i) {
        rt_strcpy(path, folder);
        uptr n = rt_strlen(path);
        path[n++] = '\\';
        rt_strcpy(path + n, found[i]);
        int r = driver_load(path);
        if (r) api_log("%s: could not be loaded (error %d)", found[i], -r);
    }
    autodetect(folder);
    cpu_restore_idt();
}

i64 driver_syscall(int operation, long a, void *buffer, dosxc_size size) {
    switch (operation) {
    case DOSXC_DRIVER_LIST: {
        int count = 0, index = 0;
        for (int i = 0; i < MAX_DRIVERS; ++i) count += drivers[i].used != 0;
        if (!buffer) return count;
        for (int i = 0; i < MAX_DRIVERS; ++i) {
            if (!drivers[i].used || index++ != a) continue;
            struct dosxc_driverinfo *out = buffer;
            memset(out, 0, sizeof(*out));
            rt_strcpy(out->file, drivers[i].file);
            const struct dk_driver *d = drivers[i].d;
            api_format(out->name, sizeof(out->name), "%s", d->name ? d->name : "");
            api_format(out->version, sizeof(out->version), "%s", d->version ? d->version : "");
            api_format(out->description, sizeof(out->description), "%s", d->description ? d->description : "");
            out->state = (u32)drivers[i].state;
            out->devices = (u32)drivers[i].bound_count;
            return count;
        }
        return -DOSXC_ENOENT;
    }
    case DOSXC_DRIVER_LOAD: {
        char canonical[PATH_MAX_RT];
        if (!buffer) return -DOSXC_EFAULT;
        int r = path_resolve(current, buffer, canonical);
        if (r) return r;
        return driver_load(canonical);
    }
    case DOSXC_DRIVER_UNLOAD: return buffer ? driver_unload(buffer) : -DOSXC_EFAULT;
    case DOSXC_DRIVER_LOG: {
        if (!buffer || !size) return (i64)log_used;
        uptr n = log_used < size - 1 ? log_used : size - 1;
        memcpy(buffer, driver_log, n);
        ((char *)buffer)[n] = 0;
        return (i64)n;
    }
    }
    return -DOSXC_EINVAL;
}
