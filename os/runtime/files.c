/*
 * DOS-XC /os/runtime/files.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"
#include "net.h"

enum { KIND_FILE = 1, KIND_CONSOLE, KIND_NULL, KIND_PIPE_READ, KIND_PIPE_WRITE, KIND_DIRECTORY, KIND_SOCKET, KIND_UNIX, KIND_SHM };
#define MAX_INODES 768
#define MAX_OPEN 2048
#define MAX_PIPES 32

struct inode {
    int refs, dirty, unlinked;
    int loaded;                 /* data holds the file; otherwise reads go to disk */
    struct fat32_cursor cursor;
    char path[PATH_MAX_RT];
    u8 *data;
    uptr size, capacity;
    u8 attributes;
    u16 date, time;
};
struct pipe { int readers, writers; u8 *data; uptr head, tail, capacity; };
struct open_file;
#define UNIX_PASSED 64
/* One end of a local stream socket: what was sent to it, waiting. */
struct unix_end {
    int used, open;                  /* open: open files referring to this end */
    struct unix_end *peer;
    u8 *data;
    uptr head, tail, capacity;
    /* descriptors that arrive with the byte at stream position at */
    struct { u64 at; struct open_file *file; } passed[UNIX_PASSED];
    int passed_count;
    u64 consumed;                    /* stream position of data[head] */
};
struct open_file {
    int refs, kind, flags;
    u64 offset;
    struct inode *inode;
    struct pipe *pipe;
    struct socket *socket;
    struct unix_end *unix_end;
    struct vm_object *object;
    char path[PATH_MAX_RT];
};

static struct inode inodes[MAX_INODES];
static struct pipe pipes[MAX_PIPES];
#define MAX_UNIX 256
static struct unix_end unix_ends[MAX_UNIX];
static struct open_file open_files[MAX_OPEN];
static u8 empty_buffer[4];

#define O_ACCMODE DOSXC_O_ACCMODE
#define O_RDONLY DOSXC_O_RDONLY
#define WRITABLE(f) (((f)->flags & O_ACCMODE) != O_RDONLY)
#define READABLE(f) (((f)->flags & O_ACCMODE) != DOSXC_O_WRONLY)

/* ---- paths ---------------------------------------------------------- */

static int device_kind(const char *path) {
    static const char *const null_names[] = {"/dev/null", "\\dev\\null", "NUL", "NUL:"};
    static const char *const console_names[] = {"/dev/tty", "/dev/console", "\\dev\\tty", "CON", "CON:"};
    for (unsigned i = 0; i < sizeof(null_names) / sizeof(*null_names); ++i)
        if (rt_equal_nocase(path, null_names[i])) return KIND_NULL;
    for (unsigned i = 0; i < sizeof(console_names) / sizeof(*console_names); ++i)
        if (rt_equal_nocase(path, console_names[i])) return KIND_CONSOLE;
    return 0;
}

/* The canonical directory a process without parent starts in. */
static void root_directory(char *out) {
    out[0] = (char)rt_upper(ctx->drive);
    out[1] = ':';
    rt_strcpy(out + 2, ctx->cwd[0] == '\\' ? ctx->cwd : "\\");
}

/* "D:\DIR\FILE" from any of: FILE, DIR/FILE, /DIR/FILE, D:/DIR/FILE, D:FILE.
   A drive spec after a separator ("C:/HOME/D:/X", which Unix programs make
   by joining the current folder with a DOS path) starts again at that
   drive's root. */
int path_resolve(const struct process *p, const char *s, char *out) {
    char buffer[PATH_MAX_RT], cwd_buffer[PATH_MAX_RT];
    uptr n;
    if (!s || !*s) return -DOSXC_ENOENT;
    const char *cwd = p ? p->cwd : cwd_buffer;
    if (!p) root_directory(cwd_buffer);
    char drive = cwd[0];
    int absolute = *s == '/' || *s == '\\';
    if (((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':') {
        drive = (char)rt_upper(s[0]);
        if (!volume_exists(drive)) return -DOSXC_ENOENT;
        if (drive != cwd[0]) absolute = 1;
        s += 2;
        if (*s == '/' || *s == '\\') absolute = 1;
    }
    buffer[0] = drive;
    buffer[1] = ':';
    if (absolute) {
        buffer[2] = '\\';
        n = 3;
    } else {
        n = rt_strlen(cwd);
        if (n < 3 || n >= PATH_MAX_RT) return -DOSXC_ENAMETOOLONG;
        memcpy(buffer, cwd, n);
    }
    while (*s) {
        while (*s == '/' || *s == '\\') ++s;
        if (!*s) break;
        const char *start = s;
        while (*s && *s != '/' && *s != '\\') ++s;
        uptr length = (uptr)(s - start);
        if (length == 1 && start[0] == '.') continue;
        if (length == 2 && start[1] == ':' && ((start[0] | 32) >= 'a' && (start[0] | 32) <= 'z')) {
            char letter = (char)rt_upper(start[0]);
            if (!volume_exists(letter)) return -DOSXC_ENOENT;
            buffer[0] = letter;
            n = 3;
            continue;
        }
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 3 && buffer[n - 1] != '\\') --n;
            if (n > 3) --n;
            continue;
        }
        for (uptr i = 0; i < length; ++i) {
            char c = start[i];
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (u8)c < 32)
                return -DOSXC_EINVAL;
        }
        if (n + length + 2 >= PATH_MAX_RT) return -DOSXC_ENAMETOOLONG;
        if (n > 3) buffer[n++] = '\\';
        memcpy(buffer + n, start, length);
        n += length;
    }
    buffer[n] = 0;
    rt_strcpy(out, buffer);
    return 0;
}

/* The POSIX spelling programs see: "D:/DIR/FILE" (as on DJGPP). */
void path_posix(const char *canonical, char *out, uptr size) {
    uptr i = 0;
    if (canonical[0] && canonical[1] == ':' && size > 3) {
        out[i++] = canonical[0];
        out[i++] = ':';
        canonical += 2;
    }
    for (; *canonical && i + 1 < size; ++canonical) out[i++] = *canonical == '\\' ? '/' : *canonical;
    out[i] = 0;
}

static void parent_of(const char *canonical, char *out) {
    rt_strcpy(out, canonical);
    uptr n = rt_strlen(out);
    while (n > 3 && out[n - 1] != '\\') --n;
    out[n > 3 ? n - 1 : 3] = 0;
}

static int fat_info(const char *canonical, struct fat32_info *info) {
    return !fs_stat(canonical, info) ? 0 : -1;
}

static u32 path_hash(const char *path) {
    u32 hash = 2166136261u;
    for (; *path; ++path) hash = (hash ^ (u32)rt_upper((u8)*path)) * 16777619u;
    return hash ? hash : 1;
}

static i64 fat_time(u16 date, u16 time) {
    if (!date) return 0;
    return rt_epoch(1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2);
}

static void fill_stat(struct dosxc_stat *st, u32 kind, u64 size, u8 attributes, i64 mtime, const char *path) {
    memset(st, 0, sizeof(*st));
    st->size = size;
    st->mtime = mtime;
    st->attributes = attributes;
    st->device = 1;
    st->inode = path ? path_hash(path) : kind;
    if (attributes & DOSXC_ATTR_DIRECTORY) st->mode = DOSXC_S_IFDIR | 0755;
    else if (kind == KIND_FILE) st->mode = DOSXC_S_IFREG | ((attributes & DOSXC_ATTR_READONLY) ? 0555 : 0755);
    else if (kind == KIND_PIPE_READ || kind == KIND_PIPE_WRITE) st->mode = DOSXC_S_IFIFO | 0600;
    else if (kind == KIND_SOCKET || kind == KIND_UNIX) st->mode = DOSXC_S_IFSOCK | 0666;
    else st->mode = DOSXC_S_IFCHR | 0666;
}

/* ---- inodes ----------------------------------------------------------- */

static struct inode *find_inode(const char *canonical) {
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs && !inodes[i].unlinked && rt_equal_nocase(inodes[i].path, canonical)) return &inodes[i];
    return 0;
}

static int reserve(struct inode *n, uptr needed) {
    if (needed <= n->capacity) return 0;
    uptr capacity = n->capacity ? n->capacity : 4096;
    while (capacity < needed) {
        if (capacity > ((uptr)-1) / 2) return -DOSXC_ENOMEM;
        capacity *= 2;
    }
    u8 *data = memory_alloc(capacity, PAGE, OWNER_KERNEL, 0);
    if (!data) return -DOSXC_ENOMEM;
    if (n->size) memcpy(data, n->data, n->size);
    if (n->data) memory_free(n->data, OWNER_KERNEL);
    n->data = data;
    n->capacity = capacity;
    return 0;
}

static int flush(struct inode *n) {
    if (!n->dirty || n->unlinked) return 0;
    if (n->size > 0xffffffffu) return -DOSXC_EFBIG;
    if (!fs_writable(n->path)) return -DOSXC_EROFS;
    if (fs_save(n->path, n->data ? n->data : empty_buffer, (u32)n->size)) return -DOSXC_ENOSPC;
    n->dirty = 0;
    struct fat32_info info;
    if (!fat_info(n->path, &info)) {
        n->date = info.date;
        n->time = info.time;
    }
    return 0;
}

static int release_inode(struct inode *n) {
    if (--n->refs) return 0;
    int result = flush(n);
    if (n->data) memory_free(n->data, OWNER_KERNEL);
    memset(n, 0, sizeof(*n));
    return result;
}

/* ---- open files ------------------------------------------------------- */

static struct open_file *new_open_file(int kind) {
    for (unsigned i = 0; i < MAX_OPEN; ++i)
        if (!open_files[i].refs) {
            memset(&open_files[i], 0, sizeof(open_files[i]));
            open_files[i].refs = 1;
            open_files[i].kind = kind;
            return &open_files[i];
        }
    static int warned;
    if (!warned++) event_log(DOSXC_EVENT_ERROR, "Files", "The open file table is full (%d)", MAX_OPEN);
    return 0;
}

static void free_pipe(struct pipe *p) {
    if (p->readers || p->writers) return;
    if (p->data) memory_free(p->data, OWNER_KERNEL);
    memset(p, 0, sizeof(*p));
}

static void unix_close(struct unix_end *e);

static int release_open(struct open_file *f) {
    if (--f->refs) return 0;
    int result = 0;
    if (f->kind == KIND_UNIX) unix_close(f->unix_end);
    if (f->kind == KIND_SHM) vm_object_close(f->object);
    if (f->kind == KIND_FILE) result = release_inode(f->inode);
    else if (f->kind == KIND_PIPE_READ) { --f->pipe->readers; free_pipe(f->pipe); }
    else if (f->kind == KIND_PIPE_WRITE) { --f->pipe->writers; free_pipe(f->pipe); }
    else if (f->kind == KIND_SOCKET) socket_close(f->socket);
    f->kind = 0;
    return result;
}

static int lowest_fd(struct process *p, int minimum) {
    if (minimum < 0) minimum = 0;
    for (int fd = minimum; fd < MAX_FDS; ++fd)
        if (!p->fds[fd]) return fd;
    static int warned;
    if (!warned++) event_log(DOSXC_EVENT_ERROR, "Files", "%s (process %d) has %d descriptors open, the most there can be", event_program(p), p->pid, MAX_FDS);
    return -DOSXC_EMFILE;
}

static struct open_file *lookup(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !current) return 0;
    return current->fds[fd];
}

static int open_inode(struct open_file *f, const char *path, int flags) {
    int writing = (flags & O_ACCMODE) != O_RDONLY || (flags & DOSXC_O_TRUNC);
    struct inode *n = find_inode(path);
    struct fat32_info info;
    memset(&info, 0, sizeof(info));
    int exists = n || !fat_info(path, &info);
    if (exists && !n && (info.attributes & DOSXC_ATTR_DIRECTORY)) {
        if (writing) return -DOSXC_EISDIR;
        f->kind = KIND_DIRECTORY;
        rt_strcpy(f->path, path);
        return 0;
    }
    if (flags & DOSXC_O_DIRECTORY) return exists ? -DOSXC_ENOTDIR : -DOSXC_ENOENT;
    if (exists && (flags & DOSXC_O_CREAT) && (flags & DOSXC_O_EXCL)) return -DOSXC_EEXIST;
    if (!exists) {
        if (!(flags & DOSXC_O_CREAT)) return -DOSXC_ENOENT;
        char parent[PATH_MAX_RT];
        struct fat32_info folder;
        parent_of(path, parent);
        if (fat_info(parent, &folder)) return -DOSXC_ENOENT;
        if (!(folder.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
        if (!fs_writable(path)) return -DOSXC_EROFS;
        if (fs_save(path, empty_buffer, 0)) return -DOSXC_ENOSPC;
        if (fat_info(path, &info)) return -DOSXC_EIO;
    } else if (writing && volume_image_busy(path)) {
        return -DOSXC_EBUSY;
    } else if (!n && writing && (info.attributes & DOSXC_ATTR_READONLY)) {
        return -DOSXC_EACCES;
    }
    if (!n) {
        for (unsigned i = 0; i < MAX_INODES && !n; ++i)
            if (!inodes[i].refs) n = &inodes[i];
        if (!n) return -DOSXC_ENFILE;
        memset(n, 0, sizeof(*n));
        rt_strcpy(n->path, path);
        n->attributes = info.attributes;
        n->date = info.date;
        n->time = info.time;
        n->loaded = 1;
        if (!(flags & DOSXC_O_TRUNC) && info.size) {
            n->size = info.size;
            if (!writing) n->loaded = 0;
            else {
                if (reserve(n, info.size)) return -DOSXC_ENOMEM;
                if (fs_read(path, n->data, info.size)) {
                    memory_free(n->data, OWNER_KERNEL);
                    memset(n, 0, sizeof(*n));
                    return -DOSXC_EIO;
                }
            }
        }
    } else if ((flags & DOSXC_O_TRUNC) && n->size) {
        n->size = 0;
        n->loaded = 1;
        n->dirty = 1;
    }
    if (writing && !n->loaded) {
        if (n->size && reserve(n, n->size)) return -DOSXC_ENOMEM;
        if (n->size && fs_read(path, n->data, (u32)n->size)) return -DOSXC_EIO;
        n->loaded = 1;
    }
    if ((flags & DOSXC_O_TRUNC) && exists && info.size) n->dirty = 1;
    ++n->refs;
    f->kind = KIND_FILE;
    f->inode = n;
    return 0;
}

static int api_open(const char *path, int flags, int mode) {
    (void)mode;
    struct process *p = current;
    int fd = lowest_fd(p, 0);
    if (fd < 0) return fd;
    int kind = device_kind(path);
    struct open_file *f = new_open_file(kind ? kind : KIND_FILE);
    if (!f) return -DOSXC_ENFILE;
    f->flags = flags & (O_ACCMODE | DOSXC_O_APPEND | DOSXC_O_NONBLOCK);
    if (!kind) {
        char canonical[PATH_MAX_RT];
        int r = path_resolve(p, path, canonical);
        if (!r) r = accounts_access(canonical, (flags & O_ACCMODE) != O_RDONLY || (flags & (DOSXC_O_CREAT | DOSXC_O_TRUNC)));
        if (!r) r = open_inode(f, canonical, flags);
        if (r) {
            f->refs = 0;
            f->kind = 0;
            return r;
        }
    }
    p->fds[fd] = f;
    p->cloexec[fd] = (flags & DOSXC_O_CLOEXEC) != 0;
    return fd;
}

static int api_close(int fd) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    current->fds[fd] = 0;
    current->cloexec[fd] = 0;
    return release_open(f);
}

/* ---- console (terminal.c) -------------------------------------------- */

int files_console_mode(int mode) { return terminal_mode(mode); }

/* ---- data transfer ---------------------------------------------------- */

/* ---- local stream sockets (AF_UNIX) ----------------------------------- */

#define UNIX_LIMIT (4u << 20)

static void unix_close(struct unix_end *e) {
    if (!e || --e->open > 0) return;
    for (int i = 0; i < e->passed_count; ++i) release_open(e->passed[i].file);
    e->passed_count = 0;
    if (e->data) memory_free(e->data, OWNER_KERNEL);
    e->data = 0;
    e->head = e->tail = e->capacity = 0;
    if (e->peer) e->peer->peer = 0;
    e->peer = 0;
    e->used = 0;
}

static int unix_room(struct unix_end *to, uptr size) {
    if (to->head && to->tail + size > to->capacity) {
        memmove(to->data, to->data + to->head, to->tail - to->head);
        to->tail -= to->head;
        to->head = 0;
    }
    if (to->tail + size <= to->capacity) return 0;
    uptr capacity = to->capacity ? to->capacity : 65536;
    while (capacity < to->tail + size) capacity *= 2;
    u8 *data = memory_alloc(capacity, PAGE, OWNER_KERNEL, 0);
    if (!data) return -DOSXC_ENOMEM;
    if (to->data) {
        memcpy(data, to->data + to->head, to->tail - to->head);
        memory_free(to->data, OWNER_KERNEL);
    }
    to->tail -= to->head;
    to->head = 0;
    to->data = data;
    to->capacity = capacity;
    return 0;
}

static int unix_wait(int nonblock) {
    if (nonblock) return -DOSXC_EAGAIN;
    socket_wait_interrupted();
    sched_sleep(200000);
    return 0;
}

static dosxc_ssize unix_send(struct open_file *f, const void *buffer, uptr size, int flags,
                             struct open_file **files, int file_count) {
    struct unix_end *e = f->unix_end;
    int nonblock = (f->flags & DOSXC_O_NONBLOCK) || (flags & DOSXC_MSG_DONTWAIT);
    for (;;) {
        struct unix_end *to = e->peer;
        if (!to) return -DOSXC_EPIPE;
        if ((to->tail - to->head + size <= UNIX_LIMIT || to->tail == to->head) &&
            to->passed_count + file_count <= UNIX_PASSED) {
            int r = unix_room(to, size ? size : 1);
            if (r) return r;
            u64 at = to->consumed + (to->tail - to->head);
            for (int i = 0; i < file_count; ++i) {
                ++files[i]->refs;
                to->passed[to->passed_count].at = at;
                to->passed[to->passed_count++].file = files[i];
            }
            memcpy(to->data + to->tail, buffer, size);
            to->tail += size;
            return (dosxc_ssize)size;
        }
        int w = unix_wait(nonblock);
        if (w) return w;
    }
}

/* Received descriptors go to the lowest free numbers; FDS gets them. */
static dosxc_ssize unix_receive(struct open_file *f, void *buffer, uptr size, int flags, int *fds, int *fd_count) {
    struct unix_end *e = f->unix_end;
    int nonblock = (f->flags & DOSXC_O_NONBLOCK) || (flags & DOSXC_MSG_DONTWAIT);
    int room = fd_count ? *fd_count : 0;
    if (fd_count) *fd_count = 0;
    while (e->tail == e->head) {
        if (!e->peer) return 0;
        if (!size && e->passed_count) break;
        int w = unix_wait(nonblock);
        if (w) return w;
    }
    uptr n = e->tail - e->head;
    if (n > size) n = size;
    if (e->passed_count) {
        u64 first = e->passed[0].at;
        if (first > e->consumed && first < e->consumed + n) n = (uptr)(first - e->consumed);
        else if (first == e->consumed) {
            u64 next = ~0ull;
            for (int i = 0; i < e->passed_count; ++i)
                if (e->passed[i].at > first && e->passed[i].at < next) next = e->passed[i].at;
            if (next != ~0ull && next < e->consumed + n) n = (uptr)(next - e->consumed);
        }
    }
    memcpy(buffer, e->data + e->head, n);
    int got = 0, kept = 0;
    for (int i = 0; i < e->passed_count; ++i) {
        if (e->passed[i].at != e->consumed) { e->passed[kept++] = e->passed[i]; continue; }
        struct open_file *file = e->passed[i].file;
        int fd = got < room ? lowest_fd(current, 0) : -1;
        if (fd < 0) { release_open(file); continue; }
        current->fds[fd] = file;
        current->cloexec[fd] = 0;
        fds[got++] = fd;
    }
    e->passed_count = kept;
    if (fd_count) *fd_count = got;
    e->head += n;
    e->consumed += n;
    if (e->head == e->tail) e->head = e->tail = 0;
    return (dosxc_ssize)n;
}

static int unix_poll(struct open_file *f, int events) {
    struct unix_end *e = f->unix_end;
    int ready = 0;
    if ((events & DOSXC_POLL_IN) && (e->tail != e->head || e->passed_count || !e->peer)) ready |= DOSXC_POLL_IN;
    if ((events & DOSXC_POLL_OUT) && e->peer && e->peer->tail - e->peer->head < UNIX_LIMIT && e->peer->passed_count < UNIX_PASSED)
        ready |= DOSXC_POLL_OUT;
    if (!e->peer) ready |= DOSXC_POLL_HUP;
    return ready;
}

/* -ENOTSOCK when FD is neither a local socket nor a pipe. */
int files_poll(int fd, int events) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (f->kind == KIND_PIPE_READ) {
        int ready = 0;
        if ((events & DOSXC_POLL_IN) && (f->pipe->tail != f->pipe->head || !f->pipe->writers)) ready |= DOSXC_POLL_IN;
        if (!f->pipe->writers) ready |= DOSXC_POLL_HUP;
        return ready;
    }
    if (f->kind == KIND_PIPE_WRITE) return f->pipe->readers ? events & DOSXC_POLL_OUT : DOSXC_POLL_ERR;
    if (f->kind != KIND_UNIX) return -DOSXC_ENOTSOCK;
    return unix_poll(f, events);
}

static int unix_pair(int fds[2]) {
    struct unix_end *a = 0, *b = 0;
    for (unsigned i = 0; i < MAX_UNIX && !b; ++i)
        if (!unix_ends[i].used) { if (!a) a = &unix_ends[i]; else b = &unix_ends[i]; }
    if (!b) return -DOSXC_ENFILE;
    int first = lowest_fd(current, 0);
    if (first < 0) return first;
    current->fds[first] = (struct open_file *)1;
    int second = lowest_fd(current, 0);
    current->fds[first] = 0;
    if (second < 0) return second;
    struct open_file *fa = new_open_file(KIND_UNIX);
    if (!fa) return -DOSXC_ENFILE;
    struct open_file *fb = new_open_file(KIND_UNIX);
    if (!fb) { fa->refs = 0; fa->kind = 0; return -DOSXC_ENFILE; }
    memset(a, 0, sizeof(*a));
    memset(b, 0, sizeof(*b));
    a->used = b->used = 1;
    a->open = b->open = 1;
    a->peer = b;
    b->peer = a;
    fa->unix_end = a;
    fb->unix_end = b;
    fa->flags = fb->flags = DOSXC_O_RDWR;
    current->fds[first] = fa;
    current->fds[second] = fb;
    current->cloexec[first] = current->cloexec[second] = 0;
    fds[0] = first;
    fds[1] = second;
    return 0;
}

struct vm_object *files_object(int fd) {
    struct open_file *f = lookup(fd);
    return f && f->kind == KIND_SHM ? f->object : 0;
}

long shm_syscall(int operation, const char *name, int flags) {
    if (!current) return -DOSXC_EINVAL;
    if (name) while (*name == '/') ++name;
    if (operation == DOSXC_SHM_UNLINK) return name && !vm_object_unlink(name) ? 0 : -DOSXC_ENOENT;
    if (operation != DOSXC_SHM_OPEN) return -DOSXC_EINVAL;
    int fd = lowest_fd(current, 0);
    if (fd < 0) return fd;
    struct open_file *f = new_open_file(KIND_SHM);
    if (!f) return -DOSXC_ENFILE;
    f->object = vm_object_open(name, !name || (flags & DOSXC_O_CREAT), (flags & DOSXC_O_EXCL) != 0);
    if (!f->object) { f->refs = 0; f->kind = 0; return name && !(flags & DOSXC_O_CREAT) ? -DOSXC_ENOENT : -DOSXC_EEXIST; }
    f->flags = DOSXC_O_RDWR;
    current->fds[fd] = f;
    current->cloexec[fd] = 0;
    return fd;
}

long unix_syscall(int operation, uptr a, uptr b) {
    if (!current) return -DOSXC_EINVAL;
    if (operation == DOSXC_UNIX_PAIR) return unix_pair((int *)a);
    struct open_file *f = lookup((int)a);
    if (!f) return -DOSXC_EBADF;
    if (f->kind != KIND_UNIX) return -DOSXC_ENOTSOCK;
    struct dosxc_unix_message *m = (struct dosxc_unix_message *)b;
    if (m->fd_count < 0 || m->fd_count > UNIX_PASSED) return -DOSXC_EINVAL;
    if (operation == DOSXC_UNIX_SEND) {
        struct open_file *files[UNIX_PASSED];
        for (int i = 0; i < m->fd_count; ++i)
            if (!(files[i] = lookup(m->fds[i]))) return -DOSXC_EBADF;
        return unix_send(f, m->buffer, m->length, m->flags, files, m->fd_count);
    }
    if (operation == DOSXC_UNIX_RECEIVE) return unix_receive(f, m->buffer, m->length, m->flags, m->fds, &m->fd_count);
    return -DOSXC_EINVAL;
}

static dosxc_ssize api_read(int fd, void *buffer, dosxc_size size) {
    struct open_file *f = lookup(fd);
    if (!f || !READABLE(f)) return -DOSXC_EBADF;
    switch (f->kind) {
    case KIND_FILE: {
        struct inode *n = f->inode;
        if (f->offset >= n->size) return 0;
        uptr available = (uptr)(n->size - f->offset);
        if (size > available) size = available;
        if (!n->loaded) {
            if (fs_read_cursor(n->path, &n->cursor, (u32)f->offset, buffer, (u32)size)) return -DOSXC_EIO;
        } else memcpy(buffer, n->data + f->offset, size);
        f->offset += size;
        return (dosxc_ssize)size;
    }
    case KIND_CONSOLE: return terminal_read(buffer, size);
    case KIND_NULL: return 0;
    case KIND_PIPE_READ: {
        struct pipe *p = f->pipe;
        while (p->tail == p->head && p->writers && size) {
            if (f->flags & DOSXC_O_NONBLOCK) return -DOSXC_EAGAIN;
            socket_wait_interrupted();
            sched_sleep(1000000);
        }
        uptr available = p->tail - p->head;
        if (size > available) size = available;
        memcpy(buffer, p->data + p->head, size);
        p->head += size;
        if (p->head == p->tail) p->head = p->tail = 0;
        return (dosxc_ssize)size;
    }
    case KIND_DIRECTORY: return -DOSXC_EISDIR;
    case KIND_SOCKET: return (dosxc_ssize)socket_read(f->socket, buffer, size, 0, (f->flags & DOSXC_O_NONBLOCK) != 0);
    case KIND_UNIX: return unix_receive(f, buffer, size, 0, 0, 0);
    }
    return -DOSXC_EBADF;
}

static dosxc_ssize api_write(int fd, const void *buffer, dosxc_size size) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    switch (f->kind) {
    case KIND_FILE: {
        if (!WRITABLE(f)) return -DOSXC_EBADF;
        struct inode *n = f->inode;
        if (f->flags & DOSXC_O_APPEND) f->offset = n->size;
        u64 end = f->offset + size;
        if (end > (uptr)-1 || end > 0xffffffffull) return -DOSXC_EFBIG;
        int r = reserve(n, (uptr)end);
        if (r) return r;
        if (f->offset > n->size) memset(n->data + n->size, 0, (uptr)(f->offset - n->size));
        memcpy(n->data + f->offset, buffer, size);
        f->offset = end;
        if (end > n->size) n->size = (uptr)end;
        n->dirty = 1;
        return (dosxc_ssize)size;
    }
    case KIND_CONSOLE: return terminal_write(buffer, size);
    case KIND_NULL: return (dosxc_ssize)size;
    case KIND_PIPE_WRITE: {
        struct pipe *p = f->pipe;
        if (!p->readers) return -DOSXC_EPIPE;
        if (p->tail + size > p->capacity) {
            uptr capacity = p->capacity ? p->capacity : 65536;
            while (capacity < p->tail + size) capacity *= 2;
            u8 *data = memory_alloc(capacity, PAGE, OWNER_KERNEL, 0);
            if (!data) return -DOSXC_ENOMEM;
            if (p->data) {
                memcpy(data, p->data, p->tail);
                memory_free(p->data, OWNER_KERNEL);
            }
            p->data = data;
            p->capacity = capacity;
        }
        memcpy(p->data + p->tail, buffer, size);
        p->tail += size;
        return (dosxc_ssize)size;
    }
    case KIND_SOCKET: return (dosxc_ssize)socket_write(f->socket, buffer, size, 0, (f->flags & DOSXC_O_NONBLOCK) != 0);
    case KIND_UNIX: return unix_send(f, buffer, size, 0, 0, 0);
    }
    return -DOSXC_EBADF;
}

static dosxc_i64 api_seek(int fd, dosxc_i64 offset, int whence) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (f->kind == KIND_NULL) return 0;
    if (f->kind != KIND_FILE && f->kind != KIND_DIRECTORY) return -DOSXC_ESPIPE;
    i64 base = whence == 0 ? 0 : whence == 1 ? (i64)f->offset : whence == 2 && f->inode ? (i64)f->inode->size : -1;
    if (base < 0 || (whence == 2 && !f->inode)) return -DOSXC_EINVAL;
    if (base + offset < 0) return -DOSXC_EINVAL;
    f->offset = (u64)(base + offset);
    return (dosxc_i64)f->offset;
}

static int api_dup(int fd, int minimum) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (minimum < 0 || minimum >= MAX_FDS) return -DOSXC_EINVAL;
    int target = lowest_fd(current, minimum);
    if (target < 0) return target;
    ++f->refs;
    current->fds[target] = f;
    current->cloexec[target] = 0;
    return target;
}

static int api_dup2(int fd, int target) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (target < 0 || target >= MAX_FDS) return -DOSXC_EBADF;
    if (fd == target) return target;
    ++f->refs;
    if (current->fds[target]) release_open(current->fds[target]);
    current->fds[target] = f;
    current->cloexec[target] = 0;
    return target;
}

static int api_fdflags(int fd, int set, int flags) {
    if (!lookup(fd)) return -DOSXC_EBADF;
    if (set) current->cloexec[fd] = (flags & DOSXC_FD_CLOEXEC) != 0;
    return current->cloexec[fd] ? DOSXC_FD_CLOEXEC : 0;
}

static int api_status_flags(int fd) {
    struct open_file *f = lookup(fd);
    return f ? f->flags : -DOSXC_EBADF;
}

static int api_pipe(int fds[2]) {
    struct pipe *p = 0;
    for (unsigned i = 0; i < MAX_PIPES && !p; ++i)
        if (!pipes[i].readers && !pipes[i].writers) p = &pipes[i];
    if (!p) return -DOSXC_ENFILE;
    int reader = lowest_fd(current, 0);
    if (reader < 0) return reader;
    current->fds[reader] = (struct open_file *)1;
    int writer = lowest_fd(current, 0);
    current->fds[reader] = 0;
    if (writer < 0) return writer;
    struct open_file *r = new_open_file(KIND_PIPE_READ);
    if (!r) return -DOSXC_ENFILE;
    struct open_file *w = new_open_file(KIND_PIPE_WRITE);
    if (!w) {
        r->refs = 0;
        return -DOSXC_ENFILE;
    }
    memset(p, 0, sizeof(*p));
    p->readers = p->writers = 1;
    r->pipe = w->pipe = p;
    r->flags = O_RDONLY;
    w->flags = DOSXC_O_WRONLY;
    current->fds[reader] = r;
    current->fds[writer] = w;
    current->cloexec[reader] = current->cloexec[writer] = 0;
    fds[0] = reader;
    fds[1] = writer;
    return 0;
}

static int api_isatty(int fd) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    return f->kind == KIND_CONSOLE;
}

static int api_ftruncate(int fd, dosxc_i64 length) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (f->kind == KIND_SHM) return length < 0 ? -DOSXC_EINVAL : vm_object_resize(f->object, (u64)length);
    if (f->kind != KIND_FILE || !WRITABLE(f)) return -DOSXC_EINVAL;
    if (length < 0 || (u64)length > 0xffffffffull) return -DOSXC_EINVAL;
    struct inode *n = f->inode;
    if ((uptr)length > n->size) {
        int r = reserve(n, (uptr)length);
        if (r) return r;
        memset(n->data + n->size, 0, (uptr)length - n->size);
    }
    n->size = (uptr)length;
    n->dirty = 1;
    return 0;
}

static int api_fsync(int fd) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    return f->kind == KIND_FILE ? flush(f->inode) : 0;
}

static int api_fstat(int fd, struct dosxc_stat *st) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (f->kind == KIND_FILE) {
        struct inode *n = f->inode;
        fill_stat(st, KIND_FILE, n->size, n->attributes, n->dirty ? cpu_rtc_time() : fat_time(n->date, n->time), n->path);
    } else if (f->kind == KIND_DIRECTORY) {
        fill_stat(st, KIND_DIRECTORY, 0, DOSXC_ATTR_DIRECTORY, 0, f->path);
    } else if (f->kind == KIND_SHM) {
        fill_stat(st, KIND_FILE, vm_object_size(f->object), 0, 0, 0);
    } else {
        fill_stat(st, (u32)f->kind, f->pipe ? f->pipe->tail - f->pipe->head : 0, 0, 0, 0);
    }
    return 0;
}

/* ---- names ------------------------------------------------------------ */

static int api_stat(const char *path, struct dosxc_stat *st) {
    int kind = device_kind(path);
    if (kind) {
        fill_stat(st, (u32)kind, 0, 0, 0, 0);
        return 0;
    }
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    if ((r = accounts_access(canonical, 0))) return r;
    struct inode *n = find_inode(canonical);
    if (n) {
        fill_stat(st, KIND_FILE, n->size, n->attributes, n->dirty ? cpu_rtc_time() : fat_time(n->date, n->time), canonical);
        return 0;
    }
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    fill_stat(st, KIND_FILE, info.size, info.attributes, fat_time(info.date, info.time), canonical);
    return 0;
}

static int api_unlink(const char *path) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    if ((r = accounts_access(canonical, 1))) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (info.attributes & DOSXC_ATTR_DIRECTORY) return -DOSXC_EISDIR;
    if (info.attributes & DOSXC_ATTR_READONLY) return -DOSXC_EACCES;
    if (volume_image_busy(canonical)) return -DOSXC_EBUSY;
    struct inode *n = find_inode(canonical);
    if (!fs_writable(canonical)) return -DOSXC_EROFS;
    if (n) n->unlinked = 1;
    return fs_remove(canonical) ? -DOSXC_EIO : 0;
}

static int api_rename(const char *from, const char *to) {
    char source[PATH_MAX_RT], target[PATH_MAX_RT];
    int r = path_resolve(current, from, source);
    if (!r) r = path_resolve(current, to, target);
    if (!r) r = accounts_access(source, 1);
    if (!r) r = accounts_access(target, 1);
    if (r) return r;
    struct fat32_info info, existing;
    if (fat_info(source, &info)) return -DOSXC_ENOENT;
    if (rt_equal_nocase(source, target)) return 0;
    if (rt_upper(source[0]) != rt_upper(target[0])) return -DOSXC_EXDEV;
    if (!fs_writable(source)) return -DOSXC_EROFS;
    if (volume_image_busy(source) || volume_image_busy(target)) return -DOSXC_EBUSY;
    struct inode *n = find_inode(source);
    if (n && (r = flush(n))) return r;
    if (!fat_info(target, &existing)) {
        if (existing.attributes & DOSXC_ATTR_DIRECTORY) return -DOSXC_EISDIR;
        if (info.attributes & DOSXC_ATTR_DIRECTORY) return -DOSXC_ENOTDIR;
        struct inode *old = find_inode(target);
        if (old) old->unlinked = 1;
        if (fs_remove(target)) return -DOSXC_EACCES;
    }
    if (fs_rename(source, target)) return -DOSXC_EACCES;
    if (n) rt_strcpy(n->path, target);
    return 0;
}

static int api_mkdir(const char *path) {
    char canonical[PATH_MAX_RT], parent[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    if ((r = accounts_access(canonical, 1))) return r;
    struct fat32_info info;
    if (!fat_info(canonical, &info)) return -DOSXC_EEXIST;
    parent_of(canonical, parent);
    if (fat_info(parent, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    if (!fs_writable(canonical)) return -DOSXC_EROFS;
    return fs_mkdir(canonical) ? -DOSXC_ENOSPC : 0;
}

static int api_rmdir(const char *path) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    if ((r = accounts_access(canonical, 1))) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    if (rt_strlen(canonical) <= 3 || rt_equal_nocase(canonical, current->cwd)) return -DOSXC_EBUSY;
    if (!fs_writable(canonical)) return -DOSXC_EROFS;
    return fs_rmdir(canonical) ? -DOSXC_ENOTEMPTY : 0;
}

static int api_chdir(const char *path) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    if ((r = accounts_access(canonical, 0))) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    rt_strcpy(current->cwd, canonical);
    return 0;
}

static int api_getcwd(char *buffer, dosxc_size size) {
    char posix[PATH_MAX_RT];
    path_posix(current->cwd, posix, sizeof(posix));
    if (rt_strlen(posix) + 1 > size) return -DOSXC_ERANGE;
    rt_strcpy(buffer, posix);
    return 0;
}

static int api_resolve(const char *path, char *buffer, dosxc_size size) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    char posix[PATH_MAX_RT];
    path_posix(canonical, posix, sizeof(posix));
    uptr n = rt_strlen(posix);
    if (n + 1 > size) return -DOSXC_ENAMETOOLONG;
    rt_strcpy(buffer, posix);
    return (int)n;
}

/* The DOS spelling, "D:\DIR\FILE". */
int files_fullpath(const char *path, char *buffer, dosxc_size size) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    uptr n = rt_strlen(canonical);
    if (n + 1 > size) return -DOSXC_ENAMETOOLONG;
    rt_strcpy(buffer, canonical);
    return (int)n;
}

struct listing { struct dosxc_dirent *entries; uptr capacity, count; };

static int collect(void *context, const struct fat32_info *info) {
    struct listing *l = context;
    if (info->name[0] == '.' && (!info->name[1] || (info->name[1] == '.' && !info->name[2]))) return 0;
    if (l->count < l->capacity) {
        struct dosxc_dirent *e = &l->entries[l->count];
        memset(e, 0, sizeof(*e));
        uptr n = rt_strlen(info->name);
        if (n > 255) n = 255;
        memcpy(e->name, info->name, n);
        e->attributes = info->attributes;
        e->size = info->size;
        e->mtime = fat_time(info->date, info->time);
    }
    ++l->count;
    return 0;
}

static dosxc_ssize api_listdir(const char *path, struct dosxc_dirent *entries, dosxc_size capacity) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    if ((r = accounts_access(canonical, 0))) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    struct listing l = {entries, capacity, 0};
    if (fs_list(canonical, collect, &l)) return -DOSXC_EIO;
    return (dosxc_ssize)l.count;
}

/* ---- sockets (socket.c) ------------------------------------------------ */

int files_socket_open(struct socket *s) {
    int fd = lowest_fd(current, 0);
    if (fd < 0) return fd;
    struct open_file *f = new_open_file(KIND_SOCKET);
    if (!f) return -DOSXC_ENFILE;
    f->flags = DOSXC_O_RDWR;
    f->socket = s;
    current->fds[fd] = f;
    current->cloexec[fd] = 0;
    return fd;
}

struct socket *files_socket(int fd, int *flags) {
    struct open_file *f = lookup(fd);
    if (!f || f->kind != KIND_SOCKET) return 0;
    if (flags) *flags = f->flags;
    return f->socket;
}

int files_set_flags(int fd, int flags) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    f->flags = (f->flags & ~(DOSXC_O_NONBLOCK | DOSXC_O_APPEND)) | (flags & (DOSXC_O_NONBLOCK | DOSXC_O_APPEND));
    return 0;
}

/* ---- process plumbing ------------------------------------------------- */

int files_is_executable(const char *canonical, u8 magic[4]) {
    struct fat32_info info;
    if (fat_info(canonical, &info) || (info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOENT;
    if (info.size < 4) return -DOSXC_ENOEXEC;
    struct inode *n = find_inode(canonical);
    if (n && n->dirty) return -DOSXC_EBUSY;
    return fs_read_at(canonical, 0, magic, 4) ? -DOSXC_EIO : 0;
}

int files_setup(struct process *child, struct process *parent, const char *output, int append) {
    memset(child->fds, 0, sizeof(child->fds));
    memset(child->cloexec, 0, sizeof(child->cloexec));
    if (parent) rt_strcpy(child->cwd, parent->cwd);
    else root_directory(child->cwd);
    if (parent) {
        for (int fd = 0; fd < MAX_FDS; ++fd)
            if (parent->fds[fd] && !parent->cloexec[fd]) {
                child->fds[fd] = parent->fds[fd];
                ++child->fds[fd]->refs;
            }
        return 0;
    }
    struct open_file *console = new_open_file(KIND_CONSOLE);
    if (!console) return -DOSXC_ENFILE;
    console->flags = DOSXC_O_RDWR;
    console->refs = 3;
    child->fds[0] = child->fds[1] = child->fds[2] = console;
    terminal_input_reset();
    terminal_reset();
    if (output) {
        struct process *saved = current;
        current = child;
        current->fds[1] = 0;
        --console->refs;
        int fd = api_open(output, DOSXC_O_WRONLY | DOSXC_O_CREAT | (append ? DOSXC_O_APPEND : DOSXC_O_TRUNC), 0666);
        current = saved;
        if (fd != 1) {
            files_release(child);
            return fd < 0 ? fd : -DOSXC_EIO;
        }
    }
    return 0;
}

int files_fork(struct process *child, struct process *parent) {
    rt_strcpy(child->cwd, parent->cwd);
    for (int fd = 0; fd < MAX_FDS; ++fd)
        if ((child->fds[fd] = parent->fds[fd])) {
            ++child->fds[fd]->refs;
            child->cloexec[fd] = parent->cloexec[fd];
        }
    return 0;
}

/* Give CHILD exactly the descriptors MAP lists (pairs: the parent's fd, the child's fd). */
int files_remap(struct process *child, struct process *parent, const int *map, int count) {
    struct open_file *chosen[MAX_FDS] = {0};
    for (int i = 0; i < count; ++i) {
        int from = map[2 * i], to = map[2 * i + 1];
        if (from < 0 || from >= MAX_FDS || to < 0 || to >= MAX_FDS || !parent->fds[from]) return -DOSXC_EBADF;
        chosen[to] = parent->fds[from];
    }
    files_release(child);
    for (int fd = 0; fd < MAX_FDS; ++fd) {
        child->cloexec[fd] = 0;
        if ((child->fds[fd] = chosen[fd])) ++chosen[fd]->refs;
    }
    return 0;
}

void files_release(struct process *p) {
    for (int fd = 0; fd < MAX_FDS; ++fd)
        if (p->fds[fd]) {
            struct open_file *f = p->fds[fd];
            p->fds[fd] = 0;
            if (release_open(f)) rt_print("Warning: a file could not be written back to disk.\r\n");
        }
}

void files_flush_all(void) {
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs) flush(&inodes[i]);
    storage_flush_all();
}

/* Drive LETTER went away (a USB disk was pulled or ejected).  Files still
   open there fail from now on instead of reaching whatever disk gets the
   letter next; directories of processes there are left as they are. */
void files_volume_gone(char letter) {
    letter = (char)rt_upper(letter);
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs && rt_upper(inodes[i].path[0]) == letter && inodes[i].path[1] == ':') {
            inodes[i].path[0] = '?';
            inodes[i].dirty = 0;
            memset(&inodes[i].cursor, 0, sizeof(inodes[i].cursor));
        }
    for (unsigned i = 0; i < MAX_OPEN; ++i)
        if (open_files[i].refs && rt_upper(open_files[i].path[0]) == letter && open_files[i].path[1] == ':')
            open_files[i].path[0] = '?';
}

/* Write back what is waiting for drive LETTER (before it is ejected). */
void files_flush_volume(char letter) {
    letter = (char)rt_upper(letter);
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs && rt_upper(inodes[i].path[0]) == letter) flush(&inodes[i]);
}

int files_init(void) {
    memset(inodes, 0, sizeof(inodes));
    memset(open_files, 0, sizeof(open_files));
    memset(pipes, 0, sizeof(pipes));
    return 0;
}

void files_fill_api(struct dosxc_api *api) {
    api->open = api_open;
    api->close = api_close;
    api->read = api_read;
    api->write = api_write;
    api->seek = api_seek;
    api->dup = api_dup;
    api->dup2 = api_dup2;
    api->fdflags = api_fdflags;
    api->status_flags = api_status_flags;
    api->pipe = api_pipe;
    api->isatty = api_isatty;
    api->ftruncate = api_ftruncate;
    api->fsync = api_fsync;
    api->fstat = api_fstat;
    api->stat = api_stat;
    api->unlink = api_unlink;
    api->rename = api_rename;
    api->mkdir = api_mkdir;
    api->rmdir = api_rmdir;
    api->chdir = api_chdir;
    api->getcwd = api_getcwd;
    api->listdir = api_listdir;
    api->resolve = api_resolve;
}
