/*
 * DOS-XC /openwork/suite.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "wm.h"
#include "office.h"

/* Icons in RIBBON16.PNG and RIBBON32.PNG, in the order icons.py draws them. */
enum {
    IC_PASTE, IC_CUT, IC_COPY, IC_SELECT_ALL, IC_FIND, IC_BOLD, IC_ITALIC, IC_UNDERLINE, IC_BULLETS,
    IC_ALIGN_LEFT, IC_ALIGN_CENTER, IC_ALIGN_RIGHT, IC_CLEAR_FORMAT, IC_DATE, IC_WORD_COUNT, IC_AUTOSUM, IC_FUNCTION, IC_CLEAR,
    IC_NEW_SLIDE, IC_LAYOUT, IC_DELETE, IC_DUPLICATE, IC_PLAY, IC_PLAY_CURRENT,
    IC_NEW_MAIL, IC_REPLY, IC_REPLY_ALL, IC_FORWARD, IC_SEND_RECEIVE, IC_READ, IC_UNREAD, IC_SEND, IC_ATTACH,
    IC_ACCOUNT, IC_SETTINGS, IC_INBOX, IC_SENT, IC_TRASH, IC_FOLDER, IC_DRAFTS,
    IC_NEW_DOC, IC_OPEN, IC_SAVE, IC_SAVE_AS, IC_INFO, IC_CLOSE, IC_BACK, IC_HELP, IC_COMPUTER, IC_MARK, IC_CANCEL, IC_SECTION,
    IC_REPLACE, IC_SYMBOL, IC_COUNT
};

#define OW_TEXT 0x444444
#define OW_FAINT 0x7a7a7a
#define OW_BORDER 0xd4d4d4
#define OW_LINE 0xe1e1e1
#define OW_PANEL 0xf3f3f3
#define OW_SELECTED 0xcde6f7

#define TAB_H 24
#define RIBBON_BODY_H 94
#define RIBBON_H (TAB_H + RIBBON_BODY_H)
#define STATUS_H 22

/* Backstage pages an application offers (struct suite_app.pages). */
#define BS_NEW 1
#define BS_OPEN 2
#define BS_SAVE 4
#define BS_SAVE_AS 8

struct suite_app {
    const char *name;                    /* "OpenPapers" */
    const char *icon;                    /* ICON.PNG and ICON32.PNG beside the program */
    const char *blurb;                   /* the About dialog's line about this program */
    const char *untitled;                /* "Document1" */
    const char *kind, *kinds;            /* "document", "Documents" */
    const char *formats;                 /* lines for Save As: "Word Document (.docx)\n..." */
    u32 accent, on_accent;               /* the accent and text drawn on it */
    int pages;                           /* BS_* */
    void (*new_file)(void);
    void (*open_file)(const char *path);
    void (*save)(void);
    void (*save_as)(void);
    void (*info)(char *out, size_t size);   /* "Label\tValue\n" lines for File > Info */
    void (*settings)(void);              /* File > Info's Settings button, if any */
};
extern const struct suite_app *app;
extern char suite_path[300];
/* Loads the shared icons from ARGV0's folder and sets the program's icon. */
void suite_start(const struct suite_app *a, const char *argv0);
/* PATH is now the open file ("" for a new one): the title and recent files follow. */
void suite_document(struct window *w, const char *path);
void suite_about(void);
u32 mix(u32 a, u32 b, int percent);      /* PERCENT of B over A */
u32 accent_text(void);                   /* the accent, darkened when too light for text on white */

void ow_icon(const struct canvas *c, int x, int y, int icon, int size, u32 tint);   /* TINT 0 keeps the colours */
void ow_app_icon(const struct canvas *c, int x, int y, int big);
void ow_text_scaled(const struct canvas *c, int x, int y, const struct font *f, const char *s, u32 color, int scale);
void ow_text_fit(const struct canvas *c, int x, int y, int width, const struct font *f, const char *s, u32 color);   /* cut with "..." */
void ow_button(const struct canvas *c, struct rect r, const char *label, int primary, int hover);
void ow_field(const struct canvas *c, const struct field *f, struct rect r, int focused, int secret);
void ow_status(const struct canvas *c, int y, int width, const char *left, const char *right);
void ow_vscroll_paint(const struct canvas *c, const struct vscroll *s, struct rect r);
int ow_vscroll_mouse(struct vscroll *s, struct rect r, int x, int y, int kind);

enum { RB_LARGE, RB_SMALL, RB_COMBO, RB_GALLERY };
#define RB_LABEL 1                       /* a small button's label beside its icon */
#define RB_MENU 2                        /* the button opens ribbon.menu(command) */
struct rb_item {
    int kind, command, icon;
    int row;                             /* small buttons and combo boxes: 0 to 2 */
    int width;                           /* combo boxes; a gallery's entries */
    int flags;
    const char *label;                   /* '\n' splits a large button's label */
    int count;                           /* a gallery's entries */
};
struct rb_group { const char *label; const struct rb_item *items; int count; };
struct rb_tab { const char *label; const struct rb_group *groups; int count; };
#define RB_ON 1
#define RB_OFF 2
#define RB_TAG 1000                      /* popup tags from RB_TAG + command belong to the ribbon */
#define RB_HITS 160
/* Something clickable that the last paint drew. */
struct rb_hit { struct rect r; int kind, a; const struct rb_item *item; };
struct ribbon {
    const struct rb_tab *tabs;
    int tab_count, tab;
    void (*command)(int command, int choice);   /* CHOICE: a menu's or gallery's entry, else -1 */
    int (*state)(int command);                  /* RB_ON | RB_OFF */
    const char *(*menu)(int command);           /* items for window_popup */
    const char *(*value)(int command);          /* a combo box's text */
    void (*gallery_paint)(const struct canvas *c, struct rect r, int command, int index, int selected);
    int (*gallery_selected)(int command);
    int backstage, page;                        /* File is open, at that page */
    int hover;                                  /* the hit under the mouse, or -1 */
    struct rb_hit hits[RB_HITS];
    int hit_count;
};
/* Draw the tabs and ribbon (or the whole File view, returning 1). */
int ribbon_paint(struct ribbon *rb, struct window *w, const struct canvas *c);
int ribbon_mouse(struct ribbon *rb, struct window *w, int x, int y, int kind);   /* 1 when it took the event */
int ribbon_key(struct ribbon *rb, struct window *w, int key);
int ribbon_popup(struct ribbon *rb, struct window *w, int tag, int index);
void ribbon_file(struct ribbon *rb, struct window *w, int page);   /* opens File at a page: 0 Info */
