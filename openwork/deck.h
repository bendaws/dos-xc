/*
 * DOS-XC /openwork/deck.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "office.h"

/* Slides are 960 x 540 units: one unit is one point, 12700 EMU in a .pptx. */
#define SLIDE_W 960
#define SLIDE_H 540
#define DECK_THEMES 6

enum { LAYOUT_TITLE, LAYOUT_CONTENT, LAYOUT_SECTION, LAYOUT_TITLE_ONLY, LAYOUT_BLANK, LAYOUT_COUNT };
enum { BOX_TITLE, BOX_BODY, BOX_COUNT };

/* Text is Latin-1.  Lines end in '\n'; a body line's leading tabs give its bullet level. */
struct slide { int layout; char *text[BOX_COUNT], *notes; };
struct deck { struct slide *slides; int count, capacity, theme; };

struct deck_theme { const char *name; unsigned background, title, text, accent; };
extern const struct deck_theme deck_themes[DECK_THEMES];
extern const char *const layout_names[LAYOUT_COUNT];

/* Where a layout puts a box, in slide units; 0 when the layout has no such box. */
struct box_style { int x, y, w, h, points, align, anchor, bullets; };   /* align 0 left 1 centre; anchor 0 top 1 middle 2 bottom */
int box_style(int layout, int box, struct box_style *out);
const char *box_prompt(int layout, int box);   /* "Click to add title" */

void deck_init(struct deck *d);
void deck_free(struct deck *d);
struct slide *deck_insert(struct deck *d, int at, int layout);
void deck_remove(struct deck *d, int at);
void deck_set(char **field, const char *s, size_t n);

/* Formats are chosen by the file's extension: pptx odp. */
int deck_load(struct deck *d, const char *path, char *error, int error_size);
int deck_save(const struct deck *d, const char *path, char *error, int error_size);
