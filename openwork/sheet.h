/*
 * DOS-XC /openwork/sheet.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "office.h"

#define SHEET_ROWS 1000
#define SHEET_COLS 52

/* What a cell shows: a number, text, or an error such as "#DIV/0!". */
struct value { double number; int kind; char text[128]; };
enum { VALUE_EMPTY, VALUE_NUMBER, VALUE_TEXT, VALUE_ERROR };

/* Cells hold what was typed (Latin-1); a leading '=' makes a formula. */
struct sheet {
    char *cells[SHEET_ROWS][SHEET_COLS];
    struct value *cache[SHEET_ROWS][SHEET_COLS];   /* results, until the next change */
    unsigned char state[SHEET_ROWS][SHEET_COLS];
    int rows, cols;                                 /* the used area */
};

void sheet_init(struct sheet *s);
void sheet_free(struct sheet *s);
const char *sheet_get(const struct sheet *s, int row, int col);   /* "" when empty */
void sheet_set(struct sheet *s, int row, int col, const char *input);
void sheet_value(struct sheet *s, int row, int col, struct value *out);
void value_format(const struct value *v, char *out, size_t size);
void cell_name(int row, int col, char *out, size_t size);         /* "B3" */
int cell_parse(const char *name, int *row, int *col);             /* characters used; 0 if not a cell */

/* Formats are chosen by the file's extension: csv tsv txt xlsx ods. */
int sheet_load(struct sheet *s, const char *path, char *error, int error_size);
int sheet_save(const struct sheet *s, const char *path, char *error, int error_size);
