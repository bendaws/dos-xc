/*
 * DOS-XC /os/runtime/clock.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "runtime.h"

static struct dosxc_clock settings;
static void config_path(char *path) {
    path[0] = (char)rt_upper(ctx->drive);
    rt_strcpy(path + 1, ":\\SYSTEM\\TIME.CFG");
}
static int valid(const struct dosxc_clock *v) {
    if (v->utc_offset < -720 || v->utc_offset > 840 || v->hour12 < 0 || v->hour12 > 1) return 0;
    unsigned i;
    for (i = 0; i < sizeof(v->server) && v->server[i]; ++i) {
        char c = v->server[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':')) return 0;
    }
    return i > 0 && i < sizeof(v->server);
}
void clock_init(void) {
    memset(&settings, 0, sizeof(settings));
    rt_strcpy(settings.server, "pool.ntp.org");
    char path[32];
    config_path(path);
    struct fat32_info info;
    struct dosxc_clock saved;
    if (!fs_stat(path, &info) && info.size == sizeof(saved) && !fs_read(path, &saved, sizeof(saved)) && valid(&saved))
        settings = saved;
}
int clock_control(int op, struct dosxc_clock *value) {
    if (!value) return -DOSXC_EFAULT;
    if (op == DOSXC_CLOCK_GET) {
        *value = settings;
        value->seconds = cpu_rtc_time();
        return 0;
    }
    if (op != DOSXC_CLOCK_SET && op != DOSXC_CLOCK_SETTINGS) return -DOSXC_EINVAL;
    if (!accounts_admin()) return -DOSXC_EACCES;
    if (op == DOSXC_CLOCK_SET) return cpu_rtc_set(value->seconds);
    struct dosxc_clock next = *value;
    if (!valid(&next)) return -DOSXC_EINVAL;
    i64 seconds = cpu_rtc_time() + (next.utc_offset - settings.utc_offset) * 60LL;
    if (seconds < 0 || seconds > 4102444799LL) return -DOSXC_EINVAL;
    next.seconds = 0;
    char path[32];
    config_path(path);
    if (fs_save(path, &next, sizeof(next))) return -DOSXC_EIO;
    if (next.utc_offset != settings.utc_offset) {
        int result = cpu_rtc_set(seconds);
        if (result) {
            fs_save(path, &settings, sizeof(settings));
            return result;
        }
    }
    settings = next;
    return 0;
}
