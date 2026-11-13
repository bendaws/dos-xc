/*
 * DOS-XC /openwork/mail.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "suite.h"
#include "mail.h"
#include <strings.h>
#include <time.h>

#define FOLDER_W 190
#define LIST_W 330
#define LIST_TOP 54
#define ITEM_H 50
#define GROUP_H 22
#define FOLDER_H 26
#define SCROLL_W 14
#define REFRESH_TICKS 6000

struct wline { int start, end; };

/* A multi-line text being edited: a message body. */
struct editor { char *s; int len, cap, caret, anchor, top; struct wline *lines; int line_count, line_cap; };

struct compose {
    struct window *w;
    struct ribbon rb;
    struct field to, cc, subject;
    int focus;                           /* 0 To, 1 Cc, 2 Subject, 3 the body */
    struct editor body;
    char files[2048];                    /* attachments, one path a line */
    char in_reply_to[160], references[600];
    int selecting;
};

struct row { int kind, index; char label[24]; };   /* kind 0 a date heading, 1 a message */

static struct mailbox mbox;
static struct window *main_window, *settings_window;
static struct folder *folders;
static int folder_count, folder_cur;
static struct message *messages;
static int message_count, selected = -1, busy, unread_only, search_focus, ticks, configured;
static struct row *rows;
static int row_count;
static struct field search;
static struct vscroll list_scroll, read_scroll;
static struct ribbon rb;
static struct text raw;
static struct parsed parsed;
static unsigned loaded_uid;
static char status_text[200];
static struct wline *read_lines;
static int read_line_count, read_line_cap, read_width;
static struct compose *cw;

enum { C_NEW, C_DELETE, C_REPLY, C_REPLY_ALL, C_FORWARD, C_UNREAD, C_SEND_RECEIVE, C_ACCOUNT };
enum { M_PASTE, M_CUT, M_COPY, M_ATTACH, M_SEND };

static const struct rb_item new_items[] = {{RB_LARGE, C_NEW, IC_NEW_MAIL, 0, 0, 0, "New\nEmail"}};
static const struct rb_item delete_items[] = {{RB_LARGE, C_DELETE, IC_DELETE, 0, 0, 0, "Delete"}};
static const struct rb_item respond_items[] = {
    {RB_LARGE, C_REPLY, IC_REPLY, 0, 0, 0, "Reply"},
    {RB_LARGE, C_REPLY_ALL, IC_REPLY_ALL, 0, 0, 0, "Reply\nAll"},
    {RB_LARGE, C_FORWARD, IC_FORWARD, 0, 0, 0, "Forward"},
};
static const struct rb_item tag_items[] = {{RB_SMALL, C_UNREAD, IC_UNREAD, 0, 0, RB_LABEL, "Unread/ Read"}};
static const struct rb_item sync_items[] = {{RB_LARGE, C_SEND_RECEIVE, IC_SEND_RECEIVE, 0, 0, 0, "Send/Receive\nAll Folders"}};
static const struct rb_group home_groups[] = {
    {"New", new_items, 1}, {"Delete", delete_items, 1}, {"Respond", respond_items, 3}, {"Tags", tag_items, 1}, {"Send/Receive", sync_items, 1},
};
static const struct rb_item account_items[] = {{RB_LARGE, C_ACCOUNT, IC_SETTINGS, 0, 0, 0, "Account\nSettings"}};
static const struct rb_group sync_groups[] = {{"Send & Receive", sync_items, 1}, {"Account", account_items, 1}};
static const struct rb_tab tabs[] = {{"HOME", home_groups, 5}, {"SEND / RECEIVE", sync_groups, 2}};

static const struct rb_item clip_items[] = {
    {RB_LARGE, M_PASTE, IC_PASTE, 0, 0, 0, "Paste"},
    {RB_SMALL, M_CUT, IC_CUT, 0, 0, RB_LABEL, "Cut"},
    {RB_SMALL, M_COPY, IC_COPY, 1, 0, RB_LABEL, "Copy"},
};
static const struct rb_item include_items[] = {{RB_LARGE, M_ATTACH, IC_ATTACH, 0, 0, 0, "Attach\nFile"}};
static const struct rb_group message_groups[] = {{"Clipboard", clip_items, 3}, {"Include", include_items, 1}};
static const struct rb_tab compose_tabs[] = {{"MESSAGE", message_groups, 2}};

static u32 ink(void) { return accent_text(); }
static u32 tint(int percent) { return mix(app->accent, 0xffffff, percent); }

static void pump(void) { gui_pump(); }

static void set_status(const char *format, const char *value) {
    snprintf(status_text, sizeof(status_text), format, value);
    for (char *p = status_text; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if (main_window) window_paint(main_window);
}

/* ---- word wrap (font_regular) ---- */

static int wrap(const char *s, int len, int width, struct wline **lines, int *cap) {
    int n = 0, at = 0;
    if (width < 40) width = 40;
    for (;;) {
        int end = at, x = 0, last_space = -1;
        while (end < len && s[end] != '\n') {
            char one[2] = {s[end] == '\t' ? ' ' : s[end], 0};
            int cw = text_width(&font_regular, one) + font_regular.spacing;
            if (x + cw > width && end > at) break;
            if (s[end] == ' ') last_space = end;
            x += cw;
            ++end;
        }
        if (end < len && s[end] != '\n' && last_space >= at) end = last_space + 1;
        if (n == *cap) {
            *cap = *cap ? *cap * 2 : 256;
            *lines = realloc(*lines, sizeof(**lines) * (size_t)*cap);
        }
        (*lines)[n++] = (struct wline){at, end};
        if (end >= len) break;
        at = s[end] == '\n' ? end + 1 : end;
        if (at > len) break;
    }
    return n;
}

static int line_height(void) { return font_regular.height + 5; }

/* ---- the editor ---- */

static void editor_set(struct editor *e, const char *s) {
    e->len = (int)strlen(s);
    e->cap = e->len + 256;
    free(e->s);
    e->s = malloc((size_t)e->cap);
    memcpy(e->s, s, (size_t)e->len + 1);
    e->caret = e->anchor = e->top = 0;
}

static void editor_splice(struct editor *e, int from, int to, const char *s, int n) {
    if (e->len - (to - from) + n + 1 > e->cap) {
        e->cap = (e->len + n) * 2 + 256;
        e->s = realloc(e->s, (size_t)e->cap);
    }
    memmove(e->s + from + n, e->s + to, (size_t)(e->len - to + 1));
    memcpy(e->s + from, s, (size_t)n);
    e->len += n - (to - from);
    e->caret = e->anchor = from + n;
}

static void editor_insert(struct editor *e, const char *s, int n) {
    int lo = e->caret < e->anchor ? e->caret : e->anchor, hi = e->caret < e->anchor ? e->anchor : e->caret;
    char *clean = malloc((size_t)n + 1);
    int m = 0;
    for (int i = 0; i < n; ++i) if (s[i] != '\r') clean[m++] = s[i];
    editor_splice(e, lo, hi, clean, m);
    free(clean);
}

static int editor_line_of(const struct editor *e, int at) {
    int found = 0;
    for (int i = 0; i < e->line_count; ++i)
        if (e->lines[i].start <= at) found = i;
    return found;
}

static int x_of(const char *s, int from, int to) {
    char piece[1024];
    int n = to - from < 1023 ? to - from : 1023;
    for (int i = 0; i < n; ++i) piece[i] = s[from + i] == '\t' ? ' ' : s[from + i];
    piece[n] = 0;
    return n ? text_width(&font_regular, piece) + font_regular.spacing : 0;
}

static int at_x(const char *s, const struct wline *l, int x) {
    int at = l->start, cx = 0;
    while (at < l->end && s[at] != '\n') {
        char one[2] = {s[at] == '\t' ? ' ' : s[at], 0};
        int cw = text_width(&font_regular, one) + font_regular.spacing;
        if (cx + cw / 2 > x) break;
        cx += cw;
        ++at;
    }
    return at;
}

static void editor_paint(const struct canvas *c, struct editor *e, struct rect r, int focused) {
    fill(c, r.x, r.y, r.w, r.h, 0xffffff);
    e->line_count = wrap(e->s, e->len, r.w - 16, &e->lines, &e->line_cap);
    int lh = line_height(), cl = editor_line_of(e, e->caret);
    if (cl * lh < e->top) e->top = cl * lh;
    if ((cl + 1) * lh > e->top + r.h - 8) e->top = (cl + 1) * lh - r.h + 8;
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, r);
    int lo = e->caret < e->anchor ? e->caret : e->anchor, hi = e->caret < e->anchor ? e->anchor : e->caret;
    for (int i = 0; i < e->line_count; ++i) {
        int y = r.y + 6 + i * lh - e->top;
        if (y + lh < r.y || y > r.y + r.h) continue;
        const struct wline *l = &e->lines[i];
        int a = l->start > lo ? l->start : lo, b = l->end < hi ? l->end : hi;
        if (focused && a < b) fill(&inner, r.x + 8 + x_of(e->s, l->start, a), y - 2, x_of(e->s, a, b), lh, 0xb4d5fe);
        char piece[1024];
        int n = l->end - l->start < 1023 ? l->end - l->start : 1023;
        for (int k = 0; k < n; ++k) piece[k] = e->s[l->start + k] == '\t' || e->s[l->start + k] == '\n' ? ' ' : e->s[l->start + k];
        piece[n] = 0;
        text(&inner, r.x + 8, y, &font_regular, piece, 0x1f1f1f);
        if (focused && i == cl) fill(&inner, r.x + 8 + x_of(e->s, l->start, e->caret), y - 2, 1, lh, 0x000000);
    }
}

static int editor_pos(struct editor *e, struct rect r, int x, int y) {
    if (!e->line_count) return 0;
    int i = (y - r.y - 6 + e->top) / line_height();
    if (i < 0) i = 0;
    if (i >= e->line_count) i = e->line_count - 1;
    return at_x(e->s, &e->lines[i], x - r.x - 8);
}

static int editor_key(struct editor *e, int k, int shift) {
    int lo = e->caret < e->anchor ? e->caret : e->anchor, hi = e->caret < e->anchor ? e->anchor : e->caret;
    if (k == 0x104) { if (e->caret > 0) --e->caret; }
    else if (k == 0x103) { if (e->caret < e->len) ++e->caret; }
    else if (k == 0x101 || k == 0x102 || k == 0x109 || k == 0x10a) {
        int li = editor_line_of(e, e->caret), step = k == 0x101 ? -1 : k == 0x102 ? 1 : k == 0x109 ? -12 : 12, target = li + step;
        if (target < 0) target = 0;
        if (target >= e->line_count) target = e->line_count - 1;
        if (target >= 0) e->caret = at_x(e->s, &e->lines[target], x_of(e->s, e->lines[li].start, e->caret));
    } else if (k == 0x105) { if (e->line_count) e->caret = e->lines[editor_line_of(e, e->caret)].start; }
    else if (k == 0x106) { if (e->line_count) { const struct wline *l = &e->lines[editor_line_of(e, e->caret)]; e->caret = l->end > l->start && (l->end == e->len || e->s[l->end - 1] == ' ') && l->end < e->len ? l->end - 1 : l->end; } }
    else if (k == '\b') { if (lo != hi) editor_splice(e, lo, hi, "", 0); else if (e->caret > 0) editor_splice(e, e->caret - 1, e->caret, "", 0); return 1; }
    else if (k == 0x108) { if (lo != hi) editor_splice(e, lo, hi, "", 0); else if (e->caret < e->len) editor_splice(e, e->caret, e->caret + 1, "", 0); return 1; }
    else if (k == 1) { e->anchor = 0; e->caret = e->len; return 1; }
    else if (k == 3 || k == 24) {
        if (lo != hi) { gui_clipboard_set(e->s + lo, hi - lo); if (k == 24) editor_splice(e, lo, hi, "", 0); }
        return 1;
    } else if (k == 22) {
        static char buffer[16384];
        int n = gui_clipboard_get(buffer, sizeof(buffer) - 1);
        if (n > 0 && buffer[0] != 1) editor_insert(e, buffer, n);
        return 1;
    } else if (k == '\n' || k == '\t' || (k >= 32 && k < 256 && k != 127)) { char c = (char)k; editor_insert(e, &c, 1); return 1; }
    else return 0;
    if (!shift) e->anchor = e->caret;
    return 1;
}

/* ---- folders and messages ---- */

static const char *folder_name(void) { return folder_cur < folder_count ? folders[folder_cur].name : "INBOX"; }

static int matches(const struct message *m) {
    if (unread_only && m->seen) return 0;
    if (!search.text[0]) return 1;
    return strcasestr(m->subject, search.text) || strcasestr(m->from, search.text) || strcasestr(m->address, search.text);
}

static long day_number(time_t t) {
    struct tm *tm = localtime(&t);
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}

static void group_name(time_t when, char *out, size_t size) {
    static const char *const days[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
    long age = day_number(time(0)) - day_number(when);
    if (!when) snprintf(out, size, "OLDER");
    else if (age <= 0) snprintf(out, size, "TODAY");
    else if (age == 1) snprintf(out, size, "YESTERDAY");
    else if (age < 7) snprintf(out, size, "%s", days[localtime(&when)->tm_wday]);
    else if (age < 14) snprintf(out, size, "LAST WEEK");
    else if (age < 31) snprintf(out, size, "EARLIER THIS MONTH");
    else if (age < 62) snprintf(out, size, "LAST MONTH");
    else snprintf(out, size, "OLDER");
}

static void short_date(time_t when, char *out, size_t size) {
    if (!when) { out[0] = 0; return; }
    long age = day_number(time(0)) - day_number(when);
    struct tm *tm = localtime(&when);
    strftime(out, size, age <= 0 ? "%H:%M" : age < 7 ? "%a %H:%M" : "%d/%m/%Y", tm);
}

static void build_rows(void) {
    free(rows);
    rows = calloc((size_t)message_count * 2 + 1, sizeof(*rows));
    row_count = 0;
    char last[24] = "";
    for (int i = 0; rows && i < message_count; ++i) {
        if (!matches(&messages[i])) continue;
        char group[24];
        group_name(messages[i].when, group, sizeof(group));
        if (strcmp(group, last)) {
            rows[row_count] = (struct row){0, -1};
            snprintf(rows[row_count++].label, sizeof(rows[0].label), "%s", group);
            snprintf(last, sizeof(last), "%s", group);
        }
        rows[row_count++] = (struct row){1, i};
    }
}

static int rows_height(void) {
    int h = 0;
    for (int i = 0; i < row_count; ++i) h += rows[i].kind ? ITEM_H : GROUP_H;
    return h;
}

static void clear_reading(void) {
    free(raw.data);
    memset(&raw, 0, sizeof(raw));
    mime_free(&parsed);
    loaded_uid = 0;
    read_scroll.top = 0;
    read_width = 0;
}

static void show_error(const char *what) {
    set_status("%s", mbox.error[0] ? mbox.error : what);
    message_box("OpenMail", what, mbox.error);
}

static void refresh(int with_folders, int quiet) {
    if (busy || !configured) return;
    busy = 1;
    set_status("Connecting to %s...", mbox.account.in_host);
    char keep[160];
    snprintf(keep, sizeof(keep), "%s", folder_name());
    unsigned keep_uid = selected >= 0 && selected < message_count ? messages[selected].uid : 0;
    if (with_folders || !folder_count) {
        if (mail_folders(&mbox, &folders, &folder_count)) { busy = 0; if (quiet) set_status("%s", mbox.error); else show_error("OpenMail could not reach your mail server."); return; }
        folder_cur = 0;
        for (int i = 0; i < folder_count; ++i)
            if (!strcmp(folders[i].name, keep)) folder_cur = i;
    }
    set_status("Updating %s...", folder_count ? folders[folder_cur].label : "Inbox");
    if (mail_list(&mbox, folder_name(), 0, &messages, &message_count)) { busy = 0; if (quiet) set_status("%s", mbox.error); else show_error("OpenMail could not read this folder."); return; }
    selected = -1;
    for (int i = 0; i < message_count; ++i)
        if (keep_uid && messages[i].uid == keep_uid) selected = i;
    if (selected < 0) clear_reading();
    if (folder_cur < folder_count) {
        int unread = 0;
        for (int i = 0; i < message_count; ++i) unread += !messages[i].seen;
        folders[folder_cur].unread = unread;
    }
    build_rows();
    busy = 0;
    set_status("Connected to %s", mbox.account.in_host);
    char title[240];
    snprintf(title, sizeof(title), "%s - %s - OpenMail", folder_count ? folders[folder_cur].label : "Inbox", mbox.account.address);
    window_title(main_window, title);
    window_paint(main_window);
}

static void open_message(int index) {
    if (index < 0 || index >= message_count) return;
    selected = index;
    window_paint(main_window);
    if (busy || messages[index].uid == loaded_uid) return;
    clear_reading();
    busy = 1;
    set_status("Downloading the message...%s", "");
    struct message *m = &messages[index];
    if (mail_fetch(&mbox, folder_name(), m, &raw)) {
        busy = 0;
        set_status("%s", mbox.error);
        return;
    }
    mime_parse(raw.data ? raw.data : "", raw.size, &parsed);
    loaded_uid = m->uid;
    if (!m->seen && !mail_set_seen(&mbox, folder_name(), m, 1)) {
        m->seen = 1;
        if (folder_cur < folder_count && folders[folder_cur].unread > 0) --folders[folder_cur].unread;
    }
    busy = 0;
    set_status("Connected to %s", mbox.account.in_host);
    window_paint(main_window);
}

static void delete_selected(void) {
    if (busy || selected < 0 || selected >= message_count) return;
    const char *trash = 0;
    for (int i = 0; i < folder_count; ++i)
        if (folders[i].kind == FOLDER_TRASH) trash = folders[i].name;
    busy = 1;
    set_status("Deleting...%s", "");
    int failed = mail_delete(&mbox, folder_name(), &messages[selected], trash);
    busy = 0;
    if (failed) { show_error("The message could not be deleted."); return; }
    if (mbox.account.protocol == PROTOCOL_POP3) { clear_reading(); selected = -1; refresh(0, 1); return; }
    memmove(messages + selected, messages + selected + 1, sizeof(*messages) * (size_t)(message_count - selected - 1));
    --message_count;
    clear_reading();
    if (selected >= message_count) selected = message_count - 1;
    build_rows();
    set_status("Connected to %s", mbox.account.in_host);
    if (selected >= 0) open_message(selected);
}

static void toggle_seen(void) {
    if (busy || selected < 0) return;
    struct message *m = &messages[selected];
    busy = 1;
    if (!mail_set_seen(&mbox, folder_name(), m, !m->seen)) {
        m->seen = !m->seen;
        if (folder_cur < folder_count) folders[folder_cur].unread += m->seen ? -1 : 1;
    }
    busy = 0;
    window_paint(main_window);
}

/* ---- compose windows ---- */

static struct rect send_rect(void) { return (struct rect){10, RIBBON_H + 10, 64, 84}; }
static struct rect compose_field(struct window *w, int i) { return (struct rect){154, RIBBON_H + 10 + i * 30, w->r.w - 166, 24}; }
static int attach_y(void) { return RIBBON_H + 104; }
static struct rect body_rect(struct compose *c) {
    int top = attach_y() + (c->files[0] ? 28 : 4);
    return (struct rect){0, top, c->w->r.w, c->w->r.h - top};
}

static void compose_paint(struct window *w, const struct canvas *c) {
    cw = w->data;
    if (ribbon_paint(&cw->rb, w, c)) return;
    fill(c, 0, RIBBON_H, w->r.w, w->r.h - RIBBON_H, 0xffffff);
    struct rect s = send_rect();
    fill(c, s.x, s.y, s.w, s.h, 0xf7f7f7);
    frame_box(c, s.x, s.y, s.w, s.h, 0xababab);
    ow_icon(c, s.x + 16, s.y + 14, IC_SEND, 32, 0);
    text(c, s.x + (s.w - text_width(&font_regular, "Send")) / 2, s.y + 58, &font_regular, "Send", OW_TEXT);
    static const char *const labels[] = {"To...", "Cc...", "Subject"};
    struct field *fields[] = {&cw->to, &cw->cc, &cw->subject};
    for (int i = 0; i < 3; ++i) {
        struct rect r = compose_field(w, i);
        if (i < 2) {
            fill(c, 84, r.y, 62, r.h, 0xf7f7f7);
            frame_box(c, 84, r.y, 62, r.h, 0xababab);
        }
        text(c, i < 2 ? 84 + (62 - text_width(&font_regular, labels[i])) / 2 : 88, r.y + 7, &font_regular, labels[i], OW_TEXT);
        ow_field(c, fields[i], r, cw->focus == i, 0);
    }
    if (cw->files[0]) {
        int x = 154, y = attach_y();
        text(c, 84, y + 6, &font_regular, "Attached", OW_TEXT);
        char list[2048];
        snprintf(list, sizeof(list), "%s", cw->files);
        for (char *p = strtok(list, "\n"); p; p = strtok(0, "\n")) {
            const char *slash = strrchr(p, '\\');
            const char *name = slash ? slash + 1 : p;
            int width = text_width(&font_regular, name) + 44;
            if (x + width > w->r.w - 10) break;
            frame_box(c, x, y, width, 22, 0xc6c6c6);
            ow_icon(c, x + 4, y + 3, IC_ATTACH, 16, 0);
            text(c, x + 22, y + 6, &font_regular, name, OW_TEXT);
            text(c, x + width - 12, y + 6, &font_regular, "x", OW_FAINT);
            x += width + 6;
        }
    }
    struct rect b = body_rect(cw);
    fill(c, 0, b.y - 1, w->r.w, 1, OW_LINE);
    editor_paint(c, &cw->body, b, cw->focus == 3);
}

static void compose_send(void) {
    struct compose *c = cw;
    if (!configured) { message_box("OpenMail", "Set up your email account first.", "File > Info > Account Settings."); return; }
    if (busy) { message_box("OpenMail", "OpenMail is busy. Try again in a moment.", ""); return; }
    static char recipients[64][120];
    int count = address_list(c->to.text, recipients, 64);
    count += address_list(c->cc.text, recipients + count, 64 - count);
    if (!count) { message_box("OpenMail", "Type at least one address in To or Cc.", "Separate addresses with commas."); return; }
    struct text message = {0};
    mail_compose(&message, &mbox.account, c->to.text, c->cc.text, c->subject.text, c->body.s, c->files, c->in_reply_to, c->references);
    busy = 1;
    set_status("Sending to %s...", recipients[0]);
    char error[200];
    int failed = smtp_send(&mbox.account, message.data, message.size, recipients, count, error, sizeof(error));
    if (!failed && mbox.account.protocol == PROTOCOL_IMAP)
        for (int i = 0; i < folder_count; ++i)
            if (folders[i].kind == FOLDER_SENT) { set_status("Saving to %s...", folders[i].label); mail_append(&mbox, folders[i].name, message.data, message.size); break; }
    busy = 0;
    free(message.data);
    if (failed) { set_status("%s", error); message_box("OpenMail", "The message could not be sent.", error); return; }
    set_status("Message sent.%s", "");
    window_close(c->w);
}

static void attached(const char *path, void *data) {
    struct compose *c = data;
    if (!path) return;
    size_t used = strlen(c->files);
    snprintf(c->files + used, sizeof(c->files) - used, "%s\n", path);
    window_paint(c->w);
}

static void compose_command(int command, int choice) {
    struct compose *c = cw;
    if (command == M_ATTACH) file_dialog("Insert File", 0, 0, attached, c);
    else if (command == M_SEND) compose_send();
    else if (c->focus == 3) editor_key(&c->body, command == M_PASTE ? 22 : command == M_CUT ? 24 : 3, 0);
    else {
        struct field *f = c->focus == 0 ? &c->to : c->focus == 1 ? &c->cc : &c->subject;
        if (command == M_PASTE) {
            char buffer[260];
            int n = gui_clipboard_get(buffer, sizeof(buffer) - 1);
            for (int i = 0; i < n; ++i) if (buffer[i] >= 32) field_key(f, (unsigned char)buffer[i]);
        } else if (command == M_COPY || command == M_CUT) {
            gui_clipboard_set(f->text, (int)strlen(f->text));
            if (command == M_CUT) field_set(f, "");
        }
    }
    if (c->w) window_paint(c->w);
}

static void compose_mouse(struct window *w, int x, int y, int kind, int value) {
    cw = w->data;
    if (cw->rb.backstage) { ribbon_mouse(&cw->rb, w, x, y, kind); return; }
    if (kind == M_UP) cw->selecting = 0;
    if (!cw->selecting && ribbon_mouse(&cw->rb, w, x, y, kind)) return;
    struct rect b = body_rect(cw);
    if (kind == M_WHEEL && rect_has(b, x, y)) {
        cw->body.top -= value * 36;
        if (cw->body.top < 0) cw->body.top = 0;
        window_paint(w);
        return;
    }
    if (kind == M_DOWN || kind == M_DOUBLE) {
        if (rect_has(send_rect(), x, y)) { compose_send(); return; }
        for (int i = 0; i < 3; ++i)
            if (rect_has(compose_field(w, i), x, y) || (i < 2 && x >= 84 && x < 146 && y >= compose_field(w, i).y && y < compose_field(w, i).y + 24)) cw->focus = i;
        if (cw->files[0] && y >= attach_y() && y < attach_y() + 22) {
            int cx = 154;
            char list[2048], kept[2048] = "";
            snprintf(list, sizeof(list), "%s", cw->files);
            for (char *p = strtok(list, "\n"); p; p = strtok(0, "\n")) {
                const char *slash = strrchr(p, '\\');
                int width = text_width(&font_regular, slash ? slash + 1 : p) + 44;
                if (!(x >= cx + width - 16 && x < cx + width)) { strcat(kept, p); strcat(kept, "\n"); }
                cx += width + 6;
            }
            snprintf(cw->files, sizeof(cw->files), "%s", kept);
        }
        if (rect_has(b, x, y)) {
            cw->focus = 3;
            cw->body.caret = cw->body.anchor = editor_pos(&cw->body, b, x, y);
            cw->selecting = 1;
        }
    } else if (kind == M_MOVE && cw->selecting) cw->body.caret = editor_pos(&cw->body, b, x, y);
    else if (kind != M_UP) return;
    window_paint(w);
}

static void compose_key(struct window *w, int k) {
    cw = w->data;
    if (ribbon_key(&cw->rb, w, k)) return;
    int shift = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0) & 1;
    if (k == '\t' && cw->focus < 3) { cw->focus = shift ? (cw->focus + 3) % 4 : cw->focus + 1; window_paint(w); return; }
    if (cw->focus == 3) editor_key(&cw->body, k, shift);
    else {
        struct field *f = cw->focus == 0 ? &cw->to : cw->focus == 1 ? &cw->cc : &cw->subject;
        if (k == '\n') cw->focus = cw->focus == 2 ? 3 : cw->focus + 1;
        else if (k == 3 || k == 24 || k == 22) compose_command(k == 3 ? M_COPY : k == 24 ? M_CUT : M_PASTE, -1);
        else field_key(f, k);
    }
    window_paint(w);
}

static void compose_popup(struct window *w, int tag, int index) { cw = w->data; ribbon_popup(&cw->rb, w, tag, index); }

static void compose_closed(struct window *w) {
    struct compose *c = w->data;
    free(c->body.s);
    free(c->body.lines);
    c->w = 0;
    if (cw == c) cw = 0;
}

static int compose_closing(struct window *w) { return 1; }

static const struct app_ops compose_ops = {compose_paint, compose_mouse, compose_key, compose_closed, compose_popup, 0, 0, compose_closing};

static void open_compose(const char *to, const char *cc, const char *subject, const char *body, const struct message *about) {
    struct compose *c = calloc(1, sizeof(*c));
    if (!c) return;
    field_set(&c->to, to);
    field_set(&c->cc, cc);
    field_set(&c->subject, subject);
    editor_set(&c->body, body);
    c->focus = to[0] ? 3 : 0;
    if (about) {
        snprintf(c->in_reply_to, sizeof(c->in_reply_to), "%s", about->message_id);
        snprintf(c->references, sizeof(c->references), "%s%s%s", about->references, about->references[0] ? " " : "", about->message_id);
    }
    c->rb = (struct ribbon){compose_tabs, 1, 0, compose_command};
    c->rb.hover = -1;
    char title[300];
    snprintf(title, sizeof(title), "%s - Message", subject[0] ? subject : "Untitled");
    int width = gui_board()->screen_w - 160, height = gui_board()->screen_h - 160;
    if (width > 760) width = 760;
    if (height > 560) height = 560;
    c->w = window_open(width < 520 ? 520 : width, height < 380 ? 380 : height, title, ICON_EDITOR, &compose_ops, c);
    if (!c->w) { free(c->body.s); free(c); return; }
    c->w->resizable = 1;
    c->w->track_mouse = 1;
}

static void respond(int kind) {
    if (selected < 0 || selected >= message_count) return;
    if (messages[selected].uid != loaded_uid) open_message(selected);
    const struct message *m = &messages[selected];
    char subject[300], to[600] = "", cc[600] = "", sent[64];
    const char *prefix = kind == C_FORWARD ? "FW: " : "RE: ";
    const char *s = m->subject;
    int has = !strncasecmp(s, prefix, 4) || (kind != C_FORWARD && !strncasecmp(s, "Re:", 3)) || (kind == C_FORWARD && !strncasecmp(s, "Fwd:", 4));
    snprintf(subject, sizeof(subject), "%s%s", has ? "" : prefix, s);
    if (kind != C_FORWARD) snprintf(to, sizeof(to), "%s", m->address);
    if (kind == C_REPLY_ALL) {
        static char list[64][120];
        int n = address_list(m->to, list, 64);
        n += address_list(m->cc, list + n, 64 - n);
        for (int i = 0; i < n; ++i) {
            if (!strcasecmp(list[i], mbox.account.address) || !strcasecmp(list[i], m->address)) continue;
            size_t used = strlen(cc);
            snprintf(cc + used, sizeof(cc) - used, "%s%s", cc[0] ? ", " : "", list[i]);
        }
    }
    short_date(m->when, sent, sizeof(sent));
    struct text body = {0};
    text_printf(&body, "\n\n-----Original Message-----\nFrom: %s <%s>\nSent: %s\nTo: %s\n", m->from, m->address, sent, m->to);
    if (m->cc[0]) text_printf(&body, "Cc: %s\n", m->cc);
    text_printf(&body, "Subject: %s\n\n", m->subject);
    if (parsed.body.data) text_put(&body, parsed.body.data);
    open_compose(to, cc, subject, body.data ? body.data : "", kind == C_FORWARD ? 0 : m);
    free(body.data);
}

/* ---- account settings ---- */

struct settings { struct account a; struct field fields[8]; int focus; };
static const char *const setting_labels[8] = {"Your name", "Email address", "Incoming server", "Port", "User name", "Password", "Outgoing server", "Port"};
static const char *const security_names[3] = {"SSL/TLS", "STARTTLS", "None"};
enum { S_NAME, S_ADDRESS, S_IN_HOST, S_IN_PORT, S_USER, S_PASSWORD, S_OUT_HOST, S_OUT_PORT };

static struct rect setting_rect(int i) {
    static const int y[8] = {44, 72, 140, 168, 196, 224, 292, 320};
    return (struct rect){150, y[i], i == S_IN_PORT || i == S_OUT_PORT ? 70 : 270, 22};
}
static struct rect choice_rect(int i) {
    static const struct rect r[4] = {{150, 114, 120, 22}, {300, 168, 120, 22}, {300, 320, 120, 22}, {150, 352, 22, 22}};
    return r[i];
}
static const struct rect settings_save = {226, 392, 92, 26}, settings_cancel = {326, 392, 92, 26};

static void settings_paint(struct window *w, const struct canvas *c) {
    struct settings *s = w->data;
    fill(c, 0, 0, w->r.w, w->r.h, 0xffffff);
    fill(c, 0, 0, w->r.w, 4, app->accent);
    text(c, 20, 16, &font_bold, "Server settings for your email account", OW_TEXT);
    text(c, 20, 98, &font_bold, "Incoming mail", OW_TEXT);
    text(c, 20, 270, &font_bold, "Outgoing mail (SMTP)", OW_TEXT);
    for (int i = 0; i < 8; ++i) {
        struct rect r = setting_rect(i);
        text(c, 20, r.y + 6, &font_regular, setting_labels[i], OW_FAINT);
        ow_field(c, &s->fields[i], r, s->focus == i, i == S_PASSWORD);
    }
    const char *values[3] = {s->a.protocol == PROTOCOL_POP3 ? "POP3" : "IMAP", security_names[s->a.in_security], security_names[s->a.out_security]};
    static const char *const choice_labels[3] = {"Account type", "Security", "Security"};
    for (int i = 0; i < 3; ++i) {
        struct rect r = choice_rect(i);
        if (i) text(c, r.x - 58, r.y + 6, &font_regular, choice_labels[i], OW_FAINT);
        else text(c, 20, r.y + 6, &font_regular, choice_labels[i], OW_FAINT);
        fill(c, r.x, r.y, r.w, r.h, 0xffffff);
        frame_box(c, r.x, r.y, r.w, r.h, 0xababab);
        text(c, r.x + 6, r.y + 6, &font_regular, values[i], OW_TEXT);
        for (int k = 0; k < 3; ++k) fill(c, r.x + r.w - 12 + k, r.y + 9 + k, 5 - 2 * k, 1, OW_TEXT);
    }
    struct rect box = choice_rect(3);
    fill(c, box.x + 4, box.y + 4, 14, 14, 0xffffff);
    frame_box(c, box.x + 4, box.y + 4, 14, 14, 0x7a7a7a);
    if (s->a.out_auth) ow_icon(c, box.x + 3, box.y + 3, IC_MARK, 16, 0);
    text(c, box.x + 28, box.y + 7, &font_regular, "The outgoing server needs the same sign-in", OW_TEXT);
    if (!mail_tls) text(c, 20, 396, &font_regular, "Built without TLS.", 0xc00000);
    ow_button(c, settings_save, "Save", 1, 0);
    ow_button(c, settings_cancel, "Cancel", 0, 0);
}

static void settings_fill(struct settings *s) {
    const struct account *a = &s->a;
    char port[16];
    field_set(&s->fields[S_NAME], a->name);
    field_set(&s->fields[S_ADDRESS], a->address);
    field_set(&s->fields[S_IN_HOST], a->in_host);
    snprintf(port, sizeof(port), "%d", a->in_port);
    field_set(&s->fields[S_IN_PORT], port);
    field_set(&s->fields[S_USER], a->user);
    field_set(&s->fields[S_PASSWORD], a->password);
    field_set(&s->fields[S_OUT_HOST], a->out_host);
    snprintf(port, sizeof(port), "%d", a->out_port);
    field_set(&s->fields[S_OUT_PORT], port);
}

static void settings_read(struct settings *s) {
    struct account *a = &s->a;
    snprintf(a->name, sizeof(a->name), "%s", s->fields[S_NAME].text);
    snprintf(a->address, sizeof(a->address), "%s", s->fields[S_ADDRESS].text);
    snprintf(a->in_host, sizeof(a->in_host), "%s", s->fields[S_IN_HOST].text);
    a->in_port = atoi(s->fields[S_IN_PORT].text);
    snprintf(a->user, sizeof(a->user), "%s", s->fields[S_USER].text);
    snprintf(a->password, sizeof(a->password), "%s", s->fields[S_PASSWORD].text);
    snprintf(a->out_host, sizeof(a->out_host), "%s", s->fields[S_OUT_HOST].text);
    a->out_port = atoi(s->fields[S_OUT_PORT].text);
}

static void settings_guess(struct settings *s) {
    settings_read(s);
    account_guess(&s->a);
    settings_fill(s);
}

static void settings_done(struct window *w) {
    struct settings *s = w->data;
    settings_read(s);
    if (!strchr(s->a.address, '@') || !s->a.in_host[0] || !s->a.out_host[0] || s->a.in_port <= 0 || s->a.out_port <= 0) {
        message_box("Account Settings", "Fill in the email address, both servers and their ports.", "");
        return;
    }
    if (account_save(&s->a)) { message_box("Account Settings", "The settings could not be saved.", ""); return; }
    mail_disconnect(&mbox);
    mbox.account = s->a;
    configured = 1;
    free(folders);
    folders = 0;
    folder_count = folder_cur = 0;
    free(messages);
    messages = 0;
    message_count = 0;
    selected = -1;
    clear_reading();
    build_rows();
    window_close(w);
    refresh(1, 0);
}

static void settings_mouse(struct window *w, int x, int y, int kind, int value) {
    struct settings *s = w->data;
    if (kind != M_DOWN) return;
    int before = s->focus;
    for (int i = 0; i < 8; ++i)
        if (rect_has(setting_rect(i), x, y)) s->focus = i;
    if (before == S_ADDRESS && s->focus != S_ADDRESS) settings_guess(s);
    for (int i = 0; i < 3; ++i)
        if (rect_has(choice_rect(i), x, y)) window_popup(w, choice_rect(i).x, choice_rect(i).y + 22, i ? "SSL/TLS\nSTARTTLS\nNone\n" : "IMAP\nPOP3\n", i);
    if (rect_has(choice_rect(3), x, y)) s->a.out_auth = !s->a.out_auth;
    if (rect_has(settings_save, x, y)) { settings_done(w); return; }
    if (rect_has(settings_cancel, x, y)) { window_close(w); return; }
    window_paint(w);
}

static void settings_popup(struct window *w, int tag, int index) {
    struct settings *s = w->data;
    if (index < 0) return;
    settings_read(s);
    if (tag == 0) {
        int was = s->a.protocol;
        s->a.protocol = index;
        if (was != index) {
            static const int ports[2][3] = {{993, 143, 143}, {995, 110, 110}};
            s->a.in_port = ports[index][s->a.in_security];
            if (!strncasecmp(s->a.in_host, index ? "imap." : "pop.", index ? 5 : 4)) {
                char rest[120];
                snprintf(rest, sizeof(rest), "%s", strchr(s->a.in_host, '.') + 1);
                snprintf(s->a.in_host, sizeof(s->a.in_host), "%s.%s", index ? "pop" : "imap", rest);
            }
        }
    } else if (tag == 1) {
        s->a.in_security = index;
        s->a.in_port = s->a.protocol == PROTOCOL_POP3 ? (index ? 110 : 995) : (index ? 143 : 993);
    } else if (tag == 2) {
        s->a.out_security = index;
        s->a.out_port = index == SECURITY_SSL ? 465 : index == SECURITY_STARTTLS ? 587 : 25;
    }
    settings_fill(s);
    window_paint(w);
}

static void settings_key(struct window *w, int k) {
    struct settings *s = w->data;
    int shift = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0) & 1;
    if (k == 27) { window_close(w); return; }
    if (k == '\n') { settings_done(w); return; }
    if (k == '\t') {
        if (s->focus == S_ADDRESS && !shift) settings_guess(s);
        s->focus = (s->focus + (shift ? 7 : 1)) % 8;
    } else field_key(&s->fields[s->focus], k);
    window_paint(w);
}

static void settings_closed(struct window *w) { free(w->data); settings_window = 0; }

static const struct app_ops settings_ops = {settings_paint, settings_mouse, settings_key, settings_closed, settings_popup};

static void open_settings(void) {
    if (settings_window) return;
    struct settings *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->a = mbox.account;
    if (!s->a.in_port) s->a.in_port = 993;
    if (!s->a.out_port) s->a.out_port = 465;
    s->focus = s->a.address[0] ? S_PASSWORD : S_NAME;
    settings_fill(s);
    settings_window = window_open(440, 430, "Account Settings", ICON_GEAR, &settings_ops, s);
    if (!settings_window) { free(s); return; }
    window_modal(settings_window);
}

/* ---- the main window ---- */

static struct rect folder_rect(struct window *w) { return (struct rect){0, RIBBON_H, FOLDER_W, w->r.h - RIBBON_H - STATUS_H}; }
static int list_width(struct window *w) { int lw = (w->r.w - FOLDER_W) * 42 / 100; return lw < 240 ? 240 : lw > LIST_W ? LIST_W : lw; }
static struct rect list_rect(struct window *w) { return (struct rect){FOLDER_W, RIBBON_H + LIST_TOP, list_width(w) - SCROLL_W, w->r.h - RIBBON_H - LIST_TOP - STATUS_H}; }
static struct rect list_scroll_rect(struct window *w) { struct rect l = list_rect(w); return (struct rect){l.x + l.w, l.y, SCROLL_W, l.h}; }
static struct rect search_rect(struct window *w) { return (struct rect){FOLDER_W + 10, RIBBON_H + 8, list_width(w) - 20, 22}; }
static struct rect read_rect(struct window *w) {
    int x = FOLDER_W + list_width(w) + 1;
    return (struct rect){x, RIBBON_H, w->r.w - x, w->r.h - RIBBON_H - STATUS_H};
}
static int header_height(void) { return parsed.attachment_count ? 132 : 104; }
static struct rect body_area(struct window *w) {
    struct rect r = read_rect(w);
    return (struct rect){r.x + 16, r.y + header_height(), r.w - 16 - SCROLL_W, r.h - header_height()};
}

static int folder_icon(int kind) {
    switch (kind) {
    case FOLDER_INBOX: return IC_INBOX;
    case FOLDER_SENT: return IC_SENT;
    case FOLDER_DRAFTS: return IC_DRAFTS;
    case FOLDER_TRASH: return IC_TRASH;
    case FOLDER_JUNK: return IC_CANCEL;
    default: return IC_FOLDER;
    }
}

static void paint_folders(struct window *w, const struct canvas *c) {
    struct rect r = folder_rect(w);
    fill(c, r.x, r.y, r.w, r.h, 0xffffff);
    fill(c, r.x + r.w - 1, r.y, 1, r.h, OW_BORDER);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, r);
    int y = r.y + 12;
    if (!configured) {
        text(&inner, 14, y, &font_regular, "No account yet.", OW_FAINT);
        text(&inner, 14, y + 18, &font_regular, "Set one up in", OW_FAINT);
        text(&inner, 14, y + 32, &font_regular, "File > Account Settings.", OW_FAINT);
        return;
    }
    ow_text_fit(&inner, 12, y, r.w - 20, &font_bold, mbox.account.address, OW_TEXT);
    y += 22;
    for (int i = 0; i < folder_count; ++i, y += FOLDER_H) {
        if (i == folder_cur) fill(&inner, 0, y, r.w - 1, FOLDER_H, tint(78));
        ow_icon(&inner, 20, y + 5, folder_icon(folders[i].kind), 16, 0);
        char count[16] = "";
        if (folders[i].unread > 0) snprintf(count, sizeof(count), "%d", folders[i].unread);
        int cwidth = count[0] ? text_width(&font_bold, count) + 8 : 0;
        ow_text_fit(&inner, 42, y + 8, r.w - 52 - cwidth, folders[i].unread ? &font_bold : &font_regular, folders[i].label, OW_TEXT);
        if (count[0]) text(&inner, r.w - 10 - text_width(&font_bold, count), y + 8, &font_bold, count, ink());
    }
}

static void paint_list(struct window *w, const struct canvas *c) {
    int x0 = FOLDER_W, width = list_width(w);
    fill(c, x0, RIBBON_H, width, w->r.h - RIBBON_H - STATUS_H, 0xffffff);
    fill(c, x0 + width, RIBBON_H, 1, w->r.h - RIBBON_H - STATUS_H, OW_BORDER);
    struct rect sr = search_rect(w);
    ow_field(c, &search, sr, search_focus, 0);
    if (!search.text[0] && !search_focus) text(c, sr.x + 6, sr.y + 6, &font_regular, "Search Current Mailbox", OW_FAINT);
    ow_icon(c, sr.x + sr.w - 20, sr.y + 3, IC_FIND, 16, 0x8a8a8a);
    text(c, x0 + 12, RIBBON_H + 38, unread_only ? &font_regular : &font_bold, "All", unread_only ? OW_TEXT : ink());
    text(c, x0 + 40, RIBBON_H + 38, unread_only ? &font_bold : &font_regular, "Unread", unread_only ? ink() : OW_TEXT);
    text(c, x0 + width - 82, RIBBON_H + 38, &font_regular, "Newest first", OW_FAINT);
    struct rect l = list_rect(w);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, l);
    list_scroll.total = rows_height();
    list_scroll.visible = l.h;
    vscroll_clamp(&list_scroll);
    int y = l.y - list_scroll.top;
    if (!row_count) text(&inner, l.x + 14, l.y + 14, &font_regular, !configured ? "" : busy ? "Loading..." : "We didn't find anything to show here.", OW_FAINT);
    for (int i = 0; i < row_count; ++i) {
        if (!rows[i].kind) {
            if (y + GROUP_H > l.y && y < l.y + l.h) {
                fill(&inner, l.x, y, l.w, GROUP_H, 0xe9e9e9);
                for (int k = 0; k < 4; ++k) fill(&inner, l.x + 10 + k, y + 13 - k, 7 - 2 * k, 1, 0x7a7a7a);
                text(&inner, l.x + 24, y + 6, &font_bold, rows[i].label, 0x555555);
            }
            y += GROUP_H;
            continue;
        }
        const struct message *m = &messages[rows[i].index];
        if (y + ITEM_H > l.y && y < l.y + l.h) {
            if (rows[i].index == selected) fill(&inner, l.x, y, l.w, ITEM_H, tint(75));
            if (!m->seen) fill(&inner, l.x, y + 2, 3, ITEM_H - 4, app->accent);
            char date[32];
            short_date(m->when, date, sizeof(date));
            int dw = text_width(&font_regular, date);
            ow_text_fit(&inner, l.x + 14, y + 8, l.w - 40, m->seen ? &font_regular : &font_bold, m->from, 0x262626);
            if (m->attachment) ow_icon(&inner, l.x + l.w - 22, y + 5, IC_ATTACH, 16, 0x7a7a7a);
            ow_text_fit(&inner, l.x + 14, y + 24, l.w - 30 - dw, m->seen ? &font_regular : &font_bold, m->subject[0] ? m->subject : "(no subject)", m->seen ? OW_TEXT : ink());
            text(&inner, l.x + l.w - 8 - dw, y + 24, &font_regular, date, m->seen ? OW_FAINT : ink());
            if (m->answered) text(&inner, l.x + 14, y + 37, &font_regular, "Replied", OW_FAINT);
            fill(&inner, l.x + 10, y + ITEM_H - 1, l.w - 20, 1, 0xededed);
        }
        y += ITEM_H;
    }
    ow_vscroll_paint(c, &list_scroll, list_scroll_rect(w));
}

static void paint_reading(struct window *w, const struct canvas *c) {
    struct rect r = read_rect(w);
    fill(c, r.x, r.y, r.w, r.h, 0xffffff);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, r);
    if (selected < 0 || selected >= message_count) {
        const char *hint = configured ? "Select an item to read." : "Welcome to OpenMail.";
        text(&inner, r.x + (r.w - text_width(&font_regular, hint)) / 2, r.y + r.h / 2 - 5, &font_regular, hint, OW_FAINT);
        return;
    }
    const struct message *m = &messages[selected];
    int x = r.x + 16, y = r.y + 14, right = r.x + r.w - 16;
    char date[64] = "";
    if (m->when) strftime(date, sizeof(date), "%a %d/%m/%Y %H:%M", localtime(&m->when));
    text(&inner, right - text_width(&font_regular, date), y + 4, &font_regular, date, OW_FAINT);
    struct canvas name = inner;
    name.clip = rect_both(inner.clip, (struct rect){x, y, right - x - text_width(&font_regular, date) - 12, 24});
    ow_text_scaled(&name, x, y, &font_regular, m->from, 0x262626, 2);
    y += 28;
    ow_text_fit(&inner, x, y, right - x, &font_regular, m->address, OW_FAINT);
    y += 18;
    char line[400];
    snprintf(line, sizeof(line), "To: %s", m->to);
    ow_text_fit(&inner, x, y, right - x, &font_regular, line, OW_TEXT);
    y += 16;
    if (m->cc[0]) { snprintf(line, sizeof(line), "Cc: %s", m->cc); ow_text_fit(&inner, x, y, right - x, &font_regular, line, OW_TEXT); }
    y += 16;
    ow_text_fit(&inner, x, y, right - x, &font_bold, m->subject[0] ? m->subject : "(no subject)", 0x262626);
    y += 18;
    if (parsed.attachment_count) {
        int ax = x;
        for (int i = 0; i < parsed.attachment_count; ++i) {
            char label[180];
            size_t bytes = parsed.attachments[i].encoding == 1 ? parsed.attachments[i].length * 3 / 4 : parsed.attachments[i].length;
            snprintf(label, sizeof(label), "%s (%lu KB)", parsed.attachments[i].name, (unsigned long)(bytes + 1023) / 1024);
            int aw = text_width(&font_regular, label) + 30;
            if (ax + aw > right) break;
            fill(&inner, ax, y + 2, aw, 22, 0xf7f7f7);
            frame_box(&inner, ax, y + 2, aw, 22, 0xc6c6c6);
            ow_icon(&inner, ax + 5, y + 5, IC_ATTACH, 16, 0);
            text(&inner, ax + 24, y + 8, &font_regular, label, OW_TEXT);
            ax += aw + 6;
        }
    }
    fill(&inner, x, r.y + header_height() - 8, right - x, 1, OW_LINE);
    struct rect b = body_area(w);
    if (!loaded_uid) { text(&inner, b.x, b.y + 4, &font_regular, busy ? "Downloading..." : "", OW_FAINT); return; }
    if (read_width != b.w) {
        read_line_count = wrap(parsed.body.data, (int)parsed.body.size, b.w - 8, &read_lines, &read_line_cap);
        read_width = b.w;
    }
    read_scroll.total = read_line_count * line_height() + 12;
    read_scroll.visible = b.h;
    vscroll_clamp(&read_scroll);
    struct canvas body = *c;
    body.clip = rect_both(c->clip, b);
    for (int i = 0; i < read_line_count; ++i) {
        int ly = b.y + i * line_height() - read_scroll.top;
        if (ly + line_height() < b.y || ly > b.y + b.h) continue;
        char piece[1024];
        int n = read_lines[i].end - read_lines[i].start;
        if (n > 1023) n = 1023;
        for (int k = 0; k < n; ++k) {
            char ch = parsed.body.data[read_lines[i].start + k];
            piece[k] = ch == '\t' || ch == '\n' ? ' ' : ch;
        }
        piece[n] = 0;
        text(&body, b.x, ly, &font_regular, piece, 0x1f1f1f);
    }
    ow_vscroll_paint(c, &read_scroll, (struct rect){b.x + b.w, b.y, SCROLL_W, b.h});
}

static void paint(struct window *w, const struct canvas *c) {
    if (ribbon_paint(&rb, w, c)) return;
    paint_folders(w, c);
    paint_list(w, c);
    paint_reading(w, c);
    char left[64];
    int unread = 0;
    for (int i = 0; i < message_count; ++i) unread += !messages[i].seen;
    if (unread) snprintf(left, sizeof(left), "ITEMS: %d     UNREAD: %d", message_count, unread);
    else snprintf(left, sizeof(left), "ITEMS: %d", message_count);
    ow_status(c, w->r.h - STATUS_H, w->r.w, left, status_text);
}

/* ---- ribbon and input ---- */

static int state(int command) {
    int has = selected >= 0 && selected < message_count;
    switch (command) {
    case C_DELETE: case C_REPLY: case C_REPLY_ALL: case C_FORWARD: case C_UNREAD: return has && !busy ? 0 : RB_OFF;
    case C_SEND_RECEIVE: return configured && !busy ? 0 : RB_OFF;
    }
    return 0;
}

static void command(int command, int choice) {
    switch (command) {
    case C_NEW: open_compose("", "", "", "", 0); break;
    case C_DELETE: delete_selected(); break;
    case C_REPLY: case C_REPLY_ALL: case C_FORWARD: respond(command); break;
    case C_UNREAD: toggle_seen(); break;
    case C_SEND_RECEIVE: refresh(1, 0); break;
    case C_ACCOUNT: open_settings(); break;
    }
    window_paint(main_window);
}

static void popup(struct window *w, int tag, int index) { ribbon_popup(&rb, w, tag, index); }

static int row_at(struct window *w, int y) {
    struct rect l = list_rect(w);
    int top = l.y - list_scroll.top;
    for (int i = 0; i < row_count; ++i) {
        int h = rows[i].kind ? ITEM_H : GROUP_H;
        if (y >= top && y < top + h) return i;
        top += h;
    }
    return -1;
}

static void attachment_saved(const char *path, void *data) {
    int index = (int)(intptr_t)data;
    if (!path || index >= parsed.attachment_count) return;
    char target[300];
    const char *ext = extension_of(parsed.attachments[index].name);
    snprintf(target, sizeof(target), "%s%s%s", path, extension_of(path)[0] || !ext[0] ? "" : ".", extension_of(path)[0] ? "" : ext);
    struct text bytes = {0};
    attachment_decode(raw.data, &parsed.attachments[index], &bytes);
    if (write_whole(target, bytes.data ? bytes.data : "", bytes.size)) message_box("OpenMail", "The attachment could not be saved.", target);
    else set_status("Saved %s", target);
    free(bytes.data);
}

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (rb.backstage) { ribbon_mouse(&rb, w, x, y, kind); return; }
    struct rect lsr = list_scroll_rect(w), b = body_area(w), bsr = {b.x + b.w, b.y, SCROLL_W, b.h};
    if (ow_vscroll_mouse(&list_scroll, lsr, x, y, kind) || ow_vscroll_mouse(&read_scroll, bsr, x, y, kind)) { window_paint(w); return; }
    if (ribbon_mouse(&rb, w, x, y, kind)) return;
    if (kind == M_WHEEL) {
        if (rect_has(list_rect(w), x, y)) list_scroll.top -= value * ITEM_H;
        else if (rect_has(read_rect(w), x, y)) read_scroll.top -= value * line_height() * 3;
        window_paint(w);
        return;
    }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    search_focus = rect_has(search_rect(w), x, y);
    if (rect_has(folder_rect(w), x, y) && configured) {
        int i = (y - RIBBON_H - 34) / FOLDER_H;
        if (i >= 0 && i < folder_count && i != folder_cur && !busy) {
            folder_cur = i;
            selected = -1;
            clear_reading();
            list_scroll.top = 0;
            free(messages);
            messages = 0;
            message_count = 0;
            build_rows();
            refresh(0, 0);
        }
    } else if (y >= RIBBON_H + 34 && y < RIBBON_H + LIST_TOP && x >= FOLDER_W && x < FOLDER_W + 90) {
        unread_only = x >= FOLDER_W + 36;
        build_rows();
    } else if (rect_has(list_rect(w), x, y)) {
        int r = row_at(w, y);
        if (r >= 0 && rows[r].kind) {
            if (kind == M_DOUBLE) { selected = rows[r].index; respond(C_REPLY); return; }
            open_message(rows[r].index);
        }
    } else if (rect_has(read_rect(w), x, y) && parsed.attachment_count && y >= read_rect(w).y + 104 && y < read_rect(w).y + 128) {
        int ax = read_rect(w).x + 16;
        for (int i = 0; i < parsed.attachment_count; ++i) {
            char label[180];
            size_t bytes = parsed.attachments[i].encoding == 1 ? parsed.attachments[i].length * 3 / 4 : parsed.attachments[i].length;
            snprintf(label, sizeof(label), "%s (%lu KB)", parsed.attachments[i].name, (unsigned long)(bytes + 1023) / 1024);
            int aw = text_width(&font_regular, label) + 30;
            if (x >= ax && x < ax + aw) {
                char title[200];
                snprintf(title, sizeof(title), "Save %s as", parsed.attachments[i].name);
                file_dialog(title, 1, 0, attachment_saved, (void *)(intptr_t)i);
                break;
            }
            ax += aw + 6;
        }
    }
    window_paint(w);
}

static void key(struct window *w, int k) {
    if (ribbon_key(&rb, w, k)) return;
    if (search_focus) {
        if (k == 27) { field_set(&search, ""); search_focus = 0; }
        else if (k != '\n') field_key(&search, k);
        build_rows();
        window_paint(w);
        return;
    }
    int at = -1;
    for (int i = 0; i < row_count; ++i)
        if (rows[i].kind && rows[i].index == selected) at = i;
    if (k == 0x101 || k == 0x102) {
        int step = k == 0x101 ? -1 : 1, i = at < 0 ? -1 : at;
        do i += step; while (i >= 0 && i < row_count && !rows[i].kind);
        if (i >= 0 && i < row_count) open_message(rows[i].index);
    } else if (k == 0x108) delete_selected();
    else if (k == 14) command(C_NEW, -1);
    else if (k == 18) command(C_REPLY, -1);
    else if (k == 6) { search_focus = 1; }
    else if (k == 0x10f) refresh(1, 0);
    else if (k == 0x10b) suite_about();
    else if (k == 0x109) read_scroll.top -= body_area(w).h - 20;
    else if (k == 0x10a || k == ' ') read_scroll.top += body_area(w).h - 20;
    else if ((k == 1 || k == 3) && parsed.body.data) gui_clipboard_set(parsed.body.data, (int)parsed.body.size);
    else return;
    window_paint(w);
}

static void tick(struct window *w) {
    if (++ticks >= REFRESH_TICKS) { ticks = 0; refresh(0, 1); }
}

static int closing(struct window *w) {
    if (busy) return 0;
    mail_disconnect(&mbox);
    return 1;
}

static void resized(struct window *w) { read_width = 0; }

static const struct app_ops ops = {paint, mouse, key, 0, popup, tick, resized, closing};

static void info(char *out, size_t size) {
    const struct account *a = &mbox.account;
    if (!configured) { snprintf(out, size, "Account\tNot set up\n"); return; }
    snprintf(out, size, "Name\t%s\nAddress\t%s\nIncoming\t%s %s:%d (%s)\nOutgoing\tSMTP %s:%d (%s)\nFolders\t%d\nMessages\t%d in %s\n",
             a->name, a->address, a->protocol == PROTOCOL_POP3 ? "POP3" : "IMAP", a->in_host, a->in_port, security_names[a->in_security],
             a->out_host, a->out_port, security_names[a->out_security], folder_count, message_count, folder_count ? folders[folder_cur].label : "Inbox");
}

static void nothing(void) {}

static const struct suite_app mail_app = {
    "OpenMail", "MAIL", "Read and send email with IMAP, POP3 and SMTP.",
    "Inbox", "message", "Messages", "",
    0xf0b400, 0x262626, 0,
    nothing, 0, nothing, nothing, info, open_settings,
};

int main(int argc, char **argv) {
    if (gui_connect()) { puts("OpenMail needs the DOS-XC desktop."); return 1; }
    suite_start(&mail_app, argv[0]);
    snprintf(mail_ca_file, sizeof(mail_ca_file), "%s", argv[0]);
    char *slash = strrchr(mail_ca_file, '\\');
    if (slash) strcpy(slash + 1, "CACERT.PEM");
    mail_idle = pump;
    rb = (struct ribbon){tabs, 2, 0, command, state};
    rb.hover = -1;
    configured = !account_load(&mbox.account);
    int width = gui_board()->screen_w - 60, height = gui_board()->screen_h - 100;
    if (width > 1100) width = 1100;
    if (height > 700) height = 700;
    char title[200];
    snprintf(title, sizeof(title), "Inbox - %s - OpenMail", configured ? mbox.account.address : "no account");
    main_window = window_open(width < 720 ? 720 : width, height < 440 ? 440 : height, title, ICON_NET, &ops, 0);
    if (!main_window) return 1;
    main_window->resizable = 1;
    main_window->track_mouse = 1;
    if (configured) {
        set_status("Connecting...%s", "");
        refresh(1, 0);
    } else open_settings();
    if (argc > 1 && argv[1][0] && !strncasecmp(argv[1], "mailto:", 7)) open_compose(argv[1] + 7, "", "", "", 0);
    gui_run();
    return 0;
}
