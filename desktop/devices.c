/*
 * DOS-XC /desktop/devices.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "../os/pkg/dsp.h"
#include <stdarg.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define NS_PER_MS 1000000ull
#define MAX_DEVICES 96
#define MAX_LOADED 32
#define ROW 20
#define LOG_LINES 300
#define LOG_WIDTH 110

struct device { struct dosxc_deviceinfo info; char name[96], status[40]; };
static struct device devices[MAX_DEVICES];
static int device_count, missing_count;
static struct dosxc_driverinfo loaded[MAX_LOADED];
static int loaded_count;
static struct dosxc_sysinfo sys;
static struct window *win;
static struct vscroll scroll, log_scroll;
static char log_text[LOG_LINES][LOG_WIDTH];
static int log_count, log_column, busy;

static const struct rect list = {12, 28, 636, 260}, log_area = {12, 330, 636, 118};
static const struct rect act_find = {12, 296, 120, 24}, act_log = {140, 296, 110, 24}, act_refresh = {258, 296, 90, 24};

static const struct { unsigned short vendor; const char *name; } vendors[] = {
    {0x8086, "Intel"}, {0x8087, "Intel"}, {0x10de, "NVIDIA"}, {0x1002, "AMD"}, {0x1022, "AMD"}, {0x10ec, "Realtek"},
    {0x14e4, "Broadcom"}, {0x1b4b, "Marvell"}, {0x144d, "Samsung"}, {0x15b7, "Western Digital"}, {0x1c5c, "SK hynix"},
    {0x1987, "Phison"}, {0x126f, "Silicon Motion"}, {0x1b21, "ASMedia"}, {0x1106, "VIA"}, {0x1234, "QEMU"},
    {0x1af4, "virtio"}, {0x15ad, "VMware"}, {0x80ee, "VirtualBox"}, {0x1414, "Microsoft"}, {0x168c, "Qualcomm Atheros"},
    {0x17cb, "Qualcomm"}, {0x1969, "Qualcomm Atheros"}, {0x104c, "Texas Instruments"}, {0x1217, "O2 Micro"},
    {0x1180, "Ricoh"}, {0x1912, "Renesas"}, {0x1d6a, "Aquantia"}, {0x2646, "Kingston"}, {0x1e0f, "KIOXIA"},
    {0x1179, "Toshiba"}, {0x1344, "Micron"}, {0xc0a9, "Crucial"}, {0x1cc1, "ADATA"}, {0x1186, "D-Link"}, {0x1000, "Broadcom/LSI"},
};

static const struct { unsigned short vendor, id; const char *name; } known[] = {
    {0x10de, 0x2507, "GeForce RTX 3050"}, {0x10de, 0x2582, "GeForce RTX 3050"}, {0x10de, 0x2584, "GeForce RTX 3050"},
    {0x10de, 0x25a2, "GeForce RTX 3050 Ti Laptop"}, {0x10de, 0x25a5, "GeForce RTX 3050 Laptop"},
    {0x10de, 0x228e, "GA106 HDMI audio"}, {0x10de, 0x2291, "GA107 HDMI audio"},
    {0x8086, 0x15bb, "Ethernet I219-LM (7)"}, {0x8086, 0x15bc, "Ethernet I219-V (7)"},
    {0x8086, 0x15bd, "Ethernet I219-LM (6)"}, {0x8086, 0x15be, "Ethernet I219-V (6)"},
    {0x8086, 0x156f, "Ethernet I219-LM"}, {0x8086, 0x1570, "Ethernet I219-V"},
    {0x8086, 0xa348, "Cannon Lake PCH HD Audio"}, {0x8086, 0xa36d, "Cannon Lake PCH USB 3.1 xHCI"},
    {0x8086, 0xa352, "Cannon Lake PCH SATA AHCI"}, {0x8086, 0x2822, "SATA RAID (Rapid Storage)"},
    {0x8086, 0xa360, "Cannon Lake PCH Management Engine"}, {0x8086, 0xa323, "Cannon Lake PCH SMBus"},
    {0x8086, 0xa309, "C246 LPC bridge"}, {0x8086, 0x100e, "PRO/1000 MT (82540EM)"}, {0x8086, 0x10d3, "82574L Gigabit"},
    {0x10ec, 0x8168, "RTL8111/8168 Gigabit Ethernet"}, {0x10ec, 0x8136, "RTL810x Fast Ethernet"},
    {0x10ec, 0x8125, "RTL8125 2.5 Gigabit Ethernet"}, {0x14e4, 0x1684, "NetXtreme BCM5764M"},
    {0x1234, 0x1111, "Standard VGA"}, {0x1af4, 0x1000, "Network device"}, {0x1af4, 0x1041, "Network device"},
};

static const struct { unsigned short class_code; const char *name; } classes[] = {
    {0x0100, "SCSI controller"}, {0x0101, "IDE controller"}, {0x0104, "RAID controller"}, {0x0106, "SATA controller"},
    {0x0108, "NVMe controller"}, {0x0180, "Storage controller"}, {0x0200, "Ethernet controller"}, {0x0280, "Network controller"},
    {0x0300, "Display adapter"}, {0x0302, "3D controller"}, {0x0380, "Display controller"}, {0x0401, "Audio device"},
    {0x0403, "HD Audio controller"}, {0x0480, "Multimedia device"}, {0x0500, "Memory controller"}, {0x0580, "Memory controller"},
    {0x0600, "Host bridge"}, {0x0601, "ISA bridge"}, {0x0604, "PCI bridge"}, {0x0680, "Bridge"}, {0x0700, "Serial controller"},
    {0x0780, "Communication controller"}, {0x0805, "SD card reader"}, {0x0880, "System peripheral"}, {0x0c03, "USB controller"},
    {0x0c05, "SMBus controller"}, {0x0c80, "Serial bus controller"}, {0x0d11, "Bluetooth controller"}, {0x0d80, "Wireless controller"},
    {0x1180, "Signal processing controller"},
};

static void log_append(char c) {
    if (!log_count) log_count = 1;
    if (c == '\r') { log_column = 0; return; }
    if (c == '\n' || log_column >= LOG_WIDTH - 1) {
        if (log_count == LOG_LINES) { memmove(log_text, log_text + 1, sizeof(log_text[0]) * (LOG_LINES - 1)); --log_count; }
        memset(log_text[log_count], 0, LOG_WIDTH);
        ++log_count;
        log_column = 0;
        if (c == '\n') return;
    }
    log_text[log_count - 1][log_column++] = c;
    log_text[log_count - 1][log_column] = 0;
}

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
    char text[300];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    for (const char *p = text; *p; ++p) log_append(*p);
    log_append('\n');
    log_scroll.total = log_count;
    log_scroll.top = log_count;
    vscroll_clamp(&log_scroll);
    if (win) window_paint(win);
}

static const char *vendor_name(unsigned short vendor) {
    for (unsigned i = 0; i < sizeof(vendors) / sizeof(*vendors); ++i)
        if (vendors[i].vendor == vendor) return vendors[i].name;
    return 0;
}

static const char *class_name(unsigned short class_code) {
    for (unsigned i = 0; i < sizeof(classes) / sizeof(*classes); ++i)
        if (classes[i].class_code == class_code) return classes[i].name;
    return 0;
}

/* What runs a device no .DRV took: the system itself, the firmware, or nothing. */
static const char *built_in(const struct dosxc_deviceinfo *d) {
    unsigned short c = (unsigned short)(d->class_code << 8 | d->subclass);
    if (c == 0x0101 || c == 0x0106 || c == 0x0108 || c == 0x0c03 || (c == 0x0104 && d->vendor == 0x8086)) return "built in";
    if (d->vendor == 0x1234 && d->id == 0x1111) return "built in";
    if (d->class_code == 0x03) return "firmware (VESA/UEFI)";
    if (d->class_code == 0x06 || c == 0x0500 || c == 0x0580 || c == 0x0880 || c == 0x0c05 || c == 0x1180 || c == 0x0780 || c == 0x0c80)
        return "not needed";
    return 0;
}

static int driver_loaded(const char *file) {
    for (int i = 0; i < loaded_count; ++i)
        if (!strcasecmp(loaded[i].file, file)) return i;
    return -1;
}

static void load_state(void) {
    loaded_count = 0;
    int n = dosxc_sys_driver(DOSXC_DRIVER_LIST, 0, 0, 0);
    for (int i = 0; i < n && loaded_count < MAX_LOADED; ++i)
        if (dosxc_sys_driver(DOSXC_DRIVER_LIST, i, &loaded[loaded_count], sizeof(loaded[0])) >= 0) ++loaded_count;
    device_count = missing_count = 0;
    n = dosxc_sys_driver(DOSXC_DRIVER_DEVICES, 0, 0, 0);
    for (int i = 0; i < n && device_count < MAX_DEVICES; ++i) {
        struct device *d = &devices[device_count];
        if (dosxc_sys_driver(DOSXC_DRIVER_DEVICES, i, &d->info, sizeof(d->info)) < 0) continue;
        const char *vendor = vendor_name(d->info.vendor), *what = class_name((unsigned short)(d->info.class_code << 8 | d->info.subclass));
        const char *model = 0;
        for (unsigned k = 0; k < sizeof(known) / sizeof(*known); ++k)
            if (known[k].vendor == d->info.vendor && known[k].id == d->info.id) model = known[k].name;
        char unknown_class[32];
        if (!what) { snprintf(unknown_class, sizeof(unknown_class), "Device (class %02x%02x)", d->info.class_code, d->info.subclass); what = unknown_class; }
        if (model) snprintf(d->name, sizeof(d->name), "%s %s", vendor ? vendor : "", model);
        else snprintf(d->name, sizeof(d->name), "%s%s%s", vendor ? vendor : "", vendor ? " " : "", what);
        const char *b = built_in(&d->info);
        if (d->info.driver[0]) { memcpy(d->status, d->info.driver, sizeof(d->info.driver)); d->status[sizeof(d->info.driver) - 1] = 0; }
        else if (b) snprintf(d->status, sizeof(d->status), "%s", b);
        else { snprintf(d->status, sizeof(d->status), "no driver"); ++missing_count; }
        ++device_count;
    }
    scroll.total = device_count;
    scroll.visible = list.h / ROW;
    vscroll_clamp(&scroll);
    if (win) window_paint(win);
}

static void pump(void) {
    if (win) gui_pump();
    else dosxc_sys_sleep(20 * NS_PER_MS);
}

/* PKG VERB FILE /Y, its output into the log.  Returns when it ends. */
static void run_pkg(const char *verb, const char *file) {
    char path[40];
    snprintf(path, sizeof(path), "%c:\\Programs\\pkg\\PKG.EXE", sys.drive);
    int console = (int)dosxc_sys_task(DOSXC_TASK_CONSOLE_OPEN, 0, 0, 0);
    if (console < 0) { say("Could not open a console for PKG."); return; }
    char *argv[5] = {"PKG.EXE", (char *)verb, (char *)file, "/Y", 0};
    long pid = dosxc_sys_task(DOSXC_TASK_START, path, (long)argv, console + 1);
    if (pid < 0) { say("PKG could not be started."); dosxc_sys_task(DOSXC_TASK_CONSOLE_CLOSE, 0, console, 0); return; }
    static unsigned char buffer[2048];
    for (;;) {
        int n, alive = dosxc_sys_task(DOSXC_TASK_ALIVE, 0, pid, 0) > 0;
        while ((n = (int)dosxc_sys_task(DOSXC_TASK_CONSOLE_READ, buffer, console, sizeof(buffer))) > 0) {
            for (int i = 0; i < n; ++i) if (buffer[i] != 27) log_append((char)buffer[i]);
            log_scroll.total = log_count;
            log_scroll.top = log_count;
            vscroll_clamp(&log_scroll);
            if (win) window_paint(win);
        }
        if (!alive) break;
        pump();
    }
    dosxc_sys_task(DOSXC_TASK_CONSOLE_CLOSE, 0, console, 0);
}

static int list_folder(const char *folder, const char *extension, struct dosxc_dirent **out) {
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 128);
    if (!entries) return 0;
    int n = (int)dosxc_sys_listdir(folder, entries, 128), kept = 0;
    for (int i = 0; i < n && i < 128; ++i) {
        size_t length = strlen(entries[i].name);
        if ((entries[i].attributes & DOSXC_ATTR_DIRECTORY) || length < 5 || strcasecmp(entries[i].name + length - 4, extension)) continue;
        entries[kept++] = entries[i];
    }
    *out = entries;
    return kept;
}

static int store_driver_running(const char *file) {
    char path[300];
    struct stat st;
    snprintf(path, sizeof(path), "%c:\\SYSTEM\\DRIVERS\\%s", sys.drive, file);
    return driver_loaded(file) >= 0 && stat(path, &st);
}

/* Drivers in \SYSTEM\DRIVERS\STORE that are not running and name a device here. */
static int start_store_drivers(void) {
    char store[40], path[300];
    snprintf(store, sizeof(store), "%c:\\SYSTEM\\DRIVERS\\STORE", sys.drive);
    struct dosxc_dirent *entries = 0;
    int n = list_folder(store, ".DRV", &entries), started = 0;
    for (int i = 0; i < n; ++i) {
        if (driver_loaded(entries[i].name) >= 0) continue;
        snprintf(path, sizeof(path), "%s\\%s", store, entries[i].name);
        if (dosxc_sys_driver(DOSXC_DRIVER_MATCH, 0, path, 0) <= 0) continue;
        int r = dosxc_sys_driver(DOSXC_DRIVER_LOAD, 0, path, 0);
        load_state();
        int at = driver_loaded(entries[i].name);
        if (r >= 0 && at >= 0 && loaded[at].devices) {
            say("Started %s (%s) from the driver store.", entries[i].name, loaded[at].name);
            ++started;
        } else {
            if (at >= 0) dosxc_sys_driver(DOSXC_DRIVER_UNLOAD, 0, entries[i].name, 0);
            say("%s matches a device here but did not take it; see Driver Log.", entries[i].name);
        }
    }
    free(entries);
    return started;
}

static int installed_version(const char *name, char *version, size_t size) {
    char inf[400], text[2048];
    snprintf(inf, sizeof(inf), "%c:\\Programs\\%s\\PACKAGE.INF", sys.drive, name);
    FILE *f = fopen(inf, "rb");
    if (!f) return 0;
    size_t got = fread(text, 1, sizeof(text), f);
    fclose(f);
    struct dsp_info info;
    if (dsp_parse_info(text, got, &info)) return 0;
    snprintf(version, size, "%s", info.version);
    return 1;
}

/* Does a driver in PACKAGE name a device without a driver?  Bit 1: yes;
   bit 2: one of its drivers runs from the store only (installing keeps it updated). */
static int package_wants(struct dsp *package) {
    char temp[64];
    snprintf(temp, sizeof(temp), "%c:\\TEMP\\DRVSCAN.DRV", sys.drive);
    int wants = 0;
    char file[64];
    for (const char *list = package->info.driver; (list = dsp_next_item(list, file, sizeof(file)));) {
        if (!file[0]) continue;
        if (driver_loaded(file) >= 0) {
            if (!sys.live && store_driver_running(file)) wants |= 2;
            continue;
        }
        for (unsigned i = 0; i < package->count; ++i) {
            const char *name = package->entries[i].name, *slash = strrchr(name, '/');
            if (strcasecmp(slash ? slash + 1 : name, file)) continue;
            unsigned char *data = 0;
            size_t size = 0;
            if (dsp_read_entry(package, i, &data, &size)) break;
            FILE *f = fopen(temp, "wb");
            int written = f && fwrite(data, 1, size, f) == size;
            if (f && fclose(f)) written = 0;
            free(data);
            if (written && dosxc_sys_driver(DOSXC_DRIVER_MATCH, 0, temp, 0) > 0) wants |= 1;
            unlink(temp);
            break;
        }
    }
    return wants;
}

/* Packages in \Packages on any drive whose drivers fit this computer. */
static int install_packages(void) {
    struct dosxc_volume volumes[26];
    int volume_count = dosxc_sys_volumes(volumes, 26), installed = 0;
    char done[32][64];
    int done_count = 0;
    for (int v = 0; v < volume_count && v < 26; ++v) {
        char folder[16];
        snprintf(folder, sizeof(folder), "%c:\\Packages", volumes[v].letter);
        struct dosxc_dirent *entries = 0;
        int n = list_folder(folder, ".DSP", &entries);
        for (int i = 0; i < n; ++i) {
            char file[300], error[120];
            snprintf(file, sizeof(file), "%s\\%s", folder, entries[i].name);
            struct dsp_info info;
            if (dsp_read_info(file, &info, error, sizeof(error)) || !info.driver[0] || info.arch != (int)sys.bits) continue;
            int seen = 0;
            for (int k = 0; k < done_count; ++k) seen |= !strcasecmp(done[k], info.name);
            char version[32];
            int present = installed_version(info.name, version, sizeof(version));
            if (seen || (present && dsp_compare_versions(info.version, version) <= 0)) continue;
            if (done_count < 32) snprintf(done[done_count++], sizeof(done[0]), "%s", info.name);
            struct dsp package;
            if (dsp_open(&package, file, error, sizeof(error))) { say("%s: %s", file, error); continue; }
            int wants = package_wants(&package);
            dsp_close(&package);
            if (!wants) continue;
            say("%s %s has drivers for this computer: %s", info.name, info.version,
                wants & 1 ? "installing it" : "installing it so its drivers are kept up to date");
            run_pkg(present ? "UPDATE" : "INSTALL", file);
            load_state();
            if (wants & 1) ++installed;
        }
        free(entries);
    }
    return installed;
}

static int is_admin(void) {
    struct dosxc_user me;
    memset(&me, 0, sizeof(me));
    if (dosxc_sys_user(DOSXC_USER_ENABLED, 0, 0) <= 0) return 1;
    return dosxc_sys_user(DOSXC_USER_WHOAMI, 0, &me) >= 0 && me.admin;
}

/* Returns how many devices gained a driver. */
static int find_drivers(int quiet) {
    if (busy) return 0;
    if (!is_admin()) { if (!quiet) say("Only an administrator can install drivers."); return 0; }
    busy = 1;
    load_state();
    int before = missing_count;
    if (!quiet) say("Looking for drivers for %d device(s) without one...", missing_count);
    start_store_drivers();
    install_packages();
    load_state();
    int gained = before - missing_count;
    if (!quiet || gained) {
        if (missing_count) say("%d device(s) still have no driver; DOS-XC has none for them.", missing_count);
        else say("Every device that needs a driver has one.");
    }
    busy = 0;
    return gained > 0 ? gained : 0;
}

static void show_driver_log(void) {
    static char text[8192];
    int n = dosxc_sys_driver(DOSXC_DRIVER_LOG, 0, text, sizeof(text));
    if (n < 0) { say("Only an administrator can read the driver log."); return; }
    say("---- Driver log ----");
    for (int i = 0; i < n && text[i]; ++i) log_append(text[i]);
    say("--------------------");
}

static void dm_paint(struct window *w, const struct canvas *c) {
    (void)w;
    char line[160];
    snprintf(line, sizeof(line), "%d devices, %d without a driver", device_count, missing_count);
    text(c, 12, 9, &font_bold, line, C_TEXT);
    bevel(c, list.x, list.y, list.w, list.h, 1);
    fill(c, list.x + 2, list.y + 2, list.w - 4, list.h - 4, C_BASE);
    struct rect rows = {list.x + 2, list.y + 2, list.w - 20, list.h - 4};
    for (int i = scroll.top; i < device_count && i < scroll.top + scroll.visible; ++i) {
        const struct device *d = &devices[i];
        int y = rows.y + (i - scroll.top) * ROW;
        u32 color = strcmp(d->status, "no driver") ? C_TEXT : 0xb00000;
        snprintf(line, sizeof(line), "%02x:%02x.%u", d->info.bus, d->info.device, d->info.function);
        text(c, rows.x + 4, y + 5, &font_regular, line, C_SHADOW1);
        char name[60];
        snprintf(name, sizeof(name), "%.56s", d->name);
        text(c, rows.x + 64, y + 5, &font_regular, name, color);
        snprintf(line, sizeof(line), "%04x:%04x", d->info.vendor, d->info.id);
        text(c, rows.x + 384, y + 5, &font_regular, line, C_SHADOW1);
        text(c, rows.x + 460, y + 5, d->info.driver[0] ? &font_bold : &font_regular, d->status, color);
    }
    vscroll_paint(c, &scroll, (struct rect){list.x + list.w - 18, list.y + 2, 16, list.h - 4});
    button(c, act_find, busy ? "Searching..." : "Find Drivers", busy, 1);
    button(c, act_log, "Driver Log", 0, 0);
    button(c, act_refresh, "Refresh", 0, 0);
    bevel(c, log_area.x, log_area.y, log_area.w, log_area.h, 1);
    fill(c, log_area.x + 2, log_area.y + 2, log_area.w - 4, log_area.h - 4, C_BASE);
    log_scroll.visible = (log_area.h - 6) / 13;
    log_scroll.total = log_count;
    vscroll_clamp(&log_scroll);
    for (int i = log_scroll.top; i < log_count && i < log_scroll.top + log_scroll.visible; ++i)
        text(c, log_area.x + 6, log_area.y + 4 + (i - log_scroll.top) * 13, &font_regular, log_text[i], C_TEXT);
    vscroll_paint(c, &log_scroll, (struct rect){log_area.x + log_area.w - 18, log_area.y + 2, 16, log_area.h - 4});
}

static void dm_mouse(struct window *w, int x, int y, int kind, int value) {
    if (vscroll_mouse(&scroll, (struct rect){list.x + list.w - 18, list.y + 2, 16, list.h - 4}, x, y, kind, value) ||
        vscroll_mouse(&log_scroll, (struct rect){log_area.x + log_area.w - 18, log_area.y + 2, 16, log_area.h - 4}, x, y, kind, value)) {
        window_paint(w);
        return;
    }
    if (kind == M_WHEEL) {
        struct vscroll *s = rect_has(log_area, x, y) ? &log_scroll : &scroll;
        s->top += value * 3;
        vscroll_clamp(s);
        window_paint(w);
        return;
    }
    if (kind != M_DOWN) return;
    if (rect_has(act_find, x, y)) { find_drivers(0); window_paint(w); }
    else if (rect_has(act_log, x, y)) show_driver_log();
    else if (rect_has(act_refresh, x, y)) load_state();
}

static const struct app_ops dm_ops = {dm_paint, dm_mouse};

static void open_window(const char *title) {
    win = window_open(660, 460, title, ICON_COMPUTER, &dm_ops, 0);
}

int main(int argc, char **argv) {
    int automatic = argc > 1 && !strcasecmp(argv[1], "/AUTO");
    dosxc_sys_sysinfo(&sys);
    if (automatic) {
        char manual[40];
        struct stat st;
        snprintf(manual, sizeof(manual), "%c:\\SYSTEM\\DRIVERS\\MANUAL", sys.drive);
        if (!stat(manual, &st) || !is_admin()) return 0;
        int gained = find_drivers(1);
        if (!gained) return 0;
        if (gui_connect()) return 0;
        say("DOS-XC installed drivers for %d new device(s).", gained);
        open_window("New Hardware");
        load_state();
        gui_run();
        return 0;
    }
    if (gui_connect()) return 1;
    open_window("Device Manager");
    if (!win) return 1;
    load_state();
    say("%d device(s) found.  Find Drivers looks in the driver store and in \\Packages on every drive.", device_count);
    gui_run();
    return 0;
}
