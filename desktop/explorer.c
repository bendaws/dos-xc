/*
 * DOS-XC /desktop/explorer.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "archive/archive.h"
#include <sys/stat.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>

#define MAX_FILES 2048
#define MAX_NODES 512
#define ROW 20
#define NAV_ROW 20
#define ADDRESS_H 34
#define COMMAND_H 28
#define DETAILS_H 54
#define NAV_W 210
#define HEADER_H 20
#define BODY_Y (ADDRESS_H + COMMAND_H)
#define BLUE_TINT 0xe8eef7
#define SELECT_FILL 0xcfe1f7
#define SELECT_LINE 0x7da2ce
#define HISTORY 64

struct node { char path[260], label[64]; int depth, expanded, kids_loaded, icon, header, eject; };
static struct node nodes[MAX_NODES];
static int node_count;
static struct vscroll nav_scroll;

struct item { char name[256]; unsigned attributes; unsigned long long size; long long mtime; };
static struct item *items, *shown;
static int item_count, shown_count, selected = -1;
static struct vscroll list_scroll;
static char folder[260];
static char back_list[HISTORY][260], forward_list[HISTORY][260];
static int back_count, forward_count;
static struct field search;
static int search_focus;
static int sort_column, sort_reverse;
static int renaming = -1;
static struct field rename_field;
static struct window *main_w;
static char copied_path[300];
static int nav_drag_active;

static void load_folder(void);
static void build_nav(void);

static struct dosxc_volume vols[26];
static int vol_count;

static void read_volumes(void) {
    vol_count = dosxc_sys_volumes(vols, 26);
    if (vol_count < 0) vol_count = 0;
    if (vol_count > 26) vol_count = 26;
}

static const struct dosxc_volume *volume_info(char letter) {
    for (int i = 0; i < vol_count; ++i)
        if (toupper((unsigned char)vols[i].letter) == toupper((unsigned char)letter)) return &vols[i];
    return 0;
}

static int ejectable(char letter) {
    const struct dosxc_volume *v = volume_info(letter);
    return v && !(v->flags & DOSXC_VOLUME_SYSTEM) && (v->flags & (DOSXC_VOLUME_REMOVABLE | DOSXC_VOLUME_IMAGE));
}

static const char *drive_kind(char letter) {
    const struct dosxc_volume *v = volume_info(letter);
    if (v && (v->flags & DOSXC_VOLUME_IMAGE)) return v->type ? "Disk Image" : "CD Image";
    if (v && (v->flags & DOSXC_VOLUME_REMOVABLE)) return "Removable Disk";
    return "Local Disk";
}

static int has_ending(const char *name, const char *const *endings) {
    size_t n = strlen(name);
    for (int i = 0; endings[i]; ++i) {
        size_t k = strlen(endings[i]);
        if (n > k && !strcasecmp(name + n - k, endings[i])) return 1;
    }
    return 0;
}

static int is_archive(const char *name) {
    static const char *const endings[] = {".zip", ".7z", ".rar", ".tar", ".tgz", ".txz", ".gz", ".xz", 0};
    return has_ending(name, endings);
}

static int is_image(const char *name) {
    static const char *const endings[] = {".iso", ".img", ".ima", 0};
    return has_ending(name, endings);
}

static void go(const char *path, int remember) {
    if (remember && strcmp(path, folder)) {
        if (back_count == HISTORY) { memmove(back_list[0], back_list[1], (HISTORY - 1) * sizeof(back_list[0])); --back_count; }
        snprintf(back_list[back_count++], sizeof(back_list[0]), "%s", folder);
        forward_count = 0;
    }
    snprintf(folder, sizeof(folder), "%s", path);
    search.text[0] = 0;
    search.cursor = 0;
    load_folder();
}

static void go_back(void) {
    if (!back_count) return;
    snprintf(forward_list[forward_count++], sizeof(forward_list[0]), "%s", folder);
    char target[260];
    snprintf(target, sizeof(target), "%s", back_list[--back_count]);
    go(target, 0);
}

static void go_forward(void) {
    if (!forward_count) return;
    snprintf(back_list[back_count++], sizeof(back_list[0]), "%s", folder);
    char target[260];
    snprintf(target, sizeof(target), "%s", forward_list[--forward_count]);
    go(target, 0);
}

static void go_up(void) {
    size_t n = strlen(folder);
    if (!n) return;
    char target[260];
    snprintf(target, sizeof(target), "%s", folder);
    if (n <= 3) target[0] = 0;
    else {
        while (n > 3 && target[n - 1] != '\\') --n;
        if (n > 3) --n;
        target[n] = 0;
    }
    go(target, 1);
}

static const char *type_of(const struct item *it) {
    if (it->attributes & DOSXC_ATTR_DIRECTORY) return "File folder";
    const char *dot = strrchr(it->name, '.');
    if (!dot) return "File";
    if (!strcasecmp(dot, ".EXE")) return "Application";
    if (!strcasecmp(dot, ".COM")) return "MS-DOS Application";
    if (!strcasecmp(dot, ".BAT")) return "Batch File";
    if (!strcasecmp(dot, ".DLL")) return "Library";
    if (!strcasecmp(dot, ".TXT")) return "Text Document";
    if (!strcasecmp(dot, ".DRV")) return "Device Driver";
    if (!strcasecmp(dot, ".DSP")) return "Package";
    if (!strcasecmp(dot, ".ZIP")) return "ZIP Archive";
    if (!strcasecmp(dot, ".7Z")) return "7-Zip Archive";
    if (!strcasecmp(dot, ".RAR")) return "RAR Archive";
    if (!strcasecmp(dot, ".TAR")) return "Tar Archive";
    if (!strcasecmp(dot, ".TGZ") || !strcasecmp(dot, ".TXZ") || ((!strcasecmp(dot, ".GZ") || !strcasecmp(dot, ".XZ")) && dot - it->name > 4 && !strncasecmp(dot - 4, ".tar", 4))) return "Compressed Tar Archive";
    if (!strcasecmp(dot, ".GZ")) return "Gzip File";
    if (!strcasecmp(dot, ".XZ")) return "XZ File";
    if (!strcasecmp(dot, ".ISO")) return "CD/DVD Image";
    if (!strcasecmp(dot, ".IMG") || !strcasecmp(dot, ".IMA")) return "Disk Image";
    if (!strcasecmp(dot, ".JPG") || !strcasecmp(dot, ".JPEG") || !strcasecmp(dot, ".PNG") || !strcasecmp(dot, ".BMP")) return "Picture";
    static char kind[32];
    snprintf(kind, sizeof(kind), "%.8s File", dot + 1);
    for (char *p = kind; *p && *p != ' '; ++p) *p = (char)toupper((unsigned char)*p);
    return kind;
}

static int compare(const void *a, const void *b) {
    const struct item *x = a, *y = b;
    int dx = !(x->attributes & DOSXC_ATTR_DIRECTORY), dy = !(y->attributes & DOSXC_ATTR_DIRECTORY);
    if (dx != dy) return dx - dy;
    long long r = 0;
    switch (sort_column) {
    case 1: r = x->mtime - y->mtime; break;
    case 2: r = strcasecmp(type_of(x), type_of(y)); break;
    case 3: r = (long long)x->size - (long long)y->size; break;
    }
    if (!r) r = strcasecmp(x->name, y->name);
    if (sort_reverse) r = -r;
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

static int matches(const char *name, const char *needle) {
    if (!needle[0]) return 1;
    size_t n = strlen(needle);
    for (const char *p = name; *p; ++p)
        if (!strncasecmp(p, needle, n)) return 1;
    return 0;
}

static void apply_view(void) {
    qsort(items, (size_t)item_count, sizeof(*items), compare);
    shown_count = 0;
    for (int i = 0; i < item_count; ++i)
        if (matches(items[i].name, search.text)) shown[shown_count++] = items[i];
    selected = -1;
    list_scroll.top = 0;
    list_scroll.total = shown_count;
    renaming = -1;
}

static void load_folder(void) {
    item_count = 0;
    read_volumes();
    if (!folder[0]) {
        for (int i = 0; i < vol_count && i < MAX_FILES; ++i) {
            memset(&items[i], 0, sizeof(items[i]));
            snprintf(items[i].name, sizeof(items[i].name), "%c:", vols[i].letter);
            items[i].attributes = DOSXC_ATTR_DIRECTORY;
            items[i].size = vols[i].total_bytes;
            ++item_count;
        }
    } else {
        struct dosxc_dirent *list = malloc(sizeof(*list) * MAX_FILES);
        if (list) {
            int n = (int)dosxc_sys_listdir(folder, list, MAX_FILES);
            for (int i = 0; i < n && i < MAX_FILES; ++i) {
                if (!strcmp(list[i].name, ".") || !strcmp(list[i].name, "..")) continue;
                struct item *it = &items[item_count++];
                snprintf(it->name, sizeof(it->name), "%s", list[i].name);
                it->attributes = list[i].attributes;
                it->size = list[i].size;
                it->mtime = list[i].mtime;
            }
            free(list);
        }
    }
    apply_view();
    char title[80];
    const char *base = strrchr(folder, '\\');
    snprintf(title, sizeof(title), "%s - File Explorer", !folder[0] ? "Computer" : base && base[1] ? base + 1 : folder);
    if (main_w) { window_title(main_w, title); window_paint(main_w); }
}

/* ---- navigation pane ---------------------------------------------------- */

static void add_node(int at, const char *path, const char *label, int depth, int icon, int header) {
    if (node_count == MAX_NODES) return;
    memmove(&nodes[at + 1], &nodes[at], (size_t)(node_count - at) * sizeof(nodes[0]));
    struct node *n = &nodes[at];
    memset(n, 0, sizeof(*n));
    snprintf(n->path, sizeof(n->path), "%s", path);
    snprintf(n->label, sizeof(n->label), "%s", label);
    n->depth = depth;
    n->icon = icon;
    n->header = header;
    ++node_count;
}

static int child_folders(const char *path, char names[][64], int capacity) {
    struct dosxc_dirent *list = malloc(sizeof(*list) * 512);
    if (!list) return 0;
    int n = (int)dosxc_sys_listdir(path, list, 512), kept = 0;
    for (int i = 0; i < n && i < 512 && kept < capacity; ++i) {
        if (!(list[i].attributes & DOSXC_ATTR_DIRECTORY)) continue;
        if (!strcmp(list[i].name, ".") || !strcmp(list[i].name, "..")) continue;
        snprintf(names[kept++], 64, "%s", list[i].name);
    }
    free(list);
    return kept;
}

static int label_order(const void *a, const void *b) { return strcasecmp(a, b); }

static void expand(int index) {
    struct node *n = &nodes[index];
    if (n->expanded) {
        int end = index + 1;
        while (end < node_count && nodes[end].depth > n->depth) ++end;
        memmove(&nodes[index + 1], &nodes[end], (size_t)(node_count - end) * sizeof(nodes[0]));
        node_count -= end - index - 1;
        n->expanded = 0;
    } else {
        char names[64][64];
        int count = child_folders(n->path, names, 64);
        qsort(names, (size_t)count, 64, label_order);
        for (int i = count - 1; i >= 0; --i) {
            char child[320];
            snprintf(child, sizeof(child), "%s%s%s", n->path, n->path[strlen(n->path) - 1] == '\\' ? "" : "\\", names[i]);
            add_node(index + 1, child, names[i], n->depth + 1, ICON_FOLDER, 0);
        }
        nodes[index].expanded = 1;
    }
    nav_scroll.total = node_count;
}

static void build_nav(void) {
    node_count = 0;
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    char path[64];
    add_node(node_count, "", "Favorites", 0, ICON_STAR, 1);
    const char *home = getenv("HOME");
    char expanded[64];
    if (home && *home == '\\') snprintf(expanded, sizeof(expanded), "%c:%s", info.drive, home);
    else snprintf(expanded, sizeof(expanded), "%s", home && *home ? home : "");
    add_node(node_count, expanded[0] ? expanded : "\\", "Home", 1, ICON_FOLDER, 0);
    snprintf(path, sizeof(path), "%c:\\Programs", info.drive);
    add_node(node_count, path, "Programs", 1, ICON_FOLDER, 0);
    snprintf(path, sizeof(path), "%c:\\SYSTEM", info.drive);
    add_node(node_count, path, "System", 1, ICON_FOLDER, 0);
    add_node(node_count, "", "Computer", 0, ICON_COMPUTER, 2);
    read_volumes();
    for (int i = 0; i < vol_count; ++i) {
        char root[8], label[40];
        snprintf(root, sizeof(root), "%c:\\", vols[i].letter);
        snprintf(label, sizeof(label), "%s (%c:)", drive_kind(vols[i].letter), vols[i].letter);
        add_node(node_count, root, label, 1, ICON_DRIVE, 0);
        nodes[node_count - 1].eject = ejectable(vols[i].letter);
    }
    nav_scroll.total = node_count;
}

static struct rect nav_rect(const struct window *w) { return (struct rect){0, BODY_Y, NAV_W, w->content.h - BODY_Y - DETAILS_H}; }
static struct rect list_rect(const struct window *w) { return (struct rect){NAV_W + 2, BODY_Y, w->content.w - NAV_W - 2, w->content.h - BODY_Y - DETAILS_H}; }
static struct rect back_rect(void) { return (struct rect){8, 6, 24, 22}; }
static struct rect forward_rect(void) { return (struct rect){34, 6, 24, 22}; }
static struct rect up_rect(void) { return (struct rect){60, 6, 24, 22}; }
static struct rect address_rect(const struct window *w) { return (struct rect){90, 6, w->content.w - 90 - 190, 22}; }
static struct rect search_rect(const struct window *w) { return (struct rect){w->content.w - 182, 6, 174, 22}; }

/* ---- painting ------------------------------------------------------------ */

static void triangle(const struct canvas *c, int x, int y, int left, u32 color) {
    for (int i = 0; i < 6; ++i) fill(c, left ? x + 5 - i : x + i, y + i, 1, 12 - 2 * i, color);
}

static void paint_address(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->content.w, ADDRESS_H, BLUE_TINT);
    button(c, back_rect(), "", 0, 0);
    triangle(c, back_rect().x + 8, back_rect().y + 5, 1, back_count ? C_TEXT : C_DISABLED);
    button(c, forward_rect(), "", 0, 0);
    triangle(c, forward_rect().x + 10, forward_rect().y + 5, 0, forward_count ? C_TEXT : C_DISABLED);
    button(c, up_rect(), "Up", 0, 0);
    struct rect a = address_rect(w);
    bevel(c, a.x, a.y, a.w, a.h, 1);
    fill(c, a.x + 2, a.y + 2, a.w - 4, a.h - 4, C_BASE);
    draw_icon(c, a.x + 4, a.y + 3, folder[0] ? ICON_FOLDER : ICON_COMPUTER);
    char crumbs[600] = "Computer";
    if (folder[0]) {
        char copy[260];
        snprintf(copy, sizeof(copy), "%s", folder);
        for (char *part = strtok(copy, "\\"); part; part = strtok(0, "\\")) {
            size_t n = strlen(crumbs);
            snprintf(crumbs + n, sizeof(crumbs) - n, "  >  %s", part);
        }
    }
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){a.x + 24, a.y, a.w - 26, a.h});
    text(&inner, a.x + 24, a.y + 6, &font_regular, crumbs, C_TEXT);
    struct rect s = search_rect(w);
    field_paint(c, &search, s, search_focus);
    if (!search.text[0] && !search_focus) text(c, s.x + 6, s.y + 6, &font_regular, "Search", C_DISABLED);
}

enum { CMD_ORGANIZE, CMD_NEW_FOLDER, CMD_OPEN, CMD_EXTRACT, CMD_MOUNT, CMD_EJECT };
static const char *const command_labels[] = {"Organize", "New folder", "Open", "Extract all", "Mount", "Eject"};

static const struct item *selected_item(void) { return selected >= 0 && selected < shown_count ? &shown[selected] : 0; }

/* The drive Eject acts on: the drive selected under Computer, or the one being shown. */
static char eject_target(void) {
    const struct item *it = selected_item();
    if (!folder[0]) return it && ejectable(it->name[0]) ? it->name[0] : 0;
    return ejectable(folder[0]) ? folder[0] : 0;
}

static int commands(int *ids) {
    int n = 0;
    const struct item *it = selected_item();
    ids[n++] = CMD_ORGANIZE;
    ids[n++] = CMD_NEW_FOLDER;
    ids[n++] = CMD_OPEN;
    if (it && folder[0] && !(it->attributes & DOSXC_ATTR_DIRECTORY) && is_archive(it->name)) ids[n++] = CMD_EXTRACT;
    if (it && folder[0] && !(it->attributes & DOSXC_ATTR_DIRECTORY) && is_image(it->name)) ids[n++] = CMD_MOUNT;
    if (eject_target()) ids[n++] = CMD_EJECT;
    return n;
}

static int command_width(int id) { return text_width(&font_regular, command_labels[id]) + (id == CMD_ORGANIZE ? 30 : 18); }

static void eject_glyph(const struct canvas *c, int x, int y, u32 color) {
    for (int k = 0; k < 5; ++k) fill(c, x + 4 - k, y + k, 2 * k + 1, 1, color);
    fill(c, x, y + 6, 9, 2, color);
}

static void paint_command(struct window *w, const struct canvas *c) {
    fill(c, 0, ADDRESS_H, w->content.w, COMMAND_H, C_WINDOW);
    fill(c, 0, ADDRESS_H + COMMAND_H - 1, w->content.w, 1, C_SHADOW1);
    int ids[8], n = commands(ids), x = 8;
    for (int i = 0; i < n; ++i) {
        int width = command_width(ids[i]);
        button(c, (struct rect){x, ADDRESS_H + 3, width, 22}, command_labels[ids[i]], 0, 0);
        if (ids[i] == CMD_ORGANIZE) for (int k = 0; k < 4; ++k) fill(c, x + width - 12 + k, ADDRESS_H + 12 + k, 7 - 2 * k, 1, C_TEXT);
        x += width + 4;
    }
}

static int command_at(int x) {
    int ids[8], n = commands(ids), left = 8;
    for (int i = 0; i < n; ++i) {
        int width = command_width(ids[i]);
        if (x >= left && x < left + width) return ids[i];
        left += width + 4;
    }
    return -1;
}

static void paint_nav(struct window *w, const struct canvas *c) {
    struct rect r = nav_rect(w);
    fill(c, r.x, r.y, r.w, r.h, C_BASE);
    fill(c, r.x + r.w, r.y, 2, r.h, C_MENU_STRIPE);
    nav_scroll.visible = r.h / NAV_ROW;
    vscroll_clamp(&nav_scroll);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, r);
    for (int i = 0; i < nav_scroll.visible + 1 && nav_scroll.top + i < node_count; ++i) {
        const struct node *n = &nodes[nav_scroll.top + i];
        int y = r.y + i * NAV_ROW;
        if (!n->header && !strcasecmp(n->path, folder)) {
            fill(&inner, r.x, y, r.w - (node_count > nav_scroll.visible ? 14 : 0), NAV_ROW, SELECT_FILL);
            frame_box(&inner, r.x, y, r.w - (node_count > nav_scroll.visible ? 14 : 0), NAV_ROW, SELECT_LINE);
        }
        int x = r.x + 8 + n->depth * 16;
        if (!n->header && n->depth > 0) {
            int tx = x - 12, ty = y + 5;
            if (n->expanded) for (int k = 0; k < 5; ++k) fill(&inner, tx + k, ty + 2 + k, 9 - 2 * k, 1, 0x505050);
            else for (int k = 0; k < 5; ++k) fill(&inner, tx + 2 + k, ty + k, 1, 9 - 2 * k, 0x505050);
        }
        draw_icon(&inner, x, y + 2, n->icon);
        text(&inner, x + 20, y + 5, n->header ? &font_bold : &font_regular, n->label, n->header ? 0x1e3a78 : C_TEXT);
        if (n->eject) {
            int ex = r.x + r.w - (node_count > nav_scroll.visible ? 14 : 0) - 16;
            fill(&inner, ex - 3, y + 1, 15, NAV_ROW - 2, C_BASE);
            eject_glyph(&inner, ex, y + 6, 0x505050);
        }
    }
    if (node_count > nav_scroll.visible) vscroll_paint(c, &nav_scroll, (struct rect){r.x + r.w - 14, r.y, 14, r.h});
}

static void format_date(long long t, char *out, size_t size) {
    if (!t) { out[0] = 0; return; }
    time_t tt = (time_t)t;
    struct tm *tm = gmtime(&tt);
    if (!tm) { out[0] = 0; return; }
    snprintf(out, size, "%02d/%02d/%04d %02d:%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900, tm->tm_hour, tm->tm_min);
}

static void format_size(unsigned long long s, char *out, size_t size) {
    if (s < 1024) snprintf(out, size, "%llu bytes", s);
    else if (s < 1024 * 1024) snprintf(out, size, "%llu KB", s / 1024);
    else if (s < 1024ull * 1024 * 1024) snprintf(out, size, "%llu.%llu MB", s >> 20, ((s >> 10) & 1023) * 10 / 1024);
    else snprintf(out, size, "%llu.%llu GB", s >> 30, ((s >> 20) & 1023) * 10 / 1024);
}

static void column_edges(const struct rect r, int edges[5]) {
    edges[0] = r.x;
    edges[4] = r.x + r.w - 14;
    edges[3] = edges[4] - 80;
    edges[2] = edges[3] - 130;
    edges[1] = edges[2] - 130;
}

static void paint_list(struct window *w, const struct canvas *c) {
    struct rect r = list_rect(w);
    fill(c, r.x, r.y, r.w, r.h, C_BASE);
    int e[5];
    column_edges(r, e);
    static const char *const headers[4] = {"Name", "Date modified", "Type", "Size"};
    for (int i = 0; i < 4; ++i) {
        fill(c, e[i], r.y, e[i + 1] - e[i], HEADER_H, BLUE_TINT);
        fill(c, e[i + 1] - 1, r.y + 3, 1, HEADER_H - 6, C_SHADOW1);
        text(c, e[i] + 6, r.y + 5, &font_regular, headers[i], C_TEXT);
        if (i == sort_column) fill(c, e[i] + 6 + text_width(&font_regular, headers[i]) + 6, r.y + 8, 6, 3, 0x505050);
    }
    fill(c, r.x, r.y + HEADER_H - 1, r.w, 1, C_SHADOW1);
    int top = r.y + HEADER_H, rows = (r.h - HEADER_H) / ROW;
    list_scroll.visible = rows;
    vscroll_clamp(&list_scroll);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){r.x, top, r.w - 14, r.h - HEADER_H});
    for (int i = 0; i <= rows && list_scroll.top + i < shown_count; ++i) {
        const struct item *it = &shown[list_scroll.top + i];
        int y = top + i * ROW, on = list_scroll.top + i == selected;
        if (on) {
            fill(&inner, r.x + 1, y, r.w - 16, ROW, SELECT_FILL);
            frame_box(&inner, r.x + 1, y, r.w - 16, ROW, SELECT_LINE);
        }
        int dir = it->attributes & DOSXC_ATTR_DIRECTORY;
        const char *dot = strrchr(it->name, '.');
        int program = dot && (!strcasecmp(dot, ".EXE") || !strcasecmp(dot, ".BAT") || !strcasecmp(dot, ".COM"));
        int icon = !folder[0] ? ICON_DRIVE : dir ? ICON_FOLDER : program ? ICON_PROGRAM : image_is_picture(it->name) ? ICON_PICTURE :
                   is_archive(it->name) ? ICON_PACKAGE : is_image(it->name) ? ICON_DRIVE : ICON_FILE;
        draw_icon(&inner, e[0] + 4, y + 2, icon);
        if (list_scroll.top + i == renaming) {
            field_paint(c, &rename_field, (struct rect){e[0] + 24, y, e[1] - e[0] - 30, ROW}, 1);
        } else {
            struct canvas name_clip = inner;
            name_clip.clip = rect_both(inner.clip, (struct rect){e[0], y, e[1] - e[0] - 4, ROW});
            char label[300];
            if (!folder[0]) snprintf(label, sizeof(label), "%s (%s)", drive_kind(it->name[0]), it->name);
            else snprintf(label, sizeof(label), "%s", it->name);
            text(&name_clip, e[0] + 26, y + 5, &font_regular, label, C_TEXT);
        }
        char line[40];
        format_date(it->mtime, line, sizeof(line));
        if (folder[0]) text(&inner, e[1] + 6, y + 5, &font_regular, line, 0x505050);
        text(&inner, e[2] + 6, y + 5, &font_regular, !folder[0] ? drive_kind(it->name[0]) : type_of(it), 0x505050);
        if (folder[0] ? !dir : it->size != 0) { format_size(it->size, line, sizeof(line)); text(&inner, e[3] + 6, y + 5, &font_regular, line, 0x505050); }
    }
    vscroll_paint(c, &list_scroll, (struct rect){r.x + r.w - 14, top, 14, r.h - HEADER_H});
}

static void paint_details(struct window *w, const struct canvas *c) {
    int y = w->content.h - DETAILS_H;
    fill(c, 0, y, w->content.w, DETAILS_H, BLUE_TINT);
    fill(c, 0, y, w->content.w, 1, C_SHADOW1);
    char line[320];
    if (selected >= 0 && selected < shown_count) {
        const struct item *it = &shown[selected];
        draw_icon(c, 14, y + 18, (it->attributes & DOSXC_ATTR_DIRECTORY) ? ICON_FOLDER : image_is_picture(it->name) ? ICON_PICTURE : ICON_FILE);
        text(c, 44, y + 9, &font_bold, it->name, C_TEXT);
        text(c, 44, y + 25, &font_regular, folder[0] ? type_of(it) : drive_kind(it->name[0]), 0x505050);
        if (!folder[0] && it->size) {
            char size[32];
            format_size(it->size, size, sizeof(size));
            snprintf(line, sizeof(line), "Size: %s%s", size, ejectable(it->name[0]) ? "    (Eject removes it safely)" : "");
            text(c, 44, y + 39, &font_regular, line, 0x505050);
        } else if (!(it->attributes & DOSXC_ATTR_DIRECTORY)) {
            char size[32];
            format_size(it->size, size, sizeof(size));
            snprintf(line, sizeof(line), "Size: %s", size);
            text(c, 44, y + 39, &font_regular, line, 0x505050);
        }
    } else {
        draw_icon(c, 14, y + 18, ICON_FOLDER);
        snprintf(line, sizeof(line), "%d item%s", shown_count, shown_count == 1 ? "" : "s");
        text(c, 44, y + 20, &font_regular, line, C_TEXT);
    }
}

static void ex_paint(struct window *w, const struct canvas *c) {
    paint_address(w, c);
    paint_command(w, c);
    paint_nav(w, c);
    paint_list(w, c);
    paint_details(w, c);
}

/* ---- file operations ----------------------------------------------------- */

static void item_path(const struct item *it, char *out, size_t size) {
    if (!folder[0]) { snprintf(out, size, "%s\\", it->name); return; }
    snprintf(out, size, "%s%s%s", folder, folder[strlen(folder) - 1] == '\\' ? "" : "\\", it->name);
}

static void ask_extract(const char *path);
static void mount_image(const char *path);

static void open_item(int index) {
    if (index < 0 || index >= shown_count) return;
    const struct item *it = &shown[index];
    char path[520];
    item_path(it, path, sizeof(path));
    if (it->attributes & DOSXC_ATTR_DIRECTORY) go(path, 1);
    else if (is_image(it->name)) mount_image(path);
    else if (is_archive(it->name)) ask_extract(path);
    else gui_open_file(path);
}

static struct window *progress_w;
static struct {
    char verb[24], name[120], current[200];
    unsigned long long done, total;
    int cancel;
    long long shown;
} job;
static const struct rect progress_cancel = {316, 92, 80, 24};

static void progress_paint(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->content.w, w->content.h, C_WINDOW);
    char line[200];
    snprintf(line, sizeof(line), "%s %s", job.verb, job.name);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){16, 0, w->content.w - 32, w->content.h});
    text(&inner, 16, 12, &font_bold, line, C_TEXT);
    text(&inner, 16, 32, &font_regular, job.current, 0x505050);
    int width = w->content.w - 32, inside = width - 4;
    bevel(c, 16, 52, width, 20, 1);
    fill(c, 18, 54, inside, 16, C_BASE);
    long long filled = job.total ? (long long)(job.done > job.total ? job.total : job.done) * inside / (long long)job.total : 0;
    fill(c, 18, 54, (int)filled, 16, C_SELECTION);
    char done[32], total[32];
    format_size(job.done > job.total ? job.total : job.done, done, sizeof(done));
    format_size(job.total, total, sizeof(total));
    snprintf(line, sizeof(line), "%s of %s", done, total);
    text(c, 16, 80, &font_regular, line, 0x505050);
    button(c, progress_cancel, job.cancel ? "Stopping" : "Cancel", 0, 0);
}

static void progress_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind == M_DOWN && rect_has(progress_cancel, x, y)) { job.cancel = 1; window_paint(w); }
}

static void progress_key(struct window *w, int key) { if (key == 27) { job.cancel = 1; window_paint(w); } }
static int progress_closing(struct window *w) { job.cancel = 1; window_paint(w); return 0; }

static const struct app_ops progress_ops = {progress_paint, progress_mouse, progress_key, 0, 0, 0, 0, progress_closing};

static int progress_report(void *context, const char *name, unsigned long long done, unsigned long long total) {
    (void)context;
    if (name) snprintf(job.current, sizeof(job.current), "%s", name);
    job.done = done;
    job.total = total;
    long long now = (long long)dosxc_sys_nanoseconds();
    if (progress_w && now - job.shown > 50000000) {
        job.shown = now;
        window_paint(progress_w);
        gui_pump();
    }
    return job.cancel;
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

static void begin_progress(const char *verb, const char *path) {
    memset(&job, 0, sizeof(job));
    snprintf(job.verb, sizeof(job.verb), "%s", verb);
    snprintf(job.name, sizeof(job.name), "%s", base_name(path));
    progress_w = window_open(412, 124, "File Explorer", ICON_FOLDER, &progress_ops, 0);
    if (progress_w) { window_modal(progress_w); gui_pump(); }
}

static void end_progress(void) {
    if (progress_w) window_close(progress_w);
    progress_w = 0;
}

static int path_exists(const char *path) {
    struct dosxc_stat st;
    return dosxc_sys_stat(path, &st) >= 0;
}

/* DIR\BASE EXT, or DIR\BASE (2) EXT and so on when that is taken. */
static void free_name(const char *dir, const char *base, const char *ext, char *out, size_t size) {
    const char *sep = dir[0] && dir[strlen(dir) - 1] != '\\' ? "\\" : "";
    snprintf(out, size, "%s%s%s%s", dir, sep, base, ext);
    for (int n = 2; n < 1000 && path_exists(out); ++n) snprintf(out, size, "%s%s%s (%d)%s", dir, sep, base, n, ext);
}

static void parent_dir(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s", path);
    char *cut = strrchr(out, '\\');
    if (!cut) { out[0] = 0; return; }
    if (cut == out + 2 && out[1] == ':') cut[1] = 0;
    else *cut = 0;
}

static void select_named(const char *name) {
    for (int i = 0; i < shown_count; ++i)
        if (!strcasecmp(shown[i].name, name)) {
            selected = i;
            if (selected >= list_scroll.top + list_scroll.visible) list_scroll.top = selected - list_scroll.visible + 1;
            break;
        }
    if (main_w) window_paint(main_w);
}

static void run_extract(const char *archive, const char *dest, int open_after) {
    char error[300];
    begin_progress("Extracting", archive);
    struct arc_progress p = {progress_report, 0};
    int r = arc_extract(archive, dest, &p, error, sizeof(error));
    int cancelled = job.cancel;
    end_progress();
    load_folder();
    if (r && !cancelled) message_box("Extract", "The files could not all be extracted.", error);
    else if (!r && open_after) go(dest, 1);
}

static void extract_to_folder(const char *archive) {
    char dir[520], stem[260], dest[600];
    parent_dir(archive, dir, sizeof(dir));
    arc_stem(archive, stem, sizeof(stem));
    free_name(dir, stem, "", dest, sizeof(dest));
    run_extract(archive, dest, 1);
}

static void extract_answer(int answer, void *data) {
    if (answer == 1) extract_to_folder(data);
    free(data);
}

static void ask_extract(const char *path) {
    char question[100];
    snprintf(question, sizeof(question), "Extract the files from \"%.50s\"?", base_name(path));
    char *copy = strdup(path);
    if (copy) ask_box("Extract", question, "Extract", "Not now", extract_answer, copy);
}

static void extract_typed(const char *text, void *data) {
    if (text && text[0]) run_extract(data, text, 1);
    free(data);
}

static void extract_selected(int how) {
    const struct item *it = selected_item();
    if (!it || !folder[0]) return;
    char path[520];
    item_path(it, path, sizeof(path));
    if (how == 0) { run_extract(path, folder, 0); return; }
    if (how == 1) { extract_to_folder(path); return; }
    char stem[260], dest[600];
    arc_stem(path, stem, sizeof(stem));
    free_name(folder, stem, "", dest, sizeof(dest));
    char *copy = strdup(path);
    if (copy) input_box("Extract", "Extract the files to this folder:", dest, extract_typed, copy);
}

static void compress_selected(int format) {
    const struct item *it = selected_item();
    if (!it || !folder[0]) return;
    char source[520], base[260], target[600], error[300];
    item_path(it, source, sizeof(source));
    snprintf(base, sizeof(base), "%s", it->name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base && !(it->attributes & DOSXC_ATTR_DIRECTORY)) *dot = 0;
    free_name(folder, base, arc_extension(format), target, sizeof(target));
    begin_progress("Compressing", source);
    const char *sources[1] = {source};
    struct arc_progress p = {progress_report, 0};
    int r = arc_create(target, format, sources, 1, &p, error, sizeof(error));
    int cancelled = job.cancel;
    end_progress();
    load_folder();
    if (r && !cancelled) message_box("Compress", "The archive could not be made.", error);
    else if (!r) select_named(base_name(target));
}

static void mount_image(const char *path) {
    int r = dosxc_sys_drive(DOSXC_DRIVE_MOUNT, 0, (char *)path, strlen(path) + 1);
    if (r < 0) {
        const char *why = r == -DOSXC_EINVAL ? "It holds no CD (ISO 9660) or FAT volume." :
                          r == -DOSXC_EXDEV ? "Copy it to a disk drive and mount it from there." :
                          r == -DOSXC_EBUSY ? "Eject a disk image first: too many are mounted." :
                          r == -DOSXC_ENOSPC ? "There is no free drive letter for it." : "It could not be mounted.";
        message_box("Mount", why, base_name(path));
        return;
    }
    build_nav();
    char root[4] = {(char)r, ':', '\\', 0};
    go(root, 1);
}

static void eject_drive(char letter) {
    if (!letter) return;
    int r = dosxc_sys_drive(DOSXC_DRIVE_EJECT, letter, 0, 0);
    if (r < 0) {
        char drive[4] = {letter, ':', 0};
        message_box("Eject", "The drive could not be ejected.", drive);
        return;
    }
    build_nav();
    if (folder[0] && toupper((unsigned char)folder[0]) == toupper((unsigned char)letter)) go("", 1);
    else load_folder();
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    FILE *out = in ? fopen(to, "wb") : 0;
    int ok = in && out;
    static char buffer[65536];
    size_t n;
    while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n) ok = 0;
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    return ok ? 0 : -1;
}

static int copy_tree(const char *from, const char *to, int depth) {
    struct dosxc_dirent info;
    (void)info;
    struct dosxc_dirent *list = malloc(sizeof(*list) * 512);
    if (!list) return -1;
    int n = (int)dosxc_sys_listdir(from, list, 512), r = 0;
    if (n < 0) { free(list); return copy_file(from, to); }
    dosxc_sys_mkdir(to);
    for (int i = 0; i < n && i < 512 && !r && depth < 16; ++i) {
        if (!strcmp(list[i].name, ".") || !strcmp(list[i].name, "..")) continue;
        char a[520], b[520];
        snprintf(a, sizeof(a), "%s\\%s", from, list[i].name);
        snprintf(b, sizeof(b), "%s\\%s", to, list[i].name);
        r = (list[i].attributes & DOSXC_ATTR_DIRECTORY) ? copy_tree(a, b, depth + 1) : copy_file(a, b);
    }
    free(list);
    return r;
}

static int delete_tree(const char *path, int is_dir, int depth) {
    if (!is_dir) return dosxc_sys_unlink(path);
    struct dosxc_dirent *list = malloc(sizeof(*list) * 512);
    if (!list) return -1;
    int n = (int)dosxc_sys_listdir(path, list, 512), r = 0;
    for (int i = 0; i < n && i < 512 && !r && depth < 16; ++i) {
        if (!strcmp(list[i].name, ".") || !strcmp(list[i].name, "..")) continue;
        char child[520];
        snprintf(child, sizeof(child), "%s\\%s", path, list[i].name);
        r = delete_tree(child, list[i].attributes & DOSXC_ATTR_DIRECTORY, depth + 1);
    }
    free(list);
    return r ? r : dosxc_sys_rmdir(path);
}

static void delete_answer(int answer, void *data) {
    (void)data;
    if (answer != 1 || selected < 0 || selected >= shown_count) return;
    char path[520];
    item_path(&shown[selected], path, sizeof(path));
    if (delete_tree(path, shown[selected].attributes & DOSXC_ATTR_DIRECTORY, 0)) message_box("File Explorer", "It could not be deleted:", path);
    load_folder();
}

static void delete_selected(void) {
    if (selected < 0 || selected >= shown_count || !folder[0]) return;
    char question[100];
    snprintf(question, sizeof(question), "Delete \"%.60s\"?", shown[selected].name);
    ask_box("Delete", question, "Delete", "Keep", delete_answer, 0);
}

static void start_rename(void) {
    if (selected < 0 || selected >= shown_count || !folder[0]) return;
    renaming = selected;
    field_set(&rename_field, shown[selected].name);
    window_paint(main_w);
}

static void finish_rename(int commit) {
    if (renaming < 0) return;
    if (commit && rename_field.text[0] && strcmp(rename_field.text, shown[renaming].name)) {
        char from[520], to[520];
        item_path(&shown[renaming], from, sizeof(from));
        snprintf(to, sizeof(to), "%s%s%s", folder, folder[strlen(folder) - 1] == '\\' ? "" : "\\", rename_field.text);
        if (dosxc_sys_rename(from, to)) message_box("File Explorer", "It could not be renamed.", rename_field.text);
    }
    renaming = -1;
    load_folder();
}

static void new_folder(void) {
    if (!folder[0]) return;
    char path[520];
    int number = 1;
    for (;;) {
        if (number == 1) snprintf(path, sizeof(path), "%s%sNew folder", folder, folder[strlen(folder) - 1] == '\\' ? "" : "\\");
        else snprintf(path, sizeof(path), "%s%sNew folder (%d)", folder, folder[strlen(folder) - 1] == '\\' ? "" : "\\", number);
        struct dosxc_stat st;
        if (dosxc_sys_stat(path, &st) < 0 || number > 99) break;
        ++number;
    }
    if (dosxc_sys_mkdir(path) < 0) { message_box("File Explorer", "The folder could not be created.", folder); return; }
    load_folder();
    for (int i = 0; i < shown_count; ++i) {
        char candidate[520];
        item_path(&shown[i], candidate, sizeof(candidate));
        if (!strcasecmp(candidate, path)) { selected = i; start_rename(); break; }
    }
}

static void copy_selected(void) {
    if (selected >= 0 && selected < shown_count) item_path(&shown[selected], copied_path, sizeof(copied_path));
    char tagged[320];
    snprintf(tagged, sizeof(tagged), "\001FILE:%s", copied_path);
    gui_clipboard_set(tagged, (int)strlen(tagged));
}

static void paste(void) {
    char tagged[320] = "";
    int n = gui_clipboard_get(tagged, sizeof(tagged) - 1);
    tagged[n] = 0;
    if (strncmp(tagged, "\001FILE:", 6) || !folder[0]) return;
    const char *source = tagged + 6, *name = source;
    for (const char *p = source; *p; ++p)
        if (*p == '\\') name = p + 1;
    char target[520];
    snprintf(target, sizeof(target), "%s%s%s", folder, folder[strlen(folder) - 1] == '\\' ? "" : "\\", name);
    if (!strcasecmp(source, target)) snprintf(target, sizeof(target), "%s%sCopy of %s", folder, folder[strlen(folder) - 1] == '\\' ? "" : "\\", name);
    if (copy_tree(source, target, 0)) message_box("File Explorer", "It could not be pasted here.", name);
    load_folder();
}

enum { A_NONE, A_OPEN, A_COPY, A_PASTE, A_NEW_FOLDER, A_RENAME, A_DELETE, A_WALLPAPER, A_SHORTCUT, A_EXTRACT_HERE,
       A_EXTRACT_FOLDER, A_EXTRACT_ASK, A_ZIP, A_7Z, A_TAR_GZ, A_TAR_XZ, A_MOUNT, A_EJECT };
static int menu_actions[40], menu_count;
static char menu_text[1200];

static void menu_add(const char *label, int action) {
    if (menu_count == 40) return;
    size_t n = strlen(menu_text);
    snprintf(menu_text + n, sizeof(menu_text) - n, "%s\n", label);
    menu_actions[menu_count++] = action;
}

static const char *build_menu(void) {
    menu_text[0] = 0;
    menu_count = 0;
    const struct item *it = selected_item();
    int file = it && folder[0] && !(it->attributes & DOSXC_ATTR_DIRECTORY);
    menu_add("Open", A_OPEN);
    if (file && is_archive(it->name)) {
        char stem[200], label[260];
        arc_stem(it->name, stem, sizeof(stem));
        snprintf(label, sizeof(label), "Extract to \"%.40s\\\"", stem);
        menu_add("Extract here", A_EXTRACT_HERE);
        menu_add(label, A_EXTRACT_FOLDER);
        menu_add("Extract to...", A_EXTRACT_ASK);
    }
    if (file && is_image(it->name)) menu_add("Mount", A_MOUNT);
    if (eject_target()) menu_add("Eject", A_EJECT);
    menu_add("-", A_NONE);
    menu_add("Copy", A_COPY);
    menu_add("Paste", A_PASTE);
    if (it && folder[0]) {
        menu_add("-", A_NONE);
        menu_add("Compress to ZIP (.zip)", A_ZIP);
        menu_add("Compress to 7-Zip (.7z)", A_7Z);
        menu_add("Compress to .tar.gz", A_TAR_GZ);
        menu_add("Compress to .tar.xz", A_TAR_XZ);
    }
    menu_add("-", A_NONE);
    menu_add("New folder", A_NEW_FOLDER);
    menu_add("Rename", A_RENAME);
    menu_add("Delete", A_DELETE);
    menu_add("-", A_NONE);
    if (it && image_is_picture(it->name)) menu_add("Set as wallpaper", A_WALLPAPER);
    menu_add("Add shortcut to desktop", A_SHORTCUT);
    return menu_text;
}

static void shortcut_to_desktop(void) {
    if (selected < 0 || selected >= shown_count) return;
    struct shortcut sc;
    memset(&sc, 0, sizeof(sc));
    item_path(&shown[selected], sc.target, sizeof(sc.target));
    const char *dot = strrchr(shown[selected].name, '.');
    sc.terminal = dot && (!strcasecmp(dot, ".EXE") || !strcasecmp(dot, ".COM") || !strcasecmp(dot, ".BAT"));
    char base[96], folder_path[300], path[420];
    snprintf(base, sizeof(base), "%s", shown[selected].name);
    char *cut = strrchr(base, '.');
    if (cut && cut != base && !(shown[selected].attributes & DOSXC_ATTR_DIRECTORY)) *cut = 0;
    home_path(folder_path, sizeof(folder_path), "Desktop");
    mkdir(folder_path, 0777);
    for (int n = 1; n < 100; ++n) {
        if (n == 1) snprintf(path, sizeof(path), "%s\\%s.LNK", folder_path, base);
        else snprintf(path, sizeof(path), "%s\\%s (%d).LNK", folder_path, base, n);
        FILE *f = fopen(path, "rb");
        if (!f) break;
        fclose(f);
    }
    if (shortcut_write(path, &sc)) message_box("File Explorer", "The shortcut could not be made.", base);
}

static void popup_action(int index) {
    if (index < 0 || index >= menu_count) return;
    switch (menu_actions[index]) {
    case A_OPEN: open_item(selected); break;
    case A_COPY: copy_selected(); break;
    case A_PASTE: paste(); break;
    case A_NEW_FOLDER: new_folder(); break;
    case A_RENAME: start_rename(); break;
    case A_DELETE: delete_selected(); break;
    case A_WALLPAPER:
        if (selected >= 0 && selected < shown_count && image_is_picture(shown[selected].name)) {
            char path[520];
            item_path(&shown[selected], path, sizeof(path));
            gui_set_wallpaper(path);
        }
        break;
    case A_SHORTCUT: shortcut_to_desktop(); break;
    case A_EXTRACT_HERE: extract_selected(0); break;
    case A_EXTRACT_FOLDER: extract_selected(1); break;
    case A_EXTRACT_ASK: extract_selected(2); break;
    case A_ZIP: compress_selected(ARC_ZIP); break;
    case A_7Z: compress_selected(ARC_7Z); break;
    case A_TAR_GZ: compress_selected(ARC_TAR_GZ); break;
    case A_TAR_XZ: compress_selected(ARC_TAR_XZ); break;
    case A_MOUNT:
        if (selected_item()) {
            char path[520];
            item_path(selected_item(), path, sizeof(path));
            mount_image(path);
        }
        break;
    case A_EJECT: eject_drive(eject_target()); break;
    }
}

/* ---- input ---------------------------------------------------------------- */

static void ex_mouse(struct window *w, int x, int y, int kind, int value) {
    struct rect nav = nav_rect(w), list = list_rect(w);
    struct rect nav_bar = {nav.x + nav.w - 14, nav.y, 14, nav.h}, list_bar = {list.x + list.w - 14, list.y + HEADER_H, 14, list.h - HEADER_H};
    if (nav_drag_active || rect_has(nav_bar, x, y) || nav_scroll.dragging) {
        if (vscroll_mouse(&nav_scroll, nav_bar, x, y, kind, value)) { nav_drag_active = nav_scroll.dragging; window_paint(w); return; }
    }
    if (vscroll_mouse(&list_scroll, list_bar, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL) {
        struct vscroll *s = x < NAV_W ? &nav_scroll : &list_scroll;
        s->top -= value * 3;
        vscroll_clamp(s);
        window_paint(w);
        return;
    }
    if (kind == M_RIGHT) {
        if (!rect_has(list, x, y)) return;
        if (y >= list.y + HEADER_H) {
            int at = list_scroll.top + (y - list.y - HEADER_H) / ROW;
            selected = at < shown_count ? at : -1;
        }
        window_paint(w);
        window_popup(w, x, y, build_menu(), 2);
        return;
    }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (renaming >= 0) finish_rename(1);
    search_focus = 0;
    if (rect_has(back_rect(), x, y)) { go_back(); return; }
    if (rect_has(forward_rect(), x, y)) { go_forward(); return; }
    if (rect_has(up_rect(), x, y)) { go_up(); return; }
    if (rect_has(search_rect(w), x, y)) { search_focus = 1; window_paint(w); return; }
    if (y >= ADDRESS_H && y < ADDRESS_H + COMMAND_H) {
        switch (command_at(x)) {
        case CMD_ORGANIZE: window_popup(w, 8, ADDRESS_H + COMMAND_H, build_menu(), 2); break;
        case CMD_NEW_FOLDER: new_folder(); break;
        case CMD_OPEN: open_item(selected); break;
        case CMD_EXTRACT: extract_selected(1); break;
        case CMD_MOUNT: open_item(selected); break;
        case CMD_EJECT: eject_drive(eject_target()); break;
        }
        return;
    }
    if (rect_has(nav, x, y)) {
        int at = nav_scroll.top + (y - nav.y) / NAV_ROW;
        if (at >= node_count) return;
        struct node *n = &nodes[at];
        if (n->header == 2) { go("", 1); return; }
        if (n->header) return;
        int ex = nav.x + nav.w - (node_count > nav_scroll.visible ? 14 : 0) - 16;
        if (n->eject && x >= ex - 3) { eject_drive(n->path[0]); return; }
        int tx = nav.x + 8 + n->depth * 16 - 14;
        if (n->depth > 0 && x >= tx && x < tx + 16) { expand(at); window_paint(w); return; }
        char target[260];
        snprintf(target, sizeof(target), "%s", n->path);
        go(target, 1);
        return;
    }
    if (rect_has(list, x, y)) {
        if (y < list.y + HEADER_H) {
            int e[5], col = -1;
            column_edges(list, e);
            for (int i = 0; i < 4; ++i) if (x >= e[i] && x < e[i + 1]) col = i;
            if (col >= 0) {
                sort_reverse = col == sort_column ? !sort_reverse : 0;
                sort_column = col;
                apply_view();
                window_paint(w);
            }
            return;
        }
        int at = list_scroll.top + (y - list.y - HEADER_H) / ROW;
        selected = at < shown_count ? at : -1;
        window_paint(w);
        if (kind == M_DOUBLE) open_item(selected);
    }
}

static void ex_key(struct window *w, int key) {
    if (renaming >= 0) {
        if (key == '\n') finish_rename(1);
        else if (key == 27) finish_rename(0);
        else if (field_key(&rename_field, key)) window_paint(w);
        return;
    }
    if (search_focus) {
        if (key == 27) { search_focus = 0; field_set(&search, ""); apply_view(); window_paint(w); return; }
        if (key == '\n') { search_focus = 0; window_paint(w); return; }
        if (field_key(&search, key)) { apply_view(); window_paint(w); }
        return;
    }
    switch (key) {
    case 0x101: if (selected > 0) --selected; break;
    case 0x102: if (selected + 1 < shown_count) ++selected; break;
    case '\n': open_item(selected); return;
    case '\b': go_up(); return;
    case 3: copy_selected(); return;
    case 22: paste(); return;
    case 0x108: delete_selected(); return;
    case 0x10c: start_rename(); return;                /* F2 */
    default: return;
    }
    if (selected >= 0 && selected < list_scroll.top) list_scroll.top = selected;
    if (selected >= list_scroll.top + list_scroll.visible) list_scroll.top = selected - list_scroll.visible + 1;
    window_paint(w);
}

static void ex_popup(struct window *w, int tag, int index) {
    (void)w;
    (void)tag;
    if (index >= 0) popup_action(index);
}

static void ex_tick(struct window *w) {
    static int counter, last_volumes = -1;
    if (++counter < 30) return;
    counter = 0;
    struct dosxc_volume volumes[26];
    int n = dosxc_sys_volumes(volumes, 26);
    if (last_volumes != -1 && n != last_volumes) {
        build_nav();
        if (folder[0] && !volume_info(folder[0])) go("", 0);
        else if (!folder[0]) load_folder();
        window_paint(w);
    }
    last_volumes = n;
}

static const struct app_ops ex_ops = {ex_paint, ex_mouse, ex_key, 0, ex_popup, ex_tick};

int main(int argc, char **argv) {
    if (gui_connect()) return 1;
    items = calloc(MAX_FILES, sizeof(*items));
    shown = calloc(MAX_FILES, sizeof(*shown));
    if (!items || !shown) return 1;
    build_nav();
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    snprintf(folder, sizeof(folder), "%c:\\", info.drive);
    const char *opening = 0;
    if (argc > 2 && !strcmp(argv[1], "/open")) {
        opening = argv[2];
        parent_dir(opening, folder, sizeof(folder));
    } else if (argc > 1) snprintf(folder, sizeof(folder), "%s", argv[1]);
    main_w = window_open(860, 540, "File Explorer", ICON_FOLDER, &ex_ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    load_folder();
    if (opening) {
        select_named(base_name(opening));
        if (is_image(opening)) mount_image(opening);
        else if (is_archive(opening)) ask_extract(opening);
    }
    gui_run();
    return 0;
}
