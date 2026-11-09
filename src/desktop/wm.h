/*
 * DOS-XC /desktop/wm.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The graphical desktop.  gfx.c draws; desktop.c is DESKTOP.EXE, the
 * compositor and window manager (one framebuffer per monitor, all placed
 * in one coordinate space), and apps.c its settings windows.  Every other
 * program is a process of its own, in the Apps folder of the DOS-XC Desktop
 * package, built on gui.c.
 *
 * Memory is shared by all programs, so a client's window pixels are drawn
 * straight from its own memory; events and requests go through one-way
 * rings on a board that the desktop publishes (DOSXC_TASK_SHARE_SET).
 */
#pragma once
#include <dosxc/syscall.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

struct rect { int x, y, w, h; };
static inline int rect_has(struct rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static inline int rect_meets(struct rect a, struct rect b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
struct rect rect_both(struct rect a, struct rect b);    /* intersection */
struct rect rect_either(struct rect a, struct rect b);  /* bounding box */

/* The look: SerenityOS's Default theme. */
#define C_DESKTOP 0x505050
#define C_ACTIVE1 theme.active1
#define C_ACTIVE2 theme.active2
#define C_ACTIVE_SHADOW theme.active_shadow
#define C_MOVING1 theme.moving1
#define C_MOVING2 theme.moving2
#define C_MOVING_SHADOW theme.moving_shadow
#define C_INACTIVE1 0x808080
#define C_INACTIVE2 0xc0c0c0
#define C_INACTIVE_TEXT 0xd5d0c7
#define C_INACTIVE_SHADOW 0x4c4c4c
#define C_WINDOW 0xd4d0c8
#define C_BASE 0xffffff
#define C_TEXT 0x000000
#define C_HIGHLIGHT 0xffffff
#define C_SHADOW1 0x808080
#define C_SHADOW2 0x404040
#define C_HOVER 0xe3dfdb
#define C_SELECTION theme.selection
#define C_MENU_STRIPE 0xdbd8d1
#define C_MENU_SELECTION theme.menu_selection
#define C_DISABLED 0x808080

/* The colors that follow the user's accent color (gfx.c). */
#define ACCENT_DEFAULT 0x84351au
struct theme { u32 active1, active2, active_shadow, moving1, moving2, moving_shadow, selection, menu_selection; };
extern struct theme theme;
void theme_set(u32 accent);

struct surface { u32 *px; int w, h, capacity; };
struct canvas { struct surface *s; int ox, oy; struct rect clip; };   /* s(x + ox, y + oy); clip uses x, y */
int surface_size(struct surface *s, int w, int h);

struct font { int height, spacing; const unsigned char *widths; const unsigned short *rows; };
extern const struct font font_regular, font_bold, font_mono;

#define ICON_SIZE 16
struct icon { u32 px[ICON_SIZE * ICON_SIZE]; };
enum { ICON_LOGO, ICON_FOLDER, ICON_FILE, ICON_PROGRAM, ICON_DRIVE, ICON_PROMPT, ICON_DISPLAY, ICON_INFO, ICON_EXIT, ICON_POWER, ICON_RESTART,
       ICON_CALC, ICON_EDITOR, ICON_PAINT, ICON_TASKS, ICON_CARDS, ICON_MINES, ICON_CHESS, ICON_PICTURE, ICON_COMPUTER, ICON_STAR,
       ICON_SOUND, ICON_NET, ICON_NET_OFF, ICON_PACKAGE, ICON_SETUP, ICON_GEAR, ICON_GAMES, ICON_TOOLS, ICON_COUNT };
extern struct icon icons[ICON_COUNT];
void icons_init(void);

void fill(const struct canvas *c, int x, int y, int w, int h, u32 color);
void gradient(const struct canvas *c, int x, int y, int w, int h, u32 left, u32 right);
void blit(const struct canvas *c, int x, int y, const struct surface *from);
void draw_pixels(const struct canvas *c, int x, int y, int w, int h, const u32 *px);  /* 0 is transparent */
void draw_icon(const struct canvas *c, int x, int y, int icon);
int text_width(const struct font *f, const char *s);
void text(const struct canvas *c, int x, int y, const struct font *f, const char *s, u32 color);
void text_big(const struct canvas *c, int x, int y, const char *s, u32 color, int scale);
void bevel(const struct canvas *c, int x, int y, int w, int h, int sunken);          /* 2 px, filled */
void button(const struct canvas *c, struct rect r, const char *label, int pressed, int bold);
void frame_box(const struct canvas *c, int x, int y, int w, int h, u32 color);

/* Windows. */
enum { M_DOWN, M_UP, M_MOVE, M_DOUBLE, M_WHEEL, M_RIGHT };
struct window;
struct gui_window_info;
struct app_ops {
    void (*paint)(struct window *, const struct canvas *);
    void (*mouse)(struct window *, int x, int y, int kind, int value);
    void (*key)(struct window *, int key);
    void (*closed)(struct window *);
    void (*popup)(struct window *, int tag, int index);   /* a menu's choice, -1 if dismissed */
    void (*tick)(struct window *);                        /* about ten times a second */
    void (*resized)(struct window *);
    int (*closing)(struct window *);                      /* 0 keeps the window open */
};
struct window {
    struct rect r;                   /* the content, in desktop coordinates */
    struct surface content;
    char title[80];
    int icon, minimized, maximized, resizable, needs_paint;
    int track_mouse;                 /* also gets M_MOVE with no button down */
    struct rect restore;
    const struct app_ops *ops;
    void *data;
    /* The desktop's view of a client window; remote is the client's own struct. */
    struct window *remote;
    int client;
    volatile u32 seq;                /* the client adds 1 after each repaint */
    u32 seen;
};
struct window *window_open(int w, int h, const char *title, int icon, const struct app_ops *ops, void *data);
void window_close(struct window *);
void window_title(struct window *, const char *);
static inline void window_paint(struct window *w) { w->needs_paint = 1; }

/* Monitors. */
#define MAX_SCREENS 8
#define MAX_MODES 24
struct scale_step { int src, weight; };   /* a device pixel: source pixel and 0..256 of the next */
struct screen {
    struct rect r;
    struct surface back, wall;
    unsigned char *framebuffer;
    int pitch, mode, mode_count;
    int preferred_width, preferred_height;
    int phys_w, phys_h;              /* the framebuffer; r is smaller when scaled */
    struct scale_step *xmap, *ymap;
    struct dosxc_display_mode modes[MAX_MODES];
    char name[32];
};
extern struct screen screens[MAX_SCREENS];
extern int screen_count, main_screen;
extern int ui_scale;                                      /* percent: 100, 125 or 150 */
void set_ui_scale(int percent);
void layout_changed(void);                                /* after screens[] positions or modes were edited */
void identify_screens(void);

void leave_desktop(void);

/* Apps of the desktop itself. */
void open_about(void);
void open_display_settings(void);
void open_wallpaper(void);
void open_sound(void);
void open_accent(void);
void set_accent(u32 color, int save);
u32 current_accent(void);
void set_sound_volume(int percent);
void set_wallpaper(const char *path);           /* "" for none */
const char *current_wallpaper(void);

/* Shared by the desktop and clients. */
void message_box(const char *title, const char *line1, const char *line2);
void file_dialog(const char *title, int save, const char *start, void (*done)(const char *path, void *data), void *data);
void ask_box(const char *title, const char *question, const char *yes, const char *no, void (*done)(int answer, void *data), void *data);  /* 1 yes, 0 no, -1 cancel */
void window_modal(struct window *w);            /* only it takes input until it closes */

/* Pictures: image.c */
int image_load(const char *path, struct surface *out);
int image_save_bmp(const char *path, const struct surface *s);
int image_is_picture(const char *name);
void image_cover(const struct surface *from, struct surface *to);
void image_scale(const struct surface *from, struct surface *to);

/* The DOS-XC Desktop package's folder (widgets.c). */
void desktop_path(char *out, size_t size, const char *relative);

/* ---- widgets (gui.c) ----------------------------------------------------- */
struct vscroll { int total, visible, top, dragging, grab_y, grab_top; };
void vscroll_paint(const struct canvas *c, const struct vscroll *s, struct rect r);
int vscroll_mouse(struct vscroll *s, struct rect r, int x, int y, int kind, int value);
void vscroll_clamp(struct vscroll *s);
struct field { char text[260]; int cursor; };
void field_paint(const struct canvas *c, const struct field *f, struct rect r, int focused);
int field_key(struct field *f, int key);
void field_set(struct field *f, const char *text);
#define MENUBAR_H 20
struct menubar { const char *titles[8], *items[8]; };
void menubar_paint(const struct canvas *c, const struct menubar *m, int width);
int menubar_mouse(struct window *w, const struct menubar *m, int x, int y, int kind);

/* ---- the client side (gui.c) ---------------------------------------------- */
int gui_connect(void);                          /* 0, or -1 when the desktop is not running */
void gui_run(void);                             /* until the last window closes */
void window_popup(struct window *w, int x, int y, const char *items, int tag);  /* items end in \n; "-" is a line */
int gui_launch(const char *program, const char *argument);  /* a program in \SYSTEM\Apps */
void gui_open_file(const char *path);           /* in the program that handles its type */
void gui_run_text(const char *path);            /* in a terminal window */
void gui_set_poll(int (*poll)(void));           /* called each pass of gui_run; nonzero means it did work */
void gui_pump(void);                            /* handle input and paint during a long operation */
void gui_set_wallpaper(const char *path);
void gui_activate(struct gui_window_info *w);
void window_request_size(struct window *w, int width, int height);
void gui_clipboard_set(const char *text, int size);
int gui_clipboard_get(char *out, int capacity);

/* ---- the board ------------------------------------------------------------ */
#define GUI_MAGIC 0x44475549u
#define GUI_CLIENTS 24
#define GUI_RING 256
#define GUI_REQUESTS 64
#define GUI_WINDOWS 32
enum { EV_MOUSE = 1, EV_KEY, EV_CLOSE, EV_RESIZE, EV_POPUP };
struct gui_event { int type, x, y, kind, value, pad; uintptr_t window; };
enum { RQ_OPEN = 1, RQ_CLOSE, RQ_POPUP, RQ_WALLPAPER, RQ_ACTIVATE, RQ_RESIZE };
struct gui_request { int type, x, y, tag; uintptr_t window; char text[320]; };
struct gui_client {
    volatile int pid;
    volatile u32 ev_read, ev_write, rq_read, rq_write;
    struct gui_event events[GUI_RING];
    struct gui_request requests[GUI_REQUESTS];
};
struct gui_window_info { int pid, active; uintptr_t window; char title[80]; };
struct gui_board {
    u32 magic;
    volatile u32 accent;            /* every program follows it */
    volatile int window_count;
    struct gui_window_info windows[GUI_WINDOWS];
    volatile int clipboard_size;
    char clipboard[65536];
    struct gui_client clients[GUI_CLIENTS];
};
struct gui_board *gui_board(void);
