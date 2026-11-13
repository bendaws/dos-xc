/*
 * DOS-XC /openwork/papers.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "suite.h"
#include "doc.h"
#include <strings.h>
#include <time.h>

#define PAGE_W 612
#define PAGE_H 792
#define PAGE_GAP 20
#define SCROLL_W 16
#define LINE_GAP 4

struct pos { int para, at; };
struct line { int para, start, end, y, height, x, page; };

static struct document doc;
static struct window *main_window;
static int dirty, typing_attr, scroll_y, content_height, page_w, margin, page_count;
static struct pos caret, anchor;
static int selecting;
static struct line *lines;
static int line_count, line_capacity;
static struct vscroll scroll;
static struct ribbon rb;
static char find_text[260];

static const char *const style_names[STYLE_COUNT] = {"Normal", "Heading 1", "Heading 2", "Heading 3", "Title", "List"};

enum { C_PASTE, C_CUT, C_COPY, C_BOLD, C_ITALIC, C_UNDERLINE, C_CLEAR_FORMAT, C_BULLETS, C_LEFT, C_CENTER, C_RIGHT,
       C_STYLES, C_FIND, C_REPLACE, C_SELECT_ALL, C_DATE, C_SYMBOL, C_WORD_COUNT };

static const struct rb_item clipboard_items[] = {
    {RB_LARGE, C_PASTE, IC_PASTE, 0, 0, 0, "Paste"},
    {RB_SMALL, C_CUT, IC_CUT, 0, 0, RB_LABEL, "Cut"},
    {RB_SMALL, C_COPY, IC_COPY, 1, 0, RB_LABEL, "Copy"},
};
static const struct rb_item font_items[] = {
    {RB_SMALL, C_BOLD, IC_BOLD, 0}, {RB_SMALL, C_ITALIC, IC_ITALIC, 0}, {RB_SMALL, C_UNDERLINE, IC_UNDERLINE, 0},
    {RB_SMALL, C_CLEAR_FORMAT, IC_CLEAR_FORMAT, 1, 0, RB_LABEL, "Clear Formatting"},
};
static const struct rb_item paragraph_items[] = {
    {RB_SMALL, C_BULLETS, IC_BULLETS, 0, 0, RB_LABEL, "Bullets"},
    {RB_SMALL, C_LEFT, IC_ALIGN_LEFT, 1}, {RB_SMALL, C_CENTER, IC_ALIGN_CENTER, 1}, {RB_SMALL, C_RIGHT, IC_ALIGN_RIGHT, 1},
};
static const struct rb_item style_items[] = {{RB_GALLERY, C_STYLES, 0, 0, 74, 0, "Styles", STYLE_COUNT}};
static const struct rb_item editing_items[] = {
    {RB_SMALL, C_FIND, IC_FIND, 0, 0, RB_LABEL, "Find"},
    {RB_SMALL, C_REPLACE, IC_REPLACE, 1, 0, RB_LABEL, "Replace"},
    {RB_SMALL, C_SELECT_ALL, IC_SELECT_ALL, 2, 0, RB_LABEL, "Select All"},
};
static const struct rb_group home_groups[] = {
    {"Clipboard", clipboard_items, 3}, {"Font", font_items, 4}, {"Paragraph", paragraph_items, 4},
    {"Styles", style_items, 1}, {"Editing", editing_items, 3},
};
static const struct rb_item insert_items[] = {
    {RB_LARGE, C_DATE, IC_DATE, 0, 0, RB_MENU, "Date &\nTime"},
    {RB_LARGE, C_SYMBOL, IC_SYMBOL, 0, 0, RB_MENU, "Symbol"},
};
static const struct rb_group insert_groups[] = {{"Text", insert_items, 2}};
static const struct rb_item proofing_items[] = {{RB_LARGE, C_WORD_COUNT, IC_WORD_COUNT, 0, 0, 0, "Word\nCount"}};
static const struct rb_group review_groups[] = {{"Proofing", proofing_items, 1}};
static const struct rb_tab tabs[] = {{"HOME", home_groups, 5}, {"INSERT", insert_groups, 1}, {"REVIEW", review_groups, 1}};

static int scale_of(int style) { return style == STYLE_TITLE || style == STYLE_HEADING1 ? 2 : 1; }
static const struct font *font_of(int style, int attr) {
    return (attr & ATTR_BOLD) || (style >= STYLE_HEADING1 && style <= STYLE_HEADING3) ? &font_bold : &font_regular;
}
static u32 color_of(int style) {
    return style >= STYLE_HEADING1 && style <= STYLE_HEADING3 ? 0x2e74b5 : style == STYLE_TITLE ? 0x262626 : 0x000000;
}

static int char_width(const struct para *p, int i) {
    char s[2] = {p->text[i] == '\t' ? ' ' : p->text[i], 0};
    int w = text_width(font_of(p->style, p->attr[i]), s) + font_regular.spacing;
    if (p->text[i] == '\t') w *= 4;
    return w * scale_of(p->style);
}

static struct rect view_rect(struct window *w) { return (struct rect){0, RIBBON_H, w->r.w - SCROLL_W, w->r.h - RIBBON_H - STATUS_H}; }
static struct rect scroll_rect(struct window *w) { return (struct rect){w->r.w - SCROLL_W, RIBBON_H, SCROLL_W, w->r.h - RIBBON_H - STATUS_H}; }
static int page_left(struct window *w) { int left = (view_rect(w).w - page_w) / 2; return left < 8 ? 8 : left; }
static int page_top(int page) { return PAGE_GAP + page * (PAGE_H + PAGE_GAP); }

static void add_line(int para, int start, int end, int y, int height, int x, int page) {
    if (line_count == line_capacity) {
        line_capacity = line_capacity ? line_capacity * 2 : 256;
        lines = realloc(lines, sizeof(*lines) * (size_t)line_capacity);
    }
    lines[line_count++] = (struct line){para, start, end, y, height, x, page};
}

/* Word-wrap every paragraph to the page width and break the lines into pages. */
static void layout(struct window *w) {
    line_count = 0;
    struct rect view = view_rect(w);
    page_w = PAGE_W;
    if (page_w > view.w - 32) page_w = view.w - 32 < 240 ? 240 : view.w - 32;
    margin = page_w >= PAGE_W ? 72 : page_w / 8;
    int width = page_w - 2 * margin, page = 0, y = page_top(0) + margin;
    for (int pi = 0; pi < doc.count; ++pi) {
        const struct para *p = &doc.paras[pi];
        int indent = p->style == STYLE_BULLET ? 24 : 0, room = width - indent;
        int height = (font_regular.height + LINE_GAP) * scale_of(p->style);
        if (p->style >= STYLE_HEADING1 && p->style <= STYLE_TITLE && y > page_top(page) + margin) y += 8;
        int start = 0;
        do {
            int x = 0, end = start, last_space = -1;
            while (end < p->length) {
                int cw = char_width(p, end);
                if (x + cw > room && end > start) break;
                if (p->text[end] == ' ') last_space = end;
                x += cw;
                ++end;
            }
            if (end < p->length && last_space >= start) end = last_space + 1;
            int line_width = 0;
            for (int i = start; i < end; ++i) line_width += char_width(p, i);
            int lx = margin + indent + (p->align == ALIGN_CENTER ? (room - line_width) / 2 : p->align == ALIGN_RIGHT ? room - line_width : 0);
            if (y + height > page_top(page) + PAGE_H - margin) y = page_top(++page) + margin;
            add_line(pi, start, end, y, height, lx, page);
            y += height;
            start = end;
        } while (start < p->length);
        y += p->style == STYLE_NORMAL || p->style == STYLE_BULLET ? 6 : p->style == STYLE_TITLE ? 12 : 6;
    }
    page_count = page + 1;
    content_height = page_top(page_count);
}

static int pos_before(struct pos a, struct pos b) { return a.para < b.para || (a.para == b.para && a.at < b.at); }
static int selected(int para, int at) {
    struct pos s = pos_before(anchor, caret) ? anchor : caret, e = pos_before(anchor, caret) ? caret : anchor;
    struct pos x = {para, at};
    return !pos_before(x, s) && pos_before(x, e);
}
static int has_selection(void) { return anchor.para != caret.para || anchor.at != caret.at; }

static void draw_italic(const struct canvas *c, int x, int y, const struct font *f, const char *s, u32 color) {
    int h = f->height;
    for (int row = 0; row < h; ++row) {
        struct canvas band = *c;
        band.clip = rect_both(c->clip, (struct rect){x - 8, y + row, 4000, 1});
        text(&band, x + (h - row) / 4, y, f, s, color);
    }
}

static int caret_line(void) {
    for (int li = 0; li < line_count; ++li)
        if (lines[li].para == caret.para && caret.at >= lines[li].start &&
            (caret.at < lines[li].end || (caret.at == lines[li].end && (li + 1 == line_count || lines[li + 1].para != caret.para))))
            return li;
    return 0;
}

static int word_count(void) {
    int words = 0;
    for (int i = 0; i < doc.count; ++i)
        for (int k = 0; k < doc.paras[i].length; ++k)
            if (doc.paras[i].text[k] != ' ' && doc.paras[i].text[k] != '\t' && (k == 0 || doc.paras[i].text[k - 1] == ' ' || doc.paras[i].text[k - 1] == '\t')) ++words;
    return words;
}

static void sync_scroll(struct window *w) {
    scroll.total = content_height;
    scroll.visible = view_rect(w).h;
    scroll.top = scroll_y;
    vscroll_clamp(&scroll);
    scroll_y = scroll.top;
}

static void paint(struct window *w, const struct canvas *c) {
    if (ribbon_paint(&rb, w, c)) return;
    layout(w);
    sync_scroll(w);
    struct rect view = view_rect(w);
    fill(c, view.x, view.y, view.w, view.h, 0xe6e6e6);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, view);
    int left = view.x + page_left(w);
    for (int k = 0; k < page_count; ++k) {
        int top = view.y + page_top(k) - scroll_y;
        if (top > view.y + view.h || top + PAGE_H < view.y) continue;
        fill(&inner, left + 2, top + 2, page_w, PAGE_H, 0xc8c8c8);
        fill(&inner, left, top, page_w, PAGE_H, 0xffffff);
        frame_box(&inner, left - 1, top - 1, page_w + 2, PAGE_H + 2, 0xd0d0d0);
    }
    int cl = caret_line();
    for (int li = 0; li < line_count; ++li) {
        const struct line *l = &lines[li];
        int y = view.y + l->y - scroll_y;
        if (y + l->height < view.y || y > view.y + view.h) continue;
        const struct para *p = &doc.paras[l->para];
        int scale = scale_of(p->style), x = left + l->x;
        u32 color = color_of(p->style);
        if (p->style == STYLE_BULLET && l->start == 0) fill(&inner, x - 14, y + font_regular.height / 2 - 2, 5, 5, 0x000000);
        for (int i = l->start; i < l->end; ++i) {
            int cw = char_width(p, i);
            if (selected(l->para, i)) fill(&inner, x, y - 2, cw, l->height, 0xc5dcf5);
            char s[2] = {p->text[i] == '\t' ? ' ' : p->text[i], 0};
            const struct font *f = font_of(p->style, p->attr[i]);
            if (scale > 1) ow_text_scaled(&inner, x, y, f, s, color, scale);
            else if (p->attr[i] & ATTR_ITALIC) draw_italic(&inner, x, y, f, s, color);
            else text(&inner, x, y, f, s, color);
            if (p->attr[i] & ATTR_UNDERLINE) fill(&inner, x, y + f->height * scale, cw, scale, color);
            x += cw;
        }
        if (li == cl) {
            int cx = left + l->x;
            for (int i = l->start; i < caret.at; ++i) cx += char_width(p, i);
            fill(&inner, cx, y - 2, 1, l->height, 0x000000);
        }
    }
    ow_vscroll_paint(c, &scroll, scroll_rect(w));
    char status[96];
    int words = word_count();
    snprintf(status, sizeof(status), "PAGE %d OF %d     %d WORD%s", line_count ? lines[cl].page + 1 : 1, page_count, words, words == 1 ? "" : "S");
    ow_status(c, w->r.h - STATUS_H, w->r.w, status, dirty ? "UNSAVED CHANGES" : 0);
}

static void keep_caret_visible(struct window *w) {
    layout(w);
    struct rect view = view_rect(w);
    const struct line *l = &lines[caret_line()];
    if (l->y < scroll_y + 8) scroll_y = l->y - 24;
    if (l->y + l->height > scroll_y + view.h - 8) scroll_y = l->y + l->height - view.h + 24;
    sync_scroll(w);
}

static void delete_selection(void) {
    if (!has_selection()) return;
    struct pos s = pos_before(anchor, caret) ? anchor : caret, e = pos_before(anchor, caret) ? caret : anchor;
    if (s.para == e.para) para_delete(&doc.paras[s.para], s.at, e.at - s.at);
    else {
        para_delete(&doc.paras[s.para], s.at, doc.paras[s.para].length - s.at);
        para_delete(&doc.paras[e.para], 0, e.at);
        for (int i = s.para + 1; i < e.para; ++i) doc_join(&doc, s.para), --e.para, --i;
        doc_join(&doc, s.para);
    }
    caret = anchor = s;
    dirty = 1;
}

static void each_selected(void (*f)(struct para *, int, int), int bit) {
    struct pos s = pos_before(anchor, caret) ? anchor : caret, e = pos_before(anchor, caret) ? caret : anchor;
    for (int pi = s.para; pi <= e.para; ++pi)
        for (int i = pi == s.para ? s.at : 0; i < (pi == e.para ? e.at : doc.paras[pi].length); ++i) f(&doc.paras[pi], i, bit);
}
static int all_have;
static void check_attr(struct para *p, int i, int bit) { if (!(p->attr[i] & bit)) all_have = 0; }
static void set_attr(struct para *p, int i, int bit) { p->attr[i] = (unsigned char)(all_have ? p->attr[i] & ~bit : p->attr[i] | bit); }
static void clear_attr(struct para *p, int i, int bit) { p->attr[i] = 0; }

static void apply_attr(int bit) {
    if (!has_selection()) { typing_attr ^= bit; return; }
    all_have = 1;
    each_selected(check_attr, bit);
    each_selected(set_attr, bit);
    dirty = 1;
}

static void set_style(int style) {
    struct pos s = pos_before(anchor, caret) ? anchor : caret, e = pos_before(anchor, caret) ? caret : anchor;
    for (int i = s.para; i <= e.para; ++i) doc.paras[i].style = style;
    dirty = 1;
}

static void set_align(int align) {
    struct pos s = pos_before(anchor, caret) ? anchor : caret, e = pos_before(anchor, caret) ? caret : anchor;
    for (int i = s.para; i <= e.para; ++i) doc.paras[i].align = align;
    dirty = 1;
}

static void copy_selection(void) {
    if (!has_selection()) return;
    struct pos s = pos_before(anchor, caret) ? anchor : caret, e = pos_before(anchor, caret) ? caret : anchor;
    struct text t = {0};
    for (int pi = s.para; pi <= e.para; ++pi) {
        int from = pi == s.para ? s.at : 0, to = pi == e.para ? e.at : doc.paras[pi].length;
        text_add(&t, doc.paras[pi].text + from, (size_t)(to - from));
        if (pi != e.para) text_put(&t, "\n");
    }
    if (t.data) gui_clipboard_set(t.data, (int)t.size);
    free(t.data);
}

static void insert_text(const char *s, int n) {
    delete_selection();
    for (int i = 0; i < n; ++i) {
        if (s[i] == '\n') { doc_split(&doc, caret.para, caret.at); ++caret.para; caret.at = 0; continue; }
        if (s[i] == '\r') continue;
        para_insert(&doc.paras[caret.para], caret.at++, s + i, 1, typing_attr);
    }
    anchor = caret;
    dirty = 1;
}

static void paste(void) {
    static char buffer[65536];
    int n = gui_clipboard_get(buffer, sizeof(buffer) - 1);
    if (n > 0 && buffer[0] != 1) insert_text(buffer, n);
}

static void new_file(void) {
    doc_free(&doc);
    doc_add_para(&doc, STYLE_NORMAL);
    caret = anchor = (struct pos){0, 0};
    scroll_y = dirty = typing_attr = 0;
    suite_document(main_window, "");
    window_paint(main_window);
}

static void open_file(const char *path) {
    if (!strcasecmp(extension_of(path), "gdoc")) { open_in_browser(path); return; }
    char error[160] = "";
    struct document fresh;
    doc_init(&fresh);
    if (doc_load(&fresh, path, error, sizeof(error))) {
        doc_free(&fresh);
        message_box("OpenPapers", "The document could not be opened.", error);
        return;
    }
    if (!fresh.count) doc_add_para(&fresh, STYLE_NORMAL);
    doc_free(&doc);
    doc = fresh;
    caret = anchor = (struct pos){0, 0};
    scroll_y = dirty = 0;
    suite_document(main_window, path);
    window_paint(main_window);
}

static void saved(const char *path, void *data) {
    if (!path) return;
    char chosen[300], error[160] = "";
    snprintf(chosen, sizeof(chosen), "%s%s", path, extension_of(path)[0] ? "" : ".docx");
    if (doc_save(&doc, chosen, error, sizeof(error))) { message_box("OpenPapers", "The document could not be saved.", error); return; }
    dirty = 0;
    suite_document(main_window, chosen);
    window_paint(main_window);
}

static void save_as(void) { file_dialog("Save As (.docx, .odt, .rtf, .html, .txt, .md)", 1, 0, saved, 0); }
static void save(void) { if (suite_path[0]) saved(suite_path, 0); else save_as(); }

static void info(char *out, size_t size) {
    int chars = 0;
    for (int i = 0; i < doc.count; ++i) chars += doc.paras[i].length;
    layout(main_window);
    snprintf(out, size, "Pages\t%d\nWords\t%d\nCharacters\t%d\nParagraphs\t%d\nChanged\t%s\n", page_count, word_count(), chars, doc.count, dirty ? "Yes, not saved" : "No");
}

static struct pos pos_at(struct window *w, int x, int y) {
    struct rect view = view_rect(w);
    int dy = y - view.y + scroll_y, dx = x - view.x - page_left(w);
    for (int li = 0; li < line_count; ++li) {
        const struct line *l = &lines[li];
        if (dy >= l->y + l->height && li + 1 < line_count) continue;
        int cx = l->x, at = l->start;
        while (at < l->end) {
            int cw = char_width(&doc.paras[l->para], at);
            if (cx + cw / 2 > dx) break;
            cx += cw;
            ++at;
        }
        return (struct pos){l->para, at};
    }
    return (struct pos){doc.count - 1, doc.paras[doc.count - 1].length};
}

static int find_next(const char *needle, int from_start) {
    size_t n = strlen(needle);
    if (!n) return 0;
    struct pos s = pos_before(anchor, caret) ? caret : anchor;
    if (from_start) s = (struct pos){0, 0};
    for (int pass = 0; pass < 2; ++pass) {
        for (int pi = pass ? 0 : s.para; pi < doc.count; ++pi) {
            const struct para *p = &doc.paras[pi];
            for (int i = pi == s.para && !pass ? s.at : 0; i + (int)n <= p->length; ++i)
                if (!strncasecmp(p->text + i, needle, n)) {
                    anchor = (struct pos){pi, i};
                    caret = (struct pos){pi, i + (int)n};
                    return 1;
                }
        }
        s = (struct pos){0, 0};
    }
    return 0;
}

static void found(const char *what, void *data) {
    if (!what) return;
    snprintf(find_text, sizeof(find_text), "%s", what);
    if (!find_next(find_text, 0)) message_box("OpenPapers", "The search item was not found:", find_text);
    keep_caret_visible(main_window);
    window_paint(main_window);
}

static void replace_with(const char *with, void *data) {
    if (!with) with = "";
    int count = 0;
    size_t n = strlen(find_text), m = strlen(with);
    for (int pi = 0; pi < doc.count; ++pi)
        for (int i = 0; i + (int)n <= doc.paras[pi].length;) {
            struct para *p = &doc.paras[pi];
            if (strncasecmp(p->text + i, find_text, n)) { ++i; continue; }
            int attr = p->attr[i];
            para_delete(p, i, (int)n);
            para_insert(p, i, with, (int)m, attr);
            i += (int)m;
            ++count;
        }
    char line[64];
    snprintf(line, sizeof(line), "%d replacement%s made.", count, count == 1 ? " was" : "s were");
    if (count) { dirty = 1; caret = anchor = (struct pos){0, 0}; }
    message_box("Replace", line, "");
    window_paint(main_window);
}

static void replace_find(const char *what, void *data) {
    if (!what) return;
    snprintf(find_text, sizeof(find_text), "%s", what);
    input_box("Replace", "Replace with:", "", replace_with, 0);
}

static int current_attr(void) {
    if (!has_selection()) return typing_attr;
    struct pos s = pos_before(anchor, caret) ? anchor : caret;
    return s.at < doc.paras[s.para].length ? doc.paras[s.para].attr[s.at] : typing_attr;
}

static int state(int command) {
    const struct para *p = &doc.paras[caret.para];
    switch (command) {
    case C_BOLD: return current_attr() & ATTR_BOLD ? RB_ON : 0;
    case C_ITALIC: return current_attr() & ATTR_ITALIC ? RB_ON : 0;
    case C_UNDERLINE: return current_attr() & ATTR_UNDERLINE ? RB_ON : 0;
    case C_BULLETS: return p->style == STYLE_BULLET ? RB_ON : 0;
    case C_LEFT: case C_CENTER: case C_RIGHT: return p->align == command - C_LEFT ? RB_ON : 0;
    case C_CUT: case C_COPY: return has_selection() ? 0 : RB_OFF;
    case C_PASTE: return gui_board()->clipboard_size > 0 ? 0 : RB_OFF;
    }
    return 0;
}

static const char *menu(int command) {
    static char items[320];
    if (command == C_STYLES) {
        items[0] = 0;
        for (int i = 0; i < STYLE_COUNT; ++i) { strcat(items, style_names[i]); strcat(items, "\n"); }
    } else if (command == C_DATE) {
        time_t now = time(0);
        struct tm *t = localtime(&now);
        static const char *const formats[] = {"%m/%d/%Y", "%A, %B %d, %Y", "%B %d, %Y", "%Y-%m-%d", "%d %B %Y", "%H:%M", "%m/%d/%Y %H:%M"};
        size_t used = 0;
        for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
            used += strftime(items + used, sizeof(items) - used - 1, formats[i], t);
            items[used++] = '\n';
            items[used] = 0;
        }
    } else if (command == C_SYMBOL)
        snprintf(items, sizeof(items), "\xa9  Copyright\n\xae  Registered\n\xb0  Degree\n\xb1  Plus-minus\n\xd7  Multiply\n\xf7  Divide\n"
                                       "\xa7  Section\n\xb6  Pilcrow\n\xbd  One half\n\xbc  One quarter\n\xb5  Micro\n\xa3  Pound\n\xa5  Yen\n");
    else items[0] = 0;
    return items;
}

static void command(int command, int choice) {
    switch (command) {
    case C_PASTE: paste(); break;
    case C_CUT: copy_selection(); delete_selection(); break;
    case C_COPY: copy_selection(); break;
    case C_BOLD: case C_ITALIC: case C_UNDERLINE: apply_attr(1 << (command - C_BOLD)); break;
    case C_CLEAR_FORMAT:
        if (has_selection()) each_selected(clear_attr, 0);
        typing_attr = 0;
        set_style(STYLE_NORMAL);
        break;
    case C_BULLETS: set_style(doc.paras[caret.para].style == STYLE_BULLET ? STYLE_NORMAL : STYLE_BULLET); break;
    case C_LEFT: case C_CENTER: case C_RIGHT: set_align(command - C_LEFT); break;
    case C_STYLES: if (choice >= 0 && choice < STYLE_COUNT) set_style(choice); break;
    case C_FIND: input_box("Find", "Find what:", find_text, found, 0); break;
    case C_REPLACE: input_box("Replace", "Find what:", find_text, replace_find, 0); break;
    case C_SELECT_ALL: anchor = (struct pos){0, 0}; caret = (struct pos){doc.count - 1, doc.paras[doc.count - 1].length}; break;
    case C_DATE: case C_SYMBOL: {
        const char *items = menu(command);
        for (int i = 0; i < choice && items; ++i) { items = strchr(items, '\n'); if (items) ++items; }
        if (!items || choice < 0) break;
        int n = command == C_SYMBOL ? 1 : (int)strcspn(items, "\n");
        insert_text(items, n);
        break;
    }
    case C_WORD_COUNT: {
        int chars = 0;
        for (int i = 0; i < doc.count; ++i) chars += doc.paras[i].length;
        char a[80], b[80];
        layout(main_window);
        snprintf(a, sizeof(a), "Pages: %d     Words: %d", page_count, word_count());
        snprintf(b, sizeof(b), "Characters: %d     Paragraphs: %d", chars, doc.count);
        message_box("Word Count", a, b);
        break;
    }
    }
    window_paint(main_window);
}

static void gallery_paint(const struct canvas *c, struct rect r, int command, int index, int chosen) {
    u32 color = color_of(index);
    const char *sample = index == STYLE_TITLE || index == STYLE_HEADING1 ? "AaBb" : "AaBbCc";
    int scale = index == STYLE_TITLE ? 2 : 1, y = r.y + 10;
    if (index == STYLE_BULLET) { fill(c, r.x + 6, y + 3, 4, 4, 0); ow_text_scaled(c, r.x + 14, y, &font_regular, sample, color, 1); }
    else ow_text_scaled(c, r.x + 6, index == STYLE_TITLE ? y - 4 : y, font_of(index, 0), sample, color, scale);
    text(c, r.x + 6, r.y + r.h - 16, &font_regular, style_names[index], OW_TEXT);
}

static int gallery_selected(int command) { return doc.paras[caret.para].style; }

static void popup(struct window *w, int tag, int index) { ribbon_popup(&rb, w, tag, index); }

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (rb.backstage) { ribbon_mouse(&rb, w, x, y, kind); return; }
    if (kind == M_UP) selecting = 0;
    if (ow_vscroll_mouse(&scroll, scroll_rect(w), x, y, kind)) { scroll_y = scroll.top; window_paint(w); return; }
    if (!selecting && ribbon_mouse(&rb, w, x, y, kind)) return;
    if (kind == M_WHEEL) {
        scroll_y -= value * 48;
        sync_scroll(w);
        window_paint(w);
        return;
    }
    if (!rect_has(view_rect(w), x, y) && !selecting) return;
    if (kind == M_DOWN || kind == M_DOUBLE) {
        caret = anchor = pos_at(w, x, y);
        selecting = 1;
        typing_attr = caret.at > 0 ? doc.paras[caret.para].attr[caret.at - 1] : 0;
        if (kind == M_DOUBLE) {
            const struct para *p = &doc.paras[caret.para];
            while (anchor.at > 0 && p->text[anchor.at - 1] != ' ') --anchor.at;
            while (caret.at < p->length && p->text[caret.at] != ' ') ++caret.at;
            selecting = 0;
        }
    } else if (kind == M_MOVE && selecting) caret = pos_at(w, x, y);
    else if (kind != M_UP) return;
    window_paint(w);
}

static void move(int dx, int dy, int extend) {
    if (dy) {
        int line = caret_line(), target = line + dy;
        if (target >= 0 && target < line_count) {
            int offset = caret.at - lines[line].start;
            caret.para = lines[target].para;
            caret.at = lines[target].start + offset;
            if (caret.at > lines[target].end) caret.at = lines[target].end;
        }
    } else if (dx < 0) {
        if (caret.at > 0) --caret.at;
        else if (caret.para > 0) { --caret.para; caret.at = doc.paras[caret.para].length; }
    } else if (dx > 0) {
        if (caret.at < doc.paras[caret.para].length) ++caret.at;
        else if (caret.para + 1 < doc.count) { ++caret.para; caret.at = 0; }
    }
    if (!extend) anchor = caret;
}

static void key(struct window *w, int k) {
    if (ribbon_key(&rb, w, k)) return;
    int shift = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0) & 1;
    layout(w);
    if (k == 0x104) move(-1, 0, shift);
    else if (k == 0x103) move(1, 0, shift);
    else if (k == 0x101) move(0, -1, shift);
    else if (k == 0x102) move(0, 1, shift);
    else if (k == 0x109 || k == 0x10a) move(0, (k == 0x109 ? -1 : 1) * (view_rect(w).h / 14), shift);
    else if (k == 0x105) { caret.at = lines[caret_line()].start; if (!shift) anchor = caret; }
    else if (k == 0x106) { caret.at = lines[caret_line()].end; if (caret.at > lines[caret_line()].start && caret.at < doc.paras[caret.para].length) --caret.at; if (!shift) anchor = caret; }
    else if (k == '\b') {
        if (has_selection()) delete_selection();
        else if (caret.at > 0) { para_delete(&doc.paras[caret.para], --caret.at, 1); anchor = caret; }
        else if (caret.para > 0) { caret.at = doc.paras[caret.para - 1].length; doc_join(&doc, --caret.para); anchor = caret; }
        dirty = 1;
    } else if (k == 0x108) {
        if (has_selection()) delete_selection();
        else if (caret.at < doc.paras[caret.para].length) para_delete(&doc.paras[caret.para], caret.at, 1);
        else doc_join(&doc, caret.para);
        dirty = 1;
    } else if (k == '\n') insert_text("\n", 1);
    else if (k == 2) apply_attr(ATTR_BOLD);
    else if (k == 21) apply_attr(ATTR_UNDERLINE);
    else if (k == 19) save();
    else if (k == 15) ribbon_file(&rb, w, 2);
    else if (k == 14) new_file();
    else if (k == 6) command(C_FIND, -1);
    else if (k == 5 || k == 12 || k == 18) set_align(k == 12 ? ALIGN_LEFT : k == 5 ? ALIGN_CENTER : ALIGN_RIGHT);
    else if (k == 3) copy_selection();
    else if (k == 24) { copy_selection(); delete_selection(); }
    else if (k == 22) paste();
    else if (k == 1) command(C_SELECT_ALL, -1);
    else if (k == 0x10d && find_text[0]) find_next(find_text, 0);
    else if (k == 0x10b) { suite_about(); return; }
    else if (k == '\t' || (k >= 32 && k < 256 && k != 127)) { char c = (char)k; insert_text(&c, 1); }
    else return;
    keep_caret_visible(w);
    window_paint(w);
}

static void ask_save(int answer, void *data) {
    if (answer == 1) save();
    if (answer == 0 || (answer == 1 && !dirty)) { dirty = 0; window_close(main_window); }
}

static int closing(struct window *w) {
    if (!dirty) return 1;
    ask_box("OpenPapers", "Save the changes to this document?", "Save", "Don't Save", ask_save, 0);
    return 0;
}

static const struct app_ops ops = {paint, mouse, key, 0, popup, 0, 0, closing};

static const struct suite_app papers = {
    "OpenPapers", "PAPERS", "Write and read Word, OpenDocument, RTF, HTML and text documents.",
    "Document1", "document", "Documents",
    "Word Document (.docx)\nOpenDocument Text (.odt)\nRich Text Format (.rtf)\nWeb Page (.html)\nMarkdown (.md)\nPlain Text (.txt)\n\nType a name ending in one of these;\nwith no ending the document is saved as .docx.",
    0x2b579a, 0xffffff, BS_NEW | BS_OPEN | BS_SAVE | BS_SAVE_AS,
    new_file, open_file, save, save_as, info, 0,
};

int main(int argc, char **argv) {
    if (gui_connect()) { puts("OpenPapers needs the DOS-XC desktop."); return 1; }
    suite_start(&papers, argv[0]);
    rb = (struct ribbon){tabs, 3, 0, command, state, menu, 0, gallery_paint, gallery_selected};
    rb.hover = -1;
    doc_init(&doc);
    doc_add_para(&doc, STYLE_NORMAL);
    int width = gui_board()->screen_w - 80, height = gui_board()->screen_h - 120;
    if (width > 900) width = 900;
    if (height > 680) height = 680;
    main_window = window_open(width < 560 ? 560 : width, height < 400 ? 400 : height, "Document1 - OpenPapers", ICON_EDITOR, &ops, 0);
    if (!main_window) return 1;
    main_window->resizable = 1;
    main_window->track_mouse = 1;
    if (argc > 1 && argv[1][0]) open_file(argv[1]);
    gui_run();
    return 0;
}
