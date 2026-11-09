/*
 * DOS-XC /desktop/explorer.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * EXPLORER.EXE: File Explorer, arranged like the one in Windows 7: back and
 * forward buttons, an address bar and search box, a command bar, a
 * navigation pane on the left, details view, and a details pane below.
 */
#include "wm.h"
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

struct node { char path[260], label[64]; int depth, expanded, kids_loaded, icon, header; };
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
    if (!folder[0]) {
        struct dosxc_volume volumes[26];
        int n = dosxc_sys_volumes(volumes, 26);
        for (int i = 0; i < n && i < MAX_FILES; ++i) {
            memset(&items[i], 0, sizeof(items[i]));
            snprintf(items[i].name, sizeof(items[i].name), "%c:", volumes[i].letter);
            items[i].attributes = DOSXC_ATTR_DIRECTORY;
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
    if (n->expanded) {                  /* collapse */
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
    struct dosxc_volume volumes[26];
    int n = dosxc_sys_volumes(volumes, 26);
    for (int i = 0; i < n && i < 26; ++i) {
        char root[8], label[32];
        snprintf(root, sizeof(root), "%c:\\", volumes[i].letter);
        snprintf(label, sizeof(label), "Local Disk (%c:)", volumes[i].letter);
        add_node(node_count, root, label, 1, ICON_DRIVE, 0);
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

static void paint_command(struct window *w, const struct canvas *c) {
    fill(c, 0, ADDRESS_H, w->content.w, COMMAND_H, C_WINDOW);
    fill(c, 0, ADDRESS_H + COMMAND_H - 1, w->content.w, 1, C_SHADOW1);
    static const char *const labels[] = {"Organize", "New folder", "Open"};
    int x = 8;
    for (int i = 0; i < 3; ++i) {
        int width = text_width(&font_regular, labels[i]) + (i == 0 ? 30 : 18);
        button(c, (struct rect){x, ADDRESS_H + 3, width, 22}, labels[i], 0, 0);
        if (i == 0) for (int k = 0; k < 4; ++k) fill(c, x + width - 12 + k, ADDRESS_H + 12 + k, 7 - 2 * k, 1, C_TEXT);
        x += width + 4;
    }
}

static int command_at(int x) {
    static const char *const labels[] = {"Organize", "New folder", "Open"};
    int left = 8;
    for (int i = 0; i < 3; ++i) {
        int width = text_width(&font_regular, labels[i]) + (i == 0 ? 30 : 18);
        if (x >= left && x < left + width) return i;
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
    else snprintf(out, size, "%llu.%llu MB", s >> 20, ((s >> 10) & 1023) * 10 / 1024);
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
        draw_icon(&inner, e[0] + 4, y + 2, !folder[0] ? ICON_DRIVE : dir ? ICON_FOLDER : program ? ICON_PROGRAM : image_is_picture(it->name) ? ICON_PICTURE : ICON_FILE);
        if (list_scroll.top + i == renaming) {
            field_paint(c, &rename_field, (struct rect){e[0] + 24, y, e[1] - e[0] - 30, ROW}, 1);
        } else {
            struct canvas name_clip = inner;
            name_clip.clip = rect_both(inner.clip, (struct rect){e[0], y, e[1] - e[0] - 4, ROW});
            text(&name_clip, e[0] + 26, y + 5, &font_regular, it->name, C_TEXT);
        }
        char line[40];
        format_date(it->mtime, line, sizeof(line));
        if (folder[0]) text(&inner, e[1] + 6, y + 5, &font_regular, line, 0x505050);
        text(&inner, e[2] + 6, y + 5, &font_regular, !folder[0] ? "Drive" : type_of(it), 0x505050);
        if (!dir && folder[0]) { format_size(it->size, line, sizeof(line)); text(&inner, e[3] + 6, y + 5, &font_regular, line, 0x505050); }
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
        text(c, 44, y + 25, &font_regular, type_of(it), 0x505050);
        if (!(it->attributes & DOSXC_ATTR_DIRECTORY)) {
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

static void open_item(int index) {
    if (index < 0 || index >= shown_count) return;
    const struct item *it = &shown[index];
    char path[520];
    item_path(it, path, sizeof(path));
    if (it->attributes & DOSXC_ATTR_DIRECTORY) go(path, 1);
    else gui_open_file(path);
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

static const char organize_menu[] = "Open\nCopy\nPaste\n-\nNew folder\nRename\nDelete\n-\nSet as wallpaper\n";

static void popup_action(int index) {
    switch (index) {
    case 0: open_item(selected); break;
    case 1: copy_selected(); break;
    case 2: paste(); break;
    case 4: new_folder(); break;
    case 5: start_rename(); break;
    case 6: delete_selected(); break;
    case 8:
        if (selected >= 0 && selected < shown_count && image_is_picture(shown[selected].name)) {
            char path[520];
            item_path(&shown[selected], path, sizeof(path));
            gui_set_wallpaper(path);
        }
        break;
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
        window_popup(w, x, y, organize_menu, 2);
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
        case 0: window_popup(w, 8, ADDRESS_H + COMMAND_H, organize_menu, 2); break;
        case 1: new_folder(); break;
        case 2: open_item(selected); break;
        }
        return;
    }
    if (rect_has(nav, x, y)) {
        int at = nav_scroll.top + (y - nav.y) / NAV_ROW;
        if (at >= node_count) return;
        struct node *n = &nodes[at];
        if (n->header == 2) { go("", 1); return; }
        if (n->header) return;
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
    /* Pick up drives that came or went. */
    static int counter, last_volumes = -1;
    if (++counter < 30) return;
    counter = 0;
    struct dosxc_volume volumes[26];
    int n = dosxc_sys_volumes(volumes, 26);
    if (last_volumes != -1 && n != last_volumes) { build_nav(); window_paint(w); }
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
    if (argc > 1) snprintf(folder, sizeof(folder), "%s", argv[1]);
    main_w = window_open(860, 540, "File Explorer", ICON_FOLDER, &ex_ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    load_folder();
    gui_run();
    return 0;
}
