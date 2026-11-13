/*
 * DOS-XC /os/runtime/iso9660.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

struct iso_volume {
    unsigned disk;
    u32 root_lba, root_size, blocks;
    int joliet, rock;
    u32 cached;
    int have;
    u8 block[2048];
};

struct iso_entry { u32 lba, size; u8 flags; u8 date[7]; char name[260]; };

static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static int read_block(struct iso_volume *v, u32 lba) {
    if (v->have && v->cached == lba) return 0;
    v->have = 0;
    if (lba >= v->blocks || disk_io(v->disk, (u64)lba * 4, 4, v->block, 0)) return -1;
    v->cached = lba;
    v->have = 1;
    return 0;
}

static uptr put_utf8(char *out, uptr n, uptr size, u32 c) {
    if (c < 0x80) { if (n + 1 < size) out[n++] = (char)c; }
    else if (c < 0x800) { if (n + 2 < size) { out[n++] = (char)(0xc0 | c >> 6); out[n++] = (char)(0x80 | (c & 63)); } }
    else if (n + 3 < size) { out[n++] = (char)(0xe0 | c >> 12); out[n++] = (char)(0x80 | (c >> 6 & 63)); out[n++] = (char)(0x80 | (c & 63)); }
    return n;
}

static void decode_name(const struct iso_volume *v, const u8 *r, struct iso_entry *e) {
    u32 name_len = r[32], len = r[0];
    const u8 *name = r + 33;
    uptr n = 0;
    if (v->rock) {
        u32 at = 33 + name_len + !(name_len & 1);
        while (at + 4 <= len) {
            const u8 *s = r + at;
            u32 slen = s[2];
            if (slen < 4 || at + slen > len) break;
            if (s[0] == 'N' && s[1] == 'M' && slen >= 5 && !(s[4] & 6))
                for (u32 i = 5; i < slen && n + 1 < sizeof(e->name); ++i) e->name[n++] = (char)s[i];
            at += slen;
        }
        if (n) { e->name[n] = 0; return; }
    }
    if (v->joliet) {
        for (u32 i = 0; i + 1 < name_len; i += 2) {
            u32 c = (u32)name[i] << 8 | name[i + 1];
            if (c == ';') break;
            n = put_utf8(e->name, n, sizeof(e->name), c);
        }
    } else {
        for (u32 i = 0; i < name_len && name[i] != ';' && n + 1 < sizeof(e->name); ++i) e->name[n++] = (char)name[i];
        if (n && e->name[n - 1] == '.') --n;
    }
    e->name[n] = 0;
}

typedef int (*each_fn)(void *context, const struct iso_entry *e);

static int each(struct iso_volume *v, u32 lba, u32 size, each_fn fn, void *context) {
    for (u32 offset = 0; offset < size;) {
        u32 in = offset % 2048;
        if (read_block(v, lba + offset / 2048)) return -1;
        const u8 *r = v->block + in;
        u32 len = r[0];
        if (!len) { offset = (offset / 2048 + 1) * 2048; continue; }
        if (len < 34 || in + len > 2048 || 33 + r[32] > len) return -1;
        offset += len;
        if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue;
        struct iso_entry e;
        e.lba = le32(r + 2);
        e.size = le32(r + 10);
        e.flags = r[25];
        memcpy(e.date, r + 18, 7);
        decode_name(v, r, &e);
        if (!e.name[0]) continue;
        int stop = fn(context, &e);
        if (stop) return stop > 0 ? 1 : -1;
    }
    return 0;
}

struct search { const char *want; struct iso_entry *found; };

static int match(void *context, const struct iso_entry *e) {
    struct search *s = context;
    if (!rt_equal_nocase(e->name, s->want)) return 0;
    *s->found = *e;
    return 1;
}

static int find(struct iso_volume *v, const char *path, struct iso_entry *out) {
    out->lba = v->root_lba;
    out->size = v->root_size;
    out->flags = 2;
    memset(out->date, 0, sizeof(out->date));
    out->name[0] = '\\';
    out->name[1] = 0;
    while (*path) {
        while (*path == '\\' || *path == '/') ++path;
        if (!*path) break;
        char part[260];
        uptr n = 0;
        while (*path && *path != '\\' && *path != '/') { if (n + 1 < sizeof(part)) part[n++] = *path; ++path; }
        part[n] = 0;
        if (!(out->flags & 2)) return -1;
        struct iso_entry found;
        struct search s = {part, &found};
        if (each(v, out->lba, out->size, match, &s) != 1) return -1;
        *out = found;
    }
    return 0;
}

static void fill(const struct iso_entry *e, struct fat32_info *info) {
    memset(info, 0, sizeof(*info));
    uptr n = rt_strlen(e->name);
    if (n >= sizeof(info->name)) n = sizeof(info->name) - 1;
    memcpy(info->name, e->name, n);
    info->size = e->flags & 2 ? 0 : e->size;
    info->attributes = e->flags & 2 ? DOSXC_ATTR_DIRECTORY : DOSXC_ATTR_READONLY;
    unsigned year = 1900u + e->date[0];
    if (year >= 1980 && e->date[1] && e->date[2]) {
        info->date = (u16)((year - 1980) << 9 | (e->date[1] & 15) << 5 | (e->date[2] & 31));
        info->time = (u16)((e->date[3] & 31) << 11 | (e->date[4] & 63) << 5 | (e->date[5] & 63) / 2);
    }
}

struct iso_volume *iso_mount(unsigned disk) {
    struct iso_volume *v = memory_alloc(sizeof(*v), 64, OWNER_KERNEL, 1);
    if (!v) return 0;
    v->disk = disk;
    v->blocks = 64;
    u32 pvd_root = 0, pvd_size = 0, joliet_root = 0, joliet_size = 0, blocks = 0;
    for (u32 lba = 16; lba < 64; ++lba) {
        if (read_block(v, lba) || memcmp(v->block + 1, "CD001", 5)) break;
        const u8 *b = v->block;
        if (b[0] == 255) break;
        if (b[0] == 1 && !pvd_root) {
            pvd_root = le32(b + 156 + 2);
            pvd_size = le32(b + 156 + 10);
            blocks = le32(b + 80);
        } else if (b[0] == 2 && b[88] == '%' && b[89] == '/' && (b[90] == '@' || b[90] == 'C' || b[90] == 'E') && !joliet_root) {
            joliet_root = le32(b + 156 + 2);
            joliet_size = le32(b + 156 + 10);
        }
    }
    if (!pvd_root || blocks < 17) { memory_free(v, OWNER_KERNEL); return 0; }
    v->blocks = blocks;
    v->have = 0;
    if (!read_block(v, pvd_root)) {
        const u8 *r = v->block;
        u32 at = 33 + r[32] + !(r[32] & 1);
        if (r[0] >= at + 7 && r[at] == 'S' && r[at + 1] == 'P' && r[at + 4] == 0xbe && r[at + 5] == 0xef) v->rock = 1;
    }
    if (!v->rock && joliet_root) {
        v->joliet = 1;
        v->root_lba = joliet_root;
        v->root_size = joliet_size;
    } else {
        v->root_lba = pvd_root;
        v->root_size = pvd_size;
    }
    return v;
}

void iso_unmount(struct iso_volume *v) { if (v) memory_free(v, OWNER_KERNEL); }

u64 iso_bytes(const struct iso_volume *v) { return (u64)v->blocks * 2048; }

int iso_stat(struct iso_volume *v, const char *path, struct fat32_info *info) {
    struct iso_entry e;
    if (find(v, path, &e)) return -1;
    fill(&e, info);
    return 0;
}

struct listing { fat32_list_fn fn; void *context; };

static int list_one(void *context, const struct iso_entry *e) {
    struct listing *l = context;
    struct fat32_info info;
    fill(e, &info);
    return l->fn(l->context, &info) ? 1 : 0;
}

int iso_list(struct iso_volume *v, const char *path, fat32_list_fn fn, void *context) {
    struct iso_entry e;
    if (find(v, path, &e) || !(e.flags & 2)) return -1;
    struct listing l = {fn, context};
    return each(v, e.lba, e.size, list_one, &l) < 0 ? -1 : 0;
}

int iso_read_cursor(struct iso_volume *v, const char *path, struct fat32_cursor *c, u32 offset, void *buffer, u32 size) {
    if (!c->size_known) {
        struct iso_entry e;
        if (find(v, path, &e) || (e.flags & 2)) return -1;
        c->first = e.lba;
        c->size = e.size;
        c->size_known = 1;
    }
    if (offset > c->size || size > c->size - offset) return -1;
    u8 *out = buffer;
    u64 at = (u64)c->first * 2048 + offset;
    while (size) {
        u32 block = (u32)(at / 2048), in = (u32)(at % 2048);
        if (!in && size >= 2048) {
            u32 n = size / 2048;
            if (n > 64) n = 64;
            if (block + n > v->blocks || disk_io(v->disk, (u64)block * 4, n * 4, out, 0)) return -1;
            out += n * 2048;
            at += (u64)n * 2048;
            size -= n * 2048;
            continue;
        }
        if (read_block(v, block)) return -1;
        u32 k = 2048 - in < size ? 2048 - in : size;
        memcpy(out, v->block + in, k);
        out += k;
        at += k;
        size -= k;
    }
    return 0;
}
