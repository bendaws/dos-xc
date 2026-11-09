/*
 * DOS-XC /runtime/terminal.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The console device seen by 32/64-bit programs.
 *
 * Output understands the common ANSI/VT100 escape sequences (cursor
 * movement and positioning, erasing, colors, scrolling regions, insert and
 * delete line), so full-screen programs such as Vim work with TERM=ansi.
 * The last column wraps lazily, as on a VT100: writing column 80 leaves the
 * cursor there until the next printable character.
 *
 * Input is either cooked (a line editor with echo; Ctrl+C interrupts, Ctrl+Z
 * or Ctrl+D at the start of a line is end of file) or raw: one byte per
 * key, Enter as CR, and the cursor and function keys as VT220 sequences.
 * Keys come from the PS/2 keyboard (through the kernel) and from USB
 * keyboards (usb.c).
 *
 * A mouse (USB on BIOS boot, the firmware's pointer under UEFI) moves a
 * block cursor over the text screen (BIOS boot) and, in raw mode, is
 * reported to programs that ask for it as an xterm does: CSI ? 1000 h
 * (buttons), 1002 h (and dragging), 1003 h (all motion), 1006 h (SGR
 * encoding "CSI < b ; x ; y M/m").
 */

#include "runtime.h"
#include "usb.h"
#include "net.h"

#define ROWS 25
#define COLUMNS 80

static void vc_output(struct vconsole *v, const u8 *buffer, uptr size);

/* ---- output ----------------------------------------------------------- */

static int state;                       /* 0 text, 1 after ESC, 2 CSI, 3 ESC + intermediate */
static int params[8], param_count, private_mode;
static unsigned row, column, saved_row, saved_column;
static int pending_wrap;
static unsigned top_margin, bottom_margin = ROWS - 1;
static unsigned char attribute = 7, default_attribute = 7;
static int bold, reverse;
static unsigned char foreground = 7, background = 0;
static char chunk[COLUMNS + 1];
static unsigned chunk_length;
static u8 replies[32];
static unsigned reply_length;
static int mouse_tracking, mouse_sgr;    /* 1000/1002/1003, 1006 */

/* ---- the mouse cursor (VGA text memory, BIOS boot) -------------------- */

static int mouse_present, mouse_row = ROWS / 2, mouse_column = COLUMNS / 2, mouse_drawn;
static int mouse_fraction_x, mouse_fraction_y, mouse_wheel;
static unsigned mouse_buttons;
static u16 mouse_saved;

static volatile u16 *mouse_cell(void) {
    return (volatile u16 *)(uptr)0xb8000 + mouse_row * COLUMNS + mouse_column;
}

void terminal_mouse_hide(void) {
    if (!mouse_drawn) return;
    mouse_drawn = 0;
    volatile u16 *cell = mouse_cell();
    if (*cell == (u16)(mouse_saved ^ 0x7700)) *cell = mouse_saved;
}

static void mouse_show(void) {
    if (mouse_drawn || !mouse_present || !ctx->bios_available) return;
    volatile u16 *cell = mouse_cell();
    mouse_saved = *cell;
    *cell = (u16)(mouse_saved ^ 0x7700);
    mouse_drawn = 1;
}

static void flush_chunk(void) {
    if (!chunk_length) return;
    chunk[chunk_length] = 0;
    ctx->write(chunk);
    chunk_length = 0;
}

static void sync_cursor(void) {
    unsigned r, c;
    ctx->get_cursor(&r, &c);
    if (r != row || c != column) pending_wrap = 0;
    row = r;
    column = c;
}

static void move_to(unsigned r, unsigned c) {
    flush_chunk();
    if (r >= ROWS) r = ROWS - 1;
    if (c >= COLUMNS) c = COLUMNS - 1;
    row = r;
    column = c;
    pending_wrap = 0;
    ctx->cursor(r, c);
}

static void apply_color(void) {
    unsigned char fg = foreground | (bold ? 8 : 0), bg = background;
    unsigned char value = reverse ? (unsigned char)((fg & 7) << 4 | bg) : (unsigned char)(bg << 4 | fg);
    if (value != attribute) {
        flush_chunk();
        attribute = value;
        ctx->text_color(attribute);
    }
}

/* Line feed: next row, scrolling the region when at its bottom margin. */
static void line_feed(void) {
    flush_chunk();
    pending_wrap = 0;
    if (row == bottom_margin && (top_margin || bottom_margin != ROWS - 1)) {
        if (!ctx->scroll || ctx->scroll(top_margin, bottom_margin, 1)) {
            ctx->putc('\n');            /* no region support: scroll everything */
            sync_cursor();
            return;
        }
        move_to(row, 0);
        return;
    }
    ctx->putc('\n');
    sync_cursor();
}

static void put_printable(char c) {
    if (pending_wrap) {
        flush_chunk();
        pending_wrap = 0;
        if (row == bottom_margin) {
            move_to(row, 0);
            line_feed();
        } else {
            move_to(row + 1, 0);
        }
    }
    if (column == COLUMNS - 1) {
        flush_chunk();
        ctx->put_at(row, column, c);
        ctx->cursor(row, column);
        pending_wrap = 1;
        return;
    }
    chunk[chunk_length++] = c;
    ++column;
    if (chunk_length == COLUMNS) flush_chunk();
}

static unsigned param(int i, unsigned fallback) {
    return i < param_count && params[i] > 0 ? (unsigned)params[i] : fallback;
}

static void erase_cells(unsigned r, unsigned c, unsigned count) {
    flush_chunk();
    if (count) ctx->erase(r, c, count);
}

static void select_graphic(void) {
    static const unsigned char vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    if (!param_count) params[param_count++] = 0;
    for (int i = 0; i < param_count; ++i) {
        int p = params[i] < 0 ? 0 : params[i];
        if (p == 0) {
            bold = reverse = 0;
            foreground = default_attribute & 15;
            background = (default_attribute >> 4) & 7;
            if (foreground & 8) { bold = 1; foreground &= 7; }
        } else if (p == 1) bold = 1;
        else if (p == 22) bold = 0;
        else if (p == 7) reverse = 1;
        else if (p == 27) reverse = 0;
        else if (p >= 30 && p <= 37) foreground = vga[p - 30];
        else if (p == 39) foreground = default_attribute & 7;
        else if (p >= 40 && p <= 47) background = vga[p - 40];
        else if (p == 49) background = (default_attribute >> 4) & 7;
        else if (p >= 90 && p <= 97) { foreground = vga[p - 90]; bold = 1; }
        else if (p >= 100 && p <= 107) background = vga[p - 100];
    }
    apply_color();
}

static void reply(const char *text) {
    while (*text && reply_length < sizeof(replies)) replies[reply_length++] = (u8)*text++;
}

static void report_cursor(void) {
    char text[24];
    unsigned n = 0, values[2] = {row + 1, column + 1};
    text[n++] = 27;
    text[n++] = '[';
    for (int i = 0; i < 2; ++i) {
        char digits[8];
        unsigned d = 0, v = values[i];
        do digits[d++] = (char)('0' + v % 10); while (v /= 10);
        while (d) text[n++] = digits[--d];
        text[n++] = i ? 'R' : ';';
    }
    text[n] = 0;
    reply(text);
}

static void control_sequence(char final) {
    unsigned n = param(0, 1);
    switch (final) {
    case 'A': move_to(row >= n ? row - n : 0, column); break;
    case 'B': case 'e': move_to(row + n, column); break;
    case 'C': case 'a': move_to(row, column + n); break;
    case 'D': move_to(row, column >= n ? column - n : 0); break;
    case 'E': move_to(row + n, 0); break;
    case 'F': move_to(row >= n ? row - n : 0, 0); break;
    case 'G': case '`': move_to(row, n - 1); break;
    case 'd': move_to(n - 1, column); break;
    case 'H': case 'f': move_to(param(0, 1) - 1, param(1, 1) - 1); break;
    case 'J': {
        unsigned mode = param_count ? (unsigned)params[0] : 0, at = row * COLUMNS + column;
        if (mode == 0) erase_cells(row, column, ROWS * COLUMNS - at);
        else if (mode == 1) erase_cells(0, 0, at + 1);
        else erase_cells(0, 0, ROWS * COLUMNS);
        break;
    }
    case 'K': {
        unsigned mode = param_count ? (unsigned)params[0] : 0;
        if (mode == 0) erase_cells(row, column, COLUMNS - column);
        else if (mode == 1) erase_cells(row, 0, column + 1);
        else erase_cells(row, 0, COLUMNS);
        break;
    }
    case 'X': erase_cells(row, column, n < COLUMNS - column ? n : COLUMNS - column); break;
    case 'L': case 'M':                   /* insert / delete lines in the scrolling region */
        flush_chunk();
        if (row >= top_margin && row <= bottom_margin && ctx->scroll) {
            for (unsigned i = 0; i < n && i <= bottom_margin - row; ++i)
                ctx->scroll(row, bottom_margin, final == 'M');
        }
        move_to(row, 0);
        break;
    case 'S': case 'T':
        flush_chunk();
        if (ctx->scroll)
            for (unsigned i = 0; i < n; ++i) ctx->scroll(top_margin, bottom_margin, final == 'S');
        break;
    case 'r': {
        unsigned top = param(0, 1) - 1, bottom = param(1, ROWS) - 1;
        if (bottom >= ROWS) bottom = ROWS - 1;
        if (top < bottom) {
            top_margin = top;
            bottom_margin = bottom;
        }
        move_to(0, 0);
        break;
    }
    case 'm': select_graphic(); break;
    case 's': saved_row = row; saved_column = column; break;
    case 'u': move_to(saved_row, saved_column); break;
    case 'n': if (param(0, 0) == 6) report_cursor(); else if (param(0, 0) == 5) reply("\033[0n"); break;
    case 'c': if (!private_mode) reply("\033[?1;0c"); break;
    case 'h': case 'l':
        if (private_mode)
            for (int i = 0; i < param_count; ++i) {
                int mode = params[i];
                if (mode == 1000 || mode == 1002 || mode == 1003) {
                    if (final == 'h') mouse_tracking = mode;
                    else if (mouse_tracking == mode) mouse_tracking = 0;
                } else if (mode == 1006) mouse_sgr = final == 'h';
            }
        break;
    default: break;                       /* h, l (modes) and anything else: ignored */
    }
}

static void put_byte(u8 b) {
    switch (state) {
    case 1:
        if (b == '[') {
            state = 2;
            param_count = 0;
            private_mode = 0;
            for (int i = 0; i < 8; ++i) params[i] = -1;
            return;
        }
        state = 0;
        if (b == '7') { saved_row = row; saved_column = column; }
        else if (b == '8') move_to(saved_row, saved_column);
        else if (b == 'M') {                /* reverse index */
            if (row == top_margin) {
                flush_chunk();
                if (ctx->scroll) ctx->scroll(top_margin, bottom_margin, 0);
            } else move_to(row - 1, column);
        } else if (b == 'D') line_feed();
        else if (b == 'E') { move_to(row, 0); line_feed(); }
        else if (b == 'c') {
            top_margin = 0;
            bottom_margin = ROWS - 1;
            params[0] = 0;
            param_count = 1;
            select_graphic();
            erase_cells(0, 0, ROWS * COLUMNS);
            move_to(0, 0);
        } else if (b == '(' || b == ')' || b == '#') state = 3;
        return;
    case 3:
        state = 0;
        return;
    case 2:
        if (b >= '0' && b <= '9') {
            if (!param_count) param_count = 1;
            int *p = &params[param_count - 1];
            *p = (*p < 0 ? 0 : *p) * 10 + (b - '0');
            if (*p > 9999) *p = 9999;
            return;
        }
        if (b == ';') {
            if (!param_count) param_count = 1;
            if (param_count < 8) ++param_count;
            return;
        }
        if (b == '?' || b == '>' || b == '=') { private_mode = 1; return; }
        if (b >= 0x20 && b < 0x40) return;
        state = 0;
        control_sequence((char)b);
        return;
    }
    switch (b) {
    case 27: flush_chunk(); state = 1; return;
    case '\r': flush_chunk(); pending_wrap = 0; ctx->putc('\r'); column = 0; return;
    case '\n': case 11: case 12: line_feed(); return;
    case '\b':
        flush_chunk();
        if (pending_wrap) pending_wrap = 0;
        else if (column) move_to(row, column - 1);
        return;
    case '\t': {
        unsigned next = (column / 8 + 1) * 8;
        move_to(row, next < COLUMNS ? next : COLUMNS - 1);
        return;
    }
    case 7: case 0: case 14: case 15: return;
    }
    if (b < 32 || b == 127) return;
    put_printable((char)b);
}

dosxc_ssize terminal_write(const u8 *buffer, uptr size) {
    if (current && current->vc) {
        vc_output(current->vc, buffer, size);
        return (dosxc_ssize)size;
    }
    int shown = mouse_drawn;
    terminal_mouse_hide();
    sync_cursor();
    for (uptr i = 0; i < size; ++i) put_byte(buffer[i]);
    flush_chunk();
    if (shown) mouse_show();
    return (dosxc_ssize)size;
}

/* A new program on the console starts with a clean terminal state. */
void terminal_reset(void) {
    state = 0;
    default_attribute = ctx->get_color ? ctx->get_color() : 7;
    attribute = default_attribute;
    foreground = default_attribute & 7;
    background = (default_attribute >> 4) & 7;
    bold = (default_attribute & 8) != 0;
    reverse = 0;
    top_margin = 0;
    bottom_margin = ROWS - 1;
    pending_wrap = 0;
    reply_length = 0;
    mouse_tracking = mouse_sgr = 0;
}

/* ---- input ------------------------------------------------------------ */

struct input {
    char line[512];
    uptr line_length, line_position;
    int raw_mode;
    u8 queue[64];
    unsigned queue_head, queue_tail;
};
static struct input console_input;

/* ---- virtual consoles ------------------------------------------------- */

/* A terminal window: programs attached to one read keys the window pushes
   and write bytes the window draws (it interprets the escape sequences). */
#define VC_COUNT 8
#define VC_OUT 16384
#define VC_KEYS 256
struct vconsole {
    int used, closed, refs, owner;
    u8 out[VC_OUT];
    volatile u32 out_read, out_write;
    int keys[VC_KEYS];
    volatile u32 key_read, key_write;
    unsigned char color;
    struct input in;
};
static struct vconsole consoles[VC_COUNT];

/* The input state the current call works on. */
static struct input *I = &console_input;
static void select_input(void) { I = current && current->vc ? &current->vc->in : &console_input; }
#define line (I->line)
#define line_length (I->line_length)
#define line_position (I->line_position)
#define raw_mode (I->raw_mode)
#define queue (I->queue)
#define queue_head (I->queue_head)
#define queue_tail (I->queue_tail)

int vc_open(int owner) {
    for (int i = 0; i < VC_COUNT; ++i) {
        struct vconsole *v = &consoles[i];
        if (v->used) continue;
        memset(v, 0, sizeof(*v));
        v->used = 1;
        v->owner = owner;
        v->color = 7;
        return i;
    }
    return -DOSXC_EAGAIN;
}

struct vconsole *vc_get(int id) {
    if (id < 0 || id >= VC_COUNT || !consoles[id].used || consoles[id].closed) return 0;
    return &consoles[id];
}

void vc_attach(struct process *p, struct vconsole *v) {
    if (p->vc == v) return;
    p->vc = v;
    if (v) ++v->refs;
}

static void vc_release_slot(struct vconsole *v) { if (v->closed && !v->refs) v->used = 0; }

void vc_detach(struct process *p) {
    struct vconsole *v = p->vc;
    p->vc = 0;
    if (!v) return;
    --v->refs;
    vc_release_slot(v);
}

/* The window went away: whatever runs in it ends. */
void vc_close(struct vconsole *v) {
    if (!v || v->closed) return;
    v->closed = 1;
    process_kill_console(v);
    vc_release_slot(v);
}

void vc_owner_died(int pid) {
    for (int i = 0; i < VC_COUNT; ++i)
        if (consoles[i].used && !consoles[i].closed && consoles[i].owner == pid) vc_close(&consoles[i]);
}

int vc_read(struct vconsole *v, u8 *buffer, uptr size) {
    uptr n = 0;
    while (n < size && v->out_read != v->out_write) {
        buffer[n++] = v->out[v->out_read];
        v->out_read = (v->out_read + 1) % VC_OUT;
    }
    return (int)n;
}

int vc_push_keys(struct vconsole *v, const int *keys, uptr count) {
    uptr n = 0;
    for (; n < count; ++n) {
        u32 next = (v->key_write + 1) % VC_KEYS;
        if (next == v->key_read) break;
        v->keys[v->key_write] = keys[n];
        v->key_write = next;
    }
    return (int)n;
}

static void vc_output(struct vconsole *v, const u8 *buffer, uptr size) {
    for (uptr i = 0; i < size; ++i) {
        u32 next = (v->out_write + 1) % VC_OUT;
        while (next == v->out_read) {
            if (v->closed) return;
            sched_sleep(1000000);
        }
        v->out[v->out_write] = buffer[i];
        v->out_write = next;
    }
}

/* Console operations on a window, as escape sequences it understands. */
int vc_clear(struct vconsole *v) {
    vc_output(v, (const u8 *)"\033[2J\033[H", 7);
    return 0;
}

static unsigned vc_number(char *out, unsigned v) {
    char digits[12];
    unsigned d = 0, n = 0;
    do digits[d++] = (char)('0' + v % 10); while (v /= 10);
    while (d) out[n++] = digits[--d];
    return n;
}

int vc_cursor(struct vconsole *v, unsigned row, unsigned column) {
    char s[32];
    unsigned n = 0;
    s[n++] = 27;
    s[n++] = '[';
    n += vc_number(s + n, row + 1);
    s[n++] = ';';
    n += vc_number(s + n, column + 1);
    s[n++] = 'H';
    vc_output(v, (const u8 *)s, n);
    return 0;
}

/* CSI attribute ; whole-screen z: DOS colors, which ANSI cannot express. */
int vc_color(struct vconsole *v, unsigned attribute, int whole) {
    char s[32];
    unsigned n = 0;
    v->color = (unsigned char)attribute;
    s[n++] = 27;
    s[n++] = '[';
    n += vc_number(s + n, attribute & 255);
    s[n++] = ';';
    n += vc_number(s + n, whole ? 1 : 0);
    s[n++] = 'z';
    vc_output(v, (const u8 *)s, n);
    return 0;
}

int vc_get_color(struct vconsole *v) { return v->color; }

static void enqueue(const char *bytes) {
    while (*bytes && queue_tail < sizeof(queue)) queue[queue_tail++] = (u8)*bytes++;
}

/* Translate one key (keyboard driver code) into raw-mode bytes. */
static void queue_key(int key) {
    static const char *const special[] = {
        [0x101 - 0x101] = "\033[A", [0x102 - 0x101] = "\033[B", [0x103 - 0x101] = "\033[C", [0x104 - 0x101] = "\033[D",
        [0x105 - 0x101] = "\033[H", [0x106 - 0x101] = "\033[F", [0x107 - 0x101] = "\033[2~", [0x108 - 0x101] = "\033[3~",
        [0x109 - 0x101] = "\033[5~", [0x10a - 0x101] = "\033[6~", [0x10b - 0x101] = "\033OP", [0x10c - 0x101] = "\033OQ",
        [0x10d - 0x101] = "\033OR", [0x10e - 0x101] = "\033OS", [0x10f - 0x101] = "\033[15~", [0x110 - 0x101] = "\033[17~",
        [0x111 - 0x101] = "\033[18~", [0x112 - 0x101] = "\033[19~", [0x113 - 0x101] = "\033[20~", [0x114 - 0x101] = "\033[21~",
        [0x115 - 0x101] = "\033[23~", [0x116 - 0x101] = "\033[24~",
    };
    if (queue_head == queue_tail) queue_head = queue_tail = 0;
    if (key == '\n') { enqueue("\r"); return; }
    if (key >= 0 && key < 256) {
        if (queue_tail < sizeof(queue)) queue[queue_tail++] = (u8)key;
        return;
    }
    if (key >= 0x101 && key <= 0x116 && special[key - 0x101]) enqueue(special[key - 0x101]);
}

static void number(char *out, unsigned *n, unsigned v) {
    char digits[8];
    unsigned d = 0;
    do digits[d++] = (char)('0' + v % 10); while (v /= 10);
    while (d) out[(*n)++] = digits[--d];
}

/* One mouse event as an xterm report: CODE is the button (0 left, 1 middle,
   2 right, 3 release in the old encoding, 64/65 wheel) + 32 for motion. */
static void report_mouse(unsigned code, int release) {
    char text[32];
    unsigned n = 0;
    text[n++] = 27;
    text[n++] = '[';
    if (mouse_sgr) {
        text[n++] = '<';
        number(text, &n, code);
        text[n++] = ';';
        number(text, &n, (unsigned)mouse_column + 1);
        text[n++] = ';';
        number(text, &n, (unsigned)mouse_row + 1);
        text[n++] = release ? 'm' : 'M';
    } else {
        text[n++] = 'M';
        text[n++] = (char)(32 + (release ? 3 : code));
        text[n++] = (char)(33 + mouse_column);
        text[n++] = (char)(33 + mouse_row);
    }
    text[n] = 0;
    if (queue_head == queue_tail) queue_head = queue_tail = 0;
    if (queue_tail + n <= sizeof(queue)) enqueue(text);
}

/* Raw movement for graphical programs; the text cursor stays out of it. */
int terminal_pointer(struct dosxc_pointer *out) {
    int dx = 0, dy = 0, wheel = 0, present = 0;
    unsigned buttons = 0;
    if (ctx->pointer) present = ctx->pointer(&dx, &dy, &wheel, &buttons) > 0;
    else {
        struct usb_mouse_state m;
        present = usb_mouse_take(&m);
        dx = m.dx; dy = m.dy; wheel = m.wheel; buttons = m.buttons;
    }
    if (out) {
        out->dx = dx;
        out->dy = dy;
        out->wheel = wheel;
        out->buttons = buttons;
    }
    return present;
}

/* Read the mice, move the cursor, and report to the program if it asked. */
static void mouse_poll_console(void);
static void mouse_poll(void) {
    struct input *saved = I;
    I = &console_input;
    mouse_poll_console();
    I = saved;
}

static void mouse_poll_console(void) {
    if (display_graphics()) return;          /* no text cursor on a graphics screen */
    int dx = 0, dy = 0, wheel = 0;
    unsigned buttons = 0;
    int present = 0;
    if (ctx->pointer) present = ctx->pointer(&dx, &dy, &wheel, &buttons) > 0;
    else {
        struct usb_mouse_state m;
        present = usb_mouse_take(&m);
        dx = m.dx; dy = m.dy; wheel = m.wheel; buttons = m.buttons;
    }
    if (!present) {
        if (mouse_present) { terminal_mouse_hide(); mouse_present = 0; }
        return;
    }
    mouse_present = 1;
    int old_row = mouse_row, old_column = mouse_column;
    mouse_fraction_x += dx;
    mouse_fraction_y += dy;
    mouse_column += mouse_fraction_x / 8;
    mouse_fraction_x %= 8;
    mouse_row += mouse_fraction_y / 16;
    mouse_fraction_y %= 16;
    if (mouse_column < 0) mouse_column = 0;
    if (mouse_column >= COLUMNS) mouse_column = COLUMNS - 1;
    if (mouse_row < 0) mouse_row = 0;
    if (mouse_row >= ROWS) mouse_row = ROWS - 1;
    int moved = mouse_row != old_row || mouse_column != old_column;
    if (moved && mouse_drawn) {
        int r = mouse_row, c = mouse_column;
        mouse_row = old_row;
        mouse_column = old_column;
        terminal_mouse_hide();
        mouse_row = r;
        mouse_column = c;
    }
    mouse_show();
    mouse_wheel += wheel;
    unsigned changed = buttons ^ mouse_buttons;
    if (raw_mode && mouse_tracking) {
        static const unsigned code_of[3] = {0, 2, 1};      /* HID left, right, middle */
        for (int b = 0; b < 3; ++b)
            if (changed & (1u << b)) report_mouse(code_of[b], !(buttons & (1u << b)));
        if (moved && (mouse_tracking == 1003 || (mouse_tracking == 1002 && buttons))) {
            unsigned code = buttons & 1 ? 0 : buttons & 4 ? 1 : buttons & 2 ? 2 : 3;
            report_mouse(32 + code, 0);
        }
        for (; wheel > 0; --wheel) report_mouse(64, 0);
        for (; wheel < 0; ++wheel) report_mouse(65, 0);
    }
    mouse_buttons = buttons;
}

int terminal_mouse(struct dosxc_mouse *out) {
    if (current && current->vc) {          /* no text mouse in a window */
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }
    mouse_poll();
    if (out) {
        out->row = mouse_row;
        out->column = mouse_column;
        out->buttons = mouse_buttons;
        out->wheel = mouse_wheel;
    }
    mouse_wheel = 0;
    return mouse_present;
}

/* The next key from any keyboard, or -1. */
static int pushed_back[16];
static unsigned pushed_count;

static int source_key(void) {
    usb_tick();
    volumes_poll();
    power_poll();
    mouse_poll();
    net_tick();
    int key = usb_key_read();
    if (key >= 0) return key;
    /* The kernel's poll briefly unmasks the keyboard interrupt. */
    uptr flags;
#if __SIZEOF_POINTER__ == 8
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
#else
    __asm__ volatile("pushfl; pop %0; cli" : "=r"(flags));
#endif
    key = ctx->read_key();
    if (flags & 0x200) __asm__ volatile("sti");
    return key;
}

static int read_key(void) {
    if (current && current->vc) {
        struct vconsole *v = current->vc;
        if (v->key_read == v->key_write) return -1;
        int key = v->keys[v->key_read];
        v->key_read = (v->key_read + 1) % VC_KEYS;
        return key;
    }
    if (pushed_count) {
        int key = pushed_back[0];
        memmove(pushed_back, pushed_back + 1, --pushed_count * sizeof(int));
        return key;
    }
    return source_key();
}

/* Is Ctrl+C waiting?  Other keys stay queued for the program. */
int terminal_interrupted(void) {
    select_input();
    if (raw_mode) return 0;
    int key;
    if (current && current->vc) {
        while ((key = read_key()) >= 0)
            if (key == 3) return 1;
        return 0;
    }
    while ((key = source_key()) >= 0) {
        if (key == 3) return 1;
        if (pushed_count < sizeof(pushed_back) / sizeof(*pushed_back)) pushed_back[pushed_count++] = key;
    }
    return 0;
}

static void take_replies(void) {
    if (!reply_length) return;
    if (queue_head == queue_tail) queue_head = queue_tail = 0;
    for (unsigned i = 0; i < reply_length && queue_tail < sizeof(queue); ++i) queue[queue_tail++] = replies[i];
    reply_length = 0;
}

int terminal_poll(void) {
    select_input();
    take_replies();
    if (raw_mode) {
        if (queue_head != queue_tail) return 1;
        int key = read_key();
        if (key >= 0) queue_key(key);
        return queue_head != queue_tail;
    }
    if (line_position < line_length) return 1;
    return 0;
}

static void wait_a_moment(void) {
    if (current && current->vc) sched_sleep(2000000);
    else sched_yield();
}

static int wait_key(void) {
    int key;
    while ((key = read_key()) < 0) wait_a_moment();
    return key;
}

static void echo(const char *text) { terminal_write((const u8 *)text, rt_strlen(text)); }

dosxc_ssize terminal_read(u8 *buffer, uptr size) {
    select_input();
    if (!size) return 0;
    if (raw_mode) {
        take_replies();
        while (queue_head == queue_tail) queue_key(wait_key());
        uptr n = 0;
        while (n < size && queue_head != queue_tail) buffer[n++] = queue[queue_head++];
        return (dosxc_ssize)n;
    }
    if (line_position == line_length) {
        line_position = line_length = 0;
        for (;;) {
            int key = wait_key();
            if (key == '\n' || key == '\r') {
                echo("\r\n");
                line[line_length++] = '\n';
                break;
            }
            if (key == '\b') {
                if (line_length) {
                    --line_length;
                    echo("\b \b");
                }
                continue;
            }
            if ((key == 26 || key == 4) && !line_length) {   /* Ctrl+Z / Ctrl+D: end of input */
                echo("^Z\r\n");
                return 0;
            }
            if (key == 3) {                                  /* Ctrl+C */
                echo("^C\r\n");
                process_exit(DOSXC_SIGNALED(2));
            }
            if (key >= 32 && key < 256 && key != 127 && line_length < sizeof(line) - 1) {
                line[line_length++] = (char)key;
                char one[2] = {(char)key, 0};
                echo(one);
            }
        }
    }
    uptr n = line_length - line_position;
    if (n > size) n = size;
    memcpy(buffer, line + line_position, n);
    line_position += n;
    return (dosxc_ssize)n;
}

int terminal_mode(int mode) {
    select_input();
    int old = raw_mode;
    raw_mode = mode == DOSXC_CONSOLE_RAW;
    if (!raw_mode) queue_head = queue_tail = 0;
    return old ? DOSXC_CONSOLE_RAW : 0;
}

/* GETKEY: the next key as a keyboard code; queued raw bytes come first. */
int terminal_get_key(int wait) {
    select_input();
    if (queue_head != queue_tail) return queue[queue_head++];
    int key;
    while ((key = read_key()) < 0 && wait) wait_a_moment();
    return key;
}

void terminal_input_reset(void) {
    select_input();
    line_position = line_length = 0;
    raw_mode = 0;
    queue_head = queue_tail = 0;
}
