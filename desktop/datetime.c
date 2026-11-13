/*
 * DOS-XC /desktop/datetime.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "wm.h"
#include <time.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <unistd.h>

/* Keep calendar input separate from preferences: applying a zone must not
   overwrite the clock with the time that was shown when the page opened. */
enum { D_DATE, D_TIME, D_ZONE, D_SERVER, D_FIELDS };
struct datetime {
    struct dosxc_clock config;
    struct field fields[D_FIELDS];
    int focus, pm, fd;
    unsigned char request[48];
    unsigned long long deadline, refresh;
    char status[128];
};
static struct window *date_window;
static struct rect dt_field(int i) { return (struct rect){132, 66 + i * 58, i == D_SERVER ? 272 : 156, 22}; }
static const struct rect dt_format = {304, 124, 132, 22}, dt_ampm = {444, 124, 60, 22};
static const struct rect dt_set = {304, 66, 132, 24}, dt_apply = {132, 298, 112, 24};
static const struct rect dt_sync = {252, 298, 112, 24}, dt_refresh = {372, 298, 112, 24};

static void dt_status(struct window *w, const char *s) {
    struct datetime *d = w->data;
    snprintf(d->status, sizeof(d->status), "%s", s);
    window_paint(w);
}
static void dt_calendar(struct datetime *d) {
    time_t now = dosxc_sys_time();
    struct tm tm;
    gmtime_r(&now, &tm); /* The system ABI already returns local seconds. */
    char line[32];
    strftime(line, sizeof(line), "%Y-%m-%d", &tm);
    field_set(&d->fields[D_DATE], line);
    d->pm = tm.tm_hour >= 12;
    snprintf(line, sizeof(line), "%02d:%02d:%02d", d->config.hour12 ? (tm.tm_hour % 12 ? tm.tm_hour % 12 : 12) : tm.tm_hour, tm.tm_min, tm.tm_sec);
    field_set(&d->fields[D_TIME], line);
}
static void dt_load(struct datetime *d) {
    dosxc_sys_clock(DOSXC_CLOCK_GET, &d->config);
    dt_calendar(d);
    char zone[16];
    int offset = abs(d->config.utc_offset);
    snprintf(zone, sizeof(zone), "%c%02d:%02d", d->config.utc_offset < 0 ? '-' : '+', offset / 60, offset % 60);
    field_set(&d->fields[D_ZONE], zone);
    field_set(&d->fields[D_SERVER], d->config.server);
}
static void dt_paint(struct window *w, const struct canvas *c) {
    struct datetime *d = w->data;
    char line[96];
    time_t now = dosxc_sys_time();
    struct tm tm;
    gmtime_r(&now, &tm);
    struct dosxc_clock actual;
    dosxc_sys_clock(DOSXC_CLOCK_GET, &actual);
    strftime(line, sizeof(line), actual.hour12 ? "%Y-%m-%d   %I:%M:%S %p" : "%Y-%m-%d   %H:%M:%S", &tm);
    text(c, 16, 16, &font_bold, line, C_TEXT);
    const char *labels[] = {"Date", "Time", "Time zone", "Time server"};
    const char *hints[] = {"YYYY-MM-DD (1970 to 2099)", "HH:MM:SS", "UTC offset: -12:00 to +14:00", "NTP hostname or IP address"};
    for (int i = 0; i < D_FIELDS; ++i) {
        struct rect r = dt_field(i);
        text(c, 16, r.y + 5, &font_regular, labels[i], C_TEXT);
        field_paint(c, &d->fields[i], r, i == d->focus);
        text(c, r.x, r.y + 28, &font_regular, hints[i], C_SHADOW2);
    }
    button(c, dt_set, "Set date/time", 0, 0);
    button(c, dt_format, d->config.hour12 ? "12-hour (AM/PM)" : "24-hour", 0, 0);
    button(c, dt_ampm, d->config.hour12 ? (d->pm ? "PM" : "AM") : "--", 0, 0);
    text(c, 132, 224, &font_regular, "Fixed offset; adjust for daylight saving manually.", C_SHADOW2);
    button(c, dt_apply, "Apply settings", 0, 1);
    button(c, dt_sync, d->fd >= 0 ? "Syncing..." : "Sync now", 0, 0);
    button(c, dt_refresh, "Refresh", 0, 0);
    text(c, 16, 340, &font_regular, d->status, C_TEXT);
    text(c, 16, 370, &font_regular, "Changing system time and settings requires an administrator.", C_SHADOW2);
}
static void dt_error(struct window *w, int result) {
    dt_status(w, result == -DOSXC_EACCES ? "Only administrators can change the clock settings." :
              result == -DOSXC_EIO ? "Could not save the settings or update the hardware clock." : "Could not change the clock settings.");
}
static int dt_apply_settings(struct window *w) {
    struct datetime *d = w->data;
    const char *s = d->fields[D_ZONE].text;
    if (strlen(s) != 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':' ||
        s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9' ||
        s[4] < '0' || s[4] > '9' || s[5] < '0' || s[5] > '9') {
        dt_status(w, "Enter the time zone as +HH:MM or -HH:MM."); return -1;
    }
    int hours = (s[1] - '0') * 10 + s[2] - '0', minutes = (s[4] - '0') * 10 + s[5] - '0';
    int offset = (hours * 60 + minutes) * (s[0] == '-' ? -1 : 1);
    if (minutes > 59 || offset < -720 || offset > 840) { dt_status(w, "The UTC offset must be between -12:00 and +14:00."); return -1; }
    const char *server = d->fields[D_SERVER].text;
    size_t n = strlen(server);
    if (!n || n >= sizeof(d->config.server) || strspn(server, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.:") != n) {
        dt_status(w, "Enter a time server hostname or IP address (up to 127 characters)."); return -1;
    }
    struct dosxc_clock next = d->config;
    next.utc_offset = offset;
    snprintf(next.server, sizeof(next.server), "%s", server);
    int result = dosxc_sys_clock(DOSXC_CLOCK_SETTINGS, &next);
    if (result < 0) { dt_error(w, result); return -1; }
    d->config = next;
    dt_calendar(d);
    dt_status(w, "Time zone, clock format and time server saved.");
    return 0;
}
static void dt_set_calendar(struct window *w) {
    struct datetime *d = w->data;
    int year, month, day, hour, minute, second;
    char extra;
    if (strlen(d->fields[D_DATE].text) != 10 || strlen(d->fields[D_TIME].text) != 8 ||
        sscanf(d->fields[D_DATE].text, "%4d-%2d-%2d%c", &year, &month, &day, &extra) != 3 ||
        sscanf(d->fields[D_TIME].text, "%2d:%2d:%2d%c", &hour, &minute, &second, &extra) != 3 ||
        year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < (d->config.hour12 ? 1 : 0) || hour > (d->config.hour12 ? 12 : 23) || minute < 0 || minute > 59 || second < 0 || second > 59) {
        dt_status(w, "Enter a valid date and time in the formats shown above."); return;
    }
    if (d->config.hour12) hour = hour % 12 + (d->pm ? 12 : 0);
    struct tm tm = {0};
    tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = second;
    time_t seconds = timegm(&tm);
    if (tm.tm_year != year - 1900 || tm.tm_mon != month - 1 || tm.tm_mday != day) {
        dt_status(w, "That date does not exist. Check the month and day."); return;
    }
    struct dosxc_clock value = {0};
    value.seconds = seconds;
    int result = dosxc_sys_clock(DOSXC_CLOCK_SET, &value);
    if (result < 0) dt_error(w, result);
    else dt_status(w, "System date and time updated.");
}
static void dt_stop(struct datetime *d) { if (d->fd >= 0) close(d->fd); d->fd = -1; }
static void dt_sync_start(struct window *w) {
    struct datetime *d = w->data;
    if (d->fd >= 0 || dt_apply_settings(w)) return;
    struct addrinfo hints = {0}, *addresses = 0;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
    dt_status(w, "Looking up the time server...");
    if (getaddrinfo(d->config.server, "123", &hints, &addresses)) { dt_status(w, "Could not resolve the server. Check DNS and firewall UDP 53 rules."); return; }
    int denied = 0;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        d->fd = socket(a->ai_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (d->fd >= 0 && !connect(d->fd, a->ai_addr, a->ai_addrlen)) break;
        denied |= errno == EACCES;
        dt_stop(d);
    }
    freeaddrinfo(addresses);
    if (d->fd < 0) { dt_status(w, denied ? "Firewall blocked time sync. Allow DESKTOP.EXE outbound UDP 123." : "Could not contact the time server. Check Network settings."); return; }
    memset(d->request, 0, sizeof(d->request));
    d->request[0] = 0x23; /* NTP v4, client. The echoed transmit cookie binds the reply. */
    if (getrandom(d->request + 40, 8, 0) != 8) { dt_stop(d); dt_status(w, "Could not create a time server request."); return; }
    if (send(d->fd, d->request, sizeof(d->request), 0) != sizeof(d->request)) {
        dt_stop(d); dt_status(w, "Could not send the time server request."); return;
    }
    d->deadline = dosxc_sys_nanoseconds() + 5000000000ULL;
    dt_status(w, "Waiting for the time server...");
}
static void dt_tick(struct window *w) {
    struct datetime *d = w->data;
    unsigned long long now = dosxc_sys_nanoseconds();
    if (d->fd >= 0) {
        unsigned char reply[512];
        int n = (int)recv(d->fd, reply, sizeof(reply), 0);
        if (n >= 0) {
            if (n < 48 || (reply[0] & 7) != 4 || ((reply[0] >> 3) & 7) < 3 || ((reply[0] >> 3) & 7) > 4 ||
                (reply[0] >> 6) == 3 || reply[1] == 0 || reply[1] > 15 ||
                memcmp(reply + 24, d->request + 40, 8) || !memcmp(reply + 40, "\0\0\0\0\0\0\0\0", 8)) {
                dt_stop(d); dt_status(w, "The time server returned an invalid or unsynchronized reply."); return;
            }
            unsigned long long stamp = (unsigned long long)reply[40] << 24 | (unsigned)reply[41] << 16 | (unsigned)reply[42] << 8 | reply[43];
            /* Unfold NTP's 2036 rollover into the supported 1970..2099 range. */
            if (stamp < 2208988800ULL) stamp += 4294967296ULL;
            struct dosxc_clock value;
            dosxc_sys_clock(DOSXC_CLOCK_GET, &value);
            value.seconds = (long long)stamp - 2208988800LL + value.utc_offset * 60LL;
            dt_stop(d);
            int result = dosxc_sys_clock(DOSXC_CLOCK_SET, &value);
            if (result < 0) dt_error(w, result);
            else { dt_calendar(d); dt_status(w, "Successfully synchronized with the time server."); }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            dt_stop(d); dt_status(w, "The time server connection failed.");
        } else if (now >= d->deadline) {
            dt_stop(d); dt_status(w, "The time server did not reply. Check the address or try another.");
        }
    }
    if (now >= d->refresh) { d->refresh = now + 1000000000ULL; window_paint(w); }
}
static void dt_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind != M_DOWN) return;
    struct datetime *d = w->data;
    for (int i = 0; i < D_FIELDS; ++i) if (rect_has(dt_field(i), x, y)) d->focus = i;
    if (rect_has(dt_set, x, y)) { if (d->fd < 0) dt_set_calendar(w); }
    else if (rect_has(dt_apply, x, y)) { if (d->fd < 0) dt_apply_settings(w); }
    else if (rect_has(dt_sync, x, y)) dt_sync_start(w);
    else if (rect_has(dt_refresh, x, y)) { dt_stop(d); dt_load(d); dt_status(w, "Current settings loaded."); }
    else if (rect_has(dt_format, x, y)) {
        int h, m, s; char extra;
        if (sscanf(d->fields[D_TIME].text, "%d:%d:%d%c", &h, &m, &s, &extra) == 3 && h >= (d->config.hour12 ? 1 : 0) && h <= (d->config.hour12 ? 12 : 23)) {
            if (d->config.hour12) h = h % 12 + (d->pm ? 12 : 0);
            d->pm = h >= 12;
            int display = d->config.hour12 ? h : (h % 12 ? h % 12 : 12);
            char line[32]; snprintf(line, sizeof(line), "%02d:%02d:%02d", display, m, s);
            field_set(&d->fields[D_TIME], line);
        }
        d->config.hour12 = !d->config.hour12;
        dt_status(w, "Click Apply settings to save the clock format.");
    } else if (rect_has(dt_ampm, x, y) && d->config.hour12) d->pm = !d->pm;
    window_paint(w);
}
static void dt_key(struct window *w, int key) {
    struct datetime *d = w->data;
    if (key == '\t') d->focus = (d->focus + 1) % D_FIELDS;
    else if (key == '\n') {
        if (d->fd < 0) { if (d->focus <= D_TIME) dt_set_calendar(w); else dt_apply_settings(w); }
    } else field_key(&d->fields[d->focus], key);
    window_paint(w);
}
static void dt_closed(struct window *w) { dt_stop(w->data); free(w->data); date_window = 0; }
static const struct app_ops dt_ops = {.paint = dt_paint, .mouse = dt_mouse, .key = dt_key, .closed = dt_closed, .tick = dt_tick};
void open_datetime_settings(void) {
    if (date_window) { window_raise(date_window); return; }
    struct datetime *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->fd = -1;
    dt_load(d);
    date_window = window_open(560, 400, "Date and Time", ICON_GEAR, &dt_ops, d);
    if (!date_window) free(d);
}
