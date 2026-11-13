/*
 * DOS-XC /desktop/archive/common.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#define _FILE_OFFSET_BITS 64
#include "arc.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

int job_fail(struct job *job, const char *format, ...) {
    if (job->error && job->error_size && !job->error[0]) {
        va_list list;
        va_start(list, format);
        vsnprintf(job->error, job->error_size, format, list);
        va_end(list);
    }
    return -1;
}

static int report(struct job *job, const char *name) {
    if (job->cancelled) return -1;
    if (job->progress && job->progress->report &&
        job->progress->report(job->progress->context, name, job->done, job->total)) {
        job->cancelled = 1;
        job_fail(job, "Cancelled.");
        return -1;
    }
    return 0;
}

int job_tick(struct job *job) {
    if (job->cancelled) return -1;
    if (++job->ticks & 15) return 0;
    return report(job, 0);
}

void job_status(struct job *job, const char *name) { report(job, name); }

int arc_seek(FILE *f, u64 offset) { return fseeko(f, (off_t)offset, SEEK_SET); }
u64 arc_tell(FILE *f) { off_t at = ftello(f); return at < 0 ? 0 : (u64)at; }
u64 arc_file_size(FILE *f) {
    off_t here = ftello(f);
    if (fseeko(f, 0, SEEK_END)) return 0;
    off_t size = ftello(f);
    fseeko(f, here, SEEK_SET);
    return size < 0 ? 0 : (u64)size;
}

void src_open(struct src *s, FILE *f, u64 offset, u64 limit, struct job *job) {
    s->f = f;
    s->job = job;
    s->pos = s->len = 0;
    s->left = limit;
    s->error = s->ended = 0;
    if (arc_seek(f, offset)) s->error = 1;
}

int src_fill(struct src *s) {
    if (s->error || !s->left) { s->ended = 1; return 0; }
    if (s->job && job_tick(s->job)) { s->error = 1; return 0; }
    size_t want = s->left < sizeof(s->buf) ? (size_t)s->left : sizeof(s->buf);
    size_t got = fread(s->buf, 1, want, s->f);
    if (!got) { s->ended = 1; if (ferror(s->f)) s->error = 1; s->left = 0; return 0; }
    s->left -= got;
    s->pos = 0;
    s->len = got;
    if (s->job) s->job->done += got;
    return 1;
}

size_t src_read(struct src *s, void *out, size_t n) {
    u8 *p = out;
    size_t done = 0;
    while (done < n) {
        if (s->pos == s->len && !src_fill(s)) break;
        size_t k = s->len - s->pos < n - done ? s->len - s->pos : n - done;
        memcpy(p + done, s->buf + s->pos, k);
        s->pos += k;
        done += k;
    }
    return done;
}

int src_skip(struct src *s, u64 n) {
    while (n) {
        if (s->pos == s->len && !src_fill(s)) return -1;
        size_t k = s->len - s->pos < n ? s->len - s->pos : (size_t)n;
        s->pos += k;
        n -= k;
    }
    return 0;
}

static int fsink_write(struct sink *sink, const u8 *data, size_t n) {
    struct fsink *s = (struct fsink *)sink;
    if (s->error) return -1;
    if (n && fwrite(data, 1, n, s->f) != n) { s->error = 1; return -1; }
    s->count += n;
    return 0;
}

void fsink_init(struct fsink *s, FILE *f) {
    s->sink.write = fsink_write;
    s->f = f;
    s->count = 0;
    s->error = 0;
}

/* One component of a name in the archive, made safe for FAT: 0 to skip it. */
static int clean_part(const char *from, size_t n, char *out, size_t size) {
    if (!n || (n == 1 && from[0] == '.')) return 0;
    if (n == 2 && from[0] == '.' && from[1] == '.') return -1;
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < size; ++i) {
        unsigned char c = (unsigned char)from[i];
#ifdef __dosxc__
        /* DOS-XC's FAT names are ASCII: one '_' for each other character. */
        if (c >= 0x80) {
            while (i + 1 < n && ((unsigned char)from[i + 1] & 0xc0) == 0x80) ++i;
            out[k++] = '_';
            continue;
        }
#endif
        out[k++] = c < 32 || strchr("<>:\"|?*", c) ? '_' : (char)c;
    }
    while (k && (out[k - 1] == '.' || out[k - 1] == ' ')) --k;
    if (!k) out[k++] = '_';
    out[k] = 0;
    return 1;
}

static int make_parents(char *path, size_t from) {
    for (size_t i = from; path[i]; ++i) {
        if (path[i] != ARC_SEP) continue;
        path[i] = 0;
        mkdir(path, 0777);
        path[i] = ARC_SEP;
    }
    return 0;
}

static int target_path(struct job *job, const char *name, char *out, size_t size) {
    size_t n = (size_t)snprintf(out, size, "%s", job->folder);
    if (n && out[n - 1] != ARC_SEP && n + 1 < size) out[n++] = ARC_SEP;
    out[n] = 0;
    size_t base = n;
    const char *p = name;
    int parts = 0;
    if (p[0] && p[1] == ':') p += 2;
    while (*p) {
        const char *end = p;
        while (*end && *end != '/' && *end != '\\') ++end;
        char part[256];
        int r = clean_part(p, (size_t)(end - p), part, sizeof(part));
        if (r < 0) return job_fail(job, "\"%s\" points outside the folder.", name);
        if (r) {
            size_t k = strlen(part);
            if (n + k + 2 >= size) return job_fail(job, "\"%s\" makes too long a path.", name);
            if (parts++) out[n++] = ARC_SEP;
            memcpy(out + n, part, k + 1);
            n += k;
        }
        p = *end ? end + 1 : end;
    }
    if (!parts) return 1;
    make_parents(out, base);
    return 0;
}

int out_begin(struct job *job, const char *name, int directory, long long mtime) {
    snprintf(job->name, sizeof(job->name), "%s", name);
    job->crc = 0;
    job->written = 0;
    job->mtime = mtime;
    int r = target_path(job, name, job->out_path, sizeof(job->out_path));
    if (r) return r < 0 ? -1 : 1;
    job_status(job, name);
    if (job->cancelled) return -1;
    if (directory) {
        mkdir(job->out_path, 0777);
        return 1;
    }
    job->out = fopen(job->out_path, "wb");
    if (!job->out) return job_fail(job, "%s could not be written.", job->out_path);
    return 0;
}

int out_write(struct job *job, const void *data, size_t n) {
    if (!job->out) return 0;
    if (n && fwrite(data, 1, n, job->out) != n) return job_fail(job, "%s could not be written (is the disk full?).", job->out_path);
    job->crc = crc32_update(job->crc, data, n);
    job->written += n;
    return 0;
}

int out_end(struct job *job) {
    if (!job->out) return 0;
    int bad = fclose(job->out);
    job->out = 0;
    if (bad) return job_fail(job, "%s could not be written (is the disk full?).", job->out_path);
    return 0;
}

void out_abort(struct job *job) {
    if (!job->out) return;
    fclose(job->out);
    job->out = 0;
    remove(job->out_path);
}

int out_copy(struct job *job, const char *name, const char *target, long long mtime) {
    char from[700];
    if (target_path(job, target, from, sizeof(from))) return 0;
    FILE *in = fopen(from, "rb");
    if (!in) return 0;
    int r = out_begin(job, name, 0, mtime);
    if (r) { fclose(in); return r < 0 ? -1 : 0; }
    static u8 buffer[65536];
    size_t n;
    while (!r && (n = fread(buffer, 1, sizeof(buffer), in)) > 0) r = out_write(job, buffer, n);
    fclose(in);
    return r ? -1 : out_end(job);
}

static int job_sink_write(struct sink *s, const u8 *data, size_t n);
static struct { struct sink sink; struct job *job; } job_sinks[1];
static int job_sink_write(struct sink *s, const u8 *data, size_t n) { (void)s; return out_write(job_sinks[0].job, data, n); }
struct sink *out_sink(struct job *job) {
    job_sinks[0].sink.write = job_sink_write;
    job_sinks[0].job = job;
    return &job_sinks[0].sink;
}

struct walk { struct arc_item *items; int n, capacity; struct job *job; };

static int add_item(struct walk *w, const char *name, const char *path, int directory, u64 size, long long mtime) {
    if (w->n == w->capacity) {
        int capacity = w->capacity ? w->capacity * 2 : 256;
        struct arc_item *grown = realloc(w->items, sizeof(*grown) * (size_t)capacity);
        if (!grown) return job_fail(w->job, "Out of memory.");
        w->items = grown;
        w->capacity = capacity;
    }
    struct arc_item *it = &w->items[w->n];
    it->name = strdup(name);
    it->path = strdup(path);
    if (!it->name || !it->path) { free(it->name); free(it->path); return job_fail(w->job, "Out of memory."); }
    it->directory = directory;
    it->size = size;
    it->mtime = mtime;
    ++w->n;
    if (!directory) w->job->total += size;
    return 0;
}

static int by_name(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static int walk_path(struct walk *w, const char *path, const char *name, int depth) {
    struct stat st;
    if (stat(path, &st)) return job_fail(w->job, "%s could not be read.", path);
    if (!S_ISDIR(st.st_mode)) return add_item(w, name, path, 0, (u64)st.st_size, (long long)st.st_mtime);
    if (depth > 32) return job_fail(w->job, "%s is nested too deeply.", path);
    if (add_item(w, name, path, 1, 0, (long long)st.st_mtime)) return -1;
    DIR *d = opendir(path);
    if (!d) return job_fail(w->job, "%s could not be read.", path);
    char **names = 0;
    int count = 0, capacity = 0, r = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            char **grown = realloc(names, sizeof(*names) * (size_t)capacity);
            if (!grown) { r = job_fail(w->job, "Out of memory."); break; }
            names = grown;
        }
        if (!(names[count] = strdup(e->d_name))) { r = job_fail(w->job, "Out of memory."); break; }
        ++count;
    }
    closedir(d);
    if (!r) qsort(names, (size_t)count, sizeof(*names), by_name);
    for (int i = 0; i < count && !r; ++i) {
        size_t a = strlen(path) + strlen(names[i]) + 2, b = strlen(name) + strlen(names[i]) + 2;
        char *child = malloc(a), *child_name = malloc(b);
        if (!child || !child_name) r = job_fail(w->job, "Out of memory.");
        else {
            snprintf(child, a, "%s%s%s", path, path[strlen(path) - 1] == ARC_SEP ? "" : (char[]){ARC_SEP, 0}, names[i]);
            snprintf(child_name, b, "%s/%s", name, names[i]);
            r = walk_path(w, child, child_name, depth + 1);
        }
        free(child);
        free(child_name);
    }
    for (int i = 0; i < count; ++i) free(names[i]);
    free(names);
    return r;
}

int arc_walk(struct job *job, const char *const *sources, int count, struct arc_item **items, int *n) {
    struct walk w = {0, 0, 0, job};
    int r = 0;
    for (int i = 0; i < count && !r; ++i) {
        char path[600];
        snprintf(path, sizeof(path), "%s", sources[i]);
        size_t len = strlen(path);
        while (len > 1 && path[len - 1] == ARC_SEP && !(len == 3 && path[1] == ':')) path[--len] = 0;
        const char *base = strrchr(path, ARC_SEP);
        base = base ? base + 1 : path;
        if (!*base || (len >= 2 && path[len - 1] == ':') || (len == 3 && path[1] == ':')) base = "Drive";
        r = walk_path(&w, path, base, 0);
    }
    if (r) { arc_walk_free(w.items, w.n); return -1; }
    *items = w.items;
    *n = w.n;
    return 0;
}

void arc_walk_free(struct arc_item *items, int n) {
    for (int i = 0; i < n; ++i) { free(items[i].name); free(items[i].path); }
    free(items);
}

int arc_feed(struct job *job, const struct arc_item *item, int (*fn)(void *, const u8 *, size_t), void *context, u32 *crc) {
    static u8 buffer[65536];
    job_status(job, item->name);
    FILE *f = fopen(item->path, "rb");
    if (!f) return job_fail(job, "%s could not be read.", item->path);
    u64 left = item->size;
    u32 c = 0;
    int r = 0;
    while (left && !r) {
        size_t want = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
        size_t got = fread(buffer, 1, want, f);
        if (got != want) { r = job_fail(job, "%s changed while it was being read.", item->path); break; }
        c = crc32_update(c, buffer, got);
        left -= got;
        job->done += got;
        if (fn(context, buffer, got)) r = -1;
        else if (job_tick(job)) r = -1;
    }
    fclose(f);
    if (crc) *crc = c;
    return r;
}

static u32 crc32_table[256];
static u64 crc64_table[256];

u32 crc32_update(u32 crc, const void *data, size_t n) {
    if (!crc32_table[1])
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
            crc32_table[i] = c;
        }
    const u8 *p = data;
    crc = ~crc;
    while (n--) crc = crc32_table[(crc ^ *p++) & 255] ^ crc >> 8;
    return ~crc;
}

u64 crc64_update(u64 crc, const void *data, size_t n) {
    if (!crc64_table[1])
        for (u64 i = 0; i < 256; ++i) {
            u64 c = i;
            for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xc96c5795d7870f42ull & (0ull - (c & 1)));
            crc64_table[i] = c;
        }
    const u8 *p = data;
    crc = ~crc;
    while (n--) crc = crc64_table[(crc ^ *p++) & 255] ^ crc >> 8;
    return ~crc;
}

void dos_time(long long t, u16 *date, u16 *time_) {
    time_t tt = (time_t)t;
    struct tm *tm = gmtime(&tt);
    if (!tm || tm->tm_year < 80) { *date = (1 << 5) | 1; *time_ = 0; return; }
    *date = (u16)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
    *time_ = (u16)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2);
}

long long from_dos_time(u16 date, u16 time_) {
    static const int days_before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int year = 1980 + (date >> 9), month = (date >> 5) & 15, day = date & 31;
    if (month < 1 || month > 12 || !day) return 0;
    long long days = (long long)(year - 1970) * 365 + (year - 1969) / 4 + days_before[month - 1] + day - 1;
    if (month > 2 && year % 4 == 0) ++days;
    return days * 86400 + (time_ >> 11) * 3600 + ((time_ >> 5) & 63) * 60 + (time_ & 31) * 2;
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcasecmp(s + a - b, tail);
}

static int by_name_format(const char *path) {
    if (ends_with(path, ".tar.gz") || ends_with(path, ".tgz")) return ARC_TAR_GZ;
    if (ends_with(path, ".tar.xz") || ends_with(path, ".txz")) return ARC_TAR_XZ;
    if (ends_with(path, ".tar")) return ARC_TAR;
    if (ends_with(path, ".zip")) return ARC_ZIP;
    if (ends_with(path, ".7z")) return ARC_7Z;
    if (ends_with(path, ".rar")) return ARC_RAR;
    if (ends_with(path, ".gz")) return ARC_GZ;
    if (ends_with(path, ".xz")) return ARC_XZ;
    return ARC_UNKNOWN;
}

int arc_detect(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return ARC_UNKNOWN;
    u8 head[512];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    int named = by_name_format(path);
    if (n >= 4 && head[0] == 'P' && head[1] == 'K' && (head[2] == 3 || head[2] == 5) && (head[3] == 4 || head[3] == 6)) return ARC_ZIP;
    if (n >= 6 && !memcmp(head, "7z\xbc\xaf\x27\x1c", 6)) return ARC_7Z;
    if (n >= 7 && !memcmp(head, "Rar!\x1a\x07", 6)) return ARC_RAR;
    if (n >= 2 && head[0] == 0x1f && head[1] == 0x8b) return named == ARC_GZ ? ARC_GZ : named == ARC_TAR_GZ ? ARC_TAR_GZ : ARC_TAR_GZ;
    if (n >= 6 && !memcmp(head, "\xfd" "7zXZ\0", 6)) return named == ARC_XZ ? ARC_XZ : ARC_TAR_XZ;
    if (n >= 263 && !memcmp(head + 257, "ustar", 5)) return ARC_TAR;
    if (n >= 512 && named == ARC_TAR) return ARC_TAR;
    return ARC_UNKNOWN;
}

const char *arc_extension(int format) {
    switch (format) {
    case ARC_ZIP: return ".zip";
    case ARC_7Z: return ".7z";
    case ARC_RAR: return ".rar";
    case ARC_TAR: return ".tar";
    case ARC_TAR_GZ: return ".tar.gz";
    case ARC_TAR_XZ: return ".tar.xz";
    case ARC_GZ: return ".gz";
    case ARC_XZ: return ".xz";
    }
    return "";
}

int arc_can_create(int format) {
    return format == ARC_ZIP || format == ARC_7Z || format == ARC_TAR || format == ARC_TAR_GZ || format == ARC_TAR_XZ;
}

void arc_stem(const char *path, char *out, size_t size) {
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(out, size, "%s", base);
    static const char *const tails[] = {".tar.gz", ".tar.xz", ".tgz", ".txz", ".tar", ".zip", ".7z", ".rar", ".gz", ".xz", 0};
    for (int i = 0; tails[i]; ++i)
        if (ends_with(out, tails[i]) && strlen(out) > strlen(tails[i])) { out[strlen(out) - strlen(tails[i])] = 0; break; }
    /* "name.part1" of a multi-volume RAR */
    char *dot = strrchr(out, '.');
    if (dot && !strncasecmp(dot, ".part", 5) && dot != out) *dot = 0;
}

int arc_extract(const char *archive, const char *folder, const struct arc_progress *progress, char *error, size_t error_size) {
    struct job job;
    memset(&job, 0, sizeof(job));
    job.progress = progress;
    job.error = error;
    job.error_size = error_size;
    job.folder = folder;
    if (error && error_size) error[0] = 0;
    int format = arc_detect(archive);
    if (format == ARC_UNKNOWN) return job_fail(&job, "This is not an archive DOS-XC can open.");
    FILE *f = fopen(archive, "rb");
    if (!f) return job_fail(&job, "%s could not be opened.", archive);
    job.total = arc_file_size(f);
    char made[700];
    snprintf(made, sizeof(made), "%s%c", folder, ARC_SEP);
    make_parents(made, made[0] && made[1] == ':' ? 3 : 1);
    struct stat st;
    if (stat(folder, &st) || !S_ISDIR(st.st_mode)) { fclose(f); return job_fail(&job, "The folder %s could not be made.", folder); }
    int r;
    switch (format) {
    case ARC_ZIP: r = zip_extract(&job, f); break;
    case ARC_7Z: r = sevenz_extract(&job, f); break;
    case ARC_RAR: r = rar_extract(&job, f); break;
    case ARC_GZ: case ARC_XZ: r = gz_extract(&job, f, archive, format); break;
    default: r = tar_extract(&job, f, format); break;
    }
    if (r) out_abort(&job);
    else if (job.out) r = out_end(&job);
    fclose(f);
    if (r) return job_fail(&job, "The archive could not be extracted.");
    job.done = job.total;
    job_status(&job, 0);
    return 0;
}

int arc_create(const char *archive, int format, const char *const *sources, int count,
               const struct arc_progress *progress, char *error, size_t error_size) {
    struct job job;
    memset(&job, 0, sizeof(job));
    job.progress = progress;
    job.error = error;
    job.error_size = error_size;
    if (error && error_size) error[0] = 0;
    if (!arc_can_create(format)) return job_fail(&job, "DOS-XC cannot make %s archives.", arc_extension(format));
    struct arc_item *items;
    int n;
    if (arc_walk(&job, sources, count, &items, &n)) return -1;
    FILE *f = fopen(archive, "wb+");
    if (!f) { arc_walk_free(items, n); return job_fail(&job, "%s could not be made.", archive); }
    int r;
    switch (format) {
    case ARC_ZIP: r = zip_create(&job, f, items, n); break;
    case ARC_7Z: r = sevenz_create(&job, f, items, n); break;
    default: r = tar_create(&job, f, format, items, n); break;
    }
    arc_walk_free(items, n);
    if (fclose(f) && !r) r = job_fail(&job, "%s could not be written (is the disk full?).", archive);
    if (r) { remove(archive); return job_fail(&job, "The archive could not be made."); }
    job.done = job.total;
    job_status(&job, 0);
    return 0;
}
