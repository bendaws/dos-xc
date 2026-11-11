/*
 * DOS-XC /os/libc/src/pwd.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <pwd.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static struct passwd user;
static char home[260], shell[260];

static struct passwd *entry(void) {
    const char *h = getenv("HOME"), *s = getenv("COMSPEC");
    strncpy(home, h ? h : "/", sizeof(home) - 1);
    strncpy(shell, s ? s : "/SYSTEM/COMMAND.EXE", sizeof(shell) - 1);
    user.pw_name = "user";
    user.pw_passwd = "";
    user.pw_uid = 0;
    user.pw_gid = 0;
    user.pw_gecos = "DOS-XC user";
    user.pw_dir = home;
    user.pw_shell = shell;
    return &user;
}

struct passwd *getpwnam(const char *name) { return name && (!strcmp(name, "user") || !strcmp(name, "root")) ? entry() : 0; }
struct passwd *getpwuid(uid_t uid) { return uid == 0 ? entry() : 0; }

static int copy_out(struct passwd *found, struct passwd *out, char *buffer, size_t size, struct passwd **result) {
    *result = 0;
    if (!found) return 0;
    size_t need = strlen(found->pw_dir) + strlen(found->pw_shell) + 2;
    if (size < need) return ERANGE;
    *out = *found;
    out->pw_dir = strcpy(buffer, found->pw_dir);
    out->pw_shell = strcpy(buffer + strlen(found->pw_dir) + 1, found->pw_shell);
    *result = out;
    return 0;
}
int getpwnam_r(const char *name, struct passwd *out, char *buffer, size_t size, struct passwd **result) {
    return copy_out(getpwnam(name), out, buffer, size, result);
}
int getpwuid_r(uid_t uid, struct passwd *out, char *buffer, size_t size, struct passwd **result) {
    return copy_out(getpwuid(uid), out, buffer, size, result);
}

static int enumerated;
struct passwd *getpwent(void) { return enumerated++ ? 0 : entry(); }
void setpwent(void) { enumerated = 0; }
void endpwent(void) { enumerated = 0; }

static char *members[] = {"user", 0};
static struct group group = {"users", "", 0, members};
struct group *getgrnam(const char *name) { return name && !strcmp(name, "users") ? &group : 0; }
struct group *getgrgid(gid_t gid) { return gid == 0 ? &group : 0; }
