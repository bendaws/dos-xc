/*
 * DOS-XC /openwork/sheet.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "sheet.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { STATE_NONE, STATE_BUSY, STATE_DONE };
#define MAX_DEPTH 400

void sheet_init(struct sheet *s) { memset(s, 0, sizeof(*s)); }

static void forget(struct sheet *s) {
    for (int r = 0; r < s->rows; ++r)
        for (int c = 0; c < s->cols; ++c) { free(s->cache[r][c]); s->cache[r][c] = 0; s->state[r][c] = STATE_NONE; }
}

void sheet_free(struct sheet *s) {
    forget(s);
    for (int r = 0; r < s->rows; ++r)
        for (int c = 0; c < s->cols; ++c) free(s->cells[r][c]);
    memset(s, 0, sizeof(*s));
}

const char *sheet_get(const struct sheet *s, int row, int col) {
    if (row < 0 || col < 0 || row >= SHEET_ROWS || col >= SHEET_COLS) return "";
    return s->cells[row][col] ? s->cells[row][col] : "";
}

void sheet_set(struct sheet *s, int row, int col, const char *input) {
    if (row < 0 || col < 0 || row >= SHEET_ROWS || col >= SHEET_COLS) return;
    forget(s);
    free(s->cells[row][col]);
    s->cells[row][col] = input && *input ? strdup(input) : 0;
    if (s->cells[row][col]) {
        if (row >= s->rows) s->rows = row + 1;
        if (col >= s->cols) s->cols = col + 1;
    }
}

void cell_name(int row, int col, char *out, size_t size) {
    if (col < 26) snprintf(out, size, "%c%d", 'A' + col, row + 1);
    else snprintf(out, size, "%c%c%d", 'A' + col / 26 - 1, 'A' + col % 26, row + 1);
}

int cell_parse(const char *name, int *row, int *col) {
    const char *p = name;
    if (*p == '$') ++p;
    int c = 0, letters = 0;
    while (isalpha((unsigned char)*p) && letters < 3) { c = c * 26 + (toupper((unsigned char)*p) - 'A' + 1); ++p; ++letters; }
    if (!letters || isalpha((unsigned char)*p)) return 0;
    if (*p == '$') ++p;
    if (!isdigit((unsigned char)*p)) return 0;
    long r = strtol(p, (char **)&p, 10);
    if (isalnum((unsigned char)*p) || *p == '_' || *p == '(') return 0;
    if (r < 1 || r > SHEET_ROWS || c < 1 || c > SHEET_COLS) return 0;
    *row = (int)r - 1;
    *col = c - 1;
    return (int)(p - name);
}

/* ---- formulas ---- */

struct parser { struct sheet *s; const char *at; int depth, failed; };

static void set_number(struct value *v, double n) { v->kind = VALUE_NUMBER; v->number = n; v->text[0] = 0; }
static void set_text(struct value *v, const char *t) { v->kind = VALUE_TEXT; v->number = 0; snprintf(v->text, sizeof(v->text), "%s", t); }
static void set_error(struct value *v, const char *e) { v->kind = VALUE_ERROR; v->number = 0; snprintf(v->text, sizeof(v->text), "%s", e); }

static void skip(struct parser *p) { while (*p->at == ' ') ++p->at; }

static int as_number(const struct value *v, double *out) {
    if (v->kind == VALUE_NUMBER) { *out = v->number; return 0; }
    if (v->kind == VALUE_EMPTY) { *out = 0; return 0; }
    if (v->kind == VALUE_TEXT) {
        char *end;
        double n = strtod(v->text, &end);
        if (end != v->text && !*end) { *out = n; return 0; }
    }
    return -1;
}

static void evaluate_cell(struct sheet *s, int row, int col, struct value *out, int depth);
static void expression(struct parser *p, struct value *out);

/* A function argument: one value, or a range of cells. */
struct argument { struct value v; int range, r1, c1, r2, c2; };

typedef void (*each_fn)(const struct value *v, void *context);
static void each(struct parser *p, const struct argument *a, each_fn fn, void *context) {
    if (!a->range) { fn(&a->v, context); return; }
    for (int r = a->r1; r <= a->r2 && r < p->s->rows; ++r)
        for (int c = a->c1; c <= a->c2 && c < p->s->cols; ++c) {
            struct value v;
            evaluate_cell(p->s, r, c, &v, p->depth + 1);
            fn(&v, context);
        }
}

struct aggregate { double sum, min, max; int count, counta, error; char error_text[16]; int from_range; };
static void gather(const struct value *v, void *context) {
    struct aggregate *g = context;
    if (v->kind == VALUE_ERROR) { if (!g->error) snprintf(g->error_text, sizeof(g->error_text), "%s", v->text); g->error = 1; return; }
    if (v->kind != VALUE_EMPTY) ++g->counta;
    double n;
    if (v->kind == VALUE_NUMBER || (!g->from_range && !as_number(v, &n))) {
        n = v->kind == VALUE_NUMBER ? v->number : n;
        if (!g->count || n < g->min) g->min = n;
        if (!g->count || n > g->max) g->max = n;
        g->sum += n;
        ++g->count;
    }
}

static int truthy(const struct value *v) {
    double n;
    if (v->kind == VALUE_TEXT && !strcasecmp(v->text, "TRUE")) return 1;
    return !as_number(v, &n) && n != 0;
}

static void call(struct parser *p, const char *name, struct argument *args, int count, struct value *out) {
    struct aggregate g = {0};
    for (int i = 0; i < count; ++i) { g.from_range = args[i].range; each(p, &args[i], gather, &g); }
    double a = 0, b = 0;
    int numbers_ok = count >= 1 && !args[0].range && !as_number(&args[0].v, &a);
    if (count >= 2 && !args[1].range && as_number(&args[1].v, &b)) numbers_ok = 0;
    if (!strcasecmp(name, "SUM")) { if (g.error) set_error(out, g.error_text); else set_number(out, g.sum); }
    else if (!strcasecmp(name, "AVERAGE")) { if (g.error) set_error(out, g.error_text); else if (!g.count) set_error(out, "#DIV/0!"); else set_number(out, g.sum / g.count); }
    else if (!strcasecmp(name, "MIN")) { if (g.error) set_error(out, g.error_text); else set_number(out, g.count ? g.min : 0); }
    else if (!strcasecmp(name, "MAX")) { if (g.error) set_error(out, g.error_text); else set_number(out, g.count ? g.max : 0); }
    else if (!strcasecmp(name, "COUNT")) set_number(out, g.count);
    else if (!strcasecmp(name, "COUNTA")) set_number(out, g.counta);
    else if (!strcasecmp(name, "IF")) {
        if (count < 2 || args[0].range) { set_error(out, "#VALUE!"); return; }
        if (truthy(&args[0].v)) *out = args[1].v;
        else if (count >= 3) *out = args[2].v;
        else set_text(out, "FALSE");
    } else if (!strcasecmp(name, "AND") || !strcasecmp(name, "OR")) {
        int and = !strcasecmp(name, "AND"), result = and;
        for (int i = 0; i < count; ++i) if (!args[i].range) result = and ? result && truthy(&args[i].v) : result || truthy(&args[i].v);
        set_text(out, result ? "TRUE" : "FALSE");
    } else if (!strcasecmp(name, "NOT") && count == 1) set_text(out, truthy(&args[0].v) ? "FALSE" : "TRUE");
    else if (!strcasecmp(name, "PI") && count == 0) set_number(out, M_PI);
    else if (!strcasecmp(name, "LEN") && count == 1 && !args[0].range) {
        char t[160];
        value_format(&args[0].v, t, sizeof(t));
        set_number(out, (double)strlen(t));
    } else if ((!strcasecmp(name, "UPPER") || !strcasecmp(name, "LOWER")) && count == 1 && !args[0].range) {
        char t[128];
        value_format(&args[0].v, t, sizeof(t));
        for (char *c = t; *c; ++c) *c = (char)(toupper(name[0]) == 'U' ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
        set_text(out, t);
    } else if (!strcasecmp(name, "CONCATENATE") || !strcasecmp(name, "CONCAT")) {
        char t[128] = "";
        for (int i = 0; i < count; ++i) {
            char part[128];
            if (args[i].range) continue;
            value_format(&args[i].v, part, sizeof(part));
            strncat(t, part, sizeof(t) - strlen(t) - 1);
        }
        set_text(out, t);
    } else if (!numbers_ok) set_error(out, strcasecmp(name, "ABS") && strcasecmp(name, "ROUND") && strcasecmp(name, "SQRT") && strcasecmp(name, "INT") &&
                                               strcasecmp(name, "MOD") && strcasecmp(name, "POWER") ? "#NAME?" : "#VALUE!");
    else if (!strcasecmp(name, "ABS")) set_number(out, fabs(a));
    else if (!strcasecmp(name, "SQRT")) { if (a < 0) set_error(out, "#NUM!"); else set_number(out, sqrt(a)); }
    else if (!strcasecmp(name, "INT")) set_number(out, floor(a));
    else if (!strcasecmp(name, "ROUND")) { double k = pow(10, count >= 2 ? b : 0); set_number(out, round(a * k) / k); }
    else if (!strcasecmp(name, "MOD") && count == 2) { if (b == 0) set_error(out, "#DIV/0!"); else set_number(out, a - b * floor(a / b)); }
    else if (!strcasecmp(name, "POWER") && count == 2) set_number(out, pow(a, b));
    else set_error(out, "#NAME?");
}

static void primary(struct parser *p, struct value *out) {
    skip(p);
    int row, col, used;
    if (*p->at == '(') {
        ++p->at;
        expression(p, out);
        skip(p);
        if (*p->at == ')') ++p->at; else p->failed = 1;
    } else if (*p->at == '"') {
        char t[128];
        size_t n = 0;
        ++p->at;
        while (*p->at && !(*p->at == '"' && p->at[1] != '"')) {
            if (*p->at == '"') ++p->at;
            if (n + 1 < sizeof(t)) t[n++] = *p->at;
            ++p->at;
        }
        t[n] = 0;
        if (*p->at == '"') ++p->at; else p->failed = 1;
        set_text(out, t);
    } else if (isdigit((unsigned char)*p->at) || *p->at == '.') {
        char *end;
        set_number(out, strtod(p->at, &end));
        p->at = end;
    } else if ((used = cell_parse(p->at, &row, &col)) > 0) {
        p->at += used;
        evaluate_cell(p->s, row, col, out, p->depth + 1);
        if (out->kind == VALUE_EMPTY) set_number(out, 0);
    } else if (isalpha((unsigned char)*p->at)) {
        char name[24];
        size_t n = 0;
        while (isalnum((unsigned char)*p->at) || *p->at == '.' || *p->at == '_') { if (n + 1 < sizeof(name)) name[n++] = *p->at; ++p->at; }
        name[n] = 0;
        skip(p);
        if (*p->at != '(') {
            if (!strcasecmp(name, "TRUE") || !strcasecmp(name, "FALSE")) { for (char *c = name; *c; ++c) *c = (char)toupper((unsigned char)*c); set_text(out, name); }
            else set_error(out, "#NAME?");
            return;
        }
        ++p->at;
        struct argument args[16];
        int count = 0;
        skip(p);
        while (*p->at && *p->at != ')') {
            struct argument *a = &args[count < 16 ? count : 15];
            memset(a, 0, sizeof(*a));
            skip(p);
            int r1, c1, r2, c2, u1 = cell_parse(p->at, &r1, &c1), u2;
            if (u1 > 0 && p->at[u1] == ':' && (u2 = cell_parse(p->at + u1 + 1, &r2, &c2)) > 0) {
                a->range = 1;
                a->r1 = r1 < r2 ? r1 : r2; a->r2 = r1 < r2 ? r2 : r1;
                a->c1 = c1 < c2 ? c1 : c2; a->c2 = c1 < c2 ? c2 : c1;
                p->at += u1 + 1 + u2;
            } else expression(p, &a->v);
            if (count < 16) ++count;
            skip(p);
            if (*p->at == ',' || *p->at == ';') ++p->at;
            else if (*p->at != ')') { p->failed = 1; break; }
        }
        if (*p->at == ')') ++p->at; else p->failed = 1;
        if (!p->failed) call(p, name, args, count, out);
    } else p->failed = 1;
}

static void unary(struct parser *p, struct value *out) {
    skip(p);
    if (*p->at == '-' || *p->at == '+') {
        int negative = *p->at++ == '-';
        unary(p, out);
        double n;
        if (out->kind == VALUE_ERROR) return;
        if (as_number(out, &n)) { set_error(out, "#VALUE!"); return; }
        set_number(out, negative ? -n : n);
        return;
    }
    primary(p, out);
    skip(p);
    if (*p->at == '%') { ++p->at; double n; if (!as_number(out, &n)) set_number(out, n / 100); }
}

/* Binary operators by precedence: 0 comparisons, 1 &, 2 + -, 3 * /, 4 ^. */
static int operator_at(const char *s, int level, int *length) {
    *length = 1;
    switch (level) {
    case 0:
        if (!strncmp(s, "<=", 2) || !strncmp(s, ">=", 2) || !strncmp(s, "<>", 2)) { *length = 2; return s[0] == '<' ? (s[1] == '=' ? 'l' : 'n') : 'g'; }
        return *s == '=' || *s == '<' || *s == '>' ? *s : 0;
    case 1: return *s == '&' ? '&' : 0;
    case 2: return *s == '+' || *s == '-' ? *s : 0;
    case 3: return *s == '*' || *s == '/' ? *s : 0;
    default: return *s == '^' ? '^' : 0;
    }
}

static void binary(struct parser *p, struct value *out, int level) {
    if (level > 4) { unary(p, out); return; }
    binary(p, out, level + 1);
    for (;;) {
        skip(p);
        int length, op = operator_at(p->at, level, &length);
        if (!op || p->failed) return;
        p->at += length;
        struct value right;
        binary(p, &right, level + 1);
        if (out->kind == VALUE_ERROR) continue;
        if (right.kind == VALUE_ERROR) { *out = right; continue; }
        if (op == '&') {
            char a[128], b[128];
            value_format(out, a, sizeof(a));
            value_format(&right, b, sizeof(b));
            strncat(a, b, sizeof(a) - strlen(a) - 1);
            set_text(out, a);
            continue;
        }
        double x, y;
        int numeric = !as_number(out, &x) && !as_number(&right, &y);
        if (level == 0) {
            int c;
            if (numeric) c = x < y ? -1 : x > y;
            else { char a[128], b[128]; value_format(out, a, sizeof(a)); value_format(&right, b, sizeof(b)); c = strcasecmp(a, b); }
            int r = op == '=' ? c == 0 : op == 'n' ? c != 0 : op == '<' ? c < 0 : op == '>' ? c > 0 : op == 'l' ? c <= 0 : c >= 0;
            set_text(out, r ? "TRUE" : "FALSE");
            continue;
        }
        if (!numeric) { set_error(out, "#VALUE!"); continue; }
        if (op == '/' && y == 0) { set_error(out, "#DIV/0!"); continue; }
        set_number(out, op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : op == '/' ? x / y : pow(x, y));
    }
}

static void expression(struct parser *p, struct value *out) { binary(p, out, 0); }

static void evaluate(struct sheet *s, const char *input, struct value *out, int depth) {
    if (!input || !*input) { out->kind = VALUE_EMPTY; out->number = 0; out->text[0] = 0; return; }
    if (input[0] != '=') {
        char *end;
        double n = strtod(input, &end);
        while (*end == ' ') ++end;
        if (end != input && !*end && !isspace((unsigned char)input[0])) set_number(out, n);
        else set_text(out, input[0] == '\'' ? input + 1 : input);
        return;
    }
    struct parser p = {s, input + 1, depth, 0};
    expression(&p, out);
    skip(&p);
    if (p.failed || *p.at) set_error(out, "#ERROR!");
}

static void evaluate_cell(struct sheet *s, int row, int col, struct value *out, int depth) {
    if (row >= s->rows || col >= s->cols || !s->cells[row][col]) { out->kind = VALUE_EMPTY; out->number = 0; out->text[0] = 0; return; }
    if (s->state[row][col] == STATE_DONE) { *out = *s->cache[row][col]; return; }
    if (s->state[row][col] == STATE_BUSY) { set_error(out, "#CYCLE!"); return; }
    if (depth > MAX_DEPTH) { set_error(out, "#DEPTH!"); return; }
    s->state[row][col] = STATE_BUSY;
    evaluate(s, s->cells[row][col], out, depth);
    if (!s->cache[row][col]) s->cache[row][col] = malloc(sizeof(struct value));
    *s->cache[row][col] = *out;
    s->state[row][col] = STATE_DONE;
}

void sheet_value(struct sheet *s, int row, int col, struct value *out) {
    if (row < s->rows && col < s->cols && s->state[row][col] != STATE_DONE)
        for (int r = 0; r < row; r += 50)
            for (int c = 0; c < s->cols; ++c)
                if (s->state[r][c] == STATE_NONE && s->cells[r][c]) { struct value v; evaluate_cell(s, r, c, &v, 0); }
    evaluate_cell(s, row, col, out, 0);
}

void value_format(const struct value *v, char *out, size_t size) {
    if (v->kind == VALUE_NUMBER) {
        if (v->number == floor(v->number) && fabs(v->number) < 1e15) snprintf(out, size, "%.0f", v->number);
        else {
            snprintf(out, size, "%.10g", v->number);
        }
    } else snprintf(out, size, "%s", v->kind == VALUE_EMPTY ? "" : v->text);
}

/* ---- files ---- */

static void put_latin1(char *out, size_t size, const char *utf8) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && o + 1 < size; ++p) {
        if (*p < 0x80) out[o++] = (char)*p;
        else if ((*p & 0xe0) == 0xc0 && p[1]) { unsigned c = (unsigned)(*p & 31) << 6 | (p[1] & 63); out[o++] = c < 256 ? (char)c : '?'; ++p; }
        else { out[o++] = '?'; while ((p[1] & 0xc0) == 0x80) ++p; }
    }
    out[o] = 0;
}

static void add_utf8(struct text *t, const char *s, int xml) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 0x80) { if (xml) text_xml(t, (const char *)p, 1); else text_add(t, (const char *)p, 1); }
        else { char u[2] = {(char)(0xc0 | *p >> 6), (char)(0x80 | (*p & 63))}; text_add(t, u, 2); }
    }
}

static void load_delimited(struct sheet *s, const char *data, char separator) {
    int row = 0, col = 0;
    struct text cell = {0};
    const char *p = data;
    if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) p += 3;
    while (*p) {
        cell.size = 0;
        if (*p == '"') {
            for (++p; *p; ++p) {
                if (*p == '"' && p[1] == '"') { text_add(&cell, "\"", 1); ++p; }
                else if (*p == '"') { ++p; break; }
                else text_add(&cell, p, 1);
            }
            while (*p && *p != separator && *p != '\n' && *p != '\r') ++p;
        } else
            while (*p && *p != separator && *p != '\n' && *p != '\r') text_add(&cell, p++, 1);
        text_add(&cell, "", 1);
        char latin[1024];
        put_latin1(latin, sizeof(latin), cell.data);
        sheet_set(s, row, col, latin);
        if (*p == separator) { ++p; ++col; }
        else {
            if (*p == '\r') ++p;
            if (*p == '\n') ++p;
            ++row;
            col = 0;
        }
    }
    free(cell.data);
}

static int save_delimited(const struct sheet *s, const char *path, char separator) {
    struct text t = {0};
    for (int r = 0; r < s->rows; ++r) {
        for (int c = 0; c < s->cols; ++c) {
            const char *v = sheet_get(s, r, c);
            if (c) text_add(&t, &separator, 1);
            if (strchr(v, separator) || strchr(v, '"') || strchr(v, '\n')) {
                text_put(&t, "\"");
                for (const char *q = v; *q; ++q) { if (*q == '"') text_put(&t, "\""); char one[2] = {*q, 0}; add_utf8(&t, one, 0); }
                text_put(&t, "\"");
            } else add_utf8(&t, v, 0);
        }
        text_put(&t, "\r\n");
    }
    int result = write_whole(path, t.data ? t.data : "", t.size);
    free(t.data);
    return result;
}

static int load_xlsx(struct sheet *s, const char *path, char *error, int error_size) {
    struct zip z;
    if (zip_open(&z, path)) { snprintf(error, (size_t)error_size, "It is not a valid .xlsx file."); return -1; }
    size_t size;
    char **shared = 0;
    int shared_count = 0;
    char *strings = zip_read(&z, "xl/sharedStrings.xml", &size);
    if (strings) {
        struct xml x;
        struct text item = {0};
        int in_si = 0, in_t = 0, in_rph = 0;
        xml_start(&x, strings, size);
        for (int k; (k = xml_next(&x)) != XML_END;) {
            if (k == XML_OPEN && xml_is(&x, "si")) { in_si = 1; item.size = 0; }
            else if (k == XML_OPEN && xml_is(&x, "rPh")) in_rph = 1;
            else if (k == XML_CLOSE && xml_is(&x, "rPh")) in_rph = 0;
            else if (k == XML_OPEN && xml_is(&x, "t")) in_t = 1;
            else if (k == XML_CLOSE && xml_is(&x, "t")) in_t = 0;
            else if (k == XML_TEXT && in_si && in_t && !in_rph) text_put(&item, x.text);
            else if ((k == XML_CLOSE || k == XML_EMPTY) && xml_is(&x, "si")) {
                text_add(&item, "", 1);
                shared = realloc(shared, sizeof(*shared) * (size_t)(shared_count + 1));
                shared[shared_count++] = strdup(item.data);
                in_si = 0;
            }
        }
        free(item.data);
        free(strings);
    }
    char *sheet = zip_read(&z, "xl/worksheets/sheet1.xml", &size);
    for (int i = 0; !sheet && i < z.count; ++i)
        if (!strncmp(z.entries[i].name, "xl/worksheets/", 14) && strstr(z.entries[i].name, ".xml") && !strstr(z.entries[i].name, "_rels"))
            sheet = zip_read(&z, z.entries[i].name, &size);
    if (!sheet) { zip_close(&z); snprintf(error, (size_t)error_size, "The workbook has no worksheet."); return -1; }
    struct xml x;
    char type[16] = "", ref[16] = "";
    struct text value = {0}, formula = {0};
    int in = 0;   /* 1 <v>, 2 <f>, 3 <is><t> */
    xml_start(&x, sheet, size);
    for (int k; (k = xml_next(&x)) != XML_END;) {
        if ((k == XML_OPEN || k == XML_EMPTY) && xml_is(&x, "c")) {
            if (xml_attribute(&x, "t", type, sizeof(type))) type[0] = 0;
            if (xml_attribute(&x, "r", ref, sizeof(ref))) ref[0] = 0;
            value.size = formula.size = 0;
        } else if (k == XML_OPEN && xml_is(&x, "v")) in = 1;
        else if (k == XML_OPEN && xml_is(&x, "f")) in = 2;
        else if (k == XML_OPEN && xml_is(&x, "t")) in = 3;
        else if (k == XML_CLOSE && (xml_is(&x, "v") || xml_is(&x, "f") || xml_is(&x, "t"))) in = 0;
        else if (k == XML_TEXT && in) text_put(in == 2 ? &formula : &value, x.text);
        else if (k == XML_CLOSE && xml_is(&x, "c")) {
            int row, col;
            if (!cell_parse(ref, &row, &col)) continue;
            text_add(&value, "", 1);
            text_add(&formula, "", 1);
            char raw[1024], latin[1024];
            if (formula.size > 1) snprintf(raw, sizeof(raw), "=%s", formula.data);
            else if (!strcmp(type, "s")) { int i = atoi(value.data); snprintf(raw, sizeof(raw), "%s", i >= 0 && i < shared_count ? shared[i] : ""); }
            else if (!strcmp(type, "b")) snprintf(raw, sizeof(raw), "%s", atoi(value.data) ? "TRUE" : "FALSE");
            else snprintf(raw, sizeof(raw), "%s", value.data);
            put_latin1(latin, sizeof(latin), raw);
            sheet_set(s, row, col, latin);
        }
    }
    free(value.data);
    free(formula.data);
    for (int i = 0; i < shared_count; ++i) free(shared[i]);
    free(shared);
    free(sheet);
    zip_close(&z);
    return 0;
}

static int save_xlsx(struct sheet *s, const char *path) {
    struct zip_writer w;
    zip_begin(&w);
    static const char types[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "</Types>";
    static const char rels[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>";
    static const char workbook[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
    static const char workbook_rels[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "</Relationships>";
    struct text t = {0};
    text_put(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                 "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
    for (int r = 0; r < s->rows; ++r) {
        text_printf(&t, "<row r=\"%d\">", r + 1);
        for (int c = 0; c < s->cols; ++c) {
            const char *input = sheet_get(s, r, c);
            if (!*input) continue;
            char ref[16];
            cell_name(r, c, ref, sizeof(ref));
            struct value v;
            sheet_value(s, r, c, &v);
            if (input[0] == '=') {
                text_printf(&t, "<c r=\"%s\"%s><f>", ref, v.kind == VALUE_NUMBER ? "" : v.kind == VALUE_ERROR ? " t=\"e\"" : " t=\"str\"");
                add_utf8(&t, input + 1, 1);
                text_put(&t, "</f><v>");
                char shown[160];
                value_format(&v, shown, sizeof(shown));
                if (v.kind == VALUE_NUMBER) text_printf(&t, "%.17g", v.number); else add_utf8(&t, shown, 1);
                text_put(&t, "</v></c>");
            } else if (v.kind == VALUE_NUMBER) text_printf(&t, "<c r=\"%s\"><v>%.17g</v></c>", ref, v.number);
            else {
                text_printf(&t, "<c r=\"%s\" t=\"inlineStr\"><is><t xml:space=\"preserve\">", ref);
                add_utf8(&t, v.text, 1);
                text_put(&t, "</t></is></c>");
            }
        }
        text_put(&t, "</row>");
    }
    text_put(&t, "</sheetData></worksheet>");
    int failed = zip_add(&w, "[Content_Types].xml", types, sizeof(types) - 1, 1) || zip_add(&w, "_rels/.rels", rels, sizeof(rels) - 1, 1) ||
                 zip_add(&w, "xl/workbook.xml", workbook, sizeof(workbook) - 1, 1) ||
                 zip_add(&w, "xl/_rels/workbook.xml.rels", workbook_rels, sizeof(workbook_rels) - 1, 1) ||
                 zip_add(&w, "xl/worksheets/sheet1.xml", t.data, t.size, 1);
    free(t.data);
    return zip_finish(&w, path) || failed ? -1 : 0;
}

/* "of:=SUM([.A1:.B2])" -> "=SUM(A1:B2)". */
static void formula_from_ods(const char *in, char *out, size_t size) {
    size_t o = 0;
    if (!strncmp(in, "of:", 3)) in += 3;
    for (; *in && o + 1 < size; ++in) {
        if (*in == '"') {
            do out[o++] = *in++; while (*in && *in != '"' && o + 2 < size);
            if (*in) out[o++] = *in;
            continue;
        }
        if (*in == '[' || *in == ']') continue;
        if (*in == '.' && (in[-1] == '[' || in[-1] == ':')) continue;
        out[o++] = *in == ';' ? ',' : *in;
    }
    out[o] = 0;
}

static void formula_to_ods(struct text *t, const char *in) {
    text_put(t, "of:=");
    for (++in; *in;) {
        int row, col, used, r2, c2, used2;
        if (*in == '"') {
            const char *end = strchr(in + 1, '"');
            size_t n = end ? (size_t)(end - in) + 1 : strlen(in);
            char part[256];
            snprintf(part, sizeof(part), "%.*s", (int)n, in);
            add_utf8(t, part, 1);
            in += n;
        } else if ((in == 0 || !isalpha((unsigned char)in[-1])) && (used = cell_parse(in, &row, &col)) > 0) {
            if (in[used] == ':' && (used2 = cell_parse(in + used + 1, &r2, &c2)) > 0) {
                text_printf(t, "[.%.*s:.%.*s]", used, in, used2, in + used + 1);
                in += used + 1 + used2;
            } else {
                text_printf(t, "[.%.*s]", used, in);
                in += used;
            }
        } else {
            char one[2] = {*in == ',' ? ';' : *in, 0};
            add_utf8(t, one, 1);
            ++in;
        }
    }
}

static int load_ods(struct sheet *s, const char *path, char *error, int error_size) {
    struct zip z;
    size_t size;
    char *content;
    if (zip_open(&z, path) || !(content = zip_read(&z, "content.xml", &size))) {
        if (z.data) zip_close(&z);
        snprintf(error, (size_t)error_size, "It is not a valid OpenDocument spreadsheet.");
        return -1;
    }
    struct xml x;
    int row = -1, col = 0, repeat_rows = 1, repeat_cols = 1, tables = 0, in_p = 0;
    char formula[1024] = "", number[64] = "", kind[24] = "";
    struct text value = {0};
    xml_start(&x, content, size);
    for (int k; (k = xml_next(&x)) != XML_END && tables < 2;) {
        char a[32];
        if (k == XML_OPEN && xml_is(&x, "table")) ++tables;
        else if (k == XML_CLOSE && xml_is(&x, "table")) tables = 2;
        else if ((k == XML_OPEN || k == XML_EMPTY) && xml_is(&x, "table-row")) {
            row += repeat_rows;
            repeat_rows = xml_attribute(&x, "number-rows-repeated", a, sizeof(a)) ? 1 : atoi(a);
            if (repeat_rows > 1 && repeat_rows > 100) repeat_rows = 1;
            col = 0;
            if (k == XML_EMPTY) { row += repeat_rows - 1; repeat_rows = 1; }
        } else if ((k == XML_OPEN || k == XML_EMPTY) && (xml_is(&x, "table-cell") || xml_is(&x, "covered-table-cell"))) {
            repeat_cols = xml_attribute(&x, "number-columns-repeated", a, sizeof(a)) ? 1 : atoi(a);
            if (xml_attribute(&x, "formula", formula, sizeof(formula))) formula[0] = 0;
            if (xml_attribute(&x, "value", number, sizeof(number))) number[0] = 0;
            if (xml_attribute(&x, "value-type", kind, sizeof(kind))) kind[0] = 0;
            value.size = 0;
            if (k == XML_EMPTY) col += repeat_cols;
        } else if (k == XML_OPEN && xml_is(&x, "p")) { if (value.size) text_put(&value, "\n"); in_p = 1; }
        else if (k == XML_CLOSE && xml_is(&x, "p")) in_p = 0;
        else if (k == XML_TEXT && in_p) text_put(&value, x.text);
        else if (k == XML_CLOSE && xml_is(&x, "table-cell")) {
            text_add(&value, "", 1);
            char raw[1024], latin[1024];
            if (formula[0]) { formula_from_ods(formula, raw, sizeof(raw)); }
            else if (number[0] && (!strcmp(kind, "float") || !strcmp(kind, "percentage") || !strcmp(kind, "currency"))) snprintf(raw, sizeof(raw), "%s", number);
            else snprintf(raw, sizeof(raw), "%s", value.data);
            put_latin1(latin, sizeof(latin), raw);
            for (int i = 0; i < repeat_cols && i < 64 && *latin; ++i)
                for (int j = 0; j < repeat_rows; ++j) sheet_set(s, row + j, col + i, latin);
            col += repeat_cols;
        }
    }
    free(value.data);
    free(content);
    zip_close(&z);
    return 0;
}

static int save_ods(struct sheet *s, const char *path) {
    struct zip_writer w;
    zip_begin(&w);
    static const char mimetype[] = "application/vnd.oasis.opendocument.spreadsheet";
    static const char manifest[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "</manifest:manifest>";
    struct text t = {0};
    text_put(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                 "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
                 "xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\" office:version=\"1.2\">"
                 "<office:body><office:spreadsheet><table:table table:name=\"Sheet1\">");
    for (int r = 0; r < s->rows; ++r) {
        text_put(&t, "<table:table-row>");
        for (int c = 0; c < s->cols; ++c) {
            const char *input = sheet_get(s, r, c);
            if (!*input) { text_put(&t, "<table:table-cell/>"); continue; }
            struct value v;
            sheet_value(s, r, c, &v);
            char shown[160];
            value_format(&v, shown, sizeof(shown));
            text_put(&t, "<table:table-cell");
            if (input[0] == '=') { text_put(&t, " table:formula=\""); formula_to_ods(&t, input); text_put(&t, "\""); }
            if (v.kind == VALUE_NUMBER) text_printf(&t, " office:value-type=\"float\" office:value=\"%.17g\"", v.number);
            else text_put(&t, " office:value-type=\"string\"");
            text_put(&t, "><text:p>");
            add_utf8(&t, shown, 1);
            text_put(&t, "</text:p></table:table-cell>");
        }
        text_put(&t, "</table:table-row>");
    }
    text_put(&t, "</table:table></office:spreadsheet></office:body></office:document-content>");
    int failed = zip_add(&w, "mimetype", mimetype, sizeof(mimetype) - 1, 0) || zip_add(&w, "META-INF/manifest.xml", manifest, sizeof(manifest) - 1, 1) ||
                 zip_add(&w, "content.xml", t.data, t.size, 1);
    free(t.data);
    return zip_finish(&w, path) || failed ? -1 : 0;
}

int sheet_load(struct sheet *s, const char *path, char *error, int error_size) {
    const char *ext = extension_of(path);
    sheet_free(s);
    if (!strcasecmp(ext, "xlsx") || !strcasecmp(ext, "xlsm")) return load_xlsx(s, path, error, error_size);
    if (!strcasecmp(ext, "ods")) return load_ods(s, path, error, error_size);
    if (!strcasecmp(ext, "xls")) { snprintf(error, (size_t)error_size, "Old .xls workbooks are not supported; save it as .xlsx."); return -1; }
    size_t size;
    char *data = read_whole(path, &size);
    if (!data) { snprintf(error, (size_t)error_size, "The file could not be read."); return -1; }
    char separator = !strcasecmp(ext, "tsv") || !strcasecmp(ext, "tab") ? '\t' : ',';
    if (!strcasecmp(ext, "txt")) separator = strchr(data, '\t') ? '\t' : ',';
    if (!strcasecmp(ext, "csv") && !strchr(data, ',') && strchr(data, ';')) separator = ';';
    load_delimited(s, data, separator);
    free(data);
    return 0;
}

int sheet_save(const struct sheet *s, const char *path, char *error, int error_size) {
    const char *ext = extension_of(path);
    struct sheet *writable = (struct sheet *)s;
    int failed;
    if (!strcasecmp(ext, "xlsx")) failed = save_xlsx(writable, path);
    else if (!strcasecmp(ext, "ods")) failed = save_ods(writable, path);
    else failed = save_delimited(s, path, !strcasecmp(ext, "tsv") || !strcasecmp(ext, "txt") ? '\t' : ',');
    if (failed) snprintf(error, (size_t)error_size, "The file could not be written.");
    return failed ? -1 : 0;
}
