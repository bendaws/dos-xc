/*
 * DOS-XC /desktop/fonts.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "ttf.h"
#include <strings.h>

#define MAX_FONTS 128
#define LIST_W 220
#define ROW 18
#define BUILT_IN 3

struct entry { char name[128], path[300]; unsigned long long size; int built_in; };

static struct entry fonts[MAX_FONTS];
static int font_count, selected;
static struct vscroll scroll;
static struct window *main_w;
static char folder[64];
static struct entry outside;              /* a font file opened from elsewhere, not installed */
static int showing_outside;
static unsigned char *loaded_data;
static size_t loaded_size;
static char loaded_path[300];
static struct ttf face;
static int face_ok;

static const struct menubar bar = {
    {"File", "Help"},
    {"Add Font...\nRemove Font\nOpen Fonts Folder\n-\nExit\n", "About Fonts\n"},
};
static const char *const built_in_names[BUILT_IN] = {"DOS-XC Sans (built in)", "DOS-XC Sans Bold (built in)", "DOS-XC Mono (built in)"};

static int font_file(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") || !strcasecmp(dot, ".ttc"));
}

static void scan(void) {
    font_count = 0;
    for (int i = 0; i < BUILT_IN; ++i) {
        memset(&fonts[font_count], 0, sizeof(fonts[0]));
        snprintf(fonts[font_count].name, sizeof(fonts[0].name), "%s", built_in_names[i]);
        fonts[font_count++].built_in = i + 1;
    }
    static struct dosxc_dirent list[MAX_FONTS];
    long n = dosxc_sys_listdir(folder, list, MAX_FONTS);
    for (long i = 0; i < n && font_count < MAX_FONTS; ++i) {
        if ((list[i].attributes & DOSXC_ATTR_DIRECTORY) || !font_file(list[i].name)) continue;
        struct entry *e = &fonts[font_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", list[i].name);
        snprintf(e->path, sizeof(e->path), "%s\\%s", folder, list[i].name);
        e->size = list[i].size;
    }
    scroll.total = font_count;
    if (selected >= font_count) selected = font_count - 1;
}

static const struct entry *current_entry(void) { return showing_outside ? &outside : &fonts[selected]; }

static void load_face(void) {
    const struct entry *e = current_entry();
    if (e->built_in) { face_ok = 0; return; }
    if (!strcmp(loaded_path, e->path)) return;
    free(loaded_data);
    loaded_data = 0;
    face_ok = 0;
    snprintf(loaded_path, sizeof(loaded_path), "%s", e->path);
    FILE *f = fopen(e->path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size > 0 && size < (64l << 20) && (loaded_data = malloc((size_t)size)) && fread(loaded_data, 1, (size_t)size, f) == (size_t)size) {
        loaded_size = (size_t)size;
        face_ok = !ttf_open(&face, loaded_data, loaded_size, 0);
    }
    fclose(f);
}

static struct rect list_rect(const struct window *w) { return (struct rect){8, MENUBAR_H + 8, LIST_W, w->content.h - MENUBAR_H - 50}; }
static struct rect button_rect(const struct window *w, int i) { return (struct rect){8 + i * 74, w->content.h - 34, 68, 24}; }

static void paint_preview(const struct canvas *c, struct rect r) {
    const struct entry *e = current_entry();
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){r.x + 2, r.y + 2, r.w - 4, r.h - 4});
    int y = r.y + 12;
    static const char sample[] = "The quick brown fox jumps over the lazy dog.";
    if (e->built_in) {
        const struct font *f = e->built_in == 1 ? &font_regular : e->built_in == 2 ? &font_bold : &font_mono;
        text(&inner, r.x + 12, y, &font_bold, e->name, C_TEXT);
        text(&inner, r.x + 12, y + 18, &font_regular, "Bitmap font used by the desktop; it cannot be removed.", C_SHADOW1);
        y += 46;
        text(&inner, r.x + 12, y, f, "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz", C_TEXT);
        text(&inner, r.x + 12, y + 20, f, "0123456789 .,;:!?\"'()[]{}<>+-*/=@#$%&", C_TEXT);
        text(&inner, r.x + 12, y + 46, f, sample, C_TEXT);
        for (int scale = 2; scale <= 3; ++scale) text_big(&inner, r.x + 12, y + 46 + 28 * (scale - 1), "Aa Bb Cc 123", C_TEXT, scale);
        return;
    }
    char full[128], style[64], line[200];
    if (face_ok) { ttf_name(&face, 4, full, sizeof(full)); ttf_name(&face, 2, style, sizeof(style)); }
    else { snprintf(full, sizeof(full), "%s", e->name); style[0] = 0; }
    text(&inner, r.x + 12, y, &font_bold, full[0] ? full : e->name, C_TEXT);
    int faces = loaded_data ? ttf_faces(loaded_data, loaded_size) : 0;
    snprintf(line, sizeof(line), "%s%s%s, %llu KB%s%s", e->name, style[0] ? ", " : "", style, e->size / 1024,
             faces > 1 ? ", collection of " : "", "");
    if (faces > 1) snprintf(line + strlen(line), sizeof(line) - strlen(line), "%d faces", faces);
    text(&inner, r.x + 12, y + 18, &font_regular, line, C_SHADOW1);
    y += 50;
    if (!face_ok) { text(&inner, r.x + 12, y, &font_regular, "This file is not a font DOS-XC can read.", 0xa00000); return; }
    if (!ttf_has_outlines(&face)) {
        text(&inner, r.x + 12, y, &font_regular, "PostScript (CFF) outlines: installed programs can use it,", C_TEXT);
        text(&inner, r.x + 12, y + 16, &font_regular, "but Fonts cannot draw a preview.", C_TEXT);
        return;
    }
    ttf_draw(&face, &inner, r.x + 12, y + 16, 18, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", C_TEXT);
    ttf_draw(&face, &inner, r.x + 12, y + 40, 18, "abcdefghijklmnopqrstuvwxyz 0123456789", C_TEXT);
    y += 60;
    static const int sizes[] = {12, 18, 24, 36, 48};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes) && y < r.y + r.h; ++i) {
        char label[8];
        snprintf(label, sizeof(label), "%d", sizes[i]);
        y += sizes[i] + 8;
        text(&inner, r.x + 12, y - font_regular.height, &font_regular, label, C_SHADOW1);
        ttf_draw(&face, &inner, r.x + 40, y, sizes[i], sample, C_TEXT);
    }
}

static void paint(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->content.w, w->content.h, C_WINDOW);
    menubar_paint(c, &bar, w->content.w);
    struct rect l = list_rect(w);
    bevel(c, l.x, l.y, l.w, l.h, 1);
    fill(c, l.x + 2, l.y + 2, l.w - 4, l.h - 4, C_BASE);
    scroll.visible = (l.h - 4) / ROW;
    vscroll_clamp(&scroll);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){l.x + 2, l.y + 2, l.w - 20, l.h - 4});
    for (int i = 0; i < scroll.visible && scroll.top + i < font_count; ++i) {
        int at = scroll.top + i, y = l.y + 2 + i * ROW, on = at == selected && !showing_outside;
        if (on) fill(&inner, l.x + 2, y, l.w - 20, ROW, C_SELECTION);
        draw_icon(&inner, l.x + 4, y + 1, fonts[at].built_in ? ICON_DISPLAY : ICON_FILE);
        text(&inner, l.x + 24, y + 4, &font_regular, fonts[at].name, on ? 0xffffff : C_TEXT);
    }
    vscroll_paint(c, &scroll, (struct rect){l.x + l.w - 16, l.y + 2, 14, l.h - 4});
    struct rect p = {LIST_W + 16, MENUBAR_H + 8, w->content.w - LIST_W - 24, w->content.h - MENUBAR_H - 50};
    bevel(c, p.x, p.y, p.w, p.h, 1);
    fill(c, p.x + 2, p.y + 2, p.w - 4, p.h - 4, C_BASE);
    load_face();
    paint_preview(c, p);
    const struct entry *e = current_entry();
    button(c, button_rect(w, 0), "Add...", 0, 0);
    button(c, button_rect(w, 1), "Remove", 0, 0);
    button(c, button_rect(w, 2), "Folder", 0, 0);
    if (showing_outside) button(c, (struct rect){w->content.w - 160, w->content.h - 34, 150, 24}, "Install This Font", 0, 1);
    else if (e->built_in) text(c, w->content.w - 240, w->content.h - 28, &font_regular, "Fonts in \\SYSTEM\\FONTS", C_SHADOW1);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buffer[16384];
    size_t n;
    int failed = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n) { failed = 1; break; }
    fclose(in);
    if (fclose(out) || failed) { remove(to); return -1; }
    return 0;
}

static void install(const char *path) {
    const char *name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    char target[300];
    snprintf(target, sizeof(target), "%s\\%s", folder, name);
    if (!strcasecmp(target, path)) return;
    dosxc_sys_mkdir(folder);
    if (copy_file(path, target)) { message_box("Fonts", "The font could not be installed.", "Only administrators can add system fonts."); return; }
    showing_outside = 0;
    scan();
    for (int i = 0; i < font_count; ++i)
        if (!strcasecmp(fonts[i].path, target)) selected = i;
}

static void chosen(const char *path, void *data) {
    (void)data;
    if (!path) return;
    if (!font_file(path)) { message_box("Fonts", "Choose a .ttf, .otf or .ttc font file.", ""); return; }
    install(path);
    window_paint(main_w);
}

static void removed(int ok, void *data) {
    (void)data;
    if (!ok || selected < 0 || fonts[selected].built_in) return;
    if (dosxc_sys_unlink(fonts[selected].path)) message_box("Fonts", "The font could not be removed.", "Only administrators can remove system fonts.");
    loaded_path[0] = 0;
    scan();
    window_paint(main_w);
}

static void act(struct window *w, int which) {
    if (which == 0) file_dialog("Add a font (.ttf, .otf, .ttc)", 0, 0, chosen, 0);
    else if (which == 1) {
        if (showing_outside || fonts[selected].built_in) message_box("Fonts", "Only installed font files can be removed.", "");
        else {
            char question[200];
            snprintf(question, sizeof(question), "Remove %s from the system fonts?", fonts[selected].name);
            confirm_box("Remove Font", question, "Remove", ICON_FILE, removed, 0);
        }
    } else if (which == 2) gui_open_file(folder);
    window_paint(w);
}

static void popup(struct window *w, int tag, int index) {
    if (tag == 0) {
        if (index >= 0 && index <= 2) act(w, index);
        else if (index == 4) window_close(w);
    } else if (tag == 1 && index == 0)
        message_box("About Fonts", "Fonts lists the system fonts in \\SYSTEM\\FONTS, which programs", "such as DOSfox use. Open a font file to preview and install it.");
    window_paint(w);
}

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    struct rect l = list_rect(w);
    if (vscroll_mouse(&scroll, (struct rect){l.x + l.w - 16, l.y + 2, 14, l.h - 4}, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL && x < l.x + l.w) { scroll.top -= value * 3; vscroll_clamp(&scroll); window_paint(w); return; }
    if (kind != M_DOWN) return;
    for (int i = 0; i < 3; ++i)
        if (rect_has(button_rect(w, i), x, y)) { act(w, i); return; }
    if (showing_outside && rect_has((struct rect){w->content.w - 160, w->content.h - 34, 150, 24}, x, y)) { install(outside.path); window_paint(w); return; }
    if (rect_has(l, x, y) && x < l.x + l.w - 18) {
        int at = scroll.top + (y - l.y - 2) / ROW;
        if (at < font_count) { selected = at; showing_outside = 0; }
    }
    window_paint(w);
}

static void key(struct window *w, int k) {
    if (k == 0x101 && selected > 0) --selected;
    else if (k == 0x102 && selected + 1 < font_count) ++selected;
    else if (k == 0x108) { act(w, 1); return; }
    else return;
    showing_outside = 0;
    if (selected < scroll.top) scroll.top = selected;
    if (selected >= scroll.top + scroll.visible) scroll.top = selected - scroll.visible + 1;
    window_paint(w);
}

static const struct app_ops ops = {paint, mouse, key, 0, popup, 0, 0, 0};

int main(int argc, char **argv) {
    if (gui_connect()) { puts("Fonts needs the DOS-XC desktop."); return 1; }
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    snprintf(folder, sizeof(folder), "%c:\\SYSTEM\\FONTS", info.drive);
    scan();
    if (argc > 1) {
        const char *name = strrchr(argv[1], '\\');
        snprintf(outside.name, sizeof(outside.name), "%s", name ? name + 1 : argv[1]);
        snprintf(outside.path, sizeof(outside.path), "%s", argv[1]);
        struct dosxc_stat st;
        if (!dosxc_sys_stat(argv[1], &st)) outside.size = st.size;
        showing_outside = 1;
        for (int i = 0; i < font_count; ++i)
            if (!strcasecmp(fonts[i].path, argv[1])) { selected = i; showing_outside = 0; }
    }
    main_w = window_open(720, 460, showing_outside ? "Fonts - Preview" : "Fonts", ICON_FILE, &ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    gui_run();
    return 0;
}
