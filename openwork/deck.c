/*
 * DOS-XC /openwork/deck.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "deck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define EMU 12700
#define MAX_RELS 1024

const struct deck_theme deck_themes[DECK_THEMES] = {
    {"Office", 0xffffff, 0x262626, 0x404040, 0xc42b1c},
    {"Ink", 0x1f2a44, 0xffffff, 0xd9dee8, 0x5b9bd5},
    {"Sky", 0xddebf7, 0x1f4e79, 0x2f3e4e, 0x2e75b6},
    {"Forest", 0xe2efda, 0x375623, 0x3a3a3a, 0x70ad47},
    {"Ember", 0x3b1f1a, 0xffc000, 0xf2e6df, 0xed7d31},
    {"Slate", 0x595959, 0xffffff, 0xf2f2f2, 0xffc000},
};
const char *const layout_names[LAYOUT_COUNT] = {"Title Slide", "Title and Content", "Section Header", "Title Only", "Blank"};

int box_style(int layout, int box, struct box_style *out) {
    static const struct box_style styles[LAYOUT_COUNT][BOX_COUNT] = {
        {{120, 150, 720, 130, 54, 1, 2, 0}, {120, 292, 720, 90, 24, 1, 0, 0}},
        {{66, 29, 828, 105, 40, 0, 1, 0}, {66, 144, 828, 343, 24, 0, 0, 1}},
        {{66, 135, 828, 225, 48, 0, 2, 0}, {66, 362, 828, 118, 20, 0, 0, 0}},
        {{66, 29, 828, 105, 40, 0, 1, 0}, {0}},
        {{0}, {0}},
    };
    if (layout < 0 || layout >= LAYOUT_COUNT || box < 0 || box >= BOX_COUNT) return 0;
    *out = styles[layout][box];
    return out->w > 0;
}

const char *box_prompt(int layout, int box) {
    if (box == BOX_TITLE) return "Click to add title";
    return layout == LAYOUT_TITLE ? "Click to add subtitle" : "Click to add text";
}

void deck_init(struct deck *d) { memset(d, 0, sizeof(*d)); }

static void slide_free(struct slide *s) {
    for (int b = 0; b < BOX_COUNT; ++b) free(s->text[b]);
    free(s->notes);
}

void deck_free(struct deck *d) {
    for (int i = 0; i < d->count; ++i) slide_free(&d->slides[i]);
    free(d->slides);
    memset(d, 0, sizeof(*d));
}

void deck_set(char **field, const char *s, size_t n) {
    char *copy = malloc(n + 1);
    if (!copy) return;
    memcpy(copy, s, n);
    copy[n] = 0;
    free(*field);
    *field = copy;
}

struct slide *deck_insert(struct deck *d, int at, int layout) {
    if (d->count == d->capacity) {
        int capacity = d->capacity ? d->capacity * 2 : 16;
        struct slide *bigger = realloc(d->slides, sizeof(*bigger) * (size_t)capacity);
        if (!bigger) return 0;
        d->slides = bigger;
        d->capacity = capacity;
    }
    if (at < 0 || at > d->count) at = d->count;
    memmove(d->slides + at + 1, d->slides + at, sizeof(struct slide) * (size_t)(d->count - at));
    ++d->count;
    struct slide *s = &d->slides[at];
    memset(s, 0, sizeof(*s));
    s->layout = layout;
    for (int b = 0; b < BOX_COUNT; ++b) deck_set(&s->text[b], "", 0);
    deck_set(&s->notes, "", 0);
    return s;
}

void deck_remove(struct deck *d, int at) {
    if (at < 0 || at >= d->count) return;
    slide_free(&d->slides[at]);
    memmove(d->slides + at, d->slides + at + 1, sizeof(struct slide) * (size_t)(d->count - at - 1));
    --d->count;
}

static void trim_newline(struct text *t) {
    while (t->size && t->data[t->size - 1] == '\n') t->data[--t->size] = 0;
}

static void take(char **field, struct text *t) {
    trim_newline(t);
    deck_set(field, t->data ? t->data : "", t->size);
    free(t->data);
    memset(t, 0, sizeof(*t));
}

static int theme_for(unsigned background) {
    for (int i = 0; i < DECK_THEMES; ++i)
        if (deck_themes[i].background == background) return i;
    return 0;
}

/* ---- PowerPoint (.pptx) ---- */

struct rel { char id[32], target[200], type[120]; };

static int read_rels(const struct zip *z, const char *name, struct rel *out, int max) {
    size_t n;
    char *data = zip_read(z, name, &n);
    if (!data) return 0;
    struct xml x;
    xml_start(&x, data, n);
    int count = 0, kind;
    while ((kind = xml_next(&x)) != XML_END && count < max)
        if ((kind == XML_EMPTY || kind == XML_OPEN) && xml_is(&x, "Relationship")) {
            struct rel *r = &out[count];
            if (xml_attribute(&x, "Id", r->id, sizeof(r->id)) || xml_attribute(&x, "Target", r->target, sizeof(r->target))) continue;
            if (xml_attribute(&x, "Type", r->type, sizeof(r->type))) r->type[0] = 0;
            ++count;
        }
    free(data);
    return count;
}

/* TARGET relative to the folder of FROM ("ppt/slides/slide1.xml"). */
static void resolve(const char *from, const char *target, char *out, size_t size) {
    if (target[0] == '/') { snprintf(out, size, "%s", target + 1); return; }
    char folder[256];
    snprintf(folder, sizeof(folder), "%s", from);
    char *slash = strrchr(folder, '/');
    if (slash) slash[1] = 0;
    else folder[0] = 0;
    while (!strncmp(target, "../", 3)) {
        target += 3;
        size_t n = strlen(folder);
        if (n) folder[n - 1] = 0;
        slash = strrchr(folder, '/');
        if (slash) slash[1] = 0;
        else folder[0] = 0;
    }
    snprintf(out, size, "%s%s", folder, target);
}

static void rels_name(const char *part, char *out, size_t size) {
    const char *slash = strrchr(part, '/');
    if (slash) snprintf(out, size, "%.*s/_rels/%s.rels", (int)(slash - part), part, slash + 1);
    else snprintf(out, size, "_rels/%s.rels", part);
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

/* Text of a slide's shapes: titles to TITLE, everything else (or a notes page's body) to BODY. */
static void read_shapes(const char *data, size_t n, struct text *title, struct text *body, int *has_title, int *centered_title, unsigned *background, int notes) {
    struct xml x;
    xml_start(&x, data, n);
    int kind, role = -1, in_text = 0, in_bg = 0, level = 0, para_open = 0;
    struct text *into = 0;
    while ((kind = xml_next(&x)) != XML_END) {
        if (kind == XML_OPEN && xml_is(&x, "sp")) { role = notes ? -2 : BOX_BODY; into = 0; }
        else if (kind == XML_CLOSE && xml_is(&x, "sp")) { role = -1; into = 0; }
        else if (kind == XML_OPEN && xml_is(&x, "bg")) in_bg = 1;
        else if (kind == XML_CLOSE && xml_is(&x, "bg")) in_bg = 0;
        else if (in_bg && kind == XML_EMPTY && xml_is(&x, "srgbClr") && background) {
            char v[16];
            if (!xml_attribute(&x, "val", v, sizeof(v))) *background = (unsigned)strtoul(v, 0, 16);
        } else if ((kind == XML_EMPTY || kind == XML_OPEN) && xml_is(&x, "ph") && role != -1) {
            char type[32] = "";
            xml_attribute(&x, "type", type, sizeof(type));
            if (notes) role = !type[0] || !strcmp(type, "body") ? BOX_BODY : -2;
            else if (!strcmp(type, "title") || !strcmp(type, "ctrTitle")) {
                role = BOX_TITLE;
                if (!strcmp(type, "ctrTitle") && centered_title) *centered_title = 1;
            } else if (!strcmp(type, "sldNum") || !strcmp(type, "dt") || !strcmp(type, "ftr")) role = -2;
            else role = BOX_BODY;
        } else if (kind == XML_OPEN && xml_is(&x, "txBody") && role >= 0) {
            into = role == BOX_TITLE ? title : body;
            if (role == BOX_TITLE && has_title) *has_title = 1;
            if (into == body && into->size && into->data[into->size - 1] != '\n') text_put(into, "\n");
        } else if (kind == XML_CLOSE && xml_is(&x, "txBody")) into = 0;
        else if (into && (kind == XML_OPEN || kind == XML_EMPTY) && xml_is(&x, "p")) { level = 0; para_open = 1; }
        else if (into && (kind == XML_OPEN || kind == XML_EMPTY) && xml_is(&x, "pPr")) {
            char v[8];
            if (!xml_attribute(&x, "lvl", v, sizeof(v))) level = atoi(v);
        } else if (into && kind == XML_OPEN && xml_is(&x, "t")) {
            if (para_open && into == body) for (int i = 0; i < level && i < 4; ++i) text_put(into, "\t");
            para_open = 0;
            in_text = 1;
        } else if (kind == XML_CLOSE && xml_is(&x, "t")) in_text = 0;
        else if (into && kind == XML_EMPTY && xml_is(&x, "br")) text_put(into, into == title ? " " : "\n");
        else if (into && kind == XML_CLOSE && xml_is(&x, "p")) {
            if (into == title) { if (into->size && into->data[into->size - 1] != ' ') text_put(into, " "); }
            else text_put(into, "\n");
            para_open = 0;
        } else if (into && in_text && kind == XML_TEXT) text_latin1(into, x.text, strlen(x.text));
    }
    while (title->size && title->data[title->size - 1] == ' ') title->data[--title->size] = 0;
}

/* The layout a slide's layout part declares, or -1. */
static int layout_of(const struct zip *z, const char *slide, const char *target) {
    char part[256];
    resolve(slide, target, part, sizeof(part));
    size_t n;
    char *data = zip_read(z, part, &n);
    if (!data) return -1;
    struct xml x;
    xml_start(&x, data, n);
    int kind, layout = -1;
    while ((kind = xml_next(&x)) != XML_END)
        if (kind == XML_OPEN && xml_is(&x, "sldLayout")) {
            char type[32] = "";
            xml_attribute(&x, "type", type, sizeof(type));
            static const char *const types[] = {"title", "secHead", "titleOnly", "blank", "obj", "tx", "twoObj", "twoTxTwoObj", "objTx", "txAndObj"};
            static const int layouts[] = {LAYOUT_TITLE, LAYOUT_SECTION, LAYOUT_TITLE_ONLY, LAYOUT_BLANK, LAYOUT_CONTENT, LAYOUT_CONTENT, LAYOUT_CONTENT, LAYOUT_CONTENT, LAYOUT_CONTENT, LAYOUT_CONTENT};
            for (size_t i = 0; i < sizeof(types) / sizeof(*types); ++i)
                if (!strcmp(type, types[i])) layout = layouts[i];
            break;
        }
    free(data);
    return layout;
}

static int load_pptx(struct deck *d, const char *path, char *error, int size) {
    struct zip z;
    if (zip_open(&z, path)) { snprintf(error, (size_t)size, "This is not a PowerPoint presentation."); return -1; }
    size_t n;
    char *pres = zip_read(&z, "ppt/presentation.xml", &n);
    struct rel *rels = calloc(MAX_RELS, sizeof(*rels));
    if (!pres || !rels) { free(pres); free(rels); zip_close(&z); snprintf(error, (size_t)size, "The presentation has no slides part."); return -1; }
    int rel_count = read_rels(&z, "ppt/_rels/presentation.xml.rels", rels, MAX_RELS);
    struct xml x;
    xml_start(&x, pres, n);
    int kind;
    unsigned background = 0xffffffff;
    while ((kind = xml_next(&x)) != XML_END) {
        if (!((kind == XML_EMPTY || kind == XML_OPEN) && xml_is(&x, "sldId"))) continue;
        char id[32], part[256] = "";
        if (xml_attribute(&x, "r:id", id, sizeof(id))) continue;
        for (int i = 0; i < rel_count; ++i)
            if (!strcmp(rels[i].id, id)) resolve("ppt/presentation.xml", rels[i].target, part, sizeof(part));
        size_t sn;
        char *slide_xml = part[0] ? zip_read(&z, part, &sn) : 0;
        if (!slide_xml) continue;
        struct text title = {0}, body = {0}, notes = {0};
        int has_title = 0, centered = 0;
        unsigned bg = 0xffffffff;
        read_shapes(slide_xml, sn, &title, &body, &has_title, &centered, &bg, 0);
        free(slide_xml);
        if (background == 0xffffffff && bg != 0xffffffff) background = bg;
        struct rel slide_rels[32];
        char name[300];
        rels_name(part, name, sizeof(name));
        int sr = read_rels(&z, name, slide_rels, 32);
        int declared = -1;
        for (int i = 0; i < sr; ++i) {
            if (ends_with(slide_rels[i].type, "/slideLayout")) declared = layout_of(&z, part, slide_rels[i].target);
            if (!ends_with(slide_rels[i].type, "/notesSlide")) continue;
            char notes_part[256];
            resolve(part, slide_rels[i].target, notes_part, sizeof(notes_part));
            size_t nn;
            char *notes_xml = zip_read(&z, notes_part, &nn);
            struct text unused = {0};
            if (notes_xml) read_shapes(notes_xml, nn, &unused, &notes, 0, 0, 0, 1);
            free(notes_xml);
            free(unused.data);
        }
        trim_newline(&body);
        int layout = centered ? LAYOUT_TITLE : body.size ? LAYOUT_CONTENT : has_title ? LAYOUT_TITLE_ONLY : LAYOUT_BLANK;
        if (layout == LAYOUT_BLANK && title.size) layout = LAYOUT_TITLE_ONLY;
        if (declared >= 0 && !(declared == LAYOUT_BLANK && (title.size || body.size)) && !(declared == LAYOUT_TITLE_ONLY && body.size)) layout = declared;
        struct slide *s = deck_insert(d, d->count, layout);
        if (!s) { free(title.data); free(body.data); free(notes.data); break; }
        take(&s->text[BOX_TITLE], &title);
        take(&s->text[BOX_BODY], &body);
        take(&s->notes, &notes);
    }
    free(pres);
    free(rels);
    zip_close(&z);
    d->theme = background == 0xffffffff ? 0 : theme_for(background);
    if (!d->count) { snprintf(error, (size_t)size, "The presentation has no slides."); return -1; }
    return 0;
}

static const char ns[] = "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                         "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
static const char group[] = "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
                            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";
static const char *const layout_types[LAYOUT_COUNT] = {"title", "obj", "secHead", "titleOnly", "blank"};

static const char theme_xml[] =
    "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Office Theme\"><a:themeElements>"
    "<a:clrScheme name=\"Office\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
    "<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2><a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2><a:accent1><a:srgbClr val=\"5B9BD5\"/></a:accent1>"
    "<a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2><a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
    "<a:accent5><a:srgbClr val=\"4472C4\"/></a:accent5><a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6><a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink>"
    "<a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink></a:clrScheme>"
    "<a:fontScheme name=\"Office\"><a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
    "<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme>"
    "<a:fmtScheme name=\"Office\"><a:fillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
    "<a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln><a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln></a:lnStyleLst>"
    "<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst>"
    "<a:bgFillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";

static const char clr_map[] = "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
                              "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>";

static void put_box(struct text *t, int id, int layout, int box, const char *s, unsigned color, int with_text) {
    struct box_style b;
    if (!box_style(layout, box, &b)) return;
    const char *ph = box == BOX_TITLE ? (layout == LAYOUT_TITLE ? "<p:ph type=\"ctrTitle\"/>" : "<p:ph type=\"title\"/>")
                                      : layout == LAYOUT_TITLE ? "<p:ph type=\"subTitle\" idx=\"1\"/>" : "<p:ph idx=\"1\"/>";
    text_printf(t, "<p:sp><p:nvSpPr><p:cNvPr id=\"%d\" name=\"%s %d\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr>%s</p:nvPr></p:nvSpPr>"
                   "<p:spPr><a:xfrm><a:off x=\"%ld\" y=\"%ld\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm></p:spPr>",
                id, box == BOX_TITLE ? "Title" : "Content Placeholder", id - 1, ph, (long)b.x * EMU, (long)b.y * EMU, (long)b.w * EMU, (long)b.h * EMU);
    if (!with_text) { text_put(t, "</p:sp>"); return; }
    text_printf(t, "<p:txBody><a:bodyPr anchor=\"%s\"><a:normAutofit/></a:bodyPr><a:lstStyle/>", b.anchor == 2 ? "b" : b.anchor == 1 ? "ctr" : "t");
    const char *p = s;
    do {
        size_t n = strcspn(p, "\n");
        int level = 0;
        while (level < (int)n && p[level] == '\t') ++level;
        int points = b.points - (b.bullets ? level * 4 : 0);
        if (points < 12) points = 12;
        text_put(t, "<a:p>");
        if (b.bullets) {
            long margin = 342900L + (long)level * 457200L;
            text_printf(t, "<a:pPr marL=\"%ld\" lvl=\"%d\" indent=\"-342900\"><a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/></a:pPr>", margin, level);
        } else text_printf(t, "<a:pPr algn=\"%s\"><a:buNone/></a:pPr>", b.align ? "ctr" : "l");
        if (n > (size_t)level) {
            text_printf(t, "<a:r><a:rPr lang=\"en-US\" sz=\"%d\" dirty=\"0\"><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill></a:rPr><a:t>", points * 100, color);
            text_utf8(t, p + level, n - (size_t)level, 1);
            text_put(t, "</a:t></a:r>");
        } else text_printf(t, "<a:endParaRPr lang=\"en-US\" sz=\"%d\" dirty=\"0\"/>", points * 100);
        text_put(t, "</a:p>");
        p += n;
    } while (*p++);
    text_put(t, "</p:txBody></p:sp>");
}

static int save_pptx(const struct deck *d, const char *path) {
    const struct deck_theme *th = &deck_themes[d->theme];
    struct text types = {0}, pres = {0}, pres_rels = {0}, master = {0}, master_rels = {0};
    text_put(&types, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                     "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                     "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                     "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
                     "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
                     "<Override PartName=\"/ppt/notesMasters/notesMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml\"/>"
                     "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
                     "<Override PartName=\"/ppt/theme/theme2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>");
    for (int i = 1; i <= LAYOUT_COUNT; ++i)
        text_printf(&types, "<Override PartName=\"/ppt/slideLayouts/slideLayout%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>", i);
    for (int i = 1; i <= d->count; ++i)
        text_printf(&types, "<Override PartName=\"/ppt/slides/slide%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>"
                            "<Override PartName=\"/ppt/notesSlides/notesSlide%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml\"/>", i, i);
    text_put(&types, "</Types>");

    static const char root_rels[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/></Relationships>";
    static const char rel_head[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    static const char rel_type[] = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

    text_printf(&pres, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:presentation %s saveSubsetFonts=\"1\">"
                       "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>"
                       "<p:notesMasterIdLst><p:notesMasterId r:id=\"rId3\"/></p:notesMasterIdLst><p:sldIdLst>", ns);
    text_printf(&pres_rels, "%s<Relationship Id=\"rId1\" Type=\"%sslideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>"
                            "<Relationship Id=\"rId2\" Type=\"%stheme\" Target=\"theme/theme1.xml\"/>"
                            "<Relationship Id=\"rId3\" Type=\"%snotesMaster\" Target=\"notesMasters/notesMaster1.xml\"/>", rel_head, rel_type, rel_type, rel_type);
    for (int i = 0; i < d->count; ++i) {
        text_printf(&pres, "<p:sldId id=\"%d\" r:id=\"rId%d\"/>", 256 + i, 10 + i);
        text_printf(&pres_rels, "<Relationship Id=\"rId%d\" Type=\"%sslide\" Target=\"slides/slide%d.xml\"/>", 10 + i, rel_type, i + 1);
    }
    text_printf(&pres, "</p:sldIdLst><p:sldSz cx=\"%ld\" cy=\"%ld\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>", (long)SLIDE_W * EMU, (long)SLIDE_H * EMU);
    text_put(&pres_rels, "</Relationships>");

    text_printf(&master, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:sldMaster %s><p:cSld>"
                         "<p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg><p:spTree>%s",
                ns, th->background, group);
    put_box(&master, 2, LAYOUT_CONTENT, BOX_TITLE, "", th->title, 0);
    put_box(&master, 3, LAYOUT_CONTENT, BOX_BODY, "", th->text, 0);
    text_printf(&master, "</p:spTree></p:cSld>%s<p:sldLayoutIdLst>", clr_map);
    text_printf(&master_rels, "%s<Relationship Id=\"rId%d\" Type=\"%stheme\" Target=\"../theme/theme1.xml\"/>", rel_head, LAYOUT_COUNT + 1, rel_type);
    for (int i = 0; i < LAYOUT_COUNT; ++i) {
        text_printf(&master, "<p:sldLayoutId id=\"%u\" r:id=\"rId%d\"/>", 2147483649u + (unsigned)i, i + 1);
        text_printf(&master_rels, "<Relationship Id=\"rId%d\" Type=\"%sslideLayout\" Target=\"../slideLayouts/slideLayout%d.xml\"/>", i + 1, rel_type, i + 1);
    }
    text_put(&master, "</p:sldLayoutIdLst></p:sldMaster>");
    text_put(&master_rels, "</Relationships>");

    struct zip_writer w;
    zip_begin(&w);
    zip_add(&w, "[Content_Types].xml", types.data, types.size, 1);
    zip_add(&w, "_rels/.rels", root_rels, strlen(root_rels), 1);
    zip_add(&w, "ppt/presentation.xml", pres.data, pres.size, 1);
    zip_add(&w, "ppt/_rels/presentation.xml.rels", pres_rels.data, pres_rels.size, 1);
    zip_add(&w, "ppt/slideMasters/slideMaster1.xml", master.data, master.size, 1);
    zip_add(&w, "ppt/slideMasters/_rels/slideMaster1.xml.rels", master_rels.data, master_rels.size, 1);
    struct text part = {0};
    text_printf(&part, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n%s", theme_xml);
    zip_add(&w, "ppt/theme/theme1.xml", part.data, part.size, 1);
    zip_add(&w, "ppt/theme/theme2.xml", part.data, part.size, 1);
    free(part.data);

    for (int i = 0; i < LAYOUT_COUNT; ++i) {
        char name[64];
        memset(&part, 0, sizeof(part));
        text_printf(&part, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:sldLayout %s type=\"%s\" preserve=\"1\"><p:cSld name=\"%s\"><p:spTree>%s",
                    ns, layout_types[i], layout_names[i], group);
        put_box(&part, 2, i, BOX_TITLE, "", th->title, 0);
        put_box(&part, 3, i, BOX_BODY, "", th->text, 0);
        text_put(&part, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>");
        snprintf(name, sizeof(name), "ppt/slideLayouts/slideLayout%d.xml", i + 1);
        zip_add(&w, name, part.data, part.size, 1);
        free(part.data);
        memset(&part, 0, sizeof(part));
        text_printf(&part, "%s<Relationship Id=\"rId1\" Type=\"%sslideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>", rel_head, rel_type);
        snprintf(name, sizeof(name), "ppt/slideLayouts/_rels/slideLayout%d.xml.rels", i + 1);
        zip_add(&w, name, part.data, part.size, 1);
        free(part.data);
    }

    memset(&part, 0, sizeof(part));
    text_printf(&part, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:notesMaster %s><p:cSld>"
                       "<p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg><p:spTree>%s</p:spTree></p:cSld>%s</p:notesMaster>", ns, group, clr_map);
    zip_add(&w, "ppt/notesMasters/notesMaster1.xml", part.data, part.size, 1);
    free(part.data);
    memset(&part, 0, sizeof(part));
    text_printf(&part, "%s<Relationship Id=\"rId1\" Type=\"%stheme\" Target=\"../theme/theme2.xml\"/></Relationships>", rel_head, rel_type);
    zip_add(&w, "ppt/notesMasters/_rels/notesMaster1.xml.rels", part.data, part.size, 1);
    free(part.data);

    for (int i = 0; i < d->count; ++i) {
        const struct slide *s = &d->slides[i];
        char name[64];
        memset(&part, 0, sizeof(part));
        text_printf(&part, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:sld %s><p:cSld>"
                           "<p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg><p:spTree>%s",
                    ns, th->background, group);
        put_box(&part, 2, s->layout, BOX_TITLE, s->text[BOX_TITLE], th->title, 1);
        put_box(&part, 3, s->layout, BOX_BODY, s->text[BOX_BODY], th->text, 1);
        text_put(&part, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>");
        snprintf(name, sizeof(name), "ppt/slides/slide%d.xml", i + 1);
        zip_add(&w, name, part.data, part.size, 1);
        free(part.data);
        memset(&part, 0, sizeof(part));
        text_printf(&part, "%s<Relationship Id=\"rId1\" Type=\"%sslideLayout\" Target=\"../slideLayouts/slideLayout%d.xml\"/>"
                           "<Relationship Id=\"rId2\" Type=\"%snotesSlide\" Target=\"../notesSlides/notesSlide%d.xml\"/></Relationships>",
                    rel_head, rel_type, s->layout + 1, rel_type, i + 1);
        snprintf(name, sizeof(name), "ppt/slides/_rels/slide%d.xml.rels", i + 1);
        zip_add(&w, name, part.data, part.size, 1);
        free(part.data);

        memset(&part, 0, sizeof(part));
        text_printf(&part, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:notes %s><p:cSld><p:spTree>%s"
                           "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image Placeholder 1\"/><p:cNvSpPr><a:spLocks noGrp=\"1\" noRot=\"1\" noChangeAspect=\"1\"/></p:cNvSpPr>"
                           "<p:nvPr><p:ph type=\"sldImg\"/></p:nvPr></p:nvSpPr><p:spPr/></p:sp>"
                           "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Notes Placeholder 2\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
                           "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>", ns, group);
        const char *p = s->notes;
        do {
            size_t n = strcspn(p, "\n");
            text_put(&part, "<a:p>");
            if (n) { text_put(&part, "<a:r><a:rPr lang=\"en-US\" dirty=\"0\"/><a:t>"); text_utf8(&part, p, n, 1); text_put(&part, "</a:t></a:r>"); }
            text_put(&part, "</a:p>");
            p += n;
        } while (*p++);
        text_put(&part, "</p:txBody></p:sp></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>");
        snprintf(name, sizeof(name), "ppt/notesSlides/notesSlide%d.xml", i + 1);
        zip_add(&w, name, part.data, part.size, 1);
        free(part.data);
        memset(&part, 0, sizeof(part));
        text_printf(&part, "%s<Relationship Id=\"rId1\" Type=\"%snotesMaster\" Target=\"../notesMasters/notesMaster1.xml\"/>"
                           "<Relationship Id=\"rId2\" Type=\"%sslide\" Target=\"../slides/slide%d.xml\"/></Relationships>", rel_head, rel_type, rel_type, i + 1);
        snprintf(name, sizeof(name), "ppt/notesSlides/_rels/notesSlide%d.xml.rels", i + 1);
        zip_add(&w, name, part.data, part.size, 1);
        free(part.data);
    }
    free(types.data);
    free(pres.data);
    free(pres_rels.data);
    free(master.data);
    free(master_rels.data);
    return zip_finish(&w, path);
}

/* ---- OpenDocument Presentation (.odp) ---- */

static int load_odp(struct deck *d, const char *path, char *error, int size) {
    struct zip z;
    if (zip_open(&z, path)) { snprintf(error, (size_t)size, "This is not an OpenDocument file."); return -1; }
    size_t n;
    char *content = zip_read(&z, "content.xml", &n);
    zip_close(&z);
    if (!content) { snprintf(error, (size_t)size, "The presentation has no content."); return -1; }
    struct xml x;
    xml_start(&x, content, n);
    int kind, role = -1, list_depth = 0, in_page = 0, in_notes = 0, para_open = 0;
    struct text title = {0}, body = {0}, notes = {0};
    int has_title = 0, centered = 0;
    unsigned background = 0xffffffff;
    char cls[32];
    while ((kind = xml_next(&x)) != XML_END) {
        if ((kind == XML_EMPTY || kind == XML_OPEN) && xml_is(&x, "drawing-page-properties") && background == 0xffffffff) {
            char v[16];
            if (!xml_attribute(&x, "fill-color", v, sizeof(v)) && v[0] == '#') background = (unsigned)strtoul(v + 1, 0, 16);
        } else if (kind == XML_OPEN && xml_is(&x, "page")) { in_page = 1; has_title = centered = 0; }
        else if (kind == XML_CLOSE && xml_is(&x, "page") && in_page) {
            in_page = 0;
            trim_newline(&body);
            int layout = centered ? LAYOUT_TITLE : body.size ? LAYOUT_CONTENT : has_title ? LAYOUT_TITLE_ONLY : LAYOUT_BLANK;
            struct slide *s = deck_insert(d, d->count, layout);
            if (s) { take(&s->text[BOX_TITLE], &title); take(&s->text[BOX_BODY], &body); take(&s->notes, &notes); }
        } else if (kind == XML_OPEN && xml_is(&x, "notes")) in_notes = 1;
        else if (kind == XML_CLOSE && xml_is(&x, "notes")) in_notes = 0;
        else if (kind == XML_OPEN && xml_is(&x, "frame") && in_page) {
            role = BOX_BODY;
            cls[0] = 0;
            if (!xml_attribute(&x, "presentation:class", cls, sizeof(cls)) || !xml_attribute(&x, "class", cls, sizeof(cls))) {
                if (!strcmp(cls, "title")) { role = BOX_TITLE; has_title = 1; }
                else if (!strcmp(cls, "subtitle")) centered = 1;
                else if (!strcmp(cls, "page") || !strcmp(cls, "graphic")) role = -1;
            }
            if (in_notes) role = !strcmp(cls, "notes") ? 2 : -1;
        } else if (kind == XML_CLOSE && xml_is(&x, "frame")) role = -1;
        else if (kind == XML_OPEN && xml_is(&x, "list")) ++list_depth;
        else if (kind == XML_CLOSE && xml_is(&x, "list")) --list_depth;
        else if (role >= 0 && kind == XML_OPEN && (xml_is(&x, "p") || xml_is(&x, "h"))) para_open = 1;
        else if (role >= 0 && (kind == XML_CLOSE || kind == XML_EMPTY) && (xml_is(&x, "p") || xml_is(&x, "h"))) {
            struct text *t = role == BOX_TITLE ? &title : role == 2 ? &notes : &body;
            if (para_open && kind == XML_EMPTY) para_open = 0;
            text_put(t, role == BOX_TITLE ? " " : "\n");
            para_open = 0;
        } else if (role >= 0 && kind == XML_EMPTY && xml_is(&x, "line-break")) text_put(role == BOX_TITLE ? &title : role == 2 ? &notes : &body, role == BOX_TITLE ? " " : "\n");
        else if (role >= 0 && kind == XML_EMPTY && xml_is(&x, "s")) text_put(role == BOX_TITLE ? &title : role == 2 ? &notes : &body, " ");
        else if (role >= 0 && kind == XML_TEXT) {
            struct text *t = role == BOX_TITLE ? &title : role == 2 ? &notes : &body;
            if (para_open && role == BOX_BODY) for (int i = 1; i < list_depth && i < 5; ++i) text_put(t, "\t");
            para_open = 0;
            text_latin1(t, x.text, strlen(x.text));
        }
    }
    free(content);
    free(title.data);
    free(body.data);
    free(notes.data);
    if (!d->count) { snprintf(error, (size_t)size, "The presentation has no slides."); return -1; }
    d->theme = background == 0xffffffff ? 0 : theme_for(background);
    for (int i = 0; i < d->count; ++i) {
        char *t = d->slides[i].text[BOX_TITLE];
        size_t len = strlen(t);
        while (len && t[len - 1] == ' ') t[--len] = 0;
    }
    return 0;
}

static void odp_text(struct text *t, const char *s, int bullets) {
    const char *p = s;
    do {
        size_t n = strcspn(p, "\n");
        int level = 0;
        while (level < (int)n && p[level] == '\t') ++level;
        if (bullets) for (int i = 0; i <= level; ++i) text_put(t, "<text:list><text:list-item>");
        text_put(t, "<text:p>");
        text_utf8(t, p + level, n - (size_t)level, 1);
        text_put(t, "</text:p>");
        if (bullets) for (int i = 0; i <= level; ++i) text_put(t, "</text:list-item></text:list>");
        p += n;
    } while (*p++);
}

static int save_odp(const struct deck *d, const char *path) {
    const struct deck_theme *th = &deck_themes[d->theme];
    struct text c = {0};
    text_printf(&c, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content "
                    "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
                    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
                    "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
                    "xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\" office:version=\"1.2\">"
                    "<office:automatic-styles><style:style style:name=\"dp1\" style:family=\"drawing-page\"><style:drawing-page-properties draw:fill=\"solid\" draw:fill-color=\"#%06x\"/></style:style>"
                    "<style:style style:name=\"T\" style:family=\"paragraph\"><style:text-properties fo:color=\"#%06x\" fo:font-size=\"40pt\"/></style:style>"
                    "<style:style style:name=\"B\" style:family=\"paragraph\"><style:text-properties fo:color=\"#%06x\" fo:font-size=\"24pt\"/></style:style>"
                    "</office:automatic-styles><office:body><office:presentation>", th->background, th->title, th->text);
    for (int i = 0; i < d->count; ++i) {
        const struct slide *s = &d->slides[i];
        text_printf(&c, "<draw:page draw:name=\"Slide %d\" draw:style-name=\"dp1\" draw:master-page-name=\"Default\">", i + 1);
        for (int b = 0; b < BOX_COUNT; ++b) {
            struct box_style st;
            if (!box_style(s->layout, b, &st)) continue;
            const char *cls = b == BOX_TITLE ? "title" : s->layout == LAYOUT_TITLE ? "subtitle" : "outline";
            text_printf(&c, "<draw:frame presentation:class=\"%s\" svg:x=\"%dpt\" svg:y=\"%dpt\" svg:width=\"%dpt\" svg:height=\"%dpt\"><draw:text-box>",
                        cls, st.x, st.y, st.w, st.h);
            struct text inner = {0};
            odp_text(&inner, s->text[b], st.bullets);
            const char *style = b == BOX_TITLE ? "<text:p text:style-name=\"T\">" : "<text:p text:style-name=\"B\">";
            for (const char *p = inner.data ? inner.data : ""; *p;) {
                if (!strncmp(p, "<text:p>", 8)) { text_put(&c, style); p += 8; }
                else { text_add(&c, p, 1); ++p; }
            }
            free(inner.data);
            text_put(&c, "</draw:text-box></draw:frame>");
        }
        text_put(&c, "<presentation:notes><draw:frame presentation:class=\"notes\"><draw:text-box>");
        odp_text(&c, s->notes, 0);
        text_put(&c, "</draw:text-box></draw:frame></presentation:notes></draw:page>");
    }
    text_put(&c, "</office:presentation></office:body></office:document-content>");
    static const char styles[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                                 "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.2\">"
                                 "<office:automatic-styles><style:page-layout style:name=\"PM1\"><style:page-layout-properties fo:page-width=\"33.867cm\" fo:page-height=\"19.05cm\" "
                                 "style:print-orientation=\"landscape\"/></style:page-layout></office:automatic-styles>"
                                 "<office:master-styles><style:master-page style:name=\"Default\" style:page-layout-name=\"PM1\"/></office:master-styles></office:document-styles>";
    static const char mimetype[] = "application/vnd.oasis.opendocument.presentation";
    static const char manifest[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
                                   "<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.presentation\"/>"
                                   "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
                                   "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/></manifest:manifest>";
    struct zip_writer w;
    zip_begin(&w);
    zip_add(&w, "mimetype", mimetype, strlen(mimetype), 0);
    zip_add(&w, "META-INF/manifest.xml", manifest, strlen(manifest), 1);
    zip_add(&w, "content.xml", c.data, c.size, 1);
    zip_add(&w, "styles.xml", styles, strlen(styles), 1);
    free(c.data);
    return zip_finish(&w, path);
}

int deck_load(struct deck *d, const char *path, char *error, int error_size) {
    const char *ext = extension_of(path);
    if (!strcasecmp(ext, "pptx") || !strcasecmp(ext, "pptm") || !strcasecmp(ext, "ppsx")) return load_pptx(d, path, error, error_size);
    if (!strcasecmp(ext, "odp")) return load_odp(d, path, error, error_size);
    if (!strcasecmp(ext, "ppt")) { snprintf(error, (size_t)error_size, "Old PowerPoint (.ppt) files cannot be opened; save them as .pptx."); return -1; }
    snprintf(error, (size_t)error_size, "OpenShow opens .pptx and .odp presentations.");
    return -1;
}

int deck_save(const struct deck *d, const char *path, char *error, int error_size) {
    const char *ext = extension_of(path);
    int r;
    if (!strcasecmp(ext, "pptx")) r = save_pptx(d, path);
    else if (!strcasecmp(ext, "odp")) r = save_odp(d, path);
    else { snprintf(error, (size_t)error_size, "OpenShow saves .pptx and .odp presentations."); return -1; }
    if (r) snprintf(error, (size_t)error_size, "The file could not be written.");
    return r;
}
