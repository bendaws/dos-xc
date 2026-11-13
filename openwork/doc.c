/*
 * DOS-XC /dosoffice/doc.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "doc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void doc_init(struct document *d) { memset(d, 0, sizeof(*d)); }

void doc_free(struct document *d) {
    for (int i = 0; i < d->count; ++i) { free(d->paras[i].text); free(d->paras[i].attr); }
    free(d->paras);
    memset(d, 0, sizeof(*d));
}

struct para *doc_add_para(struct document *d, int style) {
    if (d->count == d->capacity) {
        int capacity = d->capacity ? d->capacity * 2 : 32;
        struct para *bigger = realloc(d->paras, sizeof(*bigger) * (size_t)capacity);
        if (!bigger) return 0;
        d->paras = bigger;
        d->capacity = capacity;
    }
    struct para *p = &d->paras[d->count++];
    memset(p, 0, sizeof(*p));
    p->style = style;
    return p;
}

void para_insert(struct para *p, int at, const char *s, int n, int attr) {
    if (n <= 0) return;
    if (p->length + n + 1 > p->capacity) {
        int capacity = p->capacity ? p->capacity : 32;
        while (capacity < p->length + n + 1) capacity *= 2;
        char *t = realloc(p->text, (size_t)capacity);
        unsigned char *a = t ? realloc(p->attr, (size_t)capacity) : 0;
        if (!t || !a) { if (t) p->text = t; return; }
        p->text = t;
        p->attr = a;
        p->capacity = capacity;
    }
    if (at < 0) at = 0;
    if (at > p->length) at = p->length;
    memmove(p->text + at + n, p->text + at, (size_t)(p->length - at));
    memmove(p->attr + at + n, p->attr + at, (size_t)(p->length - at));
    memcpy(p->text + at, s, (size_t)n);
    memset(p->attr + at, attr, (size_t)n);
    p->length += n;
    p->text[p->length] = 0;
}

void para_delete(struct para *p, int at, int n) {
    if (at < 0 || n <= 0 || at >= p->length) return;
    if (at + n > p->length) n = p->length - at;
    memmove(p->text + at, p->text + at + n, (size_t)(p->length - at - n));
    memmove(p->attr + at, p->attr + at + n, (size_t)(p->length - at - n));
    p->length -= n;
    if (p->text) p->text[p->length] = 0;
}

void doc_split(struct document *d, int index, int at) {
    if (!doc_add_para(d, STYLE_NORMAL)) return;
    memmove(&d->paras[index + 2], &d->paras[index + 1], sizeof(struct para) * (size_t)(d->count - index - 2));
    struct para *p = &d->paras[index], *q = &d->paras[index + 1];
    memset(q, 0, sizeof(*q));
    q->style = p->style == STYLE_BULLET ? STYLE_BULLET : STYLE_NORMAL;
    q->align = p->align;
    if (at < p->length) {
        for (int i = at; i < p->length; ++i) para_insert(q, q->length, p->text + i, 1, p->attr[i]);
        para_delete(p, at, p->length - at);
    }
}

void doc_join(struct document *d, int index) {
    if (index + 1 >= d->count) return;
    struct para *p = &d->paras[index], *q = &d->paras[index + 1];
    for (int i = 0; i < q->length; ++i) para_insert(p, p->length, q->text + i, 1, q->attr[i]);
    free(q->text);
    free(q->attr);
    memmove(q, q + 1, sizeof(struct para) * (size_t)(d->count - index - 2));
    --d->count;
}

/* ---- text encoding: Latin-1 inside, UTF-8 in files ---------------------- */

static void add_utf8(struct para *p, const char *s, int attr) {
    char buffer[512];
    int n = 0;
    for (const unsigned char *u = (const unsigned char *)s; *u;) {
        unsigned code = *u++;
        if (code >= 0xc0) {
            int extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : 1;
            code &= 0x3f >> extra;
            for (int k = 0; k < extra && (*u & 0xc0) == 0x80; ++k) code = code << 6 | (*u++ & 0x3f);
        }
        if (code == 0x2018 || code == 0x2019) code = '\'';
        else if (code == 0x201c || code == 0x201d) code = '"';
        else if (code == 0x2013 || code == 0x2014) code = '-';
        else if (code == 0x2026) code = '.';
        else if (code == 0xa0) code = ' ';
        if (code == '\r' || code == '\n') code = ' ';
        buffer[n++] = (char)(code < 256 ? code : '?');
        if (n == (int)sizeof(buffer)) { para_insert(p, p->length, buffer, n, attr); n = 0; }
    }
    para_insert(p, p->length, buffer, n, attr);
}

static void put_utf8(struct text *t, const char *s, int n, int xml) {
    for (int i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (xml) text_xml(t, (const char *)&s[i], 1);
            else text_add(t, (const char *)&s[i], 1);
        } else {
            char two[2] = {(char)(0xc0 | c >> 6), (char)(0x80 | (c & 63))};
            text_add(t, two, 2);
        }
    }
}

/* ---- Word (.docx) ---------------------------------------------------------- */

static int heading_style(const char *v) {
    if (!strncasecmp(v, "Heading1", 8) || !strcasecmp(v, "Heading 1")) return STYLE_HEADING1;
    if (!strncasecmp(v, "Heading2", 8) || !strcasecmp(v, "Heading 2")) return STYLE_HEADING2;
    if (!strncasecmp(v, "Heading", 7)) return STYLE_HEADING3;
    if (!strcasecmp(v, "Title")) return STYLE_TITLE;
    if (strcasestr(v, "List")) return STYLE_BULLET;
    return STYLE_NORMAL;
}

static int on(const struct xml *x) {
    char v[16];
    return xml_attribute(x, "val", v, sizeof(v)) || (strcmp(v, "0") && strcasecmp(v, "false") && strcasecmp(v, "none"));
}

static int load_docx(struct document *d, const char *path, char *error, int size) {
    struct zip z;
    if (zip_open(&z, path)) { snprintf(error, (size_t)size, "This is not a Word document."); return -1; }
    size_t n;
    char *xml_text = zip_read(&z, "word/document.xml", &n);
    zip_close(&z);
    if (!xml_text) { snprintf(error, (size_t)size, "The document has no word/document.xml."); return -1; }
    struct xml x;
    xml_start(&x, xml_text, n);
    struct para *p = 0;
    int attr = 0, in_run_props = 0, in_text = 0, kind;
    while ((kind = xml_next(&x)) != XML_END) {
        if (kind == XML_OPEN && xml_is(&x, "p")) { p = doc_add_para(d, STYLE_NORMAL); attr = 0; }
        else if (kind == XML_CLOSE && xml_is(&x, "p")) p = 0;
        else if (!p) continue;
        else if ((kind == XML_OPEN || kind == XML_EMPTY) && xml_is(&x, "pStyle")) {
            char v[64];
            if (!xml_attribute(&x, "val", v, sizeof(v))) p->style = heading_style(v);
        } else if ((kind == XML_OPEN || kind == XML_EMPTY) && xml_is(&x, "numPr")) p->style = STYLE_BULLET;
        else if ((kind == XML_OPEN || kind == XML_EMPTY) && xml_is(&x, "jc")) {
            char v[16];
            if (!xml_attribute(&x, "val", v, sizeof(v))) p->align = !strcmp(v, "center") ? ALIGN_CENTER : !strcmp(v, "right") || !strcmp(v, "end") ? ALIGN_RIGHT : ALIGN_LEFT;
        } else if (kind == XML_OPEN && xml_is(&x, "r")) attr = 0;
        else if (kind == XML_OPEN && xml_is(&x, "rPr")) in_run_props = 1;
        else if (kind == XML_CLOSE && xml_is(&x, "rPr")) in_run_props = 0;
        else if (in_run_props && (kind == XML_OPEN || kind == XML_EMPTY)) {
            if (xml_is(&x, "b") && on(&x)) attr |= ATTR_BOLD;
            else if (xml_is(&x, "i") && on(&x)) attr |= ATTR_ITALIC;
            else if (xml_is(&x, "u") && on(&x)) attr |= ATTR_UNDERLINE;
        } else if (kind == XML_OPEN && xml_is(&x, "t")) in_text = 1;
        else if (kind == XML_CLOSE && xml_is(&x, "t")) in_text = 0;
        else if (kind == XML_TEXT && in_text) add_utf8(p, x.text, attr);
        else if (kind == XML_EMPTY && xml_is(&x, "tab")) para_insert(p, p->length, "\t", 1, attr);
        else if (kind == XML_EMPTY && (xml_is(&x, "br") || xml_is(&x, "cr"))) para_insert(p, p->length, " ", 1, attr);
    }
    free(xml_text);
    return 0;
}

static const char *const docx_styles[STYLE_COUNT] = {0, "Heading1", "Heading2", "Heading3", "Title", "ListBullet"};

static int save_docx(const struct document *d, const char *path) {
    struct text body = {0};
    text_put(&body, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                    "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>");
    for (int i = 0; i < d->count; ++i) {
        const struct para *p = &d->paras[i];
        text_put(&body, "<w:p><w:pPr>");
        if (docx_styles[p->style]) text_printf(&body, "<w:pStyle w:val=\"%s\"/>", docx_styles[p->style]);
        if (p->align) text_printf(&body, "<w:jc w:val=\"%s\"/>", p->align == ALIGN_CENTER ? "center" : "right");
        text_put(&body, "</w:pPr>");
        for (int a = 0; a < p->length;) {
            int b = a;
            while (b < p->length && p->attr[b] == p->attr[a]) ++b;
            text_put(&body, "<w:r>");
            if (p->attr[a]) {
                text_put(&body, "<w:rPr>");
                if (p->attr[a] & ATTR_BOLD) text_put(&body, "<w:b/>");
                if (p->attr[a] & ATTR_ITALIC) text_put(&body, "<w:i/>");
                if (p->attr[a] & ATTR_UNDERLINE) text_put(&body, "<w:u w:val=\"single\"/>");
                text_put(&body, "</w:rPr>");
            }
            text_put(&body, "<w:t xml:space=\"preserve\">");
            put_utf8(&body, p->text + a, b - a, 1);
            text_put(&body, "</w:t></w:r>");
            a = b;
        }
        text_put(&body, "</w:p>");
    }
    text_put(&body, "<w:sectPr/></w:body></w:document>");
    static const char types[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "</Types>";
    static const char rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "</Relationships>";
    static const char document_rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "</Relationships>";
    struct text styles = {0};
    text_put(&styles, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                      "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">");
    static const struct { const char *id, *name; int size; } defs[] = {
        {"Heading1", "heading 1", 32}, {"Heading2", "heading 2", 26}, {"Heading3", "heading 3", 24}, {"Title", "Title", 56}, {"ListBullet", "List Bullet", 0}};
    for (unsigned i = 0; i < sizeof(defs) / sizeof(*defs); ++i) {
        text_printf(&styles, "<w:style w:type=\"paragraph\" w:styleId=\"%s\"><w:name w:val=\"%s\"/>", defs[i].id, defs[i].name);
        if (defs[i].size) text_printf(&styles, "<w:rPr><w:b/><w:sz w:val=\"%d\"/></w:rPr>", defs[i].size);
        text_put(&styles, "</w:style>");
    }
    text_put(&styles, "</w:styles>");
    struct zip_writer w;
    zip_begin(&w);
    zip_add(&w, "[Content_Types].xml", types, strlen(types), 1);
    zip_add(&w, "_rels/.rels", rels, strlen(rels), 1);
    zip_add(&w, "word/_rels/document.xml.rels", document_rels, strlen(document_rels), 1);
    zip_add(&w, "word/document.xml", body.data, body.size, 1);
    zip_add(&w, "word/styles.xml", styles.data, styles.size, 1);
    free(body.data);
    free(styles.data);
    return zip_finish(&w, path);
}

/* ---- OpenDocument text (.odt) ------------------------------------------- */

struct odt_style { char name[48]; int attr, style; };

static int load_odt(struct document *d, const char *path, char *error, int size) {
    struct zip z;
    if (zip_open(&z, path)) { snprintf(error, (size_t)size, "This is not an OpenDocument file."); return -1; }
    size_t n;
    char *xml_text = zip_read(&z, "content.xml", &n);
    zip_close(&z);
    if (!xml_text) { snprintf(error, (size_t)size, "The document has no content.xml."); return -1; }
    static struct odt_style styles[256];
    int style_count = 0, kind, depth_span = 0, span_attr[16] = {0}, in_list = 0;
    struct xml x;
    xml_start(&x, xml_text, n);
    struct para *p = 0;
    struct odt_style *defining = 0;
    while ((kind = xml_next(&x)) != XML_END) {
        if (kind == XML_OPEN && xml_is(&x, "style") && style_count < 256) {
            defining = &styles[style_count++];
            memset(defining, 0, sizeof(*defining));
            xml_attribute(&x, "name", defining->name, sizeof(defining->name));
            char parent[48];
            if (!xml_attribute(&x, "parent-style-name", parent, sizeof(parent))) defining->style = heading_style(parent);
        } else if (kind == XML_CLOSE && xml_is(&x, "style")) defining = 0;
        else if (defining && (kind == XML_EMPTY || kind == XML_OPEN) && xml_is(&x, "text-properties")) {
            char v[32];
            if (!xml_attribute(&x, "font-weight", v, sizeof(v)) && !strcmp(v, "bold")) defining->attr |= ATTR_BOLD;
            if (!xml_attribute(&x, "font-style", v, sizeof(v)) && !strcmp(v, "italic")) defining->attr |= ATTR_ITALIC;
            if (!xml_attribute(&x, "text-underline-style", v, sizeof(v)) && strcmp(v, "none")) defining->attr |= ATTR_UNDERLINE;
        } else if (kind == XML_OPEN && xml_is(&x, "list")) ++in_list;
        else if (kind == XML_CLOSE && xml_is(&x, "list")) --in_list;
        else if (kind == XML_OPEN && (xml_is(&x, "p") || xml_is(&x, "h"))) {
            int style = in_list ? STYLE_BULLET : STYLE_NORMAL;
            char v[48];
            if (xml_is(&x, "h")) style = !xml_attribute(&x, "outline-level", v, sizeof(v)) && atoi(v) >= 3 ? STYLE_HEADING3 : !strcmp(v, "2") ? STYLE_HEADING2 : STYLE_HEADING1;
            if (!xml_attribute(&x, "style-name", v, sizeof(v))) {
                if (!strcasecmp(v, "Title")) style = STYLE_TITLE;
                for (int i = 0; i < style_count; ++i)
                    if (!strcmp(styles[i].name, v) && styles[i].style) style = styles[i].style;
            }
            p = doc_add_para(d, style);
            depth_span = 0;
            span_attr[0] = 0;
        } else if (kind == XML_CLOSE && (xml_is(&x, "p") || xml_is(&x, "h"))) p = 0;
        else if (!p) continue;
        else if (kind == XML_OPEN && xml_is(&x, "span") && depth_span < 15) {
            char v[48];
            int attr = span_attr[depth_span];
            if (!xml_attribute(&x, "style-name", v, sizeof(v)))
                for (int i = 0; i < style_count; ++i)
                    if (!strcmp(styles[i].name, v)) attr |= styles[i].attr;
            span_attr[++depth_span] = attr;
        } else if (kind == XML_CLOSE && xml_is(&x, "span") && depth_span) --depth_span;
        else if (kind == XML_TEXT) add_utf8(p, x.text, span_attr[depth_span]);
        else if (kind == XML_EMPTY && xml_is(&x, "s")) {
            char v[8];
            int count = xml_attribute(&x, "c", v, sizeof(v)) ? 1 : atoi(v);
            for (int i = 0; i < count && i < 64; ++i) para_insert(p, p->length, " ", 1, span_attr[depth_span]);
        } else if (kind == XML_EMPTY && xml_is(&x, "tab")) para_insert(p, p->length, "\t", 1, span_attr[depth_span]);
        else if (kind == XML_EMPTY && xml_is(&x, "line-break")) para_insert(p, p->length, " ", 1, span_attr[depth_span]);
    }
    free(xml_text);
    return 0;
}

static int save_odt(const struct document *d, const char *path) {
    struct text c = {0};
    text_put(&c, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                 "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
                 "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.3\"><office:automatic-styles>");
    for (int a = 1; a < 8; ++a)
        text_printf(&c, "<style:style style:name=\"T%d\" style:family=\"text\"><style:text-properties%s%s%s/></style:style>", a,
                    a & ATTR_BOLD ? " fo:font-weight=\"bold\"" : "", a & ATTR_ITALIC ? " fo:font-style=\"italic\"" : "",
                    a & ATTR_UNDERLINE ? " style:text-underline-style=\"solid\"" : "");
    text_put(&c, "<style:style style:name=\"PC\" style:family=\"paragraph\"><style:paragraph-properties fo:text-align=\"center\"/></style:style>"
                 "<style:style style:name=\"PR\" style:family=\"paragraph\"><style:paragraph-properties fo:text-align=\"end\"/></style:style>"
                 "</office:automatic-styles><office:body><office:text>");
    for (int i = 0; i < d->count; ++i) {
        const struct para *p = &d->paras[i];
        const char *align = p->align == ALIGN_CENTER ? " text:style-name=\"PC\"" : p->align == ALIGN_RIGHT ? " text:style-name=\"PR\"" : "";
        if (p->style == STYLE_BULLET) text_put(&c, "<text:list><text:list-item>");
        if (p->style >= STYLE_HEADING1 && p->style <= STYLE_HEADING3) text_printf(&c, "<text:h text:outline-level=\"%d\"%s>", p->style, align);
        else if (p->style == STYLE_TITLE) text_put(&c, "<text:p text:style-name=\"Title\">");
        else text_printf(&c, "<text:p%s>", align);
        for (int a = 0; a < p->length;) {
            int b = a;
            while (b < p->length && p->attr[b] == p->attr[a]) ++b;
            if (p->attr[a]) text_printf(&c, "<text:span text:style-name=\"T%d\">", p->attr[a] & 7);
            put_utf8(&c, p->text + a, b - a, 1);
            if (p->attr[a]) text_put(&c, "</text:span>");
            a = b;
        }
        text_put(&c, p->style >= STYLE_HEADING1 && p->style <= STYLE_HEADING3 ? "</text:h>" : "</text:p>");
        if (p->style == STYLE_BULLET) text_put(&c, "</text:list-item></text:list>");
    }
    text_put(&c, "</office:text></office:body></office:document-content>");
    static const char manifest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "</manifest:manifest>";
    static const char mimetype[] = "application/vnd.oasis.opendocument.text";
    struct zip_writer w;
    zip_begin(&w);
    zip_add(&w, "mimetype", mimetype, strlen(mimetype), 0);
    zip_add(&w, "META-INF/manifest.xml", manifest, strlen(manifest), 1);
    zip_add(&w, "content.xml", c.data, c.size, 1);
    free(c.data);
    return zip_finish(&w, path);
}

/* ---- Rich Text Format (.rtf) --------------------------------------------- */

static int load_rtf(struct document *d, const char *data) {
    int attr_stack[64], depth = 0, attr = 0, skip_depth = -1, skip_chars = 0;
    struct para *p = doc_add_para(d, STYLE_NORMAL);
    for (const char *s = data; *s && p;) {
        char c = *s;
        if (c == '{') { if (depth < 63) attr_stack[depth] = attr; ++depth; ++s; continue; }
        if (c == '}') { --depth; if (depth >= 0 && depth < 63) attr = attr_stack[depth]; if (depth < skip_depth) skip_depth = -1; ++s; continue; }
        if (c == '\r' || c == '\n') { ++s; continue; }
        if (c != '\\') {
            if (skip_depth < 0) { if (skip_chars) --skip_chars; else para_insert(p, p->length, &c, 1, attr); }
            ++s;
            continue;
        }
        ++s;
        if (*s == '\\' || *s == '{' || *s == '}') { if (skip_depth < 0) para_insert(p, p->length, s, 1, attr); ++s; continue; }
        if (*s == '\'') {
            char hex[3] = {s[1], s[2], 0};
            char v = (char)strtol(hex, 0, 16);
            if (skip_depth < 0) { if (skip_chars) --skip_chars; else para_insert(p, p->length, &v, 1, attr); }
            s += 3;
            continue;
        }
        if (*s == '*') { skip_depth = depth; ++s; continue; }
        char word[32];
        int n = 0;
        while (isalpha((unsigned char)*s) && n < 31) word[n++] = *s++;
        word[n] = 0;
        int has_value = *s == '-' || isdigit((unsigned char)*s);
        long value = has_value ? strtol(s, (char **)&s, 10) : 1;
        if (*s == ' ') ++s;
        if (!n) { ++s; continue; }
        if (!strcmp(word, "fonttbl") || !strcmp(word, "colortbl") || !strcmp(word, "stylesheet") || !strcmp(word, "info") ||
            !strcmp(word, "pict") || !strcmp(word, "header") || !strcmp(word, "footer")) skip_depth = depth;
        else if (skip_depth >= 0) continue;
        else if (!strcmp(word, "b")) attr = value ? attr | ATTR_BOLD : attr & ~ATTR_BOLD;
        else if (!strcmp(word, "i")) attr = value ? attr | ATTR_ITALIC : attr & ~ATTR_ITALIC;
        else if (!strcmp(word, "ul")) attr |= ATTR_UNDERLINE;
        else if (!strcmp(word, "ulnone")) attr &= ~ATTR_UNDERLINE;
        else if (!strcmp(word, "plain")) attr = 0;
        else if (!strcmp(word, "par") || !strcmp(word, "line")) p = doc_add_para(d, STYLE_NORMAL);
        else if (!strcmp(word, "qc")) p->align = ALIGN_CENTER;
        else if (!strcmp(word, "qr")) p->align = ALIGN_RIGHT;
        else if (!strcmp(word, "tab")) para_insert(p, p->length, "\t", 1, attr);
        else if (!strcmp(word, "u")) { char v = (char)(value > 0 && value < 256 ? value : '?'); para_insert(p, p->length, &v, 1, attr); skip_chars = 1; }
    }
    return 0;
}

static int save_rtf(const struct document *d, const char *path) {
    struct text t = {0};
    text_put(&t, "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Arial;}}\n");
    static const int sizes[STYLE_COUNT] = {22, 32, 28, 24, 40, 22};
    for (int i = 0; i < d->count; ++i) {
        const struct para *p = &d->paras[i];
        text_printf(&t, "\\pard%s\\fs%d%s ", p->align == ALIGN_CENTER ? "\\qc" : p->align == ALIGN_RIGHT ? "\\qr" : "", sizes[p->style],
                    p->style >= STYLE_HEADING1 && p->style <= STYLE_TITLE ? "\\b" : "");
        if (p->style == STYLE_BULLET) text_put(&t, "\\bullet\\tab ");
        int attr = 0;
        for (int k = 0; k < p->length; ++k) {
            if (p->attr[k] != attr) {
                text_printf(&t, "\\b%s\\i%s\\ul%s ", p->attr[k] & ATTR_BOLD ? "" : "0", p->attr[k] & ATTR_ITALIC ? "" : "0",
                            p->attr[k] & ATTR_UNDERLINE ? "" : "none");
                attr = p->attr[k];
            }
            unsigned char c = (unsigned char)p->text[k];
            if (c == '\\' || c == '{' || c == '}') text_printf(&t, "\\%c", c);
            else if (c == '\t') text_put(&t, "\\tab ");
            else if (c >= 0x80) text_printf(&t, "\\'%02x", c);
            else text_add(&t, (const char *)&c, 1);
        }
        text_put(&t, "\\b0\\i0\\ulnone\\par\n");
    }
    text_put(&t, "}\n");
    int r = write_whole(path, t.data, t.size);
    free(t.data);
    return r;
}

/* ---- HTML and plain text ------------------------------------------------- */

static int load_html(struct document *d, const char *data, size_t size) {
    struct xml x;
    xml_start(&x, data, size);
    int kind, attr = 0, style = STYLE_NORMAL, skip = 0;
    struct para *p = 0;
    while ((kind = xml_next(&x)) != XML_END) {
        if (kind == XML_OPEN || kind == XML_CLOSE) {
            int open = kind == XML_OPEN;
            const char *n = x.name;
            if (!strcasecmp(n, "script") || !strcasecmp(n, "style") || !strcasecmp(n, "head")) { skip += open ? 1 : -1; continue; }
            int bit = !strcasecmp(n, "b") || !strcasecmp(n, "strong") ? ATTR_BOLD : !strcasecmp(n, "i") || !strcasecmp(n, "em") ? ATTR_ITALIC :
                      !strcasecmp(n, "u") ? ATTR_UNDERLINE : 0;
            if (bit) { attr = open ? attr | bit : attr & ~bit; continue; }
            int block = !strcasecmp(n, "p") || !strcasecmp(n, "div") || !strcasecmp(n, "li") || (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]) ||
                        !strcasecmp(n, "tr") || !strcasecmp(n, "title");
            if (block && open) {
                style = !strcasecmp(n, "h1") || !strcasecmp(n, "title") ? STYLE_HEADING1 : !strcasecmp(n, "h2") ? STYLE_HEADING2 :
                        n[0] == 'h' && isdigit((unsigned char)n[1]) ? STYLE_HEADING3 : !strcasecmp(n, "li") ? STYLE_BULLET : STYLE_NORMAL;
                p = 0;
            } else if (block) p = 0;
        } else if (kind == XML_EMPTY && !strcasecmp(x.name, "br")) p = 0;
        else if (kind == XML_TEXT && !skip) {
            const char *t = x.text;
            while (*t && isspace((unsigned char)*t)) ++t;
            if (!*t) continue;
            if (!p) { p = doc_add_para(d, style); style = STYLE_NORMAL; }
            add_utf8(p, t, attr);
        }
    }
    return 0;
}

static void add_markdown(struct para *p, char *body) {
    int attr = 0;
    char *run = body;
    for (char *c = body; *c;) {
        int bit = 0, length = 0;
        if ((c[0] == '*' && c[1] == '*') || (c[0] == '_' && c[1] == '_')) { bit = ATTR_BOLD; length = 2; }
        else if (c[0] == '*' || (c[0] == '_' && (c == body || c[-1] == ' ' || (attr & ATTR_ITALIC)))) { bit = ATTR_ITALIC; length = 1; }
        if (!bit) { ++c; continue; }
        char saved = *c;
        *c = 0;
        add_utf8(p, run, attr);
        *c = saved;
        attr ^= bit;
        c += length;
        run = c;
    }
    add_utf8(p, run, attr);
}

static int load_text(struct document *d, const char *data, int markdown) {
    for (const char *s = data; *s;) {
        const char *end = strchr(s, '\n');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        char *line = malloc(n + 1);
        if (!line) return -1;
        memcpy(line, s, n);
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        s = end ? end + 1 : s + n;
        if (!markdown) { add_utf8(doc_add_para(d, STYLE_NORMAL), line, 0); free(line); continue; }
        int style = STYLE_NORMAL;
        char *body = line;
        while (*body == ' ') ++body;
        if (!*body) { free(line); continue; }
        if (!strncmp(body, "### ", 4)) { style = STYLE_HEADING3; body += 4; }
        else if (!strncmp(body, "## ", 3)) { style = STYLE_HEADING2; body += 3; }
        else if (!strncmp(body, "# ", 2)) { style = STYLE_HEADING1; body += 2; }
        else if (!strncmp(body, "- ", 2) || !strncmp(body, "* ", 2) || !strncmp(body, "+ ", 2)) { style = STYLE_BULLET; body += 2; }
        add_markdown(doc_add_para(d, style), body);
        free(line);
    }
    return 0;
}

static int save_text(const struct document *d, const char *path, int markdown, int html) {
    struct text t = {0};
    if (html) text_put(&t, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"></head><body>\n");
    static const char *const tags[STYLE_COUNT] = {"p", "h1", "h2", "h3", "h1", "li"};
    static const char *const marks[STYLE_COUNT] = {"", "# ", "## ", "### ", "# ", "- "};
    for (int i = 0; i < d->count; ++i) {
        const struct para *p = &d->paras[i];
        if (html) {
            if (p->style == STYLE_BULLET && (i == 0 || d->paras[i - 1].style != STYLE_BULLET)) text_put(&t, "<ul>");
            text_printf(&t, "<%s%s>", tags[p->style], p->align == ALIGN_CENTER ? " style=\"text-align:center\"" : p->align == ALIGN_RIGHT ? " style=\"text-align:right\"" : "");
            for (int a = 0; a < p->length;) {
                int b = a;
                while (b < p->length && p->attr[b] == p->attr[a]) ++b;
                if (p->attr[a] & ATTR_BOLD) text_put(&t, "<b>");
                if (p->attr[a] & ATTR_ITALIC) text_put(&t, "<i>");
                if (p->attr[a] & ATTR_UNDERLINE) text_put(&t, "<u>");
                put_utf8(&t, p->text + a, b - a, 1);
                if (p->attr[a] & ATTR_UNDERLINE) text_put(&t, "</u>");
                if (p->attr[a] & ATTR_ITALIC) text_put(&t, "</i>");
                if (p->attr[a] & ATTR_BOLD) text_put(&t, "</b>");
                a = b;
            }
            text_printf(&t, "</%s>\n", tags[p->style]);
            if (p->style == STYLE_BULLET && (i + 1 == d->count || d->paras[i + 1].style != STYLE_BULLET)) text_put(&t, "</ul>\n");
        } else {
            if (!markdown) { put_utf8(&t, p->text, p->length, 0); text_put(&t, "\r\n"); continue; }
            text_put(&t, marks[p->style]);
            for (int a = 0; a < p->length;) {
                int b = a;
                while (b < p->length && p->attr[b] == p->attr[a]) ++b;
                const char *mark = (p->attr[a] & (ATTR_BOLD | ATTR_ITALIC)) == (ATTR_BOLD | ATTR_ITALIC) ? "***" : p->attr[a] & ATTR_BOLD ? "**" : p->attr[a] & ATTR_ITALIC ? "*" : "";
                text_put(&t, mark);
                put_utf8(&t, p->text + a, b - a, 0);
                text_put(&t, mark);
                a = b;
            }
            text_put(&t, p->style == STYLE_BULLET && i + 1 < d->count && d->paras[i + 1].style == STYLE_BULLET ? "\r\n" : "\r\n\r\n");
        }
    }
    if (html) text_put(&t, "</body></html>\n");
    int r = write_whole(path, t.data ? t.data : "", t.size);
    free(t.data);
    return r;
}

int doc_load(struct document *d, const char *path, char *error, int error_size) {
    doc_free(d);
    const char *ext = extension_of(path);
    int r;
    if (!strcasecmp(ext, "docx") || !strcasecmp(ext, "docm") || !strcasecmp(ext, "dotx")) r = load_docx(d, path, error, error_size);
    else if (!strcasecmp(ext, "odt") || !strcasecmp(ext, "ott")) r = load_odt(d, path, error, error_size);
    else {
        size_t size;
        char *data = read_whole(path, &size);
        if (!data) { snprintf(error, (size_t)error_size, "The file could not be read."); return -1; }
        if (!strcasecmp(ext, "rtf") || !strncmp(data, "{\\rtf", 5)) r = load_rtf(d, data);
        else if (!strcasecmp(ext, "html") || !strcasecmp(ext, "htm") || !strcasecmp(ext, "xhtml")) r = load_html(d, data, size);
        else if (!strcasecmp(ext, "doc") && size > 8 && (unsigned char)data[0] == 0xd0 && (unsigned char)data[1] == 0xcf) {
            snprintf(error, (size_t)error_size, "Word 97-2003 (.doc) files cannot be opened; save them as .docx.");
            r = -1;
        } else r = load_text(d, data, !strcasecmp(ext, "md") || !strcasecmp(ext, "markdown"));
        free(data);
    }
    if (!d->count) doc_add_para(d, STYLE_NORMAL);
    return r;
}

int doc_save(const struct document *d, const char *path, char *error, int error_size) {
    const char *ext = extension_of(path);
    int r;
    if (!strcasecmp(ext, "docx")) r = save_docx(d, path);
    else if (!strcasecmp(ext, "odt")) r = save_odt(d, path);
    else if (!strcasecmp(ext, "rtf")) r = save_rtf(d, path);
    else if (!strcasecmp(ext, "html") || !strcasecmp(ext, "htm")) r = save_text(d, path, 0, 1);
    else r = save_text(d, path, !strcasecmp(ext, "md"), 0);
    if (r) snprintf(error, (size_t)error_size, "The file could not be written.");
    return r;
}
