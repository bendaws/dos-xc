/*
 * DOS-XC /os/runtime/accounts.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"
#include "sha256.h"

#define MAX_ACCOUNTS 32
#define DB "\\SYSTEM\\USERS.DB"

struct account {
    struct dosxc_user user;
    u8 salt[16], hash[32];
};

static struct account accounts[MAX_ACCOUNTS];
static int account_count, enabled, loaded;
static char db_text[65536];

static void db_path(char *out) {
    out[0] = (char)rt_upper(ctx->drive);
    out[1] = ':';
    rt_strcpy(out + 2, DB);
}

static int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static void hex_read(const char *text, u8 *out, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        int high = hex_value(text[i * 2]), low = high < 0 ? -1 : hex_value(text[i * 2 + 1]);
        out[i] = (u8)(high < 0 || low < 0 ? 0 : high << 4 | low);
        if (high < 0 || low < 0) return;
    }
}

static char *hex_write(char *at, const u8 *data, int bytes) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < bytes; ++i) { *at++ = digits[data[i] >> 4]; *at++ = digits[data[i] & 15]; }
    return at;
}

/* The next tab- or line-ended field of *P, copied into OUT. */
static void field(char **p, char *out, uptr size) {
    uptr n = 0;
    while (**p && **p != '\t' && **p != '\n' && **p != '\r') {
        if (n + 1 < size) out[n++] = **p;
        ++*p;
    }
    out[n] = 0;
    if (**p == '\t') ++*p;
}

static void load(void) {
    if (loaded) return;
    loaded = 1;
    account_count = 0;
    enabled = 0;
    char path[32];
    db_path(path);
    struct fat32_info info;
    if (fs_stat(path, &info) || info.size >= sizeof(db_text)) return;
    if (fs_read(path, db_text, info.size)) return;
    db_text[info.size] = 0;
    for (char *line = db_text; *line;) {
        char kind[16], number[8], salt[40], hash[72];
        char *p = line;
        field(&p, kind, sizeof(kind));
        if (!rt_strcmp(kind, "accounts")) field(&p, number, sizeof(number));
        else if (!rt_strcmp(kind, "user") && account_count < MAX_ACCOUNTS) {
            struct account *a = &accounts[account_count];
            memset(a, 0, sizeof(*a));
            field(&p, a->user.name, sizeof(a->user.name));
            field(&p, number, sizeof(number));
            a->user.admin = number[0] == '1';
            field(&p, number, sizeof(number));
            a->user.start = number[0] == '1' ? DOSXC_USER_START_PROMPT : DOSXC_USER_START_DESKTOP;
            field(&p, salt, sizeof(salt));
            field(&p, hash, sizeof(hash));
            hex_read(salt, a->salt, 16);
            hex_read(hash, a->hash, 32);
            field(&p, a->user.home, sizeof(a->user.home));
            field(&p, a->user.picture, sizeof(a->user.picture));
            field(&p, a->user.nickname, sizeof(a->user.nickname));
            if (a->user.name[0]) ++account_count;
        }
        while (*line && *line != '\n') ++line;
        if (*line) ++line;
    }
    enabled = account_count > 0;
}

static int save(void) {
    char *at = db_text, *end = db_text + sizeof(db_text) - 600;
    const char *head = "accounts\t1\n";
    for (const char *s = head; *s; ++s) *at++ = *s;
    for (int i = 0; i < account_count && at < end; ++i) {
        const struct account *a = &accounts[i];
        const char *parts[3] = {"user\t", a->user.name, "\t"};
        for (int k = 0; k < 3; ++k)
            for (const char *s = parts[k]; *s; ++s) *at++ = *s;
        *at++ = a->user.admin ? '1' : '0';
        *at++ = '\t';
        *at++ = a->user.start == DOSXC_USER_START_PROMPT ? '1' : '0';
        *at++ = '\t';
        at = hex_write(at, a->salt, 16);
        *at++ = '\t';
        at = hex_write(at, a->hash, 32);
        *at++ = '\t';
        const char *rest[5] = {a->user.home, "\t", a->user.picture, "\t", a->user.nickname};
        for (int k = 0; k < 5; ++k)
            for (const char *s = rest[k]; *s; ++s) *at++ = *s;
        *at++ = '\n';
    }
    char path[32];
    db_path(path);
    return fs_save(path, db_text, (u32)(at - db_text)) ? -DOSXC_EIO : 0;
}

static struct account *find(const char *name) {
    for (int i = 0; i < account_count; ++i)
        if (rt_equal_nocase(accounts[i].user.name, name)) return &accounts[i];
    return 0;
}

static int valid_name(const char *name) {
    uptr n = rt_strlen(name);
    if (!n || n >= sizeof(((struct dosxc_user *)0)->name)) return 0;
    for (uptr i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

static int valid_text(const char *text, uptr size) {
    uptr n = rt_strlen(text);
    if (n >= size) return 0;
    for (uptr i = 0; i < n; ++i)
        if ((u8)text[i] < 32 || (u8)text[i] > 126) return 0;
    return 1;
}

static void set_password(struct account *a, const char *password) {
    random_syscall(a->salt, sizeof(a->salt));
    password_hash(a->salt, password, a->hash);
}

static int password_ok(const struct account *a, const char *password) {
    u8 hash[32];
    password_hash(a->salt, password, hash);
    int diff = 0;
    for (int i = 0; i < 32; ++i) diff |= hash[i] ^ a->hash[i];
    return !diff;
}

static void default_home(const char *name, char *out) {
    out[0] = (char)rt_upper(ctx->drive);
    out[1] = ':';
    rt_strcpy(out + 2, "\\Users\\");
    rt_strcpy(out + 9, name);
}

static void make_folder(const char *path) {
    struct fat32_info info;
    char partial[PATH_MAX_RT];
    uptr n = rt_strlen(path);
    if (n >= sizeof(partial)) return;
    for (uptr i = 3; i <= n; ++i) {
        if (path[i] != '\\' && path[i]) continue;
        memcpy(partial, path, i);
        partial[i] = 0;
        if (fs_stat(partial, &info)) fs_mkdir(partial);
    }
}

int accounts_enabled(void) { load(); return enabled; }

void accounts_init(void) {
    char path[PATH_MAX_RT];
    path[0] = (char)rt_upper(ctx->drive);
    path[1] = ':';
    rt_strcpy(path + 2, "\\Users\\Public");
    make_folder(path);
}

/* The system (no user) and administrators may do anything. */
int accounts_admin(void) {
    load();
    if (!enabled || !current || !current->user[0]) return 1;
    struct account *a = find(current->user);
    return a && a->user.admin;
}

/* Account changes: an administrator, or a process an administrator unlocked (DOSXC_USER_UNLOCK). */
static int managing(void) { return accounts_admin() || (current && current->unlocked); }

static int under(const char *path, const char *folder) {
    uptr n = rt_strlen(folder);
    for (uptr i = 0; i < n; ++i)
        if (!path[i] || rt_upper((u8)path[i]) != rt_upper((u8)folder[i])) return 0;
    return path[n] == 0 || path[n] == '\\';
}

/* May the running process read (or WRITE) the canonical path? */
int accounts_access(const char *canonical, int write) {
    if (accounts_admin()) return 0;
    if (rt_upper((u8)canonical[0]) != rt_upper((u8)ctx->drive) || canonical[1] != ':') return 0;
    const char *path = canonical + 2;
    struct account *a = find(current->user);
    if (a && a->user.home[0] && under(canonical, a->user.home)) return 0;
    if (under(path, DB)) return -DOSXC_EACCES;
    if (under(path, "\\Users\\Public")) return 0;
    if (under(path, "\\Users") && path[6] == '\\') return -DOSXC_EACCES;
    if (!write) return 0;
    static const char *const system_folders[] = {"\\SYSTEM", "\\Programs", "\\Packages", "\\EFI", "\\AUTOEXEC.BAT"};
    for (unsigned i = 0; i < sizeof(system_folders) / sizeof(*system_folders); ++i)
        if (under(path, system_folders[i])) return -DOSXC_EACCES;
    return 0;
}

static void public_view(const struct account *a, struct dosxc_user *out) {
    memcpy(out, &a->user, sizeof(*out));
}

static int login(const char *name, const char *password) {
    if (!enabled) return -DOSXC_EINVAL;
    if (current->user[0]) return -DOSXC_EPERM;
    struct account *a = find(name);
    if (!a || !password_ok(a, password)) return -DOSXC_EACCES;
    for (struct process *p = current; p; p = p->parent) {
        rt_strcpy(p->user, a->user.name);
    }
    make_folder(a->user.home);
    env_set("HOME", a->user.home);
    env_set("USERNAME", a->user.name);
    return 0;
}

static int change(const struct dosxc_user_change *c, int adding) {
    int admin = managing();
    if (adding) {
        if (!admin) return -DOSXC_EPERM;
        if (!valid_name(c->user.name) || find(c->user.name)) return -DOSXC_EINVAL;
        if (account_count == MAX_ACCOUNTS) return -DOSXC_ENOSPC;
        if (!account_count && !c->user.admin) return -DOSXC_EINVAL;
    }
    struct account *a = adding ? &accounts[account_count] : find(c->user.name);
    if (!a) return -DOSXC_ENOENT;
    int self = current && rt_equal_nocase(current->user, c->user.name);
    int fields = adding ? 0x3f : c->fields;
    if (!admin && (!self || (fields & (DOSXC_USER_SET_HOME | DOSXC_USER_SET_ADMIN)))) return -DOSXC_EPERM;
    if ((fields & DOSXC_USER_SET_NICKNAME) && !valid_text(c->user.nickname, sizeof(c->user.nickname))) return -DOSXC_EINVAL;
    if ((fields & DOSXC_USER_SET_PICTURE) && !valid_text(c->user.picture, sizeof(c->user.picture))) return -DOSXC_EINVAL;
    if ((fields & DOSXC_USER_SET_HOME) && !valid_text(c->user.home, sizeof(c->user.home))) return -DOSXC_EINVAL;
    if ((fields & DOSXC_USER_SET_PASSWORD) && !adding && !admin && !password_ok(a, c->old_password)) return -DOSXC_EACCES;
    if (!adding && (fields & DOSXC_USER_SET_ADMIN) && !c->user.admin && a->user.admin) {
        int admins = 0;
        for (int i = 0; i < account_count; ++i) admins += accounts[i].user.admin;
        if (admins < 2) return -DOSXC_EBUSY;
    }
    if (adding) {
        memset(a, 0, sizeof(*a));
        rt_strcpy(a->user.name, c->user.name);
        ++account_count;
        enabled = 1;
    }
    if (fields & DOSXC_USER_SET_NICKNAME) rt_strcpy(a->user.nickname, c->user.nickname[0] ? c->user.nickname : a->user.name);
    if (fields & DOSXC_USER_SET_PICTURE) rt_strcpy(a->user.picture, c->user.picture);
    if (fields & DOSXC_USER_SET_HOME) {
        if (c->user.home[0]) rt_strcpy(a->user.home, c->user.home);
        else default_home(a->user.name, a->user.home);
    }
    if (fields & DOSXC_USER_SET_ADMIN) a->user.admin = c->user.admin != 0;
    if (fields & DOSXC_USER_SET_START) a->user.start = c->user.start;
    if (fields & DOSXC_USER_SET_PASSWORD) set_password(a, c->password);
    make_folder(a->user.home);
    return save();
}

static int remove_account(const char *name) {
    if (!managing()) return -DOSXC_EPERM;
    struct account *a = find(name);
    if (!a) return -DOSXC_ENOENT;
    if (current && rt_equal_nocase(current->user, name)) return -DOSXC_EBUSY;
    int admins = 0;
    for (int i = 0; i < account_count; ++i) admins += accounts[i].user.admin;
    if (a->user.admin && admins < 2) return -DOSXC_EBUSY;
    uptr at = (uptr)(a - accounts);
    memmove(a, a + 1, (account_count - at - 1) * sizeof(*a));
    --account_count;
    return save();
}


long user_syscall(int operation, const void *a, void *b) {
    load();
    if (!current) return -DOSXC_EINVAL;
    switch (operation) {
    case DOSXC_USER_ENABLED: return enabled;
    case DOSXC_USER_LIST: {
        uptr index = (uptr)a;
        if (b && index < (uptr)account_count) public_view(&accounts[index], b);
        return account_count;
    }
    case DOSXC_USER_WHOAMI: {
        struct dosxc_user *out = b;
        memset(out, 0, sizeof(*out));
        struct account *me = current->user[0] ? find(current->user) : 0;
        if (me) public_view(me, out);
        else {
            rt_strcpy(out->nickname, enabled ? "System" : "Public");
            out->admin = 1;
            out->home[0] = (char)rt_upper(ctx->drive);
            out->home[1] = ':';
            rt_strcpy(out->home + 2, "\\Users\\Public");
        }
        return 0;
    }
    case DOSXC_USER_LOGIN: {
        int r = login(a, b);
        if (r) event_log(DOSXC_EVENT_WARNING, "Accounts", "Failed login as %s", (const char *)a);
        else event_log(DOSXC_EVENT_INFO, "Accounts", "%s logged in", (const char *)a);
        return r;
    }
    case DOSXC_USER_CHECK: {
        struct account *x = find(a);
        return x && password_ok(x, b) ? 0 : -DOSXC_EACCES;
    }
    case DOSXC_USER_ADD: case DOSXC_USER_SET: {
        const struct dosxc_user_change *c = b;
        int r = change(c, operation == DOSXC_USER_ADD);
        if (!r) event_log(DOSXC_EVENT_INFO, "Accounts", operation == DOSXC_USER_ADD ? "Account %s was added" : "Account %s was changed", c->user.name);
        return r;
    }
    case DOSXC_USER_REMOVE: {
        int r = remove_account(a);
        if (!r) event_log(DOSXC_EVENT_INFO, "Accounts", "Account %s was removed", (const char *)a);
        return r;
    }
    case DOSXC_USER_UNLOCK: {
        struct account *x = find(a);
        if (!x || !x->user.admin || !password_ok(x, b)) {
            event_log(DOSXC_EVENT_WARNING, "Accounts", "Failed unlock of account management as %s", (const char *)a);
            return -DOSXC_EACCES;
        }
        current->unlocked = 1;
        event_log(DOSXC_EVENT_INFO, "Accounts", "%s unlocked account management", x->user.name);
        return 0;
    }
    case DOSXC_USER_LOCK: current->unlocked = 0; return 0;
    case DOSXC_USER_ENABLE: return a && enabled ? 0 : -DOSXC_EPERM;
    }
    return -DOSXC_EINVAL;
}
