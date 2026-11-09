/*
 * DOS-XC /vdm/dos.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The DOS services of the virtual machine, version 5.0 as a program would
 * see it: memory blocks, program loading (MZ and COM, EXEC included) and
 * files, which are the real files of DOS-XC reached through the C library.
 */
#include "vdm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIRST_MCB 0x0100
#define TOP_SEGMENT 0xa000
#define MAX_FILES 64
#define MAX_EXEC 8
#define IO_CHUNK 65536

static u16 psp, dta_segment, dta_offset;
static u8 last_exit_code, last_exit_kind;
static u8 io_buffer[IO_CHUNK];

/* ---- memory blocks -------------------------------------------------------- */

static int mcb_tag(u16 m) { return mem_read8(linear(m, 0)); }
static u16 mcb_owner(u16 m) { return mem_read16(linear(m, 1)); }
static u16 mcb_size(u16 m) { return mem_read16(linear(m, 3)); }
static void mcb_write(u16 m, int tag, u16 owner, u16 size) {
    mem_write8(linear(m, 0), (u8)tag);
    mem_write16(linear(m, 1), owner);
    mem_write16(linear(m, 3), size);
}
static u16 mcb_next(u16 m) { return (u16)(m + 1 + mcb_size(m)); }

static void mcb_merge(void) {
    u16 m = FIRST_MCB;
    while (mcb_tag(m) == 'M') {
        u16 n = mcb_next(m);
        if (!mcb_owner(m) && !mcb_owner(n)) {
            mcb_write(m, mcb_tag(n), 0, (u16)(mcb_size(m) + 1 + mcb_size(n)));
            continue;
        }
        m = n;
    }
}

static u16 mcb_largest(void) {
    u16 best = 0, m = FIRST_MCB;
    for (;;) {
        if (!mcb_owner(m) && mcb_size(m) > best) best = mcb_size(m);
        if (mcb_tag(m) == 'Z') return best;
        m = mcb_next(m);
    }
}

/* The data segment of a new block, or 0. */
static u16 mcb_alloc(u16 paragraphs, u16 owner) {
    mcb_merge();
    u16 m = FIRST_MCB;
    for (;;) {
        u16 size = mcb_size(m);
        if (!mcb_owner(m) && size >= paragraphs) {
            int tag = mcb_tag(m);
            if (size > paragraphs) {
                mcb_write((u16)(m + 1 + paragraphs), tag, 0, (u16)(size - paragraphs - 1));
                mcb_write(m, 'M', owner, paragraphs);
            } else mcb_write(m, tag, owner, size);
            return (u16)(m + 1);
        }
        if (mcb_tag(m) == 'Z') return 0;
        m = mcb_next(m);
    }
}

static int mcb_valid(u16 segment) {
    u16 m = FIRST_MCB, want = (u16)(segment - 1);
    for (;;) {
        if (m == want) return 1;
        if (mcb_tag(m) == 'Z') return 0;
        m = mcb_next(m);
    }
}

static int mcb_resize(u16 segment, u16 paragraphs) {
    if (!mcb_valid(segment)) return -7;
    u16 m = (u16)(segment - 1), size = mcb_size(m);
    if (paragraphs > size) {
        mcb_merge();
        size = mcb_size(m);
        if (mcb_tag(m) == 'M') {
            u16 n = mcb_next(m);
            if (!mcb_owner(n)) {
                mcb_write(m, mcb_tag(n), mcb_owner(m), (u16)(size + 1 + mcb_size(n)));
                size = mcb_size(m);
            }
        }
        if (paragraphs > size) {
            DX = 0;
            return size;
        }
    }
    if (paragraphs < size) {
        int tag = mcb_tag(m);
        mcb_write((u16)(segment + paragraphs), tag, 0, (u16)(size - paragraphs - 1));
        mcb_write(m, 'M', mcb_owner(m), paragraphs);
    }
    return -1;
}

static void mcb_free_owned(u16 owner) {
    u16 m = FIRST_MCB;
    for (;;) {
        if (mcb_owner(m) == owner) mem_write16(linear(m, 1), 0);
        if (mcb_tag(m) == 'Z') break;
        m = mcb_next(m);
    }
    mcb_merge();
}

/* ---- paths ---------------------------------------------------------------- */

static void read_string(u16 segment, u16 offset, char *out, size_t size) {
    size_t n = 0;
    for (; n + 1 < size; ++n) {
        char c = (char)mem_read8(linear(segment, (u16)(offset + n)));
        if (!c) break;
        out[n] = c;
    }
    out[n] = 0;
}

static void write_string(u16 segment, u16 offset, const char *text) {
    for (size_t i = 0;; ++i) {
        mem_write8(linear(segment, (u16)(offset + i)), (u8)text[i]);
        if (!text[i]) break;
    }
}

static int dos_error(void) {
    switch (errno) {
    case ENOENT: return 2;
    case ENOTDIR: return 3;
    case EMFILE: case ENFILE: return 4;
    case EACCES: case EPERM: case EEXIST: case EROFS: case EISDIR: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case ENOTEMPTY: return 5;
    default: return 5;
    }
}

/* ---- files ---------------------------------------------------------------- */

struct file { int used, fd, console, append; u16 owner; };
static struct file files[MAX_FILES];

static int file_alloc(void) {
    for (int i = 5; i < MAX_FILES; ++i)
        if (!files[i].used) return i;
    return -1;
}

static void files_init(void) {
    memset(files, 0, sizeof(files));
    for (int i = 0; i < 3; ++i) {
        files[i].used = 1;
        files[i].fd = i;
        files[i].console = isatty(i);
    }
    files[3].used = 1; files[3].fd = -1;      /* AUX */
    files[4].used = 1; files[4].fd = -1;      /* PRN */
}

static struct file *file_of(int handle) {
    if (handle < 0 || handle >= MAX_FILES || !files[handle].used) return 0;
    return &files[handle];
}

static void file_close(int handle) {
    if (files[handle].used && handle >= 5 && files[handle].fd >= 0) close(files[handle].fd);
    files[handle].used = 0;
}

static void close_owned(u16 owner) {
    for (int i = 5; i < MAX_FILES; ++i)
        if (files[i].used && files[i].owner == owner) file_close(i);
}

static int console_device(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
    return !strcasecmp(name, "CON") || !strcasecmp(name, "NUL") || !strcasecmp(name, "PRN") || !strcasecmp(name, "AUX");
}

/* The return value of a DOS call: AX, carry set for an error. */
static void succeed(void) { bios_set_return_flag(F_CF, 0); }
static void fail(int code) { AX = (u16)code; bios_set_return_flag(F_CF, 1); }

static void open_file(const char *dos_path, int flags, int create) {
    char path[300];
    if (console_device(dos_path)) {
        int h = file_alloc();
        if (h < 0) { fail(4); return; }
        memset(&files[h], 0, sizeof(files[h]));
        files[h].used = 1;
        files[h].owner = psp;
        const char *name = strrchr(dos_path, '\\');
        name = name ? name + 1 : dos_path;
        files[h].console = !strcasecmp(name, "CON");
        files[h].fd = -1;
        AX = (u16)h;
        succeed();
        return;
    }
    host_native_path(dos_path, path, sizeof(path));
    int fd = open(path, flags | (create ? O_CREAT : 0), 0666);
    if (fd < 0) { fail(dos_error()); return; }
    int h = file_alloc();
    if (h < 0) { close(fd); fail(4); return; }
    memset(&files[h], 0, sizeof(files[h]));
    files[h].used = 1;
    files[h].fd = fd;
    files[h].owner = psp;
    AX = (u16)h;
    succeed();
}

static u32 far_ds_dx(u32 offset) { return linear(cpu.seg[DS], (u16)(DX + offset)); }

static void console_write(u32 address, int count) {
    for (int i = 0; i < count; ++i) video_put_char(mem_read8(address + (u32)i));
}

static void dos_write(void) {
    struct file *f = file_of(BX);
    if (!f) { fail(6); return; }
    if (f->console) { console_write(far_ds_dx(0), CX); AX = CX; succeed(); return; }
    if (f->fd < 0) { AX = CX; succeed(); return; }
    if (!CX) {                                       /* a zero-length write truncates */
        off_t at = lseek(f->fd, 0, SEEK_CUR);
        if (at >= 0 && ftruncate(f->fd, at) < 0) { fail(dos_error()); return; }
        AX = 0;
        succeed();
        return;
    }
    for (int i = 0; i < CX; ++i) io_buffer[i] = mem_read8(far_ds_dx((u32)i));
    ssize_t n = write(f->fd, io_buffer, CX);
    if (n < 0) { fail(dos_error()); return; }
    AX = (u16)n;
    succeed();
}

/* Line input for AH=0Ah and reads from the console: one key per call, the
   call repeating while it waits. */
static char line[256];
static int line_length;

static int line_input(int capacity, int *finished) {
    *finished = 0;
    while (bios_key_available()) {
        u16 key = bios_key_read();
        int ch = key & 0xff;
        if (ch == '\r') {
            video_put_char('\r');
            video_put_char('\n');
            *finished = 1;
            return 1;
        }
        if (ch == 8 || key == 0x4b00) {
            if (line_length) {
                --line_length;
                video_put_char(8);
                video_put_char(' ');
                video_put_char(8);
            }
            continue;
        }
        if (ch == 27) {
            while (line_length) { --line_length; video_put_char(8); video_put_char(' '); video_put_char(8); }
            continue;
        }
        if (ch >= 32 && line_length + 1 < capacity) {
            line[line_length++] = (char)ch;
            video_put_char(ch);
        }
    }
    bios_wait();
    return 0;
}

static int cooked_pending, cooked_used;
static char cooked[256];

static void dos_read(void) {
    struct file *f = file_of(BX);
    if (!f) { fail(6); return; }
    if (f->console) {
        if (cooked_used >= cooked_pending) {
            int finished;
            if (!line_input(254, &finished)) return;
            memcpy(cooked, line, (size_t)line_length);
            cooked[line_length] = '\r';
            cooked[line_length + 1] = '\n';
            cooked_pending = line_length + 2;
            cooked_used = 0;
            line_length = 0;
        }
        int n = cooked_pending - cooked_used;
        if (n > CX) n = CX;
        for (int i = 0; i < n; ++i) mem_write8(far_ds_dx((u32)i), (u8)cooked[cooked_used + i]);
        cooked_used += n;
        AX = (u16)n;
        succeed();
        return;
    }
    if (f->fd < 0) { AX = 0; succeed(); return; }
    ssize_t n = read(f->fd, io_buffer, CX);
    if (n < 0) { fail(dos_error()); return; }
    for (ssize_t i = 0; i < n; ++i) mem_write8(far_ds_dx((u32)i), io_buffer[i]);
    AX = (u16)n;
    succeed();
}

static void dos_seek(void) {
    struct file *f = file_of(BX);
    if (!f) { fail(6); return; }
    if (f->fd < 0 || f->console) { AX = 0; DX = 0; succeed(); return; }
    off_t offset = (off_t)(i32)((u32)CX << 16 | DX);
    off_t at = lseek(f->fd, offset, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END);
    if (at < 0) { fail(25); return; }
    AX = (u16)at;
    DX = (u16)((u32)at >> 16);
    succeed();
}

static void dos_ioctl(void) {
    struct file *f = file_of(BX);
    switch (AL) {
    case 0x00:
        if (!f) { fail(6); return; }
        DX = f->console ? 0x80d3 : f->fd < 0 ? 0x80c4 : 0x0002;
        AX = DX;
        succeed();
        break;
    case 0x01: succeed(); break;
    case 0x06:
        if (!f) { fail(6); return; }
        if (f->console) AL = bios_key_available() ? 0xff : 0;
        else {
            off_t now = lseek(f->fd, 0, SEEK_CUR), end = lseek(f->fd, 0, SEEK_END);
            lseek(f->fd, now, SEEK_SET);
            AL = now < end ? 0xff : 0;
        }
        succeed();
        break;
    case 0x07: AL = 0xff; succeed(); break;
    case 0x08: AX = 1; succeed(); break;
    case 0x09: DX = 0; succeed(); break;
    case 0x0e: AL = 0; succeed(); break;
    default: fail(1); break;
    }
}

static int dos_date_time(i64 t, u16 *date, u16 *time) {
    i64 days = t / 86400;
    int seconds = (int)(t % 86400);
    i64 z = days + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)((i64)yoe + era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    int year = y < 1980 ? 0 : y - 1980;
    *date = (u16)(year << 9 | m << 5 | d);
    *time = (u16)((seconds / 3600) << 11 | (seconds / 60 % 60) << 5 | (seconds % 60) / 2);
    return 0;
}

/* ---- finding files ---------------------------------------------------------- */

static char search_paths[8][260];
static int next_search;

static void name_83(const char *name, char *out) {
    char base[32] = "", ext[8] = "";
    const char *dot = strrchr(name, '.');
    if (!strcmp(name, ".") || !strcmp(name, "..")) { strcpy(out, name); return; }
    size_t blen = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    size_t n = 0;
    for (size_t i = 0; i < blen && n < 8; ++i)
        if (name[i] != ' ') base[n++] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    base[n] = 0;
    if (blen > 8) { base[6] = '~'; base[7] = '1'; base[8] = 0; }
    n = 0;
    if (dot && dot != name)
        for (const char *p = dot + 1; *p && n < 3; ++p) ext[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    ext[n] = 0;
    snprintf(out, 14, "%s%s%s", base, *ext ? "." : "", ext);
}

static void pattern_83(const char *text, char pattern[12]) {
    memset(pattern, ' ', 11);
    pattern[11] = 0;
    int i = 0;
    const char *p = text;
    while (*p && *p != '.' && i < 8) {
        if (*p == '*') { memset(pattern + i, '?', (size_t)(8 - i)); i = 8; while (*p && *p != '.') ++p; break; }
        pattern[i++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        ++p;
    }
    while (*p && *p != '.') ++p;
    if (*p == '.') ++p;
    i = 8;
    while (*p && i < 11) {
        if (*p == '*') { memset(pattern + i, '?', (size_t)(11 - i)); break; }
        pattern[i++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        ++p;
    }
}

static int pattern_matches(const char pattern[12], const char *name83) {
    char fields[12];
    memset(fields, ' ', 11);
    fields[11] = 0;
    const char *p = name83;
    int i = 0;
    if (*p == '.') { fields[0] = '.'; if (p[1] == '.') fields[1] = '.'; }
    else {
        while (*p && *p != '.' && i < 8) fields[i++] = *p++;
        if (*p == '.') { ++p; i = 8; while (*p && i < 11) fields[i++] = *p++; }
    }
    for (i = 0; i < 11; ++i)
        if (pattern[i] != '?' && pattern[i] != fields[i]) return 0;
    return 1;
}

/* Reserved DTA bytes: 0 attribute mask, 1..11 pattern, 12 directory slot, 13..14 next entry. */
static int find_next(u32 dta) {
    u8 mask = mem_read8(dta);
    char pattern[12];
    for (int i = 0; i < 11; ++i) pattern[i] = (char)mem_read8(dta + 1 + (u32)i);
    pattern[11] = 0;
    int slot = mem_read8(dta + 12) & 7;
    int start = mem_read16(dta + 13);
    struct vdm_dirent *entries = malloc(sizeof(*entries) * 512);
    if (!entries) return 8;
    int count = host_listdir(search_paths[slot], entries, 512);
    for (int i = start; i < count; ++i) {
        char short_name[16];
        name_83(entries[i].name, short_name);
        int directory = (entries[i].attributes & 0x10) != 0;
        if ((entries[i].attributes & 0x08) && !(mask & 0x08)) continue;
        if (!pattern_matches(pattern, short_name)) continue;
        if ((entries[i].attributes & ~0x21u & 0x16) & ~(unsigned)mask & 0x16) continue;
        if (directory && !(mask & 0x10)) continue;
        u16 date, time;
        dos_date_time(entries[i].mtime, &date, &time);
        mem_write8(dta + 0x15, (u8)entries[i].attributes);
        mem_write16(dta + 0x16, time);
        mem_write16(dta + 0x18, date);
        mem_write16(dta + 0x1a, (u16)entries[i].size);
        mem_write16(dta + 0x1c, (u16)(entries[i].size >> 16));
        for (int k = 0; k < 13; ++k) mem_write8(dta + 0x1e + (u32)k, 0);
        for (int k = 0; short_name[k]; ++k) mem_write8(dta + 0x1e + (u32)k, (u8)short_name[k]);
        mem_write16(dta + 13, (u16)(i + 1));
        free(entries);
        return 0;
    }
    free(entries);
    return 18;
}

static void dos_find_first(void) {
    char spec[300], folder[300], path[300];
    read_string(cpu.seg[DS], DX, spec, sizeof(spec));
    const char *name = spec;
    for (const char *p = spec; *p; ++p)
        if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
    size_t length = (size_t)(name - spec);
    memcpy(folder, spec, length);
    folder[length] = 0;
    if (!*name) name = "*.*";
    if (length && folder[length - 1] != ':') {
        if (length == 1 || folder[length - 2] != ':') folder[length - 1] = 0;
    } else if (length) { folder[length] = '.'; folder[length + 1] = 0; }
    if (!*folder) strcpy(folder, ".");
    host_native_path(folder, path, sizeof(path));
    u32 dta = linear(dta_segment, dta_offset);
    int slot = next_search;
    next_search = (next_search + 1) & 7;
    snprintf(search_paths[slot], sizeof(search_paths[slot]), "%s", path);
    mem_write8(dta, CL);
    char pattern[12];
    pattern_83(name, pattern);
    for (int i = 0; i < 11; ++i) mem_write8(dta + 1 + (u32)i, (u8)pattern[i]);
    mem_write8(dta + 12, (u8)slot);
    mem_write16(dta + 13, 0);
    int r = find_next(dta);
    if (r) fail(r == 18 ? 18 : r); else succeed();
}

static void dos_find_next(void) {
    int r = find_next(linear(dta_segment, dta_offset));
    if (r) fail(18); else succeed();
}

/* ---- programs --------------------------------------------------------------- */

struct exec_frame { struct cpu saved; u16 psp, dta_segment, dta_offset; };
static struct exec_frame exec_stack[MAX_EXEC];
static int exec_depth;

static u16 make_environment(u16 parent_env, const char *program) {
    static char text[8192];
    size_t used = 0;
    if (parent_env) {
        u32 at = linear(parent_env, 0);
        while (used + 2 < sizeof(text) && mem_read8(at + used)) {
            while (used + 1 < sizeof(text) && mem_read8(at + used)) { text[used] = (char)mem_read8(at + used); ++used; }
            text[used++] = 0;
            if (used + 1 < sizeof(text) && !mem_read8(at + used)) break;
        }
    } else {
        for (const char *p = host_environment(); *p && used + strlen(p) + 2 < 2048; p += strlen(p) + 1) {
            strcpy(text + used, p);
            used += strlen(p) + 1;
        }
    }
    if (!used) text[used++] = 0;
    text[used++] = 0;
    text[used++] = 1;
    text[used++] = 0;
    size_t name = strlen(program) + 1;
    memcpy(text + used, program, name);
    used += name;
    u16 paragraphs = (u16)((used + 15) / 16);
    u16 segment = mcb_alloc(paragraphs, 0xffff);
    if (!segment) return 0;
    for (size_t i = 0; i < used; ++i) mem_write8(linear(segment, (u16)i), (u8)text[i]);
    return segment;
}

static void setup_psp(u16 segment, u16 top, u16 parent, u16 env, const char *tail) {
    u32 at = linear(segment, 0);
    for (int i = 0; i < 256; ++i) mem_write8(at + (u32)i, 0);
    mem_write16(at, 0x20cd);
    mem_write16(at + 2, top);
    mem_write8(at + 5, 0xc3);
    mem_write16(at + 6, 0xfff0);
    for (int v = 0; v < 3; ++v) {
        mem_write16(at + 0x0a + (u32)v * 4, mem_read16((0x22u + (u32)v) * 4));
        mem_write16(at + 0x0c + (u32)v * 4, mem_read16((0x22u + (u32)v) * 4 + 2));
    }
    mem_write16(at + 0x16, parent);
    for (int i = 0; i < 20; ++i) mem_write8(at + 0x18 + (u32)i, i < 5 ? (u8)i : 0xff);
    mem_write16(at + 0x2c, env);
    mem_write16(at + 0x32, 20);
    mem_write16(at + 0x34, 0x18);
    mem_write16(at + 0x36, segment);
    mem_write16(at + 0x50, 0x21cd);
    mem_write8(at + 0x52, 0xcb);
    for (int i = 0; i < 11; ++i) { mem_write8(at + 0x5d + (u32)i, ' '); mem_write8(at + 0x6d + (u32)i, ' '); }
    int n = (int)strlen(tail);
    if (n > 126) n = 126;
    mem_write8(at + 0x80, (u8)n);
    for (int i = 0; i < n; ++i) mem_write8(at + 0x81 + (u32)i, (u8)tail[i]);
    mem_write8(at + 0x81 + (u32)n, 13);
}

/* Load PATH into memory.  Fills the entry point; returns 0 or a DOS error.
   With OVERLAY set, loads the image at a segment (EXEC function 3). */
struct loaded { u16 cs, ip, ss, sp, psp; };

static int read_all(const char *dos_path, u8 **data, size_t *size) {
    char path[300];
    host_native_path(dos_path, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return dos_error();
    off_t length = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (length < 0 || length > 0x100000) { close(fd); return 11; }
    u8 *buffer = malloc(length ? (size_t)length : 1);
    if (!buffer) { close(fd); return 8; }
    ssize_t got = read(fd, buffer, (size_t)length);
    close(fd);
    if (got != length) { free(buffer); return 11; }
    *data = buffer;
    *size = (size_t)length;
    return 0;
}

static u16 peek16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

static int load_overlay(const u8 *data, size_t size, u16 segment, u16 relocation) {
    if (size >= 0x1c && peek16(data) == 0x5a4d) {
        size_t header = (size_t)peek16(data + 8) * 16;
        size_t image = (size_t)peek16(data + 4) * 512;
        if (peek16(data + 2)) image -= 512 - peek16(data + 2);
        if (image > size) image = size;
        if (header > image) return 11;
        for (size_t i = header; i < image; ++i) mem_write8(linear(segment, 0) + (u32)(i - header), data[i]);
        size_t table = peek16(data + 0x18);
        for (unsigned i = 0; i < peek16(data + 6); ++i) {
            if (table + i * 4 + 4 > size) return 11;
            u16 offset = peek16(data + table + i * 4), base = peek16(data + table + i * 4 + 2);
            u32 at = linear((u16)(segment + base), offset);
            mem_write16(at, (u16)(mem_read16(at) + relocation));
        }
    } else {
        for (size_t i = 0; i < size; ++i) mem_write8(linear(segment, 0) + (u32)i, data[i]);
    }
    return 0;
}

static int load_program(const char *dos_path, u16 env_from, const char *tail, struct loaded *out) {
    u8 *data;
    size_t size;
    int r = read_all(dos_path, &data, &size);
    if (r) return r;
    char full[300];
    if (host_fullpath(dos_path, full, sizeof(full)) < 0) snprintf(full, sizeof(full), "%s", dos_path);
    u16 env = make_environment(env_from, full);
    if (!env) { free(data); return 8; }
    u16 paragraphs_free = mcb_largest();
    u16 available;
    int is_exe = size >= 0x1c && peek16(data) == 0x5a4d;
    u32 image_size;
    u32 min_para = 0, max_para = 0xffff;
    if (is_exe) {
        size_t header = (size_t)peek16(data + 8) * 16;
        size_t image = (size_t)peek16(data + 4) * 512;
        if (peek16(data + 2)) image -= 512 - peek16(data + 2);
        if (image > size) image = size;
        if (header > image) { free(data); return 11; }
        image_size = (u32)(image - header);
        min_para = peek16(data + 0x0a);
        max_para = peek16(data + 0x0c);
    } else image_size = (u32)size;
    u32 needed = (image_size + 15) / 16 + 16 + min_para;
    if (!is_exe) needed = 0x1000 < needed ? needed : 0x1000;   /* a COM program expects a whole segment */
    if (needed > paragraphs_free) { mem_write16(linear((u16)(env - 1), 1), 0); mcb_merge(); free(data); return 8; }
    u64 wanted = (u64)((image_size + 15) / 16) + 16 + max_para;
    if (wanted > paragraphs_free || !is_exe) wanted = paragraphs_free;
    available = (u16)wanted;
    u16 block = mcb_alloc(available, 0);
    if (!block) { free(data); return 8; }
    mem_write16(linear((u16)(block - 1), 1), block);
    mem_write16(linear((u16)(env - 1), 1), block);
    setup_psp(block, (u16)(block + available), psp, env, tail);
    out->psp = block;
    u16 image_seg = (u16)(block + 16);
    if (is_exe) {
        size_t header = (size_t)peek16(data + 8) * 16;
        r = load_overlay(data, size, image_seg, image_seg);
        if (r) { mcb_free_owned(block); free(data); return r; }
        (void)header;
        out->cs = (u16)(peek16(data + 0x16) + image_seg);
        out->ip = peek16(data + 0x14);
        out->ss = (u16)(peek16(data + 0x0e) + image_seg);
        out->sp = peek16(data + 0x10);
    } else {
        for (size_t i = 0; i < size; ++i) mem_write8(linear(block, (u16)(0x100 + i)), data[i]);
        out->cs = block;
        out->ip = 0x100;
        out->ss = block;
        out->sp = 0xfffe;
        mem_write16(linear(block, 0xfffe), 0);
    }
    free(data);
    return 0;
}

static void start_loaded(const struct loaded *l) {
    psp = l->psp;
    dta_segment = psp;
    dta_offset = 0x80;
    cpu_set_seg(CS, l->cs);
    cpu.ip = l->ip;
    cpu_set_seg(SS, l->ss);
    SP = l->sp;
    cpu_set_seg(DS, psp);
    cpu_set_seg(ES, psp);
    cpu_set_seg(FS, 0);
    cpu_set_seg(GS, 0);
    for (int i = 0; i < 8; ++i)
        if (i != ESP) cpu.r[i].d = 0;
    AX = 0;
    cpu.flags = 2 | F_IF;
}

int dos_start(const char *program, const char *tail) {
    mcb_write(FIRST_MCB, 'Z', 0, (u16)(TOP_SEGMENT - FIRST_MCB - 1));
    files_init();
    psp = 0;
    struct loaded l;
    int r = load_program(program, 0, tail, &l);
    if (r) return r;
    start_loaded(&l);
    /* The exit vectors point back into our stub so a return ends the machine. */
    return 0;
}

void dos_flush(void) { }

void dos_terminate(int code) {
    u16 old = psp;
    u16 parent = mem_read16(linear(psp, 0x16));
    close_owned(old);
    u16 env = mem_read16(linear(old, 0x2c));
    if (env) mem_write16(linear((u16)(env - 1), 1), 0);
    mcb_free_owned(old);
    last_exit_code = (u8)code;
    if (!exec_depth) {
        vdm_exit_code = code;
        vdm_finished = 1;
        (void)parent;
        return;
    }
    --exec_depth;
    for (int v = 0; v < 3; ++v) {
        mem_write16((0x22u + (u32)v) * 4, mem_read16(linear(old, (u16)(0x0a + v * 4))));
        mem_write16((0x22u + (u32)v) * 4 + 2, mem_read16(linear(old, (u16)(0x0c + v * 4))));
    }
    struct exec_frame *f = &exec_stack[exec_depth];
    cpu = f->saved;
    psp = f->psp;
    dta_segment = f->dta_segment;
    dta_offset = f->dta_offset;
    AX = 0;
    bios_set_return_flag(F_CF, 0);
}

static void dos_exec(void) {
    char program[300], tail[128] = "";
    read_string(cpu.seg[DS], DX, program, sizeof(program));
    u32 block = linear(cpu.seg[ES], BX);
    if (AL == 3) {                                 /* load an overlay */
        u8 *data;
        size_t size;
        int r = read_all(program, &data, &size);
        if (r) { fail(r); return; }
        r = load_overlay(data, size, mem_read16(block), mem_read16(block + 2));
        free(data);
        if (r) fail(r); else succeed();
        return;
    }
    if (AL != 0) { fail(1); return; }
    if (exec_depth == MAX_EXEC) { fail(4); return; }
    u16 env = mem_read16(block);
    u16 tail_off = mem_read16(block + 2), tail_seg = mem_read16(block + 4);
    int n = mem_read8(linear(tail_seg, tail_off));
    if (n > 126) n = 126;
    for (int i = 0; i < n; ++i) tail[i] = (char)mem_read8(linear(tail_seg, (u16)(tail_off + 1 + i)));
    tail[n] = 0;
    struct loaded l;
    int r = load_program(program, env ? env : mem_read16(linear(psp, 0x2c)), tail, &l);
    if (r) { fail(r); return; }
    struct exec_frame *f = &exec_stack[exec_depth++];
    f->saved = cpu;
    f->psp = psp;
    f->dta_segment = dta_segment;
    f->dta_offset = dta_offset;
    mem_write16(linear(l.psp, 0x2e), SP);
    mem_write16(linear(l.psp, 0x30), cpu.seg[SS]);
    mem_write16(linear(l.psp, 0x0a), (u16)cpu.ip);
    mem_write16(linear(l.psp, 0x0c), cpu.seg[CS]);
    last_exit_kind = 0;
    start_loaded(&l);
}

/* ---- INT 21h ---------------------------------------------------------------- */

static int ext_pending = -1;

/* One keyboard character; an extended key returns 0 then its scan code. */
static int read_char(int echo) {
    if (ext_pending >= 0) { int c = ext_pending; ext_pending = -1; return c; }
    if (!bios_key_available()) { bios_wait(); return -1; }
    u16 key = bios_key_read();
    int ch = key & 0xff;
    if (!ch) ext_pending = key >> 8;
    if (echo && ch) video_put_char(ch);
    return ch;
}

void dos_int21(void) {
    char path[300], other[300];
    switch (AH) {
    case 0x00: dos_terminate(0); return;
    case 0x01: { int c = read_char(1); if (c >= 0) AL = (u8)c; return; }
    case 0x02: video_put_char(DL); AL = DL; return;
    case 0x06:
        if (DL != 0xff) { video_put_char(DL); AL = DL; return; }
        if (ext_pending >= 0) { AL = (u8)ext_pending; ext_pending = -1; bios_set_return_flag(F_ZF, 0); return; }
        if (!bios_key_available()) { AL = 0; bios_set_return_flag(F_ZF, 1); cpu.flags |= F_IF; cpu.idle = 1; return; }
        { u16 key = bios_key_read(); AL = key & 0xff; if (!AL) ext_pending = key >> 8; bios_set_return_flag(F_ZF, 0); }
        return;
    case 0x07: case 0x08: { int c = read_char(0); if (c >= 0) AL = (u8)c; return; }
    case 0x09: {
        u32 at = linear(cpu.seg[DS], DX);
        for (u32 i = 0; i < 65535; ++i) {
            u8 c = mem_read8(at + i);
            if (c == '$') break;
            video_put_char(c);
        }
        AL = '$';
        return;
    }
    case 0x0a: {
        u32 at = linear(cpu.seg[DS], DX);
        int capacity = mem_read8(at), finished;
        if (!line_input(capacity, &finished)) return;
        mem_write8(at + 1, (u8)line_length);
        for (int i = 0; i < line_length; ++i) mem_write8(at + 2 + (u32)i, (u8)line[i]);
        mem_write8(at + 2 + (u32)line_length, 13);
        line_length = 0;
        return;
    }
    case 0x0b: AL = bios_key_available() ? 0xff : 0; cpu.flags |= F_IF; cpu.idle = 1; return;
    case 0x0c: {
        while (bios_key_available()) bios_key_read();
        u8 fn = AL;
        AH = fn;
        if (fn == 1 || fn == 6 || fn == 7 || fn == 8 || fn == 10) dos_int21();
        return;
    }
    case 0x0d: return;
    case 0x0e: {
        char drive[4] = {(char)('A' + DL), ':', '\\', 0};
        if (host_fullpath(".", path, sizeof(path)) >= 0 && (DL + 'A') != path[0]) chdir(drive);
        AL = 26;
        return;
    }
    case 0x19: host_fullpath(".", path, sizeof(path)); AL = (u8)((path[0] & 0xdf) - 'A'); return;
    case 0x1a: dta_segment = cpu.seg[DS]; dta_offset = DX; return;
    case 0x25: mem_write16((u32)AL * 4, DX); mem_write16((u32)AL * 4 + 2, cpu.seg[DS]); return;
    case 0x2a: case 0x2c: {
        i64 now = host_time();
        u16 date, time;
        dos_date_time(now, &date, &time);
        if (AH == 0x2a) {
            CX = (u16)((date >> 9) + 1980);
            DH = (u8)(date >> 5 & 15);
            DL = (u8)(date & 31);
            AL = (u8)((now / 86400 + 4) % 7);
        } else {
            int s = (int)(now % 86400);
            CH = (u8)(s / 3600);
            CL = (u8)(s / 60 % 60);
            DH = (u8)(s % 60);
            DL = 0;
        }
        return;
    }
    case 0x2b: case 0x2d: AL = 0; return;
    case 0x2e: return;
    case 0x2f: cpu_set_seg(ES, dta_segment); BX = dta_offset; return;
    case 0x30: AX = 0x0005; BX = 0xff00; CX = 0; return;
    case 0x31: dos_terminate(AL); return;
    case 0x33: if (AL == 0) DL = 0; return;
    case 0x35: BX = mem_read16((u32)AL * 4); cpu_set_seg(ES, mem_read16((u32)AL * 4 + 2)); return;
    case 0x36: {
        unsigned long long space = host_disk_free(DL);
        if (!space) { AX = 0xffff; return; }
        AX = 16; BX = (u16)(space / 16 / 512 > 0xffff ? 0xffff : space / 16 / 512);
        CX = 512; DX = 0xffff;
        return;
    }
    case 0x37: if (AL == 0) { DL = '/'; AL = 0; } return;
    case 0x38: {
        u32 at = linear(cpu.seg[DS], DX);
        for (int i = 0; i < 34; ++i) mem_write8(at + (u32)i, 0);
        mem_write8(at + 2, '$');
        mem_write8(at + 7, '.');
        mem_write8(at + 9, ',');
        mem_write8(at + 11, '/');
        mem_write8(at + 13, ':');
        BX = 1;
        succeed();
        return;
    }
    case 0x39: read_string(cpu.seg[DS], DX, path, sizeof(path)); host_native_path(path, other, sizeof(other)); if (mkdir(other, 0777)) fail(dos_error()); else succeed(); return;
    case 0x3a: read_string(cpu.seg[DS], DX, path, sizeof(path)); host_native_path(path, other, sizeof(other)); if (rmdir(other)) fail(dos_error()); else succeed(); return;
    case 0x3b: read_string(cpu.seg[DS], DX, path, sizeof(path)); host_native_path(path, other, sizeof(other)); if (chdir(other)) fail(3); else succeed(); return;
    case 0x3c: read_string(cpu.seg[DS], DX, path, sizeof(path)); open_file(path, O_RDWR | O_TRUNC, 1); return;
    case 0x3d: {
        read_string(cpu.seg[DS], DX, path, sizeof(path));
        int access = AL & 3;
        open_file(path, access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR, 0);
        return;
    }
    case 0x3e: if (!file_of(BX)) { fail(6); return; } file_close(BX); succeed(); return;
    case 0x3f: dos_read(); return;
    case 0x40: dos_write(); return;
    case 0x41: read_string(cpu.seg[DS], DX, path, sizeof(path)); host_native_path(path, other, sizeof(other)); if (unlink(other)) fail(dos_error()); else succeed(); return;
    case 0x42: dos_seek(); return;
    case 0x43: {
        read_string(cpu.seg[DS], DX, path, sizeof(path));
        host_native_path(path, other, sizeof(other));
        struct stat st;
        if (stat(other, &st)) { fail(dos_error()); return; }
        if (AL == 0) CX = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        succeed();
        return;
    }
    case 0x44: dos_ioctl(); return;
    case 0x45: case 0x46: {
        struct file *f = file_of(BX);
        if (!f) { fail(6); return; }
        int target;
        if (AH == 0x45) target = file_alloc();
        else {
            target = CX;
            if (target < 0 || target >= MAX_FILES) { fail(6); return; }
            if (files[target].used && target >= 5) file_close(target);
        }
        if (target < 0) { fail(4); return; }
        files[target] = *f;
        files[target].owner = psp;
        if (f->fd >= 3) files[target].fd = dup(f->fd);
        if (AH == 0x45) AX = (u16)target;
        succeed();
        return;
    }
    case 0x47: {
        host_fullpath(".", path, sizeof(path));
        const char *inside = strlen(path) > 3 ? path + 3 : "";
        write_string(cpu.seg[DS], SI, inside);
        AX = 0x0100;
        succeed();
        return;
    }
    case 0x48: {
        u16 segment = mcb_alloc(BX, psp);
        if (!segment) { mcb_merge(); BX = mcb_largest(); fail(8); return; }
        AX = segment;
        succeed();
        return;
    }
    case 0x49:
        if (!mcb_valid(cpu.seg[ES])) { fail(9); return; }
        mem_write16(linear((u16)(cpu.seg[ES] - 1), 1), 0);
        mcb_merge();
        succeed();
        return;
    case 0x4a: {
        int r = mcb_resize(cpu.seg[ES], BX);
        if (r == -1) { succeed(); return; }
        if (r == -7) { fail(9); return; }
        BX = (u16)r;
        fail(8);
        return;
    }
    case 0x4b: dos_exec(); return;
    case 0x4c: dos_terminate(AL); return;
    case 0x4d: AL = last_exit_code; AH = last_exit_kind; return;
    case 0x4e: dos_find_first(); return;
    case 0x4f: dos_find_next(); return;
    case 0x50: psp = BX; return;
    case 0x51: case 0x62: BX = psp; return;
    case 0x54: AL = 0; return;
    case 0x56: {
        char from[300], to[300];
        read_string(cpu.seg[DS], DX, path, sizeof(path));
        host_native_path(path, from, sizeof(from));
        read_string(cpu.seg[ES], DI, path, sizeof(path));
        host_native_path(path, to, sizeof(to));
        if (rename(from, to)) fail(dos_error()); else succeed();
        return;
    }
    case 0x57: {
        struct file *f = file_of(BX);
        if (!f) { fail(6); return; }
        if (AL == 0) {
            struct stat st;
            u16 date = 0x21, time = 0;
            if (f->fd >= 0 && !fstat(f->fd, &st)) dos_date_time(st.st_mtime, &date, &time);
            CX = time;
            DX = date;
        }
        succeed();
        return;
    }
    case 0x58: if (AL == 0) AX = 0; succeed(); return;
    case 0x59: AX = 0; BX = 0; CH = 0; return;
    case 0x5a: {
        static unsigned counter = 1;
        u16 base = DX;
        read_string(cpu.seg[DS], base, path, sizeof(path));
        size_t n = strlen(path);
        char name[32];
        snprintf(name, sizeof(name), "VDM%05u.TMP", counter++ % 100000);
        snprintf(path + n, sizeof(path) - n, "%s", name);
        write_string(cpu.seg[DS], base, path);
        open_file(path, O_RDWR | O_TRUNC, 1);
        return;
    }
    case 0x5b: {
        read_string(cpu.seg[DS], DX, path, sizeof(path));
        host_native_path(path, other, sizeof(other));
        struct stat st;
        if (!stat(other, &st)) { fail(80); return; }
        open_file(path, O_RDWR | O_TRUNC, 1);
        return;
    }
    case 0x67: succeed(); return;
    case 0x68: case 0x6a: succeed(); return;
    case 0x6c: {
        read_string(cpu.seg[DS], SI, path, sizeof(path));
        host_native_path(path, other, sizeof(other));
        struct stat st;
        int exists = !stat(other, &st), action = DL;
        int access = BL & 3;
        int flags = access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR;
        if (exists) {
            if (!(action & 3)) { fail(80); return; }
            if ((action & 3) == 2) { open_file(path, O_RDWR | O_TRUNC, 1); CX = 3; return; }
            open_file(path, flags, 0);
            CX = 1;
        } else {
            if (!(action & 0x10)) { fail(2); return; }
            open_file(path, O_RDWR | O_TRUNC, 1);
            CX = 2;
        }
        return;
    }
    default: AL = 0; bios_set_return_flag(F_CF, 1); return;
    }
}
