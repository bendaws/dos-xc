/* Clock service regression tests with storage, permissions and RTC doubles. */
#include "../../os/runtime/runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct command_context context;
struct command_context *ctx = &context;
static int admin = 1, exists, write_error, rtc_error, rtc_writes;
static struct dosxc_clock disk;
static i64 rtc = 1794484800;
int rt_upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
void rt_strcpy(char *d, const char *s) { strcpy(d, s); }
int accounts_admin(void) { return admin; }
i64 cpu_rtc_time(void) { return rtc; }
int cpu_rtc_set(i64 t) {
    if (rtc_error) return -DOSXC_EIO;
    if (t < 0 || t > 4102444799LL) return -DOSXC_EINVAL;
    rtc = t; ++rtc_writes; return 0;
}
int fs_stat(const char *path, struct fat32_info *info) {
    assert(!strcmp(path, "C:\\SYSTEM\\TIME.CFG"));
    info->size = sizeof(disk); return exists ? 0 : -1;
}
int fs_read(const char *path, void *buffer, u32 size) {
    assert(size == sizeof(disk)); memcpy(buffer, &disk, size); return 0;
}
int fs_save(const char *path, const void *buffer, u32 size) {
    assert(!strcmp(path, "C:\\SYSTEM\\TIME.CFG")); assert(size == sizeof(disk));
    if (write_error) return -1;
    memcpy(&disk, buffer, size); exists = 1; return 0;
}
int main(void) {
    context.drive = 'c';
    clock_init();
    struct dosxc_clock v;
    assert(!clock_control(DOSXC_CLOCK_GET, &v));
    assert(v.seconds == rtc && v.utc_offset == 0 && !v.hour12 && !strcmp(v.server, "pool.ntp.org"));
    i64 original = rtc;
    v.utc_offset = -360; v.hour12 = 1; strcpy(v.server, "192.0.2.1");
    admin = 0;
    assert(clock_control(DOSXC_CLOCK_SETTINGS, &v) == -DOSXC_EACCES && !exists && rtc == original);
    assert(clock_control(DOSXC_CLOCK_SET, &v) == -DOSXC_EACCES);
    assert(!clock_control(DOSXC_CLOCK_GET, &v));
    admin = 1; v.utc_offset = -360; v.hour12 = 1;
    assert(!clock_control(DOSXC_CLOCK_SETTINGS, &v));
    assert(rtc == original - 360 * 60 && rtc_writes == 1 && disk.seconds == 0);
    clock_init();
    assert(!clock_control(DOSXC_CLOCK_GET, &v) && v.utc_offset == -360 && v.hour12 == 1);
    assert(rtc == original - 360 * 60); /* loading never reapplies the offset */
    v.utc_offset = 345; /* fractional-hour zone, UTC+05:45 */
    assert(!clock_control(DOSXC_CLOCK_SETTINGS, &v) && rtc == original + 345 * 60);
    v.utc_offset = 841;
    assert(clock_control(DOSXC_CLOCK_SETTINGS, &v) == -DOSXC_EINVAL);
    v.utc_offset = -721;
    assert(clock_control(DOSXC_CLOCK_SETTINGS, &v) == -DOSXC_EINVAL);
    v.utc_offset = 0; memset(v.server, 'a', sizeof(v.server));
    assert(clock_control(DOSXC_CLOCK_SETTINGS, &v) == -DOSXC_EINVAL);
    strcpy(v.server, "bad/server");
    assert(clock_control(DOSXC_CLOCK_SETTINGS, &v) == -DOSXC_EINVAL);
    strcpy(v.server, "pool.ntp.org"); write_error = 1;
    i64 before = rtc;
    assert(clock_control(DOSXC_CLOCK_SETTINGS, &v) == -DOSXC_EIO && rtc == before);
    assert(!clock_control(DOSXC_CLOCK_GET, &v) && v.utc_offset == 345);
    write_error = 0;
    v.utc_offset = 0; rtc_error = 1;
    assert(clock_control(DOSXC_CLOCK_SETTINGS, &v) == -DOSXC_EIO && rtc == before);
    assert(disk.utc_offset == 345);
    assert(!clock_control(DOSXC_CLOCK_GET, &v) && v.utc_offset == 345);
    rtc_error = 0;
    v.seconds = 4102444799LL;
    assert(!clock_control(DOSXC_CLOCK_SET, &v) && rtc == v.seconds);
    v.seconds++;
    assert(clock_control(DOSXC_CLOCK_SET, &v) == -DOSXC_EINVAL);
    v.seconds = -1;
    assert(clock_control(DOSXC_CLOCK_SET, &v) == -DOSXC_EINVAL);
    assert(clock_control(999, &v) == -DOSXC_EINVAL);
    assert(clock_control(DOSXC_CLOCK_GET, 0) == -DOSXC_EFAULT);
    disk.utc_offset = 10000;
    clock_init();
    assert(!clock_control(DOSXC_CLOCK_GET, &v) && v.utc_offset == 0 && !v.hour12);
    puts("PASS clock permissions, persistence, offsets, validation and storage errors");
}
