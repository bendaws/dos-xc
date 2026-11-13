/*
 * DOS-XC /openwork/office.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include <stddef.h>

/* ---- ZIP archives (zip.c) ---- */
struct zip_entry { char name[256]; size_t offset, packed, size; int method; };
struct zip { unsigned char *data; size_t size; struct zip_entry *entries; int count; };
int zip_open(struct zip *z, const char *path);              /* 0 or -1 */
void zip_close(struct zip *z);
char *zip_read(const struct zip *z, const char *name, size_t *size);   /* malloc'd, NUL-terminated; 0 if absent */
struct zip_writer { struct zip_entry *entries; int count, capacity; unsigned char *out; size_t used, capacity_bytes; unsigned long *crcs; };
void zip_begin(struct zip_writer *w);
int zip_add(struct zip_writer *w, const char *name, const void *data, size_t size, int compress);
int zip_finish(struct zip_writer *w, const char *path);    /* writes and frees */

/* ---- XML (xml.c): a pull parser over a whole document ---- */
enum { XML_END, XML_OPEN, XML_CLOSE, XML_EMPTY, XML_TEXT };
struct xml { const char *at, *end; char name[96]; char text[4096]; const char *attributes; size_t attributes_size; };
void xml_start(struct xml *x, const char *text, size_t size);
int xml_next(struct xml *x);                                 /* XML_*; name or text filled */
int xml_attribute(const struct xml *x, const char *name, char *out, size_t size);   /* 0 when found */
int xml_is(const struct xml *x, const char *name);           /* name matches, with or without a prefix */

/* ---- growing text ---- */
struct text { char *data; size_t size, capacity; };
void text_add(struct text *t, const char *s, size_t n);
void text_put(struct text *t, const char *s);
void text_printf(struct text *t, const char *format, ...);
void text_xml(struct text *t, const char *s, size_t n);     /* escaped for XML */
void text_utf8(struct text *t, const char *latin1, size_t n, int xml);   /* Latin-1 as UTF-8, escaped for XML if XML */
void text_latin1(struct text *t, const char *utf8, size_t n);            /* UTF-8 as Latin-1; other characters become '?' */

char *read_whole(const char *path, size_t *size);
int write_whole(const char *path, const void *data, size_t size);
const char *extension_of(const char *path);
void open_in_browser(const char *gdoc_path);               /* .gdoc/.gsheet: the document's URL in the default browser */
