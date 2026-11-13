/*
 * DOS-XC /openwork/doc.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "office.h"

enum { STYLE_NORMAL, STYLE_HEADING1, STYLE_HEADING2, STYLE_HEADING3, STYLE_TITLE, STYLE_BULLET, STYLE_COUNT };
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
#define ATTR_BOLD 1
#define ATTR_ITALIC 2
#define ATTR_UNDERLINE 4

/* A paragraph: characters (Latin-1) and each one's ATTR_* bits. */
struct para { char *text; unsigned char *attr; int length, capacity, style, align; };
struct document { struct para *paras; int count, capacity; };

void doc_init(struct document *d);
void doc_free(struct document *d);
struct para *doc_add_para(struct document *d, int style);
void para_insert(struct para *p, int at, const char *s, int n, int attr);
void para_delete(struct para *p, int at, int n);
void doc_split(struct document *d, int para, int at);       /* Enter */
void doc_join(struct document *d, int para);                /* PARA + the next one */

/* Formats are chosen by the file's extension: docx odt rtf html htm txt md. */
int doc_load(struct document *d, const char *path, char *error, int error_size);
int doc_save(const struct document *d, const char *path, char *error, int error_size);
