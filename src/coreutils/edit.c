/* DOS-XC coreutils - EDIT, a small full-screen text editor.
   Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"

#define ROWS 23                         /* text rows between the title and status bars */
static char *buffer;                    /* the text, '\n' line ends */
static size_t used, capacity, position, top;
static int dirty, save_status;
static const char *path;

static void console(int op, long a, long b) {
    fflush(stdout);
    dosxc_sys_console(op, a, b);
}

static size_t line_start(size_t at) {
    while (at && buffer[at - 1] != '\n') --at;
    return at;
}

static void render(void) {
    console(DOSXC_CONSOLE_COLOR, 0x17 | 0x100, 0);
    console(DOSXC_CONSOLE_CLEAR, 0, 0);
    console(DOSXC_CONSOLE_COLOR, 0x70, 0);
    char title[120], full[300];
    if (dosxc_sys_fullpath(path, full, sizeof(full)) < 0) snprintf(full, sizeof(full), "%s", path);
    snprintf(title, sizeof(title), " EDIT  %s%s", full, dirty ? " *" : "");
    printf("%-80.80s", title);
    console(DOSXC_CONSOLE_COLOR, 0x17, 0);
    unsigned row = 1, column = 0, caret_row = 1, caret_column = 0;
    size_t at = top;
    console(DOSXC_CONSOLE_CURSOR, 1, 0);
    while (at < used && row <= ROWS) {
        if (at == position) { caret_row = row; caret_column = column; }
        char c = buffer[at++];
        if (c == '\n') {
            ++row;
            column = 0;
            if (row <= ROWS) console(DOSXC_CONSOLE_CURSOR, row, 0);
            continue;
        }
        int width = c == '\t' ? 8 - (int)(column % 8) : 1;
        while (width--) {
            putchar(c == '\t' ? ' ' : c);
            if (++column == 80) {
                ++row;
                column = 0;
                if (row <= ROWS) console(DOSXC_CONSOLE_CURSOR, row, 0);
            }
        }
    }
    if (at == position && row <= ROWS) { caret_row = row; caret_column = column; }
    console(DOSXC_CONSOLE_CURSOR, 24, 0);
    console(DOSXC_CONSOLE_COLOR, 0x70, 0);
    const char *status = save_status < 0 ? " SAVE FAILED - changes are not saved.  F2=Retry  ESC=Exit"
                       : save_status > 0 ? " Saved.   F2/Ctrl+S=Save   ESC=Exit   Arrows/PgUp/PgDn=Move"
                                         : " F2/Ctrl+S=Save   ESC=Exit   Arrows/PgUp/PgDn=Move   Del/Bksp=Erase";
    printf("%-79.79s", status);
    console(DOSXC_CONSOLE_COLOR, 0x17, 0);
    console(DOSXC_CONSOLE_CURSOR, caret_row, caret_column);
}

static int save(void) {
    FILE *f = fopen(path, "wb");
    int failed = !f;
    for (size_t i = 0; f && i < used; ++i) {
        if (buffer[i] == '\n' && fputc('\r', f) == EOF) failed = 1;
        if (fputc(buffer[i], f) == EOF) failed = 1;
    }
    if (f && fclose(f)) failed = 1;
    save_status = failed ? -1 : 1;
    if (!failed) dirty = 0;
    return failed;
}

static int reserve(size_t more) {
    if (used + more <= capacity) return 0;
    size_t bigger = capacity ? capacity * 2 : 65536;
    while (bigger < used + more) bigger *= 2;
    char *p = realloc(buffer, bigger);
    if (!p) return -1;
    buffer = p;
    capacity = bigger;
    return 0;
}

static void move_lines(int count) {
    while (count < 0 && position) {                     /* up */
        size_t start = line_start(position), column = position - start;
        if (!start) { position = 0; break; }
        size_t previous = line_start(start - 1);
        position = previous + column < start - 1 ? previous + column : start - 1;
        ++count;
    }
    while (count > 0) {                                  /* down */
        size_t start = line_start(position), column = position - start, end = position;
        while (end < used && buffer[end] != '\n') ++end;
        if (end >= used) break;
        position = end + 1;
        while (column-- && position < used && buffer[position] != '\n') ++position;
        --count;
    }
}

int main(int argc, char **argv) {
    if (argc != 2 || !strcmp(argv[1], "/?")) { puts("EDIT file   F2 or Ctrl+S saves; ESC exits."); return argc != 2; }
    path = argv[1];
    if (is_dir(path)) { printf("%s is a folder.\n", path); return 1; }
    FILE *f = fopen(path, "rb");
    if (f) {
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (c == '\r') continue;
            if (reserve(1)) { puts("The file is too large to edit."); return 1; }
            buffer[used++] = (char)c;
        }
        fclose(f);
    }
    if (reserve(1)) return 1;
    int original = dosxc_sys_console(DOSXC_CONSOLE_GETCOLOR, 0, 0);
    for (;;) {
        render();
        int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
        save_status = 0;
        if (k == 27) {
            if (dirty) {
                console(DOSXC_CONSOLE_CURSOR, 24, 0);
                console(DOSXC_CONSOLE_COLOR, 0x4f, 0);
                printf("%-79s", " Save changes before leaving?  Y=Save  N=Discard  ESC=Keep editing");
                int answer;
                do answer = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
                while (answer != 'y' && answer != 'Y' && answer != 'n' && answer != 'N' && answer != 27);
                if (answer == 27) continue;
                if ((answer == 'y' || answer == 'Y') && save()) continue;
            }
            break;
        }
        if (k == 0x10c || k == 19) { save(); continue; }
        if (k == 0x104) { if (position) --position; }
        else if (k == 0x103) { if (position < used) ++position; }
        else if (k == 0x105) position = line_start(position);
        else if (k == 0x106) { while (position < used && buffer[position] != '\n') ++position; }
        else if (k == 0x101) move_lines(-1);
        else if (k == 0x102) move_lines(1);
        else if (k == 0x109) move_lines(-ROWS);
        else if (k == 0x10a) move_lines(ROWS);
        else if (k == '\b') {
            if (position) { memmove(buffer + position - 1, buffer + position, used - position); --position; --used; dirty = 1; }
        } else if (k == 0x108) {
            if (position < used) { memmove(buffer + position, buffer + position + 1, used - position - 1); --used; dirty = 1; }
        } else if ((k >= 32 && k < 256 && k != 127) || k == '\n' || k == '\t') {
            if (reserve(1)) continue;
            memmove(buffer + position + 1, buffer + position, used - position);
            buffer[position++] = (char)k;
            ++used;
            dirty = 1;
        }
        if (position < top) top = line_start(position);
        for (;;) {                                          /* keep the caret on screen */
            unsigned rows = 0, column = 0;
            for (size_t i = top; i < position; ++i) {
                if (buffer[i] == '\n') { ++rows; column = 0; }
                else if (++column == 80) { ++rows; column = 0; }
            }
            if (rows < ROWS) break;
            while (top < used && buffer[top++] != '\n') {}
        }
    }
    console(DOSXC_CONSOLE_COLOR, (original & 0xff) | 0x100, 0);
    console(DOSXC_CONSOLE_CLEAR, 0, 0);
    return 0;
}
