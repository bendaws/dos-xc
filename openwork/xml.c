/*
 * DOS-XC /openwork/xml.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "office.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void xml_start(struct xml *x, const char *text, size_t size) {
    memset(x, 0, sizeof(*x));
    x->at = text;
    x->end = text + size;
}

static size_t decode(const char *in, size_t n, char *out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < size;) {
        if (in[i] != '&') { out[o++] = in[i++]; continue; }
        const char *semi = memchr(in + i, ';', n - i < 10 ? n - i : 10);
        if (!semi) { out[o++] = in[i++]; continue; }
        size_t len = (size_t)(semi - (in + i));
        unsigned long code = 0;
        if (len == 3 && !strncmp(in + i, "&lt", 3)) code = '<';
        else if (len == 3 && !strncmp(in + i, "&gt", 3)) code = '>';
        else if (len == 4 && !strncmp(in + i, "&amp", 4)) code = '&';
        else if (len == 5 && !strncmp(in + i, "&quot", 5)) code = '"';
        else if (len == 5 && !strncmp(in + i, "&apos", 5)) code = '\'';
        else if (in[i + 1] == '#') code = in[i + 2] == 'x' ? strtoul(in + i + 3, 0, 16) : strtoul(in + i + 2, 0, 10);
        if (!code) { out[o++] = in[i++]; continue; }
        if (code < 0x80) out[o++] = (char)code;
        else if (code < 0x800) { out[o++] = (char)(0xc0 | code >> 6); out[o++] = (char)(0x80 | (code & 63)); }
        else { out[o++] = (char)(0xe0 | code >> 12); out[o++] = (char)(0x80 | (code >> 6 & 63)); out[o++] = (char)(0x80 | (code & 63)); }
        i += len + 1;
    }
    out[o] = 0;
    return o;
}

int xml_next(struct xml *x) {
    x->name[0] = x->text[0] = 0;
    x->attributes = 0;
    x->attributes_size = 0;
    if (x->at >= x->end) return XML_END;
    if (*x->at != '<') {
        const char *lt = memchr(x->at, '<', (size_t)(x->end - x->at));
        if (!lt) lt = x->end;
        decode(x->at, (size_t)(lt - x->at), x->text, sizeof(x->text));
        x->at = lt;
        return XML_TEXT;
    }
    if (x->at + 4 < x->end && !strncmp(x->at, "<!--", 4)) {
        const char *close = strstr(x->at, "-->");
        x->at = close ? close + 3 : x->end;
        return xml_next(x);
    }
    if (x->at + 1 < x->end && (x->at[1] == '?' || x->at[1] == '!')) {
        const char *gt = memchr(x->at, '>', (size_t)(x->end - x->at));
        x->at = gt ? gt + 1 : x->end;
        return xml_next(x);
    }
    const char *gt = x->at;
    char quote = 0;
    for (; gt < x->end; ++gt) {
        if (quote) { if (*gt == quote) quote = 0; }
        else if (*gt == '"' || *gt == '\'') quote = *gt;
        else if (*gt == '>') break;
    }
    if (gt >= x->end) { x->at = x->end; return XML_END; }
    const char *p = x->at + 1;
    int closing = *p == '/';
    if (closing) ++p;
    size_t n = 0;
    while (p < gt && *p != ' ' && *p != '/' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n + 1 < sizeof(x->name)) x->name[n++] = *p;
        ++p;
    }
    x->name[n] = 0;
    x->attributes = p;
    x->attributes_size = (size_t)(gt - p);
    int empty = gt > x->at && gt[-1] == '/';
    x->at = gt + 1;
    return closing ? XML_CLOSE : empty ? XML_EMPTY : XML_OPEN;
}

int xml_attribute(const struct xml *x, const char *name, char *out, size_t size) {
    const char *p = x->attributes, *end = p + x->attributes_size;
    size_t n = strlen(name);
    while (p && p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '/')) ++p;
        const char *key = p;
        while (p < end && *p != '=' && *p != ' ') ++p;
        size_t key_size = (size_t)(p - key);
        if (p >= end || *p != '=') return -1;
        ++p;
        char quote = *p++;
        const char *value = p;
        while (p < end && *p != quote) ++p;
        const char *colon = memchr(key, ':', key_size);
        int match = (key_size == n && !strncmp(key, name, n)) ||
                    (colon && (size_t)(key + key_size - colon - 1) == n && !strncmp(colon + 1, name, n));
        if (match) { decode(value, (size_t)(p - value), out, size); return 0; }
        ++p;
    }
    return -1;
}

int xml_is(const struct xml *x, const char *name) {
    const char *colon = strchr(x->name, ':');
    return !strcmp(x->name, name) || (colon && !strcmp(colon + 1, name));
}
