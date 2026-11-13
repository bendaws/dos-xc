/*
 * DOS-XC /openwork/mime.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "mail.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { ENC_NONE, ENC_BASE64, ENC_QP };

size_t header_size(const char *raw, size_t n) {
    for (size_t i = 0; i + 1 < n; ++i) {
        if (raw[i] == '\n' && raw[i + 1] == '\n') return i + 1;
        if (raw[i] == '\n' && raw[i + 1] == '\r' && i + 2 < n && raw[i + 2] == '\n') return i + 1;
    }
    return n;
}

static size_t body_offset(const char *raw, size_t n) {
    size_t h = header_size(raw, n);
    if (h < n && raw[h] == '\r') ++h;
    if (h < n && raw[h] == '\n') ++h;
    return h;
}

static int hex(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void base64_decode(struct text *out, const char *s, size_t n) {
    unsigned value = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        int c = (unsigned char)s[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else continue;
        value = value << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            char byte = (char)(value >> bits & 0xff);
            text_add(out, &byte, 1);
        }
    }
}

static void qp_decode(struct text *out, const char *s, size_t n, int underscore) {
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '=' && i + 2 < n + 1) {
            if (i + 1 < n && (s[i + 1] == '\r' || s[i + 1] == '\n')) {
                ++i;
                if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') ++i;
                continue;
            }
            if (i + 2 < n && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
                c = (char)(hex(s[i + 1]) << 4 | hex(s[i + 2]));
                i += 2;
            }
        } else if (c == '_' && underscore) c = ' ';
        text_add(out, &c, 1);
    }
}

void base64_encode(struct text *t, const unsigned char *data, size_t n, int wrap) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int column = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)data[i] << 16 | (i + 1 < n ? (unsigned)data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
        char four[4] = {digits[v >> 18 & 63], digits[v >> 12 & 63], i + 1 < n ? digits[v >> 6 & 63] : '=', i + 2 < n ? digits[v & 63] : '='};
        text_add(t, four, 4);
        if (wrap && (column += 4) >= 76) { text_put(t, "\r\n"); column = 0; }
    }
    if (wrap && column) text_put(t, "\r\n");
}

static int valid_utf8(const char *s, size_t n) {
    int high = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) continue;
        high = 1;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
        if (extra < 0 || i + (size_t)extra >= n + 0) return 0;
        for (int k = 1; k <= extra; ++k)
            if (i + (size_t)k >= n || ((unsigned char)s[i + k] & 0xc0) != 0x80) return 0;
        i += (size_t)extra;
    }
    return high;
}

/* Bytes in CHARSET appended to OUT as Latin-1. */
static void from_charset(struct text *out, const char *s, size_t n, const char *charset) {
    if (!strcasecmp(charset, "utf-8") || !strcasecmp(charset, "utf8") || (!charset[0] && valid_utf8(s, n))) text_latin1(out, s, n);
    else text_add(out, s, n);
}

/* Decodes RFC 2047 encoded words in S into Latin-1. */
static void decode_words(const char *s, char *out, size_t size) {
    struct text t = {0};
    const char *p = s;
    int last_encoded = 0;
    while (*p) {
        const char *start = strstr(p, "=?");
        const char *q1 = start ? strchr(start + 2, '?') : 0;
        const char *q2 = q1 && q1[1] && q1[2] == '?' ? q1 + 2 : 0;
        const char *end = q2 ? strstr(q2 + 1, "?=") : 0;
        if (!end) {
            from_charset(&t, p, strlen(p), "");
            break;
        }
        int gap_blank = 1;
        for (const char *g = p; g < start; ++g) if (*g != ' ' && *g != '\t') gap_blank = 0;
        if (!(last_encoded && gap_blank)) from_charset(&t, p, (size_t)(start - p), "");
        char charset[40];
        snprintf(charset, sizeof(charset), "%.*s", (int)(q1 - start - 2), start + 2);
        char *star = strchr(charset, '*');
        if (star) *star = 0;
        struct text raw = {0};
        if (q1[1] == 'B' || q1[1] == 'b') base64_decode(&raw, q2 + 1, (size_t)(end - q2 - 1));
        else qp_decode(&raw, q2 + 1, (size_t)(end - q2 - 1), 1);
        if (raw.data) from_charset(&t, raw.data, raw.size, charset);
        free(raw.data);
        last_encoded = 1;
        p = end + 2;
    }
    snprintf(out, size, "%s", t.data ? t.data : "");
    free(t.data);
}

void header_get(const char *raw, size_t n, const char *name, char *out, size_t size) {
    size_t limit = header_size(raw, n), len = strlen(name);
    out[0] = 0;
    for (size_t i = 0; i < limit; ++i) {
        if ((i == 0 || raw[i - 1] == '\n') && i + len < limit && !strncasecmp(raw + i, name, len) && raw[i + len] == ':') {
            struct text t = {0};
            size_t j = i + len + 1;
            while (j < limit && (raw[j] == ' ' || raw[j] == '\t')) ++j;
            for (; j < limit; ++j) {
                if (raw[j] == '\r') continue;
                if (raw[j] == '\n') {
                    if (j + 1 < limit && (raw[j + 1] == ' ' || raw[j + 1] == '\t')) {
                        while (j + 1 < limit && (raw[j + 1] == ' ' || raw[j + 1] == '\t')) ++j;
                        text_put(&t, " ");
                        continue;
                    }
                    break;
                }
                text_add(&t, raw + j, 1);
            }
            decode_words(t.data ? t.data : "", out, size);
            free(t.data);
            return;
        }
    }
}

/* A parameter of a header value such as Content-Type: "text/plain; charset=utf-8". */
static void param_get(const char *value, const char *name, char *out, size_t size) {
    out[0] = 0;
    size_t len = strlen(name);
    for (const char *p = strchr(value, ';'); p; p = strchr(p + 1, ';')) {
        const char *k = p + 1;
        while (*k == ' ' || *k == '\t') ++k;
        if (strncasecmp(k, name, len)) continue;
        const char *v = k + len;
        if (*v == '*') { while (*v && *v != '=') ++v; }
        if (*v != '=') continue;
        ++v;
        if (*v == '"') {
            ++v;
            size_t n = 0;
            while (*v && *v != '"' && n + 1 < size) { if (*v == '\\' && v[1]) ++v; out[n++] = *v++; }
            out[n] = 0;
        } else {
            size_t n = strcspn(v, "; \t");
            snprintf(out, size, "%.*s", (int)n, v);
            char *tick = strstr(out, "''");
            if (tick) memmove(out, tick + 2, strlen(tick + 2) + 1);
        }
        return;
    }
}

time_t mail_date(const char *s) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int day = 0, year = 0, hour = 0, minute = 0, second = 0, month = -1;
    char name[8] = "";
    const char *p = s;
    while (*p && !isdigit((unsigned char)*p)) ++p;
    if (sscanf(p, "%d%*[ -]%3s%*[ -]%d %d:%d:%d", &day, name, &year, &hour, &minute, &second) < 3) return 0;
    for (int i = 0; i < 12; ++i)
        if (!strncasecmp(name, months + i * 3, 3)) month = i;
    if (month < 0) return 0;
    if (year < 100) year += year < 70 ? 2000 : 1900;
    long zone = 0;
    const char *z = strchr(p, ':');
    if (z) z = strchr(z, ' ');
    while (z && *z == ' ') ++z;
    if (z && (*z == '+' || *z == '-') && isdigit((unsigned char)z[1])) {
        long hhmm = strtol(z + 1, 0, 10);
        zone = (hhmm / 100 * 60 + hhmm % 100) * 60 * (*z == '-' ? -1 : 1);
    }
    static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    long days = (long)(year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400 + before[month] + day - 1;
    if (month > 1 && ((year % 4 == 0 && year % 100) || year % 400 == 0)) ++days;
    return (time_t)(days * 86400 + hour * 3600 + minute * 60 + second - zone);
}

void address_name(const char *field, char *name, size_t name_size, char *address, size_t address_size) {
    char first[300];
    size_t n = 0;
    int quoted = 0;
    for (const char *p = field; *p && n + 1 < sizeof(first); ++p) {
        if (*p == '"') quoted = !quoted;
        if (*p == ',' && !quoted) break;
        first[n++] = *p;
    }
    first[n] = 0;
    const char *lt = strchr(first, '<'), *gt = lt ? strchr(lt, '>') : 0;
    if (lt && gt) {
        snprintf(address, address_size, "%.*s", (int)(gt - lt - 1), lt + 1);
        const char *a = first, *b = lt;
        while (a < b && (*a == ' ' || *a == '"')) ++a;
        while (b > a && (b[-1] == ' ' || b[-1] == '"')) --b;
        if (b > a) snprintf(name, name_size, "%.*s", (int)(b - a), a);
        else snprintf(name, name_size, "%s", address);
    } else {
        const char *a = first;
        while (*a == ' ') ++a;
        snprintf(address, address_size, "%s", a);
        snprintf(name, name_size, "%s", a);
    }
}

int address_list(const char *field, char out[][120], int max) {
    int count = 0, quoted = 0;
    const char *start = field;
    for (const char *p = field;; ++p) {
        if (*p == '"') quoted = !quoted;
        if ((*p == ',' || *p == ';' || !*p) && !quoted) {
            char one[300], name[120], address[120];
            snprintf(one, sizeof(one), "%.*s", (int)(p - start), start);
            address_name(one, name, sizeof(name), address, sizeof(address));
            char *end = address + strlen(address);
            while (end > address && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
            if (strchr(address, '@') && count < max) snprintf(out[count++], 120, "%s", address);
            if (!*p) break;
            start = p + 1;
        }
    }
    return count;
}

/* HTML as plain text: tags removed, blocks on their own lines. */
static void html_text(struct text *out, const char *s, size_t n) {
    int space = 1, newlines = 2;
    for (size_t i = 0; i < n;) {
        if (s[i] == '<') {
            size_t end = i + 1;
            while (end < n && s[end] != '>') ++end;
            char tag[16];
            size_t k = 0, j = i + 1;
            int closing = j < n && s[j] == '/';
            if (closing) ++j;
            while (j < end && k + 1 < sizeof(tag) && isalnum((unsigned char)s[j])) tag[k++] = (char)tolower((unsigned char)s[j++]);
            tag[k] = 0;
            if (!closing && (!strcmp(tag, "style") || !strcmp(tag, "script") || !strcmp(tag, "head") || !strcmp(tag, "title"))) {
                char close[24];
                snprintf(close, sizeof(close), "</%s", tag);
                size_t m = end;
                while (m + strlen(close) < n && strncasecmp(s + m, close, strlen(close))) ++m;
                end = m;
                while (end < n && s[end] != '>') ++end;
            }
            int block = !strcmp(tag, "br") || !strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "tr") || !strcmp(tag, "li") ||
                        !strcmp(tag, "table") || !strcmp(tag, "blockquote") || (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]);
            if (block && (!strcmp(tag, "br") || closing || !strcmp(tag, "li") || !strcmp(tag, "p"))) {
                int want = !strcmp(tag, "br") || !strcmp(tag, "li") || !strcmp(tag, "tr") ? 1 : 2;
                while (newlines < want) { text_put(out, "\n"); ++newlines; }
                space = 1;
                if (!strcmp(tag, "li") && !closing) { text_put(out, "\xb7 "); newlines = 0; }
            } else if (!strcmp(tag, "td") && closing) { text_put(out, "  "); space = 1; }
            i = end + 1;
            continue;
        }
        unsigned code = (unsigned char)s[i++];
        if (code == '&') {
            const char *semi = memchr(s + i, ';', n - i < 10 ? n - i : 10);
            if (semi) {
                size_t len = (size_t)(semi - (s + i));
                static const struct { const char *name; unsigned code; } entities[] = {
                    {"nbsp", ' '}, {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"copy", 0xa9},
                    {"reg", 0xae}, {"eacute", 0xe9}, {"egrave", 0xe8}, {"agrave", 0xe0}, {"ouml", 0xf6}, {"uuml", 0xfc}, {"auml", 0xe4},
                    {"rsquo", '\''}, {"lsquo", '\''}, {"ldquo", '"'}, {"rdquo", '"'}, {"mdash", '-'}, {"ndash", '-'}, {"hellip", '.'},
                    {"middot", 0xb7}, {"bull", 0xb7}, {"pound", 0xa3}, {"euro", 'E'}, {"zwnj", 0}, {"zwj", 0},
                };
                unsigned found = 0x110000;
                if (s[i] == '#') found = (unsigned)(s[i + 1] == 'x' || s[i + 1] == 'X' ? strtoul(s + i + 2, 0, 16) : strtoul(s + i + 1, 0, 10));
                else
                    for (size_t e = 0; e < sizeof(entities) / sizeof(*entities); ++e)
                        if (strlen(entities[e].name) == len && !strncmp(s + i, entities[e].name, len)) found = entities[e].code;
                if (found != 0x110000) {
                    i += len + 1;
                    if (found == 0x2018 || found == 0x2019) found = '\'';
                    else if (found == 0x201c || found == 0x201d) found = '"';
                    else if (found == 0x2013 || found == 0x2014) found = '-';
                    if (!found || found == 0x200b || found == 0x200c || found == 0x34f) continue;
                    code = found < 256 ? found : '?';
                }
            }
        }
        if (code == ' ' || code == '\t' || code == '\r' || code == '\n') {
            if (!space) text_put(out, " ");
            space = 1;
            continue;
        }
        char c = (char)code;
        text_add(out, &c, 1);
        space = 0;
        newlines = 0;
    }
}

static void add_attachment(struct parsed *out, const char *name, const char *type, size_t offset, size_t length, int encoding) {
    struct attachment *bigger = realloc(out->attachments, sizeof(*bigger) * (size_t)(out->attachment_count + 1));
    if (!bigger) return;
    out->attachments = bigger;
    struct attachment *a = &out->attachments[out->attachment_count++];
    snprintf(a->name, sizeof(a->name), "%s", name[0] ? name : "attachment");
    snprintf(a->type, sizeof(a->type), "%s", type);
    a->offset = offset;
    a->length = length;
    a->encoding = encoding;
}

struct walk { struct parsed *out; const char *root; struct text plain, html; int have_plain, have_html; };

static void decode_part(struct text *out, const char *s, size_t n, int encoding) {
    if (encoding == ENC_BASE64) base64_decode(out, s, n);
    else if (encoding == ENC_QP) qp_decode(out, s, n, 0);
    else text_add(out, s, n);
}

static void walk_part(struct walk *w, const char *part, size_t n, int depth) {
    char type[160], encoding_name[40], disposition[200], name[128], charset[40];
    header_get(part, n, "Content-Type", type, sizeof(type));
    header_get(part, n, "Content-Transfer-Encoding", encoding_name, sizeof(encoding_name));
    header_get(part, n, "Content-Disposition", disposition, sizeof(disposition));
    if (!type[0]) snprintf(type, sizeof(type), "text/plain");
    param_get(disposition, "filename", name, sizeof(name));
    if (!name[0]) param_get(type, "name", name, sizeof(name));
    param_get(type, "charset", charset, sizeof(charset));
    int encoding = !strncasecmp(encoding_name, "base64", 6) ? ENC_BASE64 : !strncasecmp(encoding_name, "quoted-printable", 16) ? ENC_QP : ENC_NONE;
    size_t body = body_offset(part, n);
    const char *data = part + body;
    size_t size = n - body;
    char bare[64];
    snprintf(bare, sizeof(bare), "%.*s", (int)strcspn(type, "; \t"), type);
    if (!strncasecmp(bare, "multipart/", 10) && depth < 8) {
        char boundary[200];
        param_get(type, "boundary", boundary, sizeof(boundary));
        if (!boundary[0]) return;
        char marker[210];
        snprintf(marker, sizeof(marker), "--%s", boundary);
        size_t mlen = strlen(marker);
        const char *p = data, *end = data + size, *start = 0;
        while (p < end) {
            const char *line_end = memchr(p, '\n', (size_t)(end - p));
            if (!line_end) line_end = end;
            if ((size_t)(line_end - p) >= mlen && !strncmp(p, marker, mlen)) {
                if (start) {
                    const char *stop = p;
                    if (stop > start && stop[-1] == '\n') --stop;
                    if (stop > start && stop[-1] == '\r') --stop;
                    walk_part(w, start, (size_t)(stop - start), depth + 1);
                }
                if (p + mlen + 1 < end && p[mlen] == '-' && p[mlen + 1] == '-') return;
                start = line_end < end ? line_end + 1 : end;
            }
            p = line_end + 1;
        }
        return;
    }
    int attached = !strncasecmp(disposition, "attachment", 10) || (name[0] && strncasecmp(bare, "text/", 5));
    if (!attached && !strcasecmp(bare, "text/plain") && !w->have_plain) {
        struct text raw = {0};
        decode_part(&raw, data, size, encoding);
        if (raw.data) from_charset(&w->plain, raw.data, raw.size, charset);
        free(raw.data);
        w->have_plain = 1;
    } else if (!attached && !strcasecmp(bare, "text/html") && !w->have_html) {
        struct text raw = {0}, latin = {0};
        decode_part(&raw, data, size, encoding);
        if (raw.data) from_charset(&latin, raw.data, raw.size, charset);
        if (latin.data) html_text(&w->html, latin.data, latin.size);
        free(raw.data);
        free(latin.data);
        w->have_html = 1;
    } else if (attached || strncasecmp(bare, "text/", 5)) {
        if (!name[0] && !strcasecmp(bare, "message/rfc822")) snprintf(name, sizeof(name), "message.eml");
        add_attachment(w->out, name, bare, (size_t)(data - w->root), size, encoding);
    }
}

void mime_parse(const char *raw, size_t n, struct parsed *out) {
    memset(out, 0, sizeof(*out));
    struct walk w = {out, raw};
    walk_part(&w, raw, n, 0);
    struct text *best = w.have_plain && w.plain.size ? &w.plain : &w.html;
    for (size_t i = 0; best->data && i < best->size; ++i) {
        if (best->data[i] == '\r') continue;
        text_add(&out->body, best->data + i, 1);
    }
    free(w.plain.data);
    free(w.html.data);
    if (!out->body.data) text_put(&out->body, "");
}

void mime_free(struct parsed *p) {
    free(p->body.data);
    free(p->attachments);
    memset(p, 0, sizeof(*p));
}

int attachment_decode(const char *raw, const struct attachment *a, struct text *out) {
    decode_part(out, raw + a->offset, a->length, a->encoding);
    return out->data ? 0 : -1;
}

static void header_text(struct text *t, const char *label, const char *latin1) {
    int plain = 1;
    for (const char *p = latin1; *p; ++p) if ((unsigned char)*p >= 0x80) plain = 0;
    text_printf(t, "%s: ", label);
    if (plain) { text_put(t, latin1); text_put(t, "\r\n"); return; }
    struct text utf8 = {0};
    text_utf8(&utf8, latin1, strlen(latin1), 0);
    text_put(t, "=?UTF-8?B?");
    base64_encode(t, (const unsigned char *)utf8.data, utf8.size, 0);
    text_put(t, "?=\r\n");
    free(utf8.data);
}

/* An address list with only the display names encoded, as RFC 2047 requires. */
static void address_header(struct text *t, const char *label, const char *field) {
    text_printf(t, "%s: ", label);
    int quoted = 0, first = 1;
    const char *start = field;
    for (const char *p = field;; ++p) {
        if (*p == '"') quoted = !quoted;
        if ((*p == ',' || *p == ';' || !*p) && !quoted) {
            char one[300], name[120], address[120];
            snprintf(one, sizeof(one), "%.*s", (int)(p - start), start);
            address_name(one, name, sizeof(name), address, sizeof(address));
            if (strchr(address, '@')) {
                if (!first) text_put(t, ", ");
                first = 0;
                int plain = 1;
                for (const char *q = name; *q; ++q) if ((unsigned char)*q >= 0x80) plain = 0;
                if (!strcmp(name, address)) text_put(t, address);
                else if (plain) text_printf(t, "\"%s\" <%s>", name, address);
                else {
                    struct text utf8 = {0};
                    text_utf8(&utf8, name, strlen(name), 0);
                    text_put(t, "=?UTF-8?B?");
                    base64_encode(t, (const unsigned char *)utf8.data, utf8.size, 0);
                    text_printf(t, "?= <%s>", address);
                    free(utf8.data);
                }
            }
            if (!*p) break;
            start = p + 1;
        }
    }
    text_put(t, "\r\n");
}

/* A file name for Content-Disposition, encoded when it is not ASCII. */
static void file_name(struct text *t, const char *name) {
    int plain = 1;
    for (const char *q = name; *q; ++q) if ((unsigned char)*q >= 0x80 || *q == '"') plain = 0;
    if (plain) { text_printf(t, "\"%s\"", name); return; }
    struct text utf8 = {0};
    text_utf8(&utf8, name, strlen(name), 0);
    text_put(t, "\"=?UTF-8?B?");
    base64_encode(t, (const unsigned char *)utf8.data, utf8.size, 0);
    text_put(t, "?=\"");
    free(utf8.data);
}

static const char *mime_type(const char *path) {
    static const struct { const char *ext, *type; } types[] = {
        {"txt", "text/plain"}, {"htm", "text/html"}, {"html", "text/html"}, {"csv", "text/csv"}, {"pdf", "application/pdf"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"bmp", "image/bmp"},
        {"zip", "application/zip"}, {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"odt", "application/vnd.oasis.opendocument.text"}, {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
        {"odp", "application/vnd.oasis.opendocument.presentation"}, {"mp3", "audio/mpeg"}, {"wav", "audio/wav"},
    };
    const char *ext = extension_of(path);
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); ++i)
        if (!strcasecmp(ext, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}

static void quoted_printable(struct text *t, const char *s, size_t n) {
    int column = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') { text_put(t, "\r\n"); column = 0; continue; }
        if (c == '\r') continue;
        char piece[4];
        int len;
        int last = i + 1 == n || s[i + 1] == '\n' || s[i + 1] == '\r';
        if ((c >= 33 && c <= 126 && c != '=') || (c == ' ' && !last) || (c == '\t' && !last)) { piece[0] = (char)c; len = 1; }
        else { snprintf(piece, sizeof(piece), "=%02X", c); len = 3; }
        if (column + len > 75) { text_put(t, "=\r\n"); column = 0; }
        if (column == 0 && c == '.') { text_put(t, "=2E"); column = 3; continue; }
        text_add(t, piece, (size_t)len);
        column += len;
    }
}

void mail_compose(struct text *out, const struct account *a, const char *to, const char *cc, const char *subject,
                  const char *text, const char *files, const char *in_reply_to, const char *references) {
    time_t now = time(0);
    struct tm *g = gmtime(&now);
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", g);
    text_printf(out, "Date: %s\r\n", date);
    if (a->name[0]) {
        char from[220];
        snprintf(from, sizeof(from), "\"%s\"", a->name);
        int plain = 1;
        for (const char *p = a->name; *p; ++p) if ((unsigned char)*p >= 0x80) plain = 0;
        if (plain) text_printf(out, "From: %s <%s>\r\n", from, a->address);
        else {
            struct text utf8 = {0};
            text_utf8(&utf8, a->name, strlen(a->name), 0);
            text_put(out, "From: =?UTF-8?B?");
            base64_encode(out, (const unsigned char *)utf8.data, utf8.size, 0);
            text_printf(out, "?= <%s>\r\n", a->address);
            free(utf8.data);
        }
    } else text_printf(out, "From: %s\r\n", a->address);
    address_header(out, "To", to);
    if (cc[0]) address_header(out, "Cc", cc);
    header_text(out, "Subject", subject);
    const char *at = strchr(a->address, '@');
    text_printf(out, "Message-ID: <%lx.%x@%s>\r\n", (unsigned long)now, (unsigned)rand(), at ? at + 1 : "dosxc.local");
    if (in_reply_to && in_reply_to[0]) text_printf(out, "In-Reply-To: %s\r\n", in_reply_to);
    if (references && references[0]) text_printf(out, "References: %s\r\n", references);
    text_put(out, "MIME-Version: 1.0\r\nUser-Agent: OpenMail (DOS-XC)\r\n");
    struct text utf8 = {0};
    text_utf8(&utf8, text, strlen(text), 0);
    char boundary[64];
    snprintf(boundary, sizeof(boundary), "=_OpenMail_%lx_%x", (unsigned long)now, (unsigned)rand());
    int with_files = files && files[0];
    if (with_files) text_printf(out, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\nThis is a multi-part message in MIME format.\r\n--%s\r\n", boundary, boundary);
    text_put(out, "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n");
    quoted_printable(out, utf8.data ? utf8.data : "", utf8.size);
    text_put(out, "\r\n");
    free(utf8.data);
    if (!with_files) return;
    char list[2048];
    snprintf(list, sizeof(list), "%s", files);
    for (char *path = strtok(list, "\n"); path; path = strtok(0, "\n")) {
        size_t size;
        char *data = read_whole(path, &size);
        if (!data) continue;
        const char *slash = strrchr(path, '\\');
        const char *name = slash ? slash + 1 : path;
        text_printf(out, "--%s\r\nContent-Type: %s; name=", boundary, mime_type(path));
        file_name(out, name);
        text_put(out, "\r\nContent-Disposition: attachment; filename=");
        file_name(out, name);
        text_put(out, "\r\nContent-Transfer-Encoding: base64\r\n\r\n");
        base64_encode(out, (const unsigned char *)data, size, 1);
        free(data);
    }
    text_printf(out, "--%s--\r\n", boundary);
}
