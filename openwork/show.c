/*
 * DOS-XC /openwork/show.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "suite.h"
#include "deck.h"
#include <strings.h>

#define PANEL_W 168
#define THUMB_W 120
#define THUMB_H 68
#define THUMB_STEP 84
#define NOTES_H 70
#define BOX_NOTES 2
#define MAX_LINES 300

/* A wrapped line of a text box: characters START to END, drawn at X, Y; FROM is where
   its paragraph's leading tabs begin on the first line. */
struct tline { int from, start, end, x, y, h, scale, bullet; };

static struct deck deck;
static struct window *main_window, *show_window;
static int cur, box = -1, caret, anchor, dirty, selecting, show_notes = 1, panel_top, showing;
static struct ribbon rb;
static struct surface thumb_full, thumb_small;

enum { C_PASTE, C_CUT, C_COPY, C_NEW_SLIDE, C_LAYOUT, C_DUPLICATE, C_DELETE, C_SELECT_ALL, C_THEMES,
       C_FROM_START, C_FROM_CURRENT, C_NOTES, C_INDENT, C_OUTDENT };

static const struct rb_item clipboard_items[] = {
    {RB_LARGE, C_PASTE, IC_PASTE, 0, 0, 0, "Paste"},
    {RB_SMALL, C_CUT, IC_CUT, 0, 0, RB_LABEL, "Cut"},
    {RB_SMALL, C_COPY, IC_COPY, 1, 0, RB_LABEL, "Copy"},
};
static const struct rb_item slide_items[] = {
    {RB_LARGE, C_NEW_SLIDE, IC_NEW_SLIDE, 0, 0, RB_MENU, "New\nSlide"},
    {RB_SMALL, C_LAYOUT, IC_LAYOUT, 0, 0, RB_LABEL | RB_MENU, "Layout"},
    {RB_SMALL, C_DUPLICATE, IC_DUPLICATE, 1, 0, RB_LABEL, "Duplicate"},
    {RB_SMALL, C_DELETE, IC_DELETE, 2, 0, RB_LABEL, "Delete"},
};
static const struct rb_item paragraph_items[] = {
    {RB_SMALL, C_OUTDENT, IC_ALIGN_LEFT, 0, 0, RB_LABEL, "Decrease List Level"},
    {RB_SMALL, C_INDENT, IC_BULLETS, 1, 0, RB_LABEL, "Increase List Level"},
};
static const struct rb_item editing_items[] = {{RB_SMALL, C_SELECT_ALL, IC_SELECT_ALL, 0, 0, RB_LABEL, "Select All"}};
static const struct rb_group home_groups[] = {
    {"Clipboard", clipboard_items, 3}, {"Slides", slide_items, 4}, {"Paragraph", paragraph_items, 2}, {"Editing", editing_items, 1},
};
static const struct rb_item theme_items[] = {{RB_GALLERY, C_THEMES, 0, 0, 78, 0, "Themes", DECK_THEMES}};
static const struct rb_group design_groups[] = {{"Themes", theme_items, 1}};
static const struct rb_item start_items[] = {
    {RB_LARGE, C_FROM_START, IC_PLAY, 0, 0, 0, "From\nBeginning"},
    {RB_LARGE, C_FROM_CURRENT, IC_PLAY_CURRENT, 0, 0, 0, "From\nCurrent Slide"},
};
static const struct rb_group show_groups[] = {{"Start Slide Show", start_items, 2}};
static const struct rb_item view_items[] = {{RB_LARGE, C_NOTES, IC_DRAFTS, 0, 0, 0, "Notes"}};
static const struct rb_group view_groups[] = {{"Show", view_items, 1}};
static const struct rb_tab tabs[] = {
    {"HOME", home_groups, 4}, {"DESIGN", design_groups, 1}, {"SLIDE SHOW", show_groups, 1}, {"VIEW", view_groups, 1},
};

static struct slide *slide(void) { return &deck.slides[cur]; }
static char **field_of(int b) { return b == BOX_NOTES ? &slide()->notes : &slide()->text[b]; }

static int char_width(const struct font *f, char c, int scale) {
    if (c == '\t') return (f->widths[' '] + f->spacing) * 4 * scale;
    return (f->widths[(unsigned char)c] + f->spacing) * scale;
}

/* Wraps S into AREA (pixels) for a box of POINTS-sized text at K pixels per point. */
static int text_layout(const char *s, struct rect area, int points, int bold, int bullets, int align, int anchor_at, double k, struct tline *out) {
    const struct font *f = bold ? &font_bold : &font_regular;
    int pad = (int)(7 * k) + 1, width = area.w - 2 * pad, n = 0, y = 0, idx = 0;
    for (;;) {
        int from = idx;
        while (s[idx] && s[idx] != '\n') ++idx;
        int end_para = idx, level = 0;
        while (from + level < end_para && s[from + level] == '\t' && bullets) ++level;
        int scale = (int)(points * k / 11.0 + 0.5);
        if (scale < 1) scale = 1;
        int lh = (f->height + 5) * scale, indent = bullets ? (int)((level * 36 + 27) * k) : 0, room = width - indent;
        int start = from + level;
        do {
            int x = 0, end = start, last_space = -1;
            while (end < end_para) {
                int cw = char_width(f, s[end], scale);
                if (x + cw > room && end > start) break;
                if (s[end] == ' ') last_space = end;
                x += cw;
                ++end;
            }
            if (end < end_para && last_space >= start) end = last_space + 1;
            int line_width = 0;
            for (int i = start; i < end; ++i) line_width += char_width(f, s[i], scale);
            int first = start == from + level;
            if (n < MAX_LINES) out[n++] = (struct tline){first ? from : start, start, end, indent + (align ? (room - line_width) / 2 : 0), y, lh, scale, bullets && first};
            y += lh;
            start = end;
        } while (start < end_para);
        y += (int)(6 * k);
        if (!s[idx]) break;
        ++idx;
    }
    int room_h = area.h - 2 * pad, dy = anchor_at == 1 ? (room_h - y) / 2 : anchor_at == 2 ? room_h - y : 0;
    if (dy < 0) dy = 0;
    for (int i = 0; i < n; ++i) { out[i].x += area.x + pad; out[i].y += area.y + pad + dy; }
    return n;
}

static struct rect box_rect(struct rect slide_r, const struct box_style *b) {
    double k = slide_r.w / (double)SLIDE_W;
    return (struct rect){slide_r.x + (int)(b->x * k), slide_r.y + (int)(b->y * k), (int)(b->w * k), (int)(b->h * k)};
}

static void dashed(const struct canvas *c, struct rect r, u32 color) {
    for (int x = r.x; x < r.x + r.w; x += 6) { fill(c, x, r.y, 3, 1, color); fill(c, x, r.y + r.h - 1, 3, 1, color); }
    for (int y = r.y; y < r.y + r.h; y += 6) { fill(c, r.x, y, 1, 3, color); fill(c, r.x + r.w - 1, y, 1, 3, color); }
}

static int line_of(const struct tline *lines, int n, int at) {
    int found = 0;
    for (int i = 0; i < n; ++i)
        if (lines[i].from <= at) found = i;
    return found;
}

static int caret_x(const struct tline *l, const char *s, const struct font *f, int at) {
    int x = l->x;
    for (int i = l->start; i < at && i < l->end; ++i) x += char_width(f, s[i], l->scale);
    return x;
}

static void paint_text(const struct canvas *c, const char *s, const struct tline *lines, int n, int bold, u32 color, int focused) {
    const struct font *f = bold ? &font_bold : &font_regular;
    int lo = caret < anchor ? caret : anchor, hi = caret < anchor ? anchor : caret;
    for (int li = 0; li < n; ++li) {
        const struct tline *l = &lines[li];
        if (l->bullet) fill(c, l->x - (int)(l->scale * 9), l->y + l->h / 2 - l->scale * 2, l->scale * 3, l->scale * 3, color);
        int x = l->x;
        for (int i = l->start; i < l->end; ++i) {
            int cw = char_width(f, s[i], l->scale);
            if (focused && i >= lo && i < hi) fill(c, x, l->y, cw, l->h - l->scale * 2, 0xb4d5fe);
            char one[2] = {s[i] == '\t' ? ' ' : s[i], 0};
            ow_text_scaled(c, x, l->y + l->scale * 2, f, one, color, l->scale);
            x += cw;
        }
    }
    if (focused && n) {
        const struct tline *l = &lines[line_of(lines, n, caret)];
        fill(c, caret_x(l, s, f, caret), l->y, l->scale > 1 ? 2 : 1, l->h - l->scale * 2, color);
    }
}

/* Draws a slide into R; EDITING adds prompts in empty boxes and the caret. */
static void paint_slide(const struct canvas *c, struct rect r, const struct slide *s, int editing) {
    const struct deck_theme *th = &deck_themes[deck.theme];
    fill(c, r.x, r.y, r.w, r.h, th->background);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, r);
    double k = r.w / (double)SLIDE_W;
    static struct tline lines[MAX_LINES];
    for (int b = 0; b < BOX_COUNT; ++b) {
        struct box_style st;
        if (!box_style(s->layout, b, &st)) continue;
        struct rect br = box_rect(r, &st);
        int focused = editing && box == b, empty = !s->text[b][0];
        u32 color = b == BOX_TITLE ? th->title : th->text;
        if (editing && (focused || empty)) dashed(&inner, br, focused ? 0x707070 : mix(th->background, th->text, 45));
        if (empty && editing && !focused) {
            int n = text_layout(box_prompt(s->layout, b), br, st.points, b == BOX_TITLE, 0, st.align, st.anchor, k, lines);
            paint_text(&inner, box_prompt(s->layout, b), lines, n, b == BOX_TITLE, mix(th->background, th->text, 55), 0);
            continue;
        }
        int n = text_layout(s->text[b], br, st.points, b == BOX_TITLE, st.bullets, st.align, st.anchor, k, lines);
        paint_text(&inner, s->text[b], lines, n, b == BOX_TITLE, color, focused);
    }
}

static struct rect panel_rect(struct window *w) { return (struct rect){0, RIBBON_H, PANEL_W, w->r.h - RIBBON_H - STATUS_H}; }
static struct rect notes_rect(struct window *w) {
    return show_notes ? (struct rect){PANEL_W, w->r.h - STATUS_H - NOTES_H, w->r.w - PANEL_W, NOTES_H} : (struct rect){0, 0, 0, 0};
}
static struct rect slide_rect(struct window *w) {
    struct rect area = {PANEL_W, RIBBON_H, w->r.w - PANEL_W, w->r.h - RIBBON_H - STATUS_H - (show_notes ? NOTES_H : 0)};
    int width = area.w - 48, height = width * SLIDE_H / SLIDE_W;
    if (height > area.h - 40) { height = area.h - 40; width = height * SLIDE_W / SLIDE_H; }
    if (width < 32) width = 32;
    if (height < 18) height = 18;
    return (struct rect){area.x + (area.w - width) / 2, area.y + (area.h - height) / 2, width, height};
}

static int notes_layout(struct window *w, struct tline *lines) {
    struct rect nr = notes_rect(w);
    nr.x += 12;
    nr.w -= 24;
    return text_layout(slide()->notes, nr, 11, 0, 0, 0, 0, 1.0, lines);
}

static void paint_panel(struct window *w, const struct canvas *c) {
    struct rect p = panel_rect(w);
    fill(c, p.x, p.y, p.w, p.h, 0xffffff);
    fill(c, p.x + p.w - 1, p.y, 1, p.h, OW_BORDER);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, p);
    if (!thumb_full.px) surface_size(&thumb_full, 320, 180);
    if (!thumb_small.px) surface_size(&thumb_small, THUMB_W, THUMB_H);
    for (int i = 0; i < deck.count; ++i) {
        int y = p.y + 12 + i * THUMB_STEP - panel_top;
        if (y + THUMB_STEP < p.y || y > p.y + p.h) continue;
        int x = p.x + 30;
        struct canvas full = {&thumb_full, 0, 0, {0, 0, 320, 180}};
        paint_slide(&full, (struct rect){0, 0, 320, 180}, &deck.slides[i], 0);
        image_scale(&thumb_full, &thumb_small);
        char number[8];
        snprintf(number, sizeof(number), "%d", i + 1);
        text(&inner, x - 10 - text_width(&font_regular, number), y + 2, &font_regular, number, i == cur ? app->accent : OW_TEXT);
        if (i == cur) {
            fill(&inner, x - 3, y - 3, THUMB_W + 6, THUMB_H + 6, app->accent);
            fill(&inner, x - 1, y - 1, THUMB_W + 2, THUMB_H + 2, 0xffffff);
        } else frame_box(&inner, x - 1, y - 1, THUMB_W + 2, THUMB_H + 2, 0xc6c6c6);
        blit(&inner, x, y, &thumb_small);
    }
}

static void paint(struct window *w, const struct canvas *c) {
    if (ribbon_paint(&rb, w, c)) return;
    paint_panel(w, c);
    struct rect area = {PANEL_W, RIBBON_H, w->r.w - PANEL_W, w->r.h - RIBBON_H - STATUS_H};
    fill(c, area.x, area.y, area.w, area.h, 0xe3e3e3);
    struct rect sr = slide_rect(w);
    fill(c, sr.x + 2, sr.y + 2, sr.w, sr.h, 0xc0c0c0);
    frame_box(c, sr.x - 1, sr.y - 1, sr.w + 2, sr.h + 2, 0xc6c6c6);
    paint_slide(c, sr, slide(), 1);
    if (show_notes) {
        struct rect nr = notes_rect(w);
        fill(c, nr.x, nr.y, nr.w, nr.h, 0xffffff);
        fill(c, nr.x, nr.y, nr.w, 1, OW_BORDER);
        struct canvas inner = *c;
        inner.clip = rect_both(c->clip, (struct rect){nr.x + 1, nr.y + 1, nr.w - 2, nr.h - 2});
        static struct tline lines[MAX_LINES];
        if (!slide()->notes[0] && box != BOX_NOTES) text(&inner, nr.x + 20, nr.y + 12, &font_regular, "Click to add notes", OW_FAINT);
        else paint_text(&inner, slide()->notes, lines, notes_layout(w, lines), 0, 0x262626, box == BOX_NOTES);
    }
    char status[48];
    snprintf(status, sizeof(status), "SLIDE %d OF %d", cur + 1, deck.count);
    ow_status(c, w->r.h - STATUS_H, w->r.w, status, show_notes ? "[NOTES]" : "NOTES");
}

static void keep_thumb_visible(struct window *w) {
    struct rect p = panel_rect(w);
    int y = 12 + cur * THUMB_STEP;
    if (y < panel_top) panel_top = y - 12;
    if (y + THUMB_STEP > panel_top + p.h) panel_top = y + THUMB_STEP - p.h + 12;
    if (panel_top < 0) panel_top = 0;
}

static void go(int index) {
    if (index < 0) index = 0;
    if (index >= deck.count) index = deck.count - 1;
    cur = index;
    box = -1;
    caret = anchor = 0;
    keep_thumb_visible(main_window);
}

static void splice(int from, int to, const char *ins, int n) {
    char **f = field_of(box);
    size_t len = strlen(*f);
    char *fresh = malloc(len - (size_t)(to - from) + (size_t)n + 1);
    if (!fresh) return;
    memcpy(fresh, *f, (size_t)from);
    memcpy(fresh + from, ins, (size_t)n);
    memcpy(fresh + from + n, *f + to, len - (size_t)to + 1);
    free(*f);
    *f = fresh;
    caret = anchor = from + n;
    dirty = 1;
}

static void insert_text(const char *s, int n) {
    int lo = caret < anchor ? caret : anchor, hi = caret < anchor ? anchor : caret;
    char *clean = malloc((size_t)n + 1);
    if (!clean) return;
    int m = 0;
    for (int i = 0; i < n; ++i) {
        char ch = s[i];
        if (ch == '\r') continue;
        if (box == BOX_TITLE && (ch == '\n' || ch == '\t')) ch = ' ';
        clean[m++] = ch;
    }
    splice(lo, hi, clean, m);
    free(clean);
}

static void copy_text(int cut) {
    if (box < 0 || caret == anchor) return;
    int lo = caret < anchor ? caret : anchor, hi = caret < anchor ? anchor : caret;
    gui_clipboard_set(*field_of(box) + lo, hi - lo);
    if (cut) splice(lo, hi, "", 0);
}

static void change_level(int more) {
    if (box != BOX_BODY) return;
    struct box_style st;
    if (!box_style(slide()->layout, BOX_BODY, &st) || !st.bullets) return;
    char *s = *field_of(box);
    int start = caret < anchor ? caret : anchor;
    while (start > 0 && s[start - 1] != '\n') --start;
    int keep_caret = caret, keep_anchor = anchor;
    if (more) {
        int level = 0;
        while (s[start + level] == '\t') ++level;
        if (level >= 4) return;
        splice(start, start, "\t", 1);
        caret = keep_caret + 1;
        anchor = keep_anchor + 1;
    } else if (s[start] == '\t') {
        splice(start, start + 1, "", 0);
        caret = keep_caret > start ? keep_caret - 1 : keep_caret;
        anchor = keep_anchor > start ? keep_anchor - 1 : keep_anchor;
    }
}

static void new_slide(int layout) {
    if (layout < 0) layout = slide()->layout == LAYOUT_TITLE ? LAYOUT_CONTENT : slide()->layout;
    if (deck_insert(&deck, cur + 1, layout)) { go(cur + 1); dirty = 1; }
}

static void duplicate_slide(void) {
    struct slide *s = deck_insert(&deck, cur + 1, slide()->layout);
    if (!s) return;
    struct slide copy = deck.slides[cur];
    for (int b = 0; b < BOX_COUNT; ++b) deck_set(&s->text[b], copy.text[b], strlen(copy.text[b]));
    deck_set(&s->notes, copy.notes, strlen(copy.notes));
    go(cur + 1);
    dirty = 1;
}

static void delete_slide(void) {
    if (deck.count == 1) {
        for (int b = 0; b < BOX_COUNT; ++b) deck_set(&slide()->text[b], "", 0);
        deck_set(&slide()->notes, "", 0);
    } else deck_remove(&deck, cur);
    go(cur);
    dirty = 1;
}

/* ---- the slide show: a borderless window over the whole screen ---- */

static void show_paint(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->r.w, w->r.h, 0x000000);
    if (showing >= deck.count) {
        text(c, 24, 20, &font_regular, "End of slide show, click to exit.", 0xffffff);
        return;
    }
    int width = w->r.w, height = width * SLIDE_H / SLIDE_W;
    if (height > w->r.h) { height = w->r.h; width = height * SLIDE_W / SLIDE_H; }
    paint_slide(c, (struct rect){(w->r.w - width) / 2, (w->r.h - height) / 2, width, height}, &deck.slides[showing], 0);
}

static void show_step(struct window *w, int by) {
    showing += by;
    if (showing < 0) showing = 0;
    if (showing > deck.count) { window_close(w); return; }
    window_paint(w);
}

static void show_mouse(struct window *w, int x, int y, int kind, int value) {
    if (kind == M_DOWN) show_step(w, 1);
    else if (kind == M_RIGHT) show_step(w, -1);
    else if (kind == M_WHEEL) show_step(w, value > 0 ? -1 : 1);
}

static void show_key(struct window *w, int k) {
    if (k == 27) window_close(w);
    else if (k == 0x103 || k == 0x102 || k == 0x10a || k == ' ' || k == '\n' || k == 'n' || k == 'N') show_step(w, 1);
    else if (k == 0x104 || k == 0x101 || k == 0x109 || k == '\b' || k == 'p' || k == 'P') show_step(w, -1);
    else if (k == 0x105) { showing = 0; window_paint(w); }
    else if (k == 0x106) { showing = deck.count - 1; window_paint(w); }
}

static void show_closed(struct window *w) {
    show_window = 0;
    if (showing < deck.count) go(showing);
    window_paint(main_window);
}

static const struct app_ops show_ops = {show_paint, show_mouse, show_key, show_closed};

static void start_show(int from) {
    if (show_window) return;
    showing = from;
    window_popup_next(0, 0);
    show_window = window_open(gui_board()->screen_w, gui_board()->screen_h, "Slide Show", ICON_DISPLAY, &show_ops, 0);
    if (show_window) window_modal(show_window);
}

/* ---- files ---- */

static void reset(void) {
    deck_free(&deck);
    deck_init(&deck);
    deck_insert(&deck, 0, LAYOUT_TITLE);
    cur = 0;
    box = -1;
    caret = anchor = panel_top = dirty = 0;
}

static void new_file(void) {
    reset();
    suite_document(main_window, "");
    window_paint(main_window);
}

static void open_file(const char *path) {
    if (!strcasecmp(extension_of(path), "gslides")) { open_in_browser(path); return; }
    struct deck fresh;
    char error[160] = "";
    deck_init(&fresh);
    if (deck_load(&fresh, path, error, sizeof(error))) {
        deck_free(&fresh);
        message_box("OpenShow", "The presentation could not be opened.", error);
        return;
    }
    reset();
    deck_free(&deck);
    deck = fresh;
    suite_document(main_window, path);
    window_paint(main_window);
}

static void saved(const char *path, void *data) {
    if (!path) return;
    char chosen[300], error[160] = "";
    snprintf(chosen, sizeof(chosen), "%s%s", path, extension_of(path)[0] ? "" : ".pptx");
    if (deck_save(&deck, chosen, error, sizeof(error))) { message_box("OpenShow", "The presentation could not be saved.", error); return; }
    dirty = 0;
    suite_document(main_window, chosen);
    window_paint(main_window);
}

static void save_as(void) { file_dialog("Save As (.pptx, .odp)", 1, 0, saved, 0); }
static void save(void) {
    const char *ext = extension_of(suite_path);
    if (suite_path[0] && (!strcasecmp(ext, "pptx") || !strcasecmp(ext, "odp"))) saved(suite_path, 0);
    else save_as();
}

static void info(char *out, size_t size) {
    int words = 0, notes = 0;
    for (int i = 0; i < deck.count; ++i) {
        for (int b = 0; b < BOX_COUNT; ++b)
            for (const char *p = deck.slides[i].text[b]; *p; ++p)
                if (*p != ' ' && *p != '\n' && *p != '\t' && (p == deck.slides[i].text[b] || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t')) ++words;
        if (deck.slides[i].notes[0]) ++notes;
    }
    snprintf(out, size, "Slides\t%d\nWords\t%d\nSlides with notes\t%d\nTheme\t%s\nChanged\t%s\n", deck.count, words, notes, deck_themes[deck.theme].name, dirty ? "Yes, not saved" : "No");
}

/* ---- ribbon ---- */

static const char *menu(int command) {
    static char items[200];
    items[0] = 0;
    if (command == C_NEW_SLIDE || command == C_LAYOUT)
        for (int i = 0; i < LAYOUT_COUNT; ++i) { strcat(items, layout_names[i]); strcat(items, "\n"); }
    else if (command == C_THEMES)
        for (int i = 0; i < DECK_THEMES; ++i) { strcat(items, deck_themes[i].name); strcat(items, "\n"); }
    return items;
}

static int state(int command) {
    switch (command) {
    case C_CUT: case C_COPY: return box >= 0 && caret != anchor ? 0 : RB_OFF;
    case C_PASTE: return box >= 0 && gui_board()->clipboard_size > 0 ? 0 : RB_OFF;
    case C_SELECT_ALL: return box >= 0 ? 0 : RB_OFF;
    case C_NOTES: return show_notes ? RB_ON : 0;
    case C_INDENT: case C_OUTDENT: {
        struct box_style st;
        return box == BOX_BODY && box_style(slide()->layout, BOX_BODY, &st) && st.bullets ? 0 : RB_OFF;
    }
    }
    return 0;
}

static void command(int command, int choice) {
    switch (command) {
    case C_PASTE: {
        static char buffer[16384];
        int n = box >= 0 ? gui_clipboard_get(buffer, sizeof(buffer) - 1) : 0;
        if (n > 0 && buffer[0] != 1) insert_text(buffer, n);
        break;
    }
    case C_CUT: copy_text(1); break;
    case C_COPY: copy_text(0); break;
    case C_NEW_SLIDE: new_slide(choice); break;
    case C_LAYOUT: if (choice >= 0 && choice < LAYOUT_COUNT) { slide()->layout = choice; box = -1; dirty = 1; } break;
    case C_DUPLICATE: duplicate_slide(); break;
    case C_DELETE: delete_slide(); break;
    case C_SELECT_ALL: if (box >= 0) { anchor = 0; caret = (int)strlen(*field_of(box)); } break;
    case C_THEMES: if (choice >= 0 && choice < DECK_THEMES) { deck.theme = choice; dirty = 1; } break;
    case C_FROM_START: start_show(0); break;
    case C_FROM_CURRENT: start_show(cur); break;
    case C_NOTES: show_notes = !show_notes; if (!show_notes && box == BOX_NOTES) box = -1; break;
    case C_INDENT: change_level(1); break;
    case C_OUTDENT: change_level(0); break;
    }
    window_paint(main_window);
}

static void gallery_paint(const struct canvas *c, struct rect r, int command, int index, int chosen) {
    const struct deck_theme *th = &deck_themes[index];
    struct rect s = {r.x + 4, r.y + 4, r.w - 8, (r.w - 8) * 9 / 16};
    fill(c, s.x, s.y, s.w, s.h, th->background);
    frame_box(c, s.x, s.y, s.w, s.h, 0xc6c6c6);
    ow_text_scaled(c, s.x + 6, s.y + 8, &font_bold, "Aa", th->title, 2);
    fill(c, s.x + 1, s.y + s.h - 5, s.w - 2, 4, th->accent);
    text(c, r.x + (r.w - text_width(&font_regular, th->name)) / 2, r.y + r.h - 14, &font_regular, th->name, OW_TEXT);
}

static int gallery_selected(int command) { return deck.theme; }

static void popup(struct window *w, int tag, int index) { ribbon_popup(&rb, w, tag, index); }

/* ---- input ---- */

static int hit_box(struct window *w, int x, int y, int *at) {
    static struct tline lines[MAX_LINES];
    struct rect sr = slide_rect(w);
    double k = sr.w / (double)SLIDE_W;
    int n = -1, which = -1;
    if (show_notes && rect_has(notes_rect(w), x, y)) { which = BOX_NOTES; n = notes_layout(w, lines); }
    else
        for (int b = BOX_COUNT - 1; b >= 0; --b) {
            struct box_style st;
            if (!box_style(slide()->layout, b, &st) || !rect_has(box_rect(sr, &st), x, y)) continue;
            which = b;
            n = text_layout(slide()->text[b], box_rect(sr, &st), st.points, b == BOX_TITLE, st.bullets, st.align, st.anchor, k, lines);
            break;
        }
    if (which < 0) return -1;
    const char *s = which == BOX_NOTES ? slide()->notes : slide()->text[which];
    const struct font *f = which == BOX_TITLE ? &font_bold : &font_regular;
    *at = (int)strlen(s);
    for (int i = 0; i < n; ++i) {
        const struct tline *l = &lines[i];
        if (y >= l->y + l->h && i + 1 < n) continue;
        int cx = l->x, a = l->start;
        while (a < l->end) {
            int cw = char_width(f, s[a], l->scale);
            if (cx + cw / 2 > x) break;
            cx += cw;
            ++a;
        }
        *at = a;
        break;
    }
    return which;
}

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (rb.backstage) { ribbon_mouse(&rb, w, x, y, kind); return; }
    if (kind == M_UP) selecting = 0;
    if (!selecting && ribbon_mouse(&rb, w, x, y, kind)) return;
    struct rect p = panel_rect(w);
    if (kind == M_WHEEL) {
        if (rect_has(p, x, y)) {
            panel_top -= value * 40;
            int most = deck.count * THUMB_STEP + 12 - p.h;
            if (panel_top > most) panel_top = most;
            if (panel_top < 0) panel_top = 0;
        } else go(cur - value);
        window_paint(w);
        return;
    }
    if (kind == M_DOWN && y >= w->r.h - STATUS_H) {
        if (x > w->r.w - 90) command(C_NOTES, -1);
        return;
    }
    if (kind == M_DOWN && rect_has(p, x, y)) {
        int i = (y - p.y - 6 + panel_top) / THUMB_STEP;
        if (i >= 0 && i < deck.count) go(i);
        window_paint(w);
        return;
    }
    if (kind == M_DOUBLE && rect_has(p, x, y)) { start_show(cur); return; }
    int at;
    if (kind == M_DOWN || kind == M_DOUBLE) {
        int which = hit_box(w, x, y, &at);
        box = which;
        caret = anchor = which >= 0 ? at : 0;
        selecting = which >= 0;
        if (kind == M_DOUBLE && which >= 0) {
            const char *s = *field_of(box);
            while (anchor > 0 && s[anchor - 1] != ' ' && s[anchor - 1] != '\n') --anchor;
            while (s[caret] && s[caret] != ' ' && s[caret] != '\n') ++caret;
            selecting = 0;
        }
    } else if (kind == M_MOVE && selecting) {
        int keep = box;
        if (hit_box(w, x, y, &at) == keep) caret = at;
        box = keep;
    } else if (kind != M_UP) return;
    window_paint(w);
}

static void move_line(int dir) {
    static struct tline lines[MAX_LINES];
    int n;
    const struct font *f = box == BOX_TITLE ? &font_bold : &font_regular;
    if (box == BOX_NOTES) n = notes_layout(main_window, lines);
    else {
        struct box_style st;
        struct rect sr = slide_rect(main_window);
        box_style(slide()->layout, box, &st);
        n = text_layout(slide()->text[box], box_rect(sr, &st), st.points, box == BOX_TITLE, st.bullets, st.align, st.anchor, sr.w / (double)SLIDE_W, lines);
    }
    const char *s = *field_of(box);
    int li = line_of(lines, n, caret), target = li + dir;
    if (target < 0 || target >= n) return;
    int x = caret_x(&lines[li], s, f, caret), a = lines[target].start, cx = lines[target].x;
    while (a < lines[target].end) {
        int cw = char_width(f, s[a], lines[target].scale);
        if (cx + cw / 2 > x) break;
        cx += cw;
        ++a;
    }
    caret = a;
}

static void key(struct window *w, int k) {
    if (ribbon_key(&rb, w, k)) return;
    int shift = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0) & 1;
    if (k == 19) { save(); return; }
    if (k == 15) { ribbon_file(&rb, w, 2); return; }
    if (k == 14) { new_file(); return; }
    if (k == 0x10f) { start_show(shift ? cur : 0); return; }
    if (k == 0x10b) { suite_about(); return; }
    if (box < 0) {
        if (k == 0x101 || k == 0x104 || k == 0x109) go(cur - 1);
        else if (k == 0x102 || k == 0x103 || k == 0x10a) go(cur + 1);
        else if (k == 0x105) go(0);
        else if (k == 0x106) go(deck.count - 1);
        else if (k == 0x108) delete_slide();
        else if (k == '\n') new_slide(-1);
        else if (k == 22) command(C_PASTE, -1);
        else if (k >= 32 && k < 256 && k != 127) {
            struct box_style st;
            if (box_style(slide()->layout, BOX_TITLE, &st)) {
                box = BOX_TITLE;
                caret = anchor = (int)strlen(slide()->text[BOX_TITLE]);
                char c = (char)k;
                insert_text(&c, 1);
            }
        } else return;
        window_paint(w);
        return;
    }
    char *s = *field_of(box);
    int len = (int)strlen(s);
    if (k == 27) box = -1;
    else if (k == 0x104) { if (caret > 0) --caret; if (!shift) anchor = caret; }
    else if (k == 0x103) { if (caret < len) ++caret; if (!shift) anchor = caret; }
    else if (k == 0x101 || k == 0x102) { move_line(k == 0x101 ? -1 : 1); if (!shift) anchor = caret; }
    else if (k == 0x105) { while (caret > 0 && s[caret - 1] != '\n') --caret; if (!shift) anchor = caret; }
    else if (k == 0x106) { while (caret < len && s[caret] != '\n') ++caret; if (!shift) anchor = caret; }
    else if (k == '\b') {
        if (caret != anchor) insert_text("", 0);
        else if (caret > 0) splice(caret - 1, caret, "", 0);
    } else if (k == 0x108) {
        if (caret != anchor) insert_text("", 0);
        else if (caret < len) splice(caret, caret + 1, "", 0);
    } else if (k == '\t') {
        if (box == BOX_BODY) {
            struct box_style st;
            if (box_style(slide()->layout, BOX_BODY, &st) && st.bullets) change_level(!shift);
            else insert_text("    ", 4);
        } else if (box == BOX_NOTES) insert_text("    ", 4);
        else if (!shift) { struct box_style st; if (box_style(slide()->layout, BOX_BODY, &st)) { box = BOX_BODY; caret = anchor = (int)strlen(slide()->text[BOX_BODY]); } }
    } else if (k == '\n') {
        if (box == BOX_TITLE) { struct box_style st; if (box_style(slide()->layout, BOX_BODY, &st)) { box = BOX_BODY; caret = anchor = (int)strlen(slide()->text[BOX_BODY]); } }
        else {
            int start = caret;
            while (start > 0 && s[start - 1] != '\n') --start;
            char lead[8] = "\n";
            int n = 1;
            while (box == BOX_BODY && s[start + n - 1] == '\t' && n < 5) lead[n++] = '\t';
            insert_text(lead, n);
        }
    } else if (k == 1) command(C_SELECT_ALL, -1);
    else if (k == 3) copy_text(0);
    else if (k == 24) copy_text(1);
    else if (k == 22) command(C_PASTE, -1);
    else if (k >= 32 && k < 256 && k != 127) { char c = (char)k; insert_text(&c, 1); }
    else return;
    window_paint(w);
}

static void ask_save(int answer, void *data) {
    if (answer == 1) save();
    if (answer == 0 || (answer == 1 && !dirty)) { dirty = 0; window_close(main_window); }
}

static int closing(struct window *w) {
    if (show_window) window_close(show_window);
    if (!dirty) return 1;
    ask_box("OpenShow", "Save the changes to this presentation?", "Save", "Don't Save", ask_save, 0);
    return 0;
}

static const struct app_ops ops = {paint, mouse, key, 0, popup, 0, 0, closing};

static const struct suite_app show_app = {
    "OpenShow", "SHOW", "Create, present and read PowerPoint and OpenDocument slides.",
    "Presentation1", "presentation", "Presentations",
    "PowerPoint Presentation (.pptx)\nOpenDocument Presentation (.odp)\n\nType a name ending in one of these;\nwith no ending the presentation is saved as .pptx.",
    0xc42b1c, 0xffffff, BS_NEW | BS_OPEN | BS_SAVE | BS_SAVE_AS,
    new_file, open_file, save, save_as, info, 0,
};

int main(int argc, char **argv) {
    if (gui_connect()) { puts("OpenShow needs the DOS-XC desktop."); return 1; }
    suite_start(&show_app, argv[0]);
    rb = (struct ribbon){tabs, 4, 0, command, state, menu, 0, gallery_paint, gallery_selected};
    rb.hover = -1;
    reset();
    int width = gui_board()->screen_w - 80, height = gui_board()->screen_h - 120;
    if (width > 980) width = 980;
    if (height > 660) height = 660;
    main_window = window_open(width < 600 ? 600 : width, height < 420 ? 420 : height, "Presentation1 - OpenShow", ICON_DISPLAY, &ops, 0);
    if (!main_window) return 1;
    main_window->resizable = 1;
    main_window->track_mouse = 1;
    if (argc > 1 && argv[1][0]) open_file(argv[1]);
    gui_run();
    return 0;
}
