/*
 * DOS-XC /dosoffice/writer.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "doc.h"
#include <strings.h>

#define TOOLBAR_H 28
#define MARGIN 24
#define LINE_GAP 4

struct pos { int para, at; };
struct line { int para, start, end, y, height, x; };

static struct document doc;
static struct window *main_window;
static char file_path[300];
static int dirty, typing_attr, scroll_y, content_height;
static struct pos caret, anchor;
static int selecting;
static struct line *lines;
static int line_count, line_capacity;

static const struct menubar menu = {{"File", "Edit", "Format", "Help"},
    {"New\nOpen...\nSave\nSave As...\n-\nExit\n", "Select All\nCut\nCopy\nPaste\n",
     "Bold\nItalic\nUnderline\n-\nNormal\nTitle\nHeading 1\nHeading 2\nHeading 3\nBulleted list\n-\nAlign Left\nCenter\nAlign Right\n",
     "About\n"}};
static const char *const style_names[STYLE_COUNT] = {"Normal", "Heading 1", "Heading 2", "Heading 3", "Title", "Bulleted"};

static int scale_of(int style) { return style == STYLE_TITLE || style == STYLE_HEADING1 ? 2 : 1; }
static const struct font *font_of(int style, int attr) {
    return (attr & ATTR_BOLD) || (style >= STYLE_HEADING1 && style <= STYLE_TITLE) ? &font_bold : &font_regular;
}

static int char_width(const struct para *p, int i) {
    char s[2] = {p->text[i] == '\t' ? ' ' : p->text[i], 0};
    int w = text_width(font_of(p->style, p->attr[i]), s) + font_regular.spacing;
    if (p->text[i] == '\t') w *= 4;
    return w * scale_of(p->style);
}

static struct rect page_rect(struct window *w) { return (struct rect){0, MENUBAR_H + TOOLBAR_H, w->r.w, w->r.h - MENUBAR_H - TOOLBAR_H - 18}; }

static void add_line(int para, int start, int end, int y, int height, int x) {
    if (line_count == line_capacity) {
        line_capacity = line_capacity ? line_capacity * 2 : 256;
        lines = realloc(lines, sizeof(*lines) * (size_t)line_capacity);
    }
    lines[line_count++] = (struct line){para, start, end, y, height, x};
}

/* Word-wrap every paragraph to the page width. */
static void layout(struct window *w) {
    line_count = 0;
    int width = w->r.w - 2 * MARGIN, y = 12;
    for (int pi = 0; pi < doc.count; ++pi) {
        const struct para *p = &doc.paras[pi];
        int indent = p->style == STYLE_BULLET ? 18 : 0, room = width - indent;
        int height = (font_regular.height + LINE_GAP) * scale_of(p->style);
        if (p->style >= STYLE_HEADING1 && p->style <= STYLE_TITLE) y += 6;
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
            int lx = MARGIN + indent + (p->align == ALIGN_CENTER ? (room - line_width) / 2 : p->align == ALIGN_RIGHT ? room - line_width : 0);
            add_line(pi, start, end, y, height, lx);
            y += height;
            start = end;
        } while (start < p->length);
        y += p->style == STYLE_NORMAL || p->style == STYLE_BULLET ? 4 : 8;
    }
    content_height = y;
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

static void paint(struct window *w, const struct canvas *c) {
    menubar_paint(c, &menu, w->r.w);
    fill(c, 0, MENUBAR_H, w->r.w, TOOLBAR_H, C_WINDOW);
    static const char *const tools[] = {"B", "I", "U"};
    int attr = typing_attr;
    for (int i = 0; i < 3; ++i) button(c, (struct rect){6 + i * 28, MENUBAR_H + 3, 24, 22}, tools[i], (attr >> i) & 1, i == 0);
    const struct para *cp = &doc.paras[caret.para];
    button(c, (struct rect){96, MENUBAR_H + 3, 110, 22}, style_names[cp->style], 0, 0);
    static const char *const aligns[] = {"Left", "Center", "Right"};
    button(c, (struct rect){212, MENUBAR_H + 3, 64, 22}, aligns[cp->align], 0, 0);
    struct rect page = page_rect(w);
    fill(c, page.x, page.y, page.w, page.h, 0xffffff);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, page);
    layout(w);
    for (int li = 0; li < line_count; ++li) {
        const struct line *l = &lines[li];
        int y = page.y + l->y - scroll_y;
        if (y + l->height < page.y || y > page.y + page.h) continue;
        const struct para *p = &doc.paras[l->para];
        int scale = scale_of(p->style);
        if (p->style == STYLE_BULLET && l->start == 0) fill(&inner, l->x - 12, y + font_regular.height / 2, 5, 5, 0x000000);
        int x = l->x;
        for (int i = l->start; i < l->end; ++i) {
            int cw = char_width(p, i);
            if (selected(l->para, i)) fill(&inner, x, y - 1, cw, l->height, 0xb4d5fe);
            char s[2] = {p->text[i] == '\t' ? ' ' : p->text[i], 0};
            const struct font *f = font_of(p->style, p->attr[i]);
            u32 color = p->style >= STYLE_HEADING1 && p->style <= STYLE_HEADING3 ? 0x1f3f8f : 0x000000;
            if (scale > 1) text_big(&inner, x, y, s, color, scale);
            else if (p->attr[i] & ATTR_ITALIC) draw_italic(&inner, x, y, f, s, color);
            else text(&inner, x, y, f, s, color);
            if (p->attr[i] & ATTR_UNDERLINE) fill(&inner, x, y + f->height * scale, cw, 1, color);
            x += cw;
        }
        if (l->para == caret.para && caret.at >= l->start && (caret.at < l->end || (caret.at == l->end && (li + 1 == line_count || lines[li + 1].para != l->para)))) {
            int cx = l->x;
            for (int i = l->start; i < caret.at; ++i) cx += char_width(p, i);
            fill(&inner, cx, y - 1, 2, l->height, 0x000000);
        }
    }
    fill(c, 0, w->r.h - 18, w->r.w, 18, C_WINDOW);
    char status[160];
    int words = 0;
    for (int i = 0; i < doc.count; ++i)
        for (int k = 0; k < doc.paras[i].length; ++k)
            if (doc.paras[i].text[k] != ' ' && (k == 0 || doc.paras[i].text[k - 1] == ' ')) ++words;
    snprintf(status, sizeof(status), "%s%s   %d words", file_path[0] ? file_path : "Untitled", dirty ? " (changed)" : "", words);
    text(c, 6, w->r.h - 14, &font_regular, status, C_TEXT);
}

static void set_title(void) {
    char title[96];
    const char *name = strrchr(file_path, '\\');
    snprintf(title, sizeof(title), "%s - DOS Writer", file_path[0] ? (name ? name + 1 : file_path) : "Untitled");
    window_title(main_window, title);
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

static void apply_attr(int bit) {
    if (!has_selection()) { typing_attr ^= bit; return; }
    struct pos s = pos_before(anchor, caret) ? anchor : caret, e = pos_before(anchor, caret) ? caret : anchor;
    int all = 1;
    for (int pi = s.para; pi <= e.para; ++pi)
        for (int i = pi == s.para ? s.at : 0; i < (pi == e.para ? e.at : doc.paras[pi].length); ++i)
            if (!(doc.paras[pi].attr[i] & bit)) all = 0;
    for (int pi = s.para; pi <= e.para; ++pi)
        for (int i = pi == s.para ? s.at : 0; i < (pi == e.para ? e.at : doc.paras[pi].length); ++i)
            doc.paras[pi].attr[i] = (unsigned char)(all ? doc.paras[pi].attr[i] & ~bit : doc.paras[pi].attr[i] | bit);
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

static void loaded(const char *path, void *data) {
    (void)data;
    if (!path) return;
    const char *ext = extension_of(path);
    if (!strcasecmp(ext, "gdoc")) { open_in_browser(path); return; }
    char error[160] = "";
    if (doc_load(&doc, path, error, sizeof(error))) {
        message_box("DOS Writer", "The document could not be opened.", error);
        file_path[0] = 0;
    } else snprintf(file_path, sizeof(file_path), "%s", path);
    caret = anchor = (struct pos){0, 0};
    scroll_y = 0;
    dirty = 0;
    set_title();
    window_paint(main_window);
}

static void saved(const char *path, void *data) {
    (void)data;
    if (!path) return;
    char chosen[300];
    snprintf(chosen, sizeof(chosen), "%s%s", path, extension_of(path)[0] ? "" : ".docx");
    char error[160] = "";
    if (doc_save(&doc, chosen, error, sizeof(error))) { message_box("DOS Writer", "The document could not be saved.", error); return; }
    snprintf(file_path, sizeof(file_path), "%s", chosen);
    dirty = 0;
    set_title();
    window_paint(main_window);
}

static void save(void) {
    if (file_path[0]) saved(file_path, 0);
    else file_dialog("Save As (.docx, .odt, .rtf, .html, .txt, .md)", 1, 0, saved, 0);
}

static struct pos pos_at(struct window *w, int x, int y) {
    struct rect page = page_rect(w);
    int dy = y - page.y + scroll_y;
    struct pos p = {doc.count - 1, doc.paras[doc.count - 1].length};
    for (int li = 0; li < line_count; ++li) {
        const struct line *l = &lines[li];
        if (dy >= l->y + l->height && li + 1 < line_count) continue;
        int cx = l->x, at = l->start;
        while (at < l->end) {
            int cw = char_width(&doc.paras[l->para], at);
            if (cx + cw / 2 > x) break;
            cx += cw;
            ++at;
        }
        return (struct pos){l->para, at};
    }
    return p;
}

static void popup(struct window *w, int tag, int index) {
    if (tag == 0) {
        if (index == 0) { doc_free(&doc); doc_add_para(&doc, STYLE_NORMAL); file_path[0] = 0; caret = anchor = (struct pos){0, 0}; dirty = 0; set_title(); }
        else if (index == 1) file_dialog("Open a document", 0, 0, loaded, 0);
        else if (index == 2) save();
        else if (index == 3) file_dialog("Save As (.docx, .odt, .rtf, .html, .txt, .md)", 1, 0, saved, 0);
        else if (index == 5) window_close(w);
    } else if (tag == 1) {
        if (index == 0) { anchor = (struct pos){0, 0}; caret = (struct pos){doc.count - 1, doc.paras[doc.count - 1].length}; }
        else if (index == 1) { copy_selection(); delete_selection(); }
        else if (index == 2) copy_selection();
        else if (index == 3) {
            char buffer[16384];
            int n = gui_clipboard_get(buffer, sizeof(buffer) - 1);
            if (n > 0 && buffer[0] != 1) insert_text(buffer, n);
        }
    } else if (tag == 2) {
        if (index <= 2) apply_attr(1 << index);
        else if (index == 4) set_style(STYLE_NORMAL);
        else if (index == 5) set_style(STYLE_TITLE);
        else if (index >= 6 && index <= 8) set_style(STYLE_HEADING1 + index - 6);
        else if (index == 9) set_style(STYLE_BULLET);
        else if (index >= 11) set_align(index - 11);
    } else if (tag == 3) {
        message_box("About DOS Writer", "DOS Writer, part of DOS Office for DOS-XC.", "Word, OpenDocument, RTF, HTML and text documents.");
    } else if (tag == 4 && index >= 0) set_style(index);
    else if (tag == 5 && index >= 0) set_align(index);
    window_paint(w);
}

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &menu, x, y, kind)) return;
    if (kind == M_WHEEL) {
        scroll_y -= value * 40;
        if (scroll_y > content_height - 40) scroll_y = content_height - 40;
        if (scroll_y < 0) scroll_y = 0;
        window_paint(w);
        return;
    }
    if (y >= MENUBAR_H && y < MENUBAR_H + TOOLBAR_H && kind == M_DOWN) {
        if (x >= 6 && x < 90) apply_attr(1 << ((x - 6) / 28));
        else if (x >= 96 && x < 206) window_popup(w, 96, MENUBAR_H + TOOLBAR_H, "Normal\nHeading 1\nHeading 2\nHeading 3\nTitle\nBulleted list\n", 4);
        else if (x >= 212 && x < 276) window_popup(w, 212, MENUBAR_H + TOOLBAR_H, "Left\nCenter\nRight\n", 5);
        window_paint(w);
        return;
    }
    if (!rect_has(page_rect(w), x, y) && !selecting) return;
    if (kind == M_DOWN || kind == M_DOUBLE) {
        caret = anchor = pos_at(w, x, y);
        selecting = 1;
        typing_attr = caret.at > 0 ? doc.paras[caret.para].attr[caret.at - 1] : 0;
    } else if (kind == M_MOVE && selecting) caret = pos_at(w, x, y);
    else if (kind == M_UP) selecting = 0;
    window_paint(w);
}

static void move(int dx, int dy, int extend) {
    if (dy) {
        int line = 0;
        for (int li = 0; li < line_count; ++li)
            if (lines[li].para == caret.para && caret.at >= lines[li].start && (caret.at <= lines[li].end)) { line = li; break; }
        int target = line + dy;
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
    int shift = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0) & 1;
    if (k == 0x104) move(-1, 0, shift);
    else if (k == 0x103) move(1, 0, shift);
    else if (k == 0x101) move(0, -1, shift);
    else if (k == 0x102) move(0, 1, shift);
    else if (k == 0x105) { caret.at = 0; if (!shift) anchor = caret; }
    else if (k == 0x106) { caret.at = doc.paras[caret.para].length; if (!shift) anchor = caret; }
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
    else if (k == 15) file_dialog("Open a document", 0, 0, loaded, 0);
    else if (k == 3) copy_selection();
    else if (k == 24) { copy_selection(); delete_selection(); }
    else if (k == 22) popup(w, 1, 3);
    else if (k == 1) popup(w, 1, 0);
    else if (k == '\t' || (k >= 32 && k < 256 && k != 127)) { char c = (char)k; insert_text(&c, 1); }
    else return;
    layout(w);
    struct rect page = page_rect(w);
    for (int li = 0; li < line_count; ++li)
        if (lines[li].para == caret.para && caret.at >= lines[li].start && caret.at <= lines[li].end) {
            if (lines[li].y < scroll_y) scroll_y = lines[li].y - 8;
            if (lines[li].y + lines[li].height > scroll_y + page.h) scroll_y = lines[li].y + lines[li].height - page.h + 8;
            break;
        }
    if (scroll_y < 0) scroll_y = 0;
    window_paint(w);
}

static void ask_save(int answer, void *data) {
    (void)data;
    if (answer == 1) save();
    if (answer == 0 || (answer == 1 && !dirty)) { dirty = 0; window_close(main_window); }
}

static int closing(struct window *w) {
    (void)w;
    if (!dirty) return 1;
    ask_box("DOS Writer", "Save the changes to this document?", "Save", "Don't Save", ask_save, 0);
    return 0;
}
static const struct app_ops ops = {paint, mouse, key, 0, popup, 0, 0, closing};

int main(int argc, char **argv) {
    if (gui_connect()) { puts("DOS Writer needs the DOS-XC desktop."); return 1; }
    char icon[260];
    snprintf(icon, sizeof(icon), "%s", argv[0]);
    char *slash = strrchr(icon, '\\');
    if (slash) { strcpy(slash + 1, "WRITER.PNG"); gui_set_icon_file(icon); }
    doc_init(&doc);
    doc_add_para(&doc, STYLE_NORMAL);
    main_window = window_open(620, 460, "Untitled - DOS Writer", ICON_EDITOR, &ops, 0);
    if (!main_window) return 1;
    main_window->resizable = 1;
    main_window->track_mouse = 1;
    if (argc > 1) loaded(argv[1], 0);
    gui_run();
    return 0;
}
