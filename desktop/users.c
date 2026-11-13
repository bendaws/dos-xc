/*
 * DOS-XC /desktop/users.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <strings.h>

/* ---- choosing a profile picture ----------------------------------------- */

static const char *const pictures[] = {"Dog", "Flower", "Duck", "Football", "Beach", "Chess", "Karate", "Shuttle"};
#define PICTURES ((int)(sizeof(pictures) / sizeof(*pictures)))
#define THUMB 64

struct chooser {
    struct surface thumbs[PICTURES];
    int selected;
    void (*done)(const char *path, void *data);
    void *data;
};
static const struct rect ch_ok = {196, 186, 76, 24}, ch_cancel = {280, 186, 76, 24}, ch_browse = {12, 186, 96, 24};

static void picture_path(int i, char *out, size_t size) {
    char relative[64];
    snprintf(relative, sizeof(relative), "Pictures\\%s.jpg", pictures[i]);
    desktop_path(out, size, relative);
}

static struct rect thumb_rect(int i) { return (struct rect){12 + i % 5 * 70, 10 + i / 5 * 82, THUMB + 4, THUMB + 4}; }

static void ch_paint(struct window *w, const struct canvas *c) {
    struct chooser *ch = w->data;
    for (int i = 0; i < PICTURES; ++i) {
        struct rect r = thumb_rect(i);
        fill(c, r.x, r.y, r.w, r.h, i == ch->selected ? C_SELECTION : C_WINDOW);
        if (ch->thumbs[i].px) blit(c, r.x + 2, r.y + 2, &ch->thumbs[i]);
        text(c, r.x + 2, r.y + r.h + 2, &font_regular, pictures[i], C_TEXT);
    }
    button(c, ch_browse, "Browse...", 0, 0);
    button(c, ch_ok, "OK", 0, 1);
    button(c, ch_cancel, "Cancel", 0, 0);
}

static void ch_finish(struct window *w, const char *path) {
    struct chooser *ch = w->data;
    void (*done)(const char *, void *) = ch->done;
    void *data = ch->data;
    ch->done = 0;
    window_close(w);
    if (done && path) done(path, data);
}

static void browsed_picture(const char *path, void *data) {
    struct chooser *ch = data;
    if (path && ch->done) ch->done(path, ch->data);
    free(ch);
}

static void ch_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    struct chooser *ch = w->data;
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    for (int i = 0; i < PICTURES; ++i)
        if (rect_has(thumb_rect(i), x, y)) {
            ch->selected = i;
            window_paint(w);
            if (kind != M_DOUBLE) return;
        }
    if (rect_has(ch_ok, x, y) || kind == M_DOUBLE) {
        char path[260];
        picture_path(ch->selected, path, sizeof(path));
        ch_finish(w, path);
    } else if (rect_has(ch_cancel, x, y)) {
        ch_finish(w, 0);
    } else if (rect_has(ch_browse, x, y)) {
        struct chooser *keep = malloc(sizeof(*keep));
        if (!keep) return;
        keep->done = ch->done;
        keep->data = ch->data;
        ch->done = 0;
        window_close(w);
        file_dialog("Profile Picture", 0, 0, browsed_picture, keep);
    }
}

static void ch_key(struct window *w, int key) {
    struct chooser *ch = w->data;
    if (key == 27) ch_finish(w, 0);
    else if (key == '\n') { char path[260]; picture_path(ch->selected, path, sizeof(path)); ch_finish(w, path); }
    else if (key == '\t' || key == 0x103) { ch->selected = (ch->selected + 1) % PICTURES; window_paint(w); }
    else if (key == 0x104) { ch->selected = (ch->selected + PICTURES - 1) % PICTURES; window_paint(w); }
}

static void ch_closed(struct window *w) {
    struct chooser *ch = w->data;
    for (int i = 0; i < PICTURES; ++i) free(ch->thumbs[i].px);
    free(ch);
}
static const struct app_ops ch_ops = {ch_paint, ch_mouse, ch_key, ch_closed};

void picture_chooser(void (*done)(const char *path, void *data), void *data) {
    struct chooser *ch = calloc(1, sizeof(*ch));
    if (!ch) return;
    ch->done = done;
    ch->data = data;
    for (int i = 0; i < PICTURES; ++i) {
        char path[260];
        picture_path(i, path, sizeof(path));
        user_picture(path, THUMB, &ch->thumbs[i]);
    }
    struct window *w = window_open(368, 220, "Choose a Picture", ICON_PICTURE, &ch_ops, ch);
    if (!w) ch_closed(&(struct window){.data = ch});
    else window_modal(w);
}

/* ---- adding a user ------------------------------------------------------ */

struct adding { struct field fields[4]; int focus, admin; char picture[160], error[80]; };
static const char *const add_labels[4] = {"User name:", "Nickname:", "Password:", "Again:"};
static struct rect add_field(int i) { return (struct rect){100, 12 + i * 30, 220, 22}; }
static const struct rect add_admin = {100, 136, 220, 18}, add_ok = {160, 168, 76, 24}, add_cancel = {244, 168, 76, 24};
static struct window *panel;
static void panel_reload(void);

static void add_paint(struct window *w, const struct canvas *c) {
    struct adding *a = w->data;
    for (int i = 0; i < 4; ++i) {
        text(c, 12, add_field(i).y + 5, &font_regular, add_labels[i], C_TEXT);
        struct field shown = a->fields[i];
        if (i >= 2) for (size_t k = 0; shown.text[k]; ++k) shown.text[k] = '*';
        field_paint(c, &shown, add_field(i), a->focus == i);
    }
    bevel(c, add_admin.x, add_admin.y + 2, 14, 14, 1);
    fill(c, add_admin.x + 2, add_admin.y + 4, 10, 10, C_BASE);
    if (a->admin) fill(c, add_admin.x + 4, add_admin.y + 6, 6, 6, C_TEXT);
    text(c, add_admin.x + 20, add_admin.y + 4, &font_regular, "Administrator", C_TEXT);
    if (a->error[0]) text(c, 12, 158, &font_regular, a->error, 0xb00000);
    button(c, add_ok, "Add", 0, 1);
    button(c, add_cancel, "Cancel", 0, 0);
}

static void add_accept(struct window *w) {
    struct adding *a = w->data;
    if (strcmp(a->fields[2].text, a->fields[3].text)) { snprintf(a->error, sizeof(a->error), "The passwords are not the same."); window_paint(w); return; }
    struct dosxc_user_change c;
    memset(&c, 0, sizeof(c));
    snprintf(c.user.name, sizeof(c.user.name), "%s", a->fields[0].text);
    snprintf(c.user.nickname, sizeof(c.user.nickname), "%s", a->fields[1].text);
    snprintf(c.password, sizeof(c.password), "%s", a->fields[2].text);
    snprintf(c.user.picture, sizeof(c.user.picture), "%s", a->picture);
    c.user.admin = a->admin;
    long r = dosxc_sys_user(DOSXC_USER_ADD, 0, &c);
    memset(&c, 0, sizeof(c));
    if (r) {
        snprintf(a->error, sizeof(a->error), r == -DOSXC_EINVAL ? "Use letters and digits, a name not taken." : "The account could not be added.");
        window_paint(w);
        return;
    }
    window_close(w);
    panel_reload();
}

static void add_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    struct adding *a = w->data;
    if (kind != M_DOWN) return;
    for (int i = 0; i < 4; ++i)
        if (rect_has(add_field(i), x, y)) { a->focus = i; window_paint(w); return; }
    if (rect_has(add_admin, x, y)) { a->admin = !a->admin; window_paint(w); }
    else if (rect_has(add_ok, x, y)) add_accept(w);
    else if (rect_has(add_cancel, x, y)) window_close(w);
}

static void add_key(struct window *w, int key) {
    struct adding *a = w->data;
    if (key == '\t') { a->focus = (a->focus + 1) % 4; window_paint(w); }
    else if (key == '\n') add_accept(w);
    else if (key == 27) window_close(w);
    else if (field_key(&a->fields[a->focus], key)) window_paint(w);
}

static void add_closed(struct window *w) { memset(w->data, 0, sizeof(struct adding)); free(w->data); }
static const struct app_ops add_ops = {add_paint, add_mouse, add_key, add_closed};

static void open_add_user(int first) {
    struct adding *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->admin = first;
    picture_path(0, a->picture, sizeof(a->picture));
    struct window *w = window_open(332, 202, first ? "Add the First Administrator" : "Add a User", ICON_USER, &add_ops, a);
    if (!w) free(a);
    else window_modal(w);
}

/* ---- the panel ----------------------------------------------------------- */

#define ROW 22
struct panel {
    struct dosxc_user me, users[32];
    int count, selected, enabled, admin, unlocked;
    char unlocked_by[32];
    struct surface picture, thumbs[32];
};
static struct panel *pd;
static const struct rect list_box = {12, 140, 330, 198};
static const struct rect lock_button = {360, 104, 108, 24};
/* Managing accounts: unlocked by an administrator, or anyone while accounts are off. */
static int managing(void) { return pd->unlocked || !pd->enabled; }

static void panel_reload(void) {
    if (!pd) return;
    pd->enabled = dosxc_sys_user(DOSXC_USER_ENABLED, 0, 0) > 0;
    dosxc_sys_user(DOSXC_USER_WHOAMI, 0, &pd->me);
    pd->admin = pd->me.admin;
    if (!pd->enabled) pd->unlocked = 0;
    long n = dosxc_sys_user(DOSXC_USER_LIST, 0, &pd->users[0]);
    pd->count = 0;
    for (long i = 0; i < n && i < 32; ++i) dosxc_sys_user(DOSXC_USER_LIST, (void *)i, &pd->users[pd->count++]);
    if (pd->selected >= pd->count) pd->selected = pd->count - 1;
    free(pd->picture.px);
    pd->picture = (struct surface){0};
    user_picture(pd->me.picture, 48, &pd->picture);
    for (int i = 0; i < 32; ++i) {
        free(pd->thumbs[i].px);
        pd->thumbs[i] = (struct surface){0};
        if (i < pd->count) user_picture(pd->users[i].picture, ROW - 4, &pd->thumbs[i]);
    }
    if (panel) window_paint(panel);
}

static const char *const own_buttons[4] = {"Picture...", "Nickname...", "Password...", "Start in"};
static const char *const admin_buttons[5] = {"Add...", "Remove", "Administrator", "Home Folder...", "Picture..."};
static struct rect own_button(int i) { return (struct rect){12 + i * 76, 66, 72, 24}; }
static struct rect admin_button(int i) { return (struct rect){360, 140 + i * 29, 108, 24}; }

static void draw_lock(const struct canvas *c, int x, int y, int open) {
    u32 ink = open ? 0x2f7d32 : 0x8a6d00;
    fill(c, x + 2, y + 8, 12, 9, ink);
    for (int i = 0; i < 2; ++i) {
        fill(c, x + 4 + i, y + 2, 1, open ? 3 : 6, ink);
        fill(c, x + 10 + i + (open ? 4 : 0), y + 2, 1, 6, ink);
    }
    fill(c, x + 4, y + 1, open ? 12 : 8, 2, ink);
    fill(c, x + 7, y + 11, 2, 3, 0xffffff);
}

static void panel_paint(struct window *w, const struct canvas *c) {
    (void)w;
    if (pd->picture.px) blit(c, 12, 10, &pd->picture);
    else draw_icon_big(c, 20, 18, &icons[ICON_USER]);
    char line[120];
    if (pd->enabled && pd->me.name[0]) snprintf(line, sizeof(line), "%s (%s)%s", pd->me.nickname, pd->me.name, pd->me.admin ? ", administrator" : "");
    else snprintf(line, sizeof(line), "There are no accounts yet: add the first administrator.");
    text(c, 70, 14, &font_bold, line, C_TEXT);
    if (pd->enabled && pd->me.name[0]) {
        snprintf(line, sizeof(line), "Home: %s.  Starts at the %s.", pd->me.home, pd->me.start == DOSXC_USER_START_PROMPT ? "command prompt" : "desktop");
        text(c, 70, 34, &font_regular, line, C_TEXT);
        for (int i = 0; i < 4; ++i) button(c, own_button(i), own_buttons[i], 0, 0);
    }
    fill(c, 12, 96, 456, 1, C_SHADOW1);
    draw_lock(c, 12, 108, managing());
    if (!pd->enabled) text(c, 34, 110, &font_regular, "Until there is an administrator, anyone can add accounts.", C_TEXT);
    else if (pd->unlocked) {
        snprintf(line, sizeof(line), "Unlocked by %s. Lock it when you are done.", pd->unlocked_by);
        text(c, 34, 110, &font_regular, line, C_TEXT);
    } else text(c, 34, 110, &font_regular, "Locked. An administrator can unlock it to manage accounts.", C_TEXT);
    if (pd->enabled) button(c, lock_button, pd->unlocked ? "Lock" : "Unlock...", 0, !pd->unlocked);
    bevel(c, list_box.x - 2, list_box.y - 2, list_box.w + 4, list_box.h + 4, 1);
    fill(c, list_box.x, list_box.y, list_box.w, list_box.h, C_BASE);
    for (int i = 0; i < pd->count && (i + 1) * ROW <= list_box.h; ++i) {
        int y = list_box.y + i * ROW;
        if (i == pd->selected) fill(c, list_box.x, y, list_box.w, ROW, C_SELECTION);
        snprintf(line, sizeof(line), "%s (%s)%s", pd->users[i].nickname, pd->users[i].name, pd->users[i].admin ? " - administrator" : "");
        if (pd->thumbs[i].px) blit(c, list_box.x + 2, y + 2, &pd->thumbs[i]);
        else draw_icon(c, list_box.x + 3, y + 3, ICON_USER);
        text(c, list_box.x + 24, y + 6, &font_regular, line, i == pd->selected ? 0xffffff : C_TEXT);
    }
    for (int i = 0; i < 5; ++i) {
        const char *label = admin_buttons[i];
        struct rect r = admin_button(i);
        if (managing()) button(c, r, label, 0, 0);
        else {
            bevel(c, r.x, r.y, r.w, r.h, 0);
            text(c, r.x + (r.w - text_width(&font_regular, label)) / 2, r.y + 7, &font_regular, label, C_DISABLED);
        }
    }
}

static void report(long r, const char *what) {
    if (!r) { panel_reload(); me_reload(); return; }
    message_box("User Accounts", what, r == -DOSXC_EPERM ? "Only administrators can do that." :
                r == -DOSXC_EBUSY ? "There must always be an administrator." :
                r == -DOSXC_EACCES ? "The password is not right." : "It did not work.");
    panel_reload();
}

static void set_field(const char *name, int field, const struct dosxc_user *values, const char *password, const char *old) {
    struct dosxc_user_change c;
    memset(&c, 0, sizeof(c));
    c.user = *values;
    snprintf(c.user.name, sizeof(c.user.name), "%s", name);
    c.fields = field;
    if (password) snprintf(c.password, sizeof(c.password), "%s", password);
    if (old) snprintf(c.old_password, sizeof(c.old_password), "%s", old);
    long r = dosxc_sys_user(DOSXC_USER_SET, 0, &c);
    memset(&c, 0, sizeof(c));
    report(r, "The account could not be changed.");
}

static void picture_set(const char *path, void *data) {
    (void)data;
    struct dosxc_user u = pd->me;
    snprintf(u.picture, sizeof(u.picture), "%s", path);
    set_field(pd->me.name, DOSXC_USER_SET_PICTURE, &u, 0, 0);
}

static void selected_picture_set(const char *path, void *data) {
    (void)data;
    if (!path || !pd || pd->selected < 0 || pd->selected >= pd->count) return;
    struct dosxc_user u = pd->users[pd->selected];
    snprintf(u.picture, sizeof(u.picture), "%s", path);
    set_field(u.name, DOSXC_USER_SET_PICTURE, &u, 0, 0);
}

static void nickname_set(const char *text, void *data) {
    (void)data;
    if (!text) return;
    struct dosxc_user u = pd->me;
    snprintf(u.nickname, sizeof(u.nickname), "%s", text);
    set_field(pd->me.name, DOSXC_USER_SET_NICKNAME, &u, 0, 0);
}

static char old_password[64];
static void new_password_set(const char *text, void *data) {
    (void)data;
    if (text) set_field(pd->me.name, DOSXC_USER_SET_PASSWORD, &pd->me, text, old_password);
    memset(old_password, 0, sizeof(old_password));
}
static void old_password_given(const char *text, void *data) {
    (void)data;
    if (!text) return;
    snprintf(old_password, sizeof(old_password), "%s", text);
    secret_box("Password", "New password:", new_password_set, 0);
}

static void home_set(const char *text, void *data) {
    (void)data;
    if (!text || pd->selected < 0) return;
    struct dosxc_user u = pd->users[pd->selected];
    snprintf(u.home, sizeof(u.home), "%s", text);
    set_field(u.name, DOSXC_USER_SET_HOME, &u, 0, 0);
}

static void remove_confirmed(int ok, void *data) {
    (void)data;
    if (ok && pd->selected >= 0) report(dosxc_sys_user(DOSXC_USER_REMOVE, pd->users[pd->selected].name, 0), "The account could not be removed.");
}

static char unlock_name[32];
static void unlock_password(const char *text, void *data) {
    (void)data;
    if (!text || !pd) return;
    long r = dosxc_sys_user(DOSXC_USER_UNLOCK, unlock_name, (void *)text);
    if (r) message_box("User Account Center", "It could not be unlocked.", "Use the name and password of an administrator.");
    else { pd->unlocked = 1; snprintf(pd->unlocked_by, sizeof(pd->unlocked_by), "%s", unlock_name); }
    memset(unlock_name, 0, sizeof(unlock_name));
    panel_reload();
}
static void unlock_named(const char *text, void *data) {
    (void)data;
    if (!text || !text[0]) return;
    snprintf(unlock_name, sizeof(unlock_name), "%s", text);
    secret_box("Unlock", "Administrator's password:", unlock_password, 0);
}

static void panel_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (rect_has(list_box, x, y)) {
        int at = (y - list_box.y) / ROW;
        if (at < pd->count) { pd->selected = at; window_paint(w); }
        return;
    }
    if (pd->enabled && pd->me.name[0]) {
        if (rect_has(own_button(0), x, y)) { picture_chooser(picture_set, 0); return; }
        if (rect_has(own_button(1), x, y)) { input_box("Nickname", "Your nickname:", pd->me.nickname, nickname_set, 0); return; }
        if (rect_has(own_button(2), x, y)) { secret_box("Password", "Your current password:", old_password_given, 0); return; }
        if (rect_has(own_button(3), x, y)) {
            struct dosxc_user u = pd->me;
            u.start = u.start == DOSXC_USER_START_PROMPT ? DOSXC_USER_START_DESKTOP : DOSXC_USER_START_PROMPT;
            set_field(pd->me.name, DOSXC_USER_SET_START, &u, 0, 0);
            return;
        }
    }
    if (pd->enabled && rect_has(lock_button, x, y)) {
        if (pd->unlocked) { dosxc_sys_user(DOSXC_USER_LOCK, 0, 0); pd->unlocked = 0; panel_reload(); }
        else input_box("Unlock", "Administrator's user name:", pd->me.admin ? pd->me.name : "", unlock_named, 0);
        return;
    }
    if (!managing()) return;
    if (rect_has(admin_button(0), x, y)) { open_add_user(!pd->count); return; }
    if (pd->selected < 0 || pd->selected >= pd->count) return;
    struct dosxc_user *u = &pd->users[pd->selected];
    if (rect_has(admin_button(1), x, y)) {
        char question[96];
        snprintf(question, sizeof(question), "Remove %s's account?", u->nickname);
        confirm_box("User Accounts", question, "Remove", ICON_USER, remove_confirmed, 0);
    } else if (rect_has(admin_button(2), x, y)) {
        struct dosxc_user changed = *u;
        changed.admin = !u->admin;
        set_field(u->name, DOSXC_USER_SET_ADMIN, &changed, 0, 0);
    } else if (rect_has(admin_button(3), x, y)) {
        input_box("Home Folder", "Home folder (empty: C:\\Users\\name):", u->home, home_set, 0);
    } else if (rect_has(admin_button(4), x, y)) {
        picture_chooser(selected_picture_set, 0);
    }
}

static void panel_closed(struct window *w) {
    (void)w;
    dosxc_sys_user(DOSXC_USER_LOCK, 0, 0);
    free(pd->picture.px);
    for (int i = 0; i < 32; ++i) free(pd->thumbs[i].px);
    free(pd);
    pd = 0;
    panel = 0;
}
static const struct app_ops panel_ops = {panel_paint, panel_mouse, 0, panel_closed};

void open_account_center(void) {
    if (panel) { window_raise(panel); return; }
    pd = calloc(1, sizeof(*pd));
    if (!pd) return;
    panel_reload();
    panel = window_open(480, 350, "User Account Center", ICON_USER, &panel_ops, pd);
    if (!panel) { free(pd); pd = 0; }
}
