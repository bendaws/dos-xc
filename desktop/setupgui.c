/*
 * DOS-XC /desktop/setupgui.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "../os/setup/install.h"

enum { PAGE_WELCOME, PAGE_DISK, PAGE_PACKAGES, PAGE_CONFIRM, PAGE_COPY, PAGE_DONE, PAGE_FAILED, PAGE_HELP, PAGE_ACCOUNTS };
#define WIDTH 560
#define HEIGHT 400
#define LIST_ROW 20

static struct window *win;
static int page = PAGE_WELCOME, return_page, selected_disk, package_cursor;
static struct vscroll package_scroll;
static char copy_name[300], status_text[100];
static unsigned copy_percent;
static unsigned long copy_done, copy_total;

static const struct rect back_button = {WIDTH - 268, HEIGHT - 36, 80, 24}, next_button = {WIDTH - 180, HEIGHT - 36, 84, 24};
static const struct rect cancel_button = {WIDTH - 88, HEIGHT - 36, 76, 24}, help_button = {12, HEIGHT - 36, 66, 24};
static const struct rect list_area = {24, 144, WIDTH - 48, 200};

static void paint_text(const struct canvas *c, int x, int y, const char *text_block) {
    char line[160];
    while (*text_block) {
        size_t n = strcspn(text_block, "\n");
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text_block, n);
        line[n] = 0;
        text(c, x, y, &font_regular, line, C_TEXT);
        y += 15;
        text_block += n;
        if (*text_block) ++text_block;
    }
}

static void wizard_header(const struct canvas *c, const char *title) {
    char line[60];
    snprintf(line, sizeof(line), "DOS-XC %s Setup", strncmp(setup_system.version, "DOS-XC v", 8) ? setup_system.version : setup_system.version + 8);
    fill(c, 0, 0, WIDTH, 46, C_SELECTION);
    text(c, 16, 9, &font_bold, line, 0xffffff);
    text(c, 16, 26, &font_regular, title, 0xffffff);
}

static void paint_disks(const struct canvas *c) {
    paint_text(c, 24, 66, "DOS-XC requires a partition on your hard drive to copy files to.\n\n"
                          "Installing DOS-XC will erase all data on your hard drive, so please back up\n"
                          "your documents before installing.");
    if (!setup_disk_count) {
        paint_text(c, 24, 160, "No suitable hard disk was found.  Setup needs an internal disk of at\nleast 64 MB with 512-byte sectors.");
        return;
    }
    bevel(c, list_area.x - 2, list_area.y - 2, list_area.w + 4, list_area.h + 4, 1);
    fill(c, list_area.x, list_area.y, list_area.w, list_area.h, C_BASE);
    for (int i = 0; i < setup_disk_count; ++i) {
        char line[64], size[32];
        setup_size_text(size, sizeof(size), setup_disks[i].sectors);
        snprintf(line, sizeof(line), "Disk %d    %s", i + 1, size);
        int y = list_area.y + i * LIST_ROW, on = i == selected_disk;
        if (on) fill(c, list_area.x, y, list_area.w, LIST_ROW, C_SELECTION);
        draw_icon(c, list_area.x + 4, y + 2, ICON_DRIVE);
        text(c, list_area.x + 26, y + 5, &font_regular, line, on ? 0xffffff : C_TEXT);
    }
}

static void checkbox(const struct canvas *c, int x, int y, int on, int disabled) {
    bevel(c, x, y, 13, 13, 1);
    fill(c, x + 2, y + 2, 9, 9, disabled ? C_WINDOW : C_BASE);
    if (on)
        for (int i = 0; i < 5; ++i) {
            fill(c, x + 3 + i, y + 7 - (i > 1 ? i - 1 : 1 - i), 1, 3, disabled ? C_DISABLED : C_TEXT);
        }
}

static void paint_packages(const struct canvas *c) {
    paint_text(c, 24, 66, "DOS-XC Setup can configure and install the following programs for you\nautomatically.\n\nChoose the programs you would like Setup to install.");
    if (!setup_package_count) { paint_text(c, 24, 160, "(No packages were found on the installation media.)"); return; }
    bevel(c, list_area.x - 2, list_area.y - 2, list_area.w + 4, list_area.h + 4, 1);
    fill(c, list_area.x, list_area.y, list_area.w, list_area.h, C_BASE);
    package_scroll.total = setup_package_count;
    package_scroll.visible = list_area.h / LIST_ROW;
    vscroll_clamp(&package_scroll);
    struct canvas clip = *c;
    clip.clip = rect_both(c->clip, list_area);
    for (int i = 0; i < package_scroll.visible && package_scroll.top + i < setup_package_count; ++i) {
        const struct package_choice *p = &setup_packages[package_scroll.top + i];
        int y = list_area.y + i * LIST_ROW, on = package_scroll.top + i == package_cursor;
        if (on) fill(&clip, list_area.x, y, list_area.w - 14, LIST_ROW, C_MENU_SELECTION);
        checkbox(&clip, list_area.x + 6, y + 3, p->selected, p->required);
        text(&clip, list_area.x + 26, y + 5, &font_bold, p->info.name, C_TEXT);
        text(&clip, list_area.x + 140, y + 5, &font_regular, p->info.version, C_TEXT);
        text(&clip, list_area.x + 205, y + 5, &font_regular, p->info.description, C_TEXT);
        if (p->required) text(&clip, list_area.x + list_area.w - 80, y + 5, &font_regular, "(required)", C_DISABLED);
    }
    vscroll_paint(c, &package_scroll, (struct rect){list_area.x + list_area.w - 14, list_area.y, 14, list_area.h});
}

static struct field account_fields[4];
static int account_focus;
static char account_error[80];
static const char *const account_labels[4] = {"User name:", "Nickname:", "Password:", "Password again:"};
static struct rect account_field(int i) { return (struct rect){150, 160 + i * 30, 240, 22}; }

static void paint_accounts(const struct canvas *c) {
    paint_text(c, 24, 66, "Create the administrator: the account that changes the system and\n"
                          "installs programs.  Everyone who uses this computer gets a user name,\n"
                          "a password and folders of their own; add them later in Control Panel.");
    for (int i = 0; i < 4; ++i) {
        text(c, 40, account_field(i).y + 5, &font_regular, account_labels[i], C_TEXT);
        struct field shown = account_fields[i];
        if (i >= 2) for (size_t k = 0; shown.text[k]; ++k) shown.text[k] = '*';
        field_paint(c, &shown, account_field(i), account_focus == i);
    }
    if (account_error[0]) text(c, 40, 284, &font_regular, account_error, 0xb01010);
}

static int accounts_accept(void) {
    account_error[0] = 0;
    memset(&setup_account, 0, sizeof(setup_account));
    if (!setup_valid_name(account_fields[0].text)) snprintf(account_error, sizeof(account_error), "The user name may only have letters and digits.");
    else if (strcmp(account_fields[2].text, account_fields[3].text)) snprintf(account_error, sizeof(account_error), "The two passwords are not the same.");
    if (account_error[0]) return -1;
    setup_account.enabled = 1;
    snprintf(setup_account.name, sizeof(setup_account.name), "%s", account_fields[0].text);
    snprintf(setup_account.nickname, sizeof(setup_account.nickname), "%s", account_fields[1].text);
    snprintf(setup_account.password, sizeof(setup_account.password), "%s", account_fields[2].text);
    return 0;
}

static void paint_confirm(const struct canvas *c) {
    char size[32], line[200], list[300] = "";
    setup_size_text(size, sizeof(size), setup_disks[selected_disk].sectors);
    snprintf(line, sizeof(line), "Setup is ready to install DOS-XC on disk %d (%s).", selected_disk + 1, size);
    text(c, 24, 66, &font_regular, line, C_TEXT);
    text(c, 24, 90, &font_bold, "ALL DATA ON THIS DISK WILL BE ERASED.", 0xb01010);
    for (int i = 0; i < setup_package_count; ++i) {
        if (!setup_packages[i].selected) continue;
        if (*list) strcat(list, ", ");
        if (strlen(list) + strlen(setup_packages[i].info.name) < sizeof(list) - 8) strcat(list, setup_packages[i].info.name);
    }
    text(c, 24, 118, &font_regular, "Programs to install:", C_TEXT);
    text(c, 24, 134, &font_regular, *list ? list : "(none)", C_TEXT);
    snprintf(line, sizeof(line), "Account: administrator %s", setup_account.name);
    text(c, 24, 160, &font_regular, line, C_TEXT);
}

static void paint_copy(const struct canvas *c) {
    paint_text(c, 24, 66, "Setup is copying files to your hard disk.\n\nDo not turn off your computer.");
    text(c, 24, 130, &font_regular, status_text[0] ? status_text : copy_name, C_TEXT);
    char line[64];
    snprintf(line, sizeof(line), "%u%% complete   (%lu of %lu files)", copy_percent, copy_done, copy_total);
    text(c, 24, 150, &font_regular, line, C_TEXT);
    struct rect bar = {24, 172, WIDTH - 48, 22};
    bevel(c, bar.x, bar.y, bar.w, bar.h, 1);
    fill(c, bar.x + 2, bar.y + 2, (bar.w - 4) * (int)copy_percent / 100, bar.h - 4, C_SELECTION);
}

static void wizard_paint(struct window *w, const struct canvas *c) {
    (void)w;
    static const char *const titles[] = {"Welcome", "Choose a disk", "Choose programs to install", "Confirm", "Copying files", "Finished", "Setup failed", "Help", "User accounts"};
    wizard_header(c, titles[page]);
    switch (page) {
    case PAGE_WELCOME:
        paint_text(c, 24, 66, "Welcome to DOS-XC!\n\nSetup will prepare to install DOS-XC on your system.\n\nChoose Next to continue.");
        break;
    case PAGE_DISK: paint_disks(c); break;
    case PAGE_PACKAGES: paint_packages(c); break;
    case PAGE_ACCOUNTS: paint_accounts(c); break;
    case PAGE_CONFIRM: paint_confirm(c); break;
    case PAGE_COPY: paint_copy(c); break;
    case PAGE_DONE:
        paint_text(c, 24, 66, "Setup has finished installing DOS-XC.\n\nRemove the installation media, then choose Restart to start your\ncomputer from the new installation.");
        break;
    case PAGE_FAILED:
        paint_text(c, 24, 66, "Setup could not complete the installation, so do not start the computer\nfrom this disk.");
        text(c, 24, 110, &font_bold, setup_failure, 0xb01010);
        paint_text(c, 24, 134, "Check the disk and its connection, then run Setup again.");
        break;
    case PAGE_HELP:
        paint_text(c, 24, 66, "Setup installs DOS-XC on an internal hard disk.  The disk is erased and\n"
                              "gets a single FAT32 partition; DOS-XC then starts from it with BIOS or\n"
                              "UEFI firmware.\n\n"
                              "The disk must use 512-byte sectors and hold at least 64 MB.  The disk\n"
                              "Setup is running from is never offered.");
        break;
    }
    fill(c, 0, HEIGHT - 44, WIDTH, 1, C_SHADOW1);
    fill(c, 0, HEIGHT - 43, WIDTH, 1, C_HIGHLIGHT);
    if (page == PAGE_DONE) {
        button(c, next_button, "Restart", 0, 1);
        button(c, cancel_button, "Close", 0, 0);
    } else if (page == PAGE_FAILED) button(c, cancel_button, "Close", 0, 0);
    else if (page == PAGE_HELP) button(c, next_button, "OK", 0, 1);
    else if (page != PAGE_COPY) {
        if (page != PAGE_WELCOME) button(c, back_button, "< Back", 0, 0);
        button(c, next_button, page == PAGE_CONFIRM ? "Erase" : "Next >", 0, 1);
        button(c, cancel_button, "Exit", 0, 0);
        button(c, help_button, "Help", 0, 0);
    }
}

static void on_status(const char *status) {
    snprintf(status_text, sizeof(status_text), "%s", status);
    window_paint(win);
    gui_pump();
}

static void on_progress(const char *name, unsigned percent, unsigned long done, unsigned long total) {
    status_text[0] = 0;
    snprintf(copy_name, sizeof(copy_name), "%s", name);
    copy_percent = percent;
    copy_done = done;
    copy_total = total;
    window_paint(win);
    gui_pump();
}

static void go_next(void) {
    switch (page) {
    case PAGE_WELCOME: setup_find_disks(); page = PAGE_DISK; break;
    case PAGE_DISK:
        if (!setup_disk_count) return;
        if (!setup_package_count) setup_find_packages();
        page = PAGE_PACKAGES;
        break;
    case PAGE_PACKAGES: page = PAGE_ACCOUNTS; break;
    case PAGE_ACCOUNTS: if (!accounts_accept()) page = PAGE_CONFIRM; break;
    case PAGE_CONFIRM:
        page = PAGE_COPY;
        copy_percent = 0;
        window_paint(win);
        gui_pump();
        page = setup_install(&setup_disks[selected_disk], on_progress, on_status) ? PAGE_FAILED : PAGE_DONE;
        break;
    case PAGE_DONE: dosxc_sys_reboot(); break;
    case PAGE_HELP: page = return_page; break;
    }
    window_paint(win);
}

static void go_back(void) {
    if (page == PAGE_DISK) page = PAGE_WELCOME;
    else if (page == PAGE_PACKAGES) page = PAGE_DISK;
    else if (page == PAGE_ACCOUNTS) page = PAGE_PACKAGES;
    else if (page == PAGE_CONFIRM) page = PAGE_ACCOUNTS;
    window_paint(win);
}

static void wizard_mouse(struct window *w, int x, int y, int kind, int value) {
    struct rect bar = {list_area.x + list_area.w - 14, list_area.y, 14, list_area.h};
    if (page == PAGE_PACKAGES && setup_package_count) {
        if (vscroll_mouse(&package_scroll, bar, x, y, kind, value)) { window_paint(w); return; }
        if (kind == M_WHEEL) { package_scroll.top -= value * 2; vscroll_clamp(&package_scroll); window_paint(w); return; }
    }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (rect_has(next_button, x, y) && page != PAGE_COPY && page != PAGE_FAILED) { go_next(); return; }
    if (rect_has(cancel_button, x, y) && page != PAGE_COPY) { window_close(w); return; }
    if (page == PAGE_COPY || page == PAGE_DONE || page == PAGE_FAILED || page == PAGE_HELP) return;
    if (rect_has(back_button, x, y) && page != PAGE_WELCOME) { go_back(); return; }
    if (rect_has(help_button, x, y)) { return_page = page; page = PAGE_HELP; window_paint(w); return; }
    if (page == PAGE_ACCOUNTS) {
        for (int i = 0; i < 4; ++i)
            if (rect_has(account_field(i), x, y)) account_focus = i;
        window_paint(w);
        return;
    }
    if (!rect_has(list_area, x, y)) return;
    int at = (y - list_area.y) / LIST_ROW;
    if (page == PAGE_DISK && at < setup_disk_count) { selected_disk = at; window_paint(w); }
    if (page == PAGE_PACKAGES && package_scroll.top + at < setup_package_count && x < list_area.x + list_area.w - 14) {
        at += package_scroll.top;
        package_cursor = at;
        if (!setup_packages[at].required) { setup_packages[at].selected ^= 1; setup_select_requirements(); }
        window_paint(w);
    }
}

static void wizard_key(struct window *w, int key) {
    if (page == PAGE_COPY) return;
    if (key == '\n' || key == '\r') { if (page != PAGE_FAILED) go_next(); return; }
    if (key == 27 && page != PAGE_DONE && page != PAGE_FAILED) { if (page == PAGE_HELP) go_next(); else if (page == PAGE_WELCOME) window_close(w); else go_back(); return; }
    if (page == PAGE_ACCOUNTS) {
        if (key == '\t') account_focus = (account_focus + 1) % 4;
        else field_key(&account_fields[account_focus], key);
        window_paint(w);
        return;
    }
    if (page == PAGE_DISK) {
        if (key == 0x101 && selected_disk) --selected_disk;
        if (key == 0x102 && selected_disk + 1 < setup_disk_count) ++selected_disk;
        window_paint(w);
    } else if (page == PAGE_PACKAGES && setup_package_count) {
        if (key == 0x101 && package_cursor) --package_cursor;
        if (key == 0x102 && package_cursor + 1 < setup_package_count) ++package_cursor;
        if (key == ' ' && !setup_packages[package_cursor].required) {
            setup_packages[package_cursor].selected ^= 1;
            setup_select_requirements();
        }
        if (package_cursor < package_scroll.top) package_scroll.top = package_cursor;
        if (package_cursor >= package_scroll.top + package_scroll.visible) package_scroll.top = package_cursor - package_scroll.visible + 1;
        window_paint(w);
    }
}

static int wizard_closing(struct window *w) { (void)w; return page != PAGE_COPY; }

static const struct app_ops wizard_ops = {wizard_paint, wizard_mouse, wizard_key, 0, 0, 0, 0, wizard_closing};

int main(void) {
    if (gui_connect()) return 1;
    setup_init();
    if (!setup_system.live) {
        win = window_open(10, 10, "", ICON_SETUP, &wizard_ops, 0);
        window_close(win);
        message_box("DOS-XC Setup", "Setup runs from the installation media.", "Start the computer from the installation USB drive or CD.");
        gui_run();
        return 1;
    }
    win = window_open(WIDTH, HEIGHT, "DOS-XC Setup", ICON_SETUP, &wizard_ops, 0);
    if (!win) return 1;
    gui_run();
    return 0;
}
