/*
 * DOS-XC /batch/batch.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <ctype.h>
#include <dosxc/syscall.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINE_LIMIT 1024
static char *text;
static size_t size, ip;
static char *args[64];
static int arg_count, shift;
static int echo_on = 1, errorlevel;
static size_t returns[16];
static int return_count;

static int same(const char *a, const char *b) { return !strcasecmp(a, b); }
static char *blanks(char *s) { while (*s == ' ' || *s == '\t') ++s; return s; }

static char *word(char *s, char *out, size_t cap) {
    s = blanks(s);
    size_t n = 0;
    int quoted = 0;
    while (*s && (quoted || (*s != ' ' && *s != '\t'))) {
        if (*s == '"') { quoted = !quoted; ++s; continue; }
        if (n + 1 < cap) out[n++] = *s;
        ++s;
    }
    out[n] = 0;
    return blanks(s);
}

static const char *arg(int i) {
    i += shift;
    return i >= 0 && i < arg_count ? args[i] : "";
}

/* %0..%9, %*, %%, %NAME%, %ERRORLEVEL%, %CD%; LOOP is a FOR variable. */
static void expand(const char *in, char *out, size_t cap, char loop, const char *loop_value) {
    size_t n = 0;
    while (*in && n + 1 < cap) {
        if (*in != '%') { out[n++] = *in++; continue; }
        const char *value = 0;
        char buffer[4096];
        if (in[1] == '%' && loop && toupper((unsigned char)in[2]) == toupper((unsigned char)loop)) { value = loop_value; in += 3; }
        else if (in[1] == '%') { out[n++] = '%'; in += 2; continue; }
        else if (in[1] >= '0' && in[1] <= '9') { value = arg(in[1] - '0'); in += 2; }
        else if (in[1] == '*') {
            buffer[0] = 0;
            for (int i = 1 + shift; i < arg_count; ++i) {
                if (strlen(buffer) + strlen(args[i]) + 2 >= sizeof(buffer)) break;
                if (i > 1 + shift) strcat(buffer, " ");
                strcat(buffer, args[i]);
            }
            value = buffer;
            in += 2;
        } else {
            const char *end = strchr(in + 1, '%');
            char name[256];
            if (!end || (size_t)(end - in - 1) >= sizeof(name)) { out[n++] = *in++; continue; }
            memcpy(name, in + 1, (size_t)(end - in - 1));
            name[end - in - 1] = 0;
            in = end + 1;
            if (same(name, "ERRORLEVEL")) snprintf(buffer, sizeof(buffer), "%d", errorlevel);
            else if (same(name, "CD")) { if (dosxc_sys_fullpath(".", buffer, sizeof(buffer)) < 0) buffer[0] = 0; }
            else if (dosxc_sys_environ(DOSXC_ENV_GET, name, buffer, sizeof(buffer)) < 0) buffer[0] = 0;
            value = buffer;
        }
        while (value && *value && n + 1 < cap) out[n++] = *value++;
    }
    out[n] = 0;
}

static size_t find_label(const char *wanted) {
    size_t p = 0;
    while (p < size) {
        size_t start = p;
        while (p < size && text[p] != '\n') ++p;
        size_t end = p;
        if (p < size) ++p;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
        if (start < end && text[start] == ':' && (start + 1 == end || text[start + 1] != ':')) {
            char name[256];
            size_t n = 0;
            ++start;
            while (start < end && text[start] != '\r' && text[start] != ' ' && n + 1 < sizeof(name)) name[n++] = text[start++];
            name[n] = 0;
            if (same(name, wanted)) return p;
        }
    }
    return (size_t)-1;
}

/* Run LINE through the command shell; returns its errorlevel. */
static int shell(const char *line) {
    char comspec[300];
    if (dosxc_sys_environ(DOSXC_ENV_GET, "COMSPEC", comspec, sizeof(comspec)) < 0) strcpy(comspec, "\\SYSTEM\\COMMAND.EXE");
    char *argv[] = {"COMMAND.EXE", "/C", (char *)line, 0};
    fflush(stdout);
    int status = dosxc_sys_spawn(comspec, argv, 0);
    if (status < 0) { printf("Cannot run the command shell (%s).\n", comspec); return 1; }
    return status & 0x7f ? 128 + (status & 0x7f) : (status >> 8) & 0xff;
}

static int change_directory(const char *target) {
    char path[300];
    snprintf(path, sizeof(path), "%s", target);
    if (strlen(path) == 2 && path[1] == ':') strcat(path, "\\");
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode) || chdir(path)) {
        puts("The system cannot find the path specified.");
        return 1;
    }
    dosxc_sys_shell(DOSXC_SHELL_CHDIR, path, 0);
    return 0;
}

static int command(char *line, int depth);

static int wildcard(const char *pattern, const char *text) {
    if (!strcmp(pattern, "*.*")) pattern = "*";
    while (*pattern) {
        if (*pattern == '*') {
            do if (wildcard(pattern + 1, text)) return 1; while (*text++);
            return 0;
        }
        if (!*text || (*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*text))) return 0;
        ++pattern;
        ++text;
    }
    return !*text;
}

/* FOR %%X IN (set) DO command   (the set may use wildcards) */
static int for_loop(char *rest) {
    char variable[16], keyword[16];
    rest = word(rest, variable, sizeof(variable));
    rest = word(rest, keyword, sizeof(keyword));
    char letter = variable[0] == '%' ? variable[variable[1] == '%' ? 2 : 1] : 0;
    char *open = strchr(rest, '('), *close = open ? strchr(open, ')') : 0;
    if (!letter || !same(keyword, "IN") || !open || !close) { puts("FOR %%X IN (set) DO command"); return 1; }
    *close = 0;
    char *after = blanks(close + 1), doword[8];
    after = word(after, doword, sizeof(doword));
    if (!same(doword, "DO")) { puts("FOR %%X IN (set) DO command"); return 1; }
    char set[LINE_LIMIT];
    expand(open + 1, set, sizeof(set), 0, 0);
    char *p = set;
    int r = 0;
    while (*(p = blanks(p))) {
        char item[300];
        p = word(p, item, sizeof(item));
        if (strchr(item, '*') || strchr(item, '?')) {
            char folder[300] = ".";
            const char *pattern = item;
            char *cut = strrchr(item, '\\');
            if (cut) { *cut = 0; snprintf(folder, sizeof(folder), "%s", *item ? item : "\\"); pattern = cut + 1; }
            struct dosxc_dirent *entries = malloc(sizeof(*entries) * 512);
            int count = entries ? (int)dosxc_sys_listdir(folder, entries, 512) : 0;
            for (int i = 0; i < count && i < 512; ++i) {
                if (entries[i].attributes & DOSXC_ATTR_DIRECTORY) continue;
                int match = wildcard(pattern, entries[i].name);
                if (!match) continue;
                char name[600], expanded[LINE_LIMIT];
                snprintf(name, sizeof(name), "%s%s%s", cut ? folder : "", cut && strlen(folder) > 1 ? "\\" : "", entries[i].name);
                expand(after, expanded, sizeof(expanded), letter, name);
                r = command(expanded, 1);
            }
            free(entries);
        } else {
            char expanded[LINE_LIMIT];
            expand(after, expanded, sizeof(expanded), letter, item);
            r = command(expanded, 1);
        }
    }
    return r;
}

static int stop;
static int command(char *line, int depth) {
    char name[300];
    char *p = blanks(line);
    if (*p == '@') p = blanks(p + 1);
    if (!*p || *p == ':') return errorlevel;
    char *rest = word(p, name, sizeof(name));
    if (same(name, "REM")) return errorlevel;
    if (same(name, "ECHO") || !strncasecmp(name, "ECHO.", 5)) {
        if (same(rest, "ON")) echo_on = 1;
        else if (same(rest, "OFF")) echo_on = 0;
        else if (!*rest && name[4] != '.') printf("ECHO is %s.\n", echo_on ? "on" : "off");
        else if (strpbrk(rest, "<>|")) return errorlevel = shell(p);
        else puts(name[4] == '.' ? p + 5 : rest);
        return errorlevel = 0;
    }
    if (same(name, "GOTO")) {
        char label[256];
        word(rest, label, sizeof(label));
        const char *target = label[0] == ':' ? label + 1 : label;
        if (same(target, "EOF")) {
            if (return_count) ip = returns[--return_count];
            else stop = 1;
            return errorlevel;
        }
        size_t at = find_label(target);
        if (at == (size_t)-1) { printf("Label not found: %s\n", target); stop = 1; return errorlevel = 1; }
        ip = at;
        return errorlevel;
    }
    if (same(name, "CALL")) {
        rest = blanks(rest);
        if (*rest == ':') {
            char label[256];
            word(rest + 1, label, sizeof(label));
            size_t at = find_label(label);
            if (at == (size_t)-1 || return_count == 16) { printf("Label not found: %s\n", label); return errorlevel = 1; }
            returns[return_count++] = ip;
            ip = at;
            return errorlevel;
        }
        return errorlevel = shell(rest);
    }
    if (same(name, "IF")) {
        char a[300];
        int negate = 0, condition;
        rest = word(rest, a, sizeof(a));
        if (same(a, "NOT")) { negate = 1; rest = word(rest, a, sizeof(a)); }
        if (same(a, "EXIST")) {
            rest = word(rest, a, sizeof(a));
            struct stat st;
            condition = !stat(a, &st);
        } else if (same(a, "ERRORLEVEL")) {
            rest = word(rest, a, sizeof(a));
            condition = errorlevel >= atoi(a);
        } else if (same(a, "DEFINED")) {
            char value[8];
            rest = word(rest, a, sizeof(a));
            condition = dosxc_sys_environ(DOSXC_ENV_GET, a, value, sizeof(value)) != -DOSXC_ENOENT;
        } else {
            char *eq = strstr(a, "==");
            char left[300], right[300];
            if (eq) {
                *eq = 0;
                snprintf(left, sizeof(left), "%s", a);
                if (eq[2]) snprintf(right, sizeof(right), "%s", eq + 2);
                else rest = word(rest, right, sizeof(right));
            } else {
                snprintf(left, sizeof(left), "%s", a);
                rest = blanks(rest);
                if (strncmp(rest, "==", 2)) { puts("IF [NOT] EXIST file | ERRORLEVEL n | DEFINED name | a==b  command"); return errorlevel = 1; }
                rest = word(rest + 2, right, sizeof(right));
            }
            condition = !strcmp(left, right);
        }
        if (condition != negate) return command(rest, depth + 1);
        return errorlevel;
    }
    if (same(name, "FOR")) return errorlevel = for_loop(rest);
    if (same(name, "SHIFT")) { ++shift; return errorlevel; }
    if (same(name, "PAUSE")) {
        fputs("Press any key to continue . . . ", stdout);
        fflush(stdout);
        dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
        putchar('\n');
        return errorlevel;
    }
    if (same(name, "EXIT")) {
        char value[32] = "";
        rest = blanks(rest);
        if (!strncasecmp(rest, "/B", 2)) rest = blanks(rest + 2);
        word(rest, value, sizeof(value));
        if (*value) errorlevel = atoi(value);
        if (return_count && !strncasecmp(blanks(p + 4), "/B", 2)) ip = returns[--return_count];
        else stop = 1;
        return errorlevel;
    }
    if (same(name, "CD") || same(name, "CHDIR")) {
        char target[300];
        word(rest, target, sizeof(target));
        if (!*target) { char cwd[300]; if (dosxc_sys_fullpath(".", cwd, sizeof(cwd)) >= 0) puts(cwd); return errorlevel = 0; }
        return errorlevel = change_directory(target);
    }
    if (strlen(name) == 2 && name[1] == ':' && isalpha((unsigned char)name[0]) && !*rest) return errorlevel = change_directory(name);
    if (same(name, "SET") && *rest && strchr(rest, '=') && !strpbrk(rest, "<>|")) {
        char *eq = strchr(rest, '=');
        *eq = 0;
        int r = dosxc_sys_environ(DOSXC_ENV_SET, rest, eq[1] ? eq + 1 : 0, 0);
        if (r < 0) puts("Out of environment space.");
        return errorlevel = r < 0;
    }
    (void)depth;
    return errorlevel = shell(p);
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "/?")) {
        puts("BATCH script.bat [arguments]   Runs a batch file (the shell does this for .BAT files).");
        return argc < 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("Cannot read the batch file %s.\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = malloc((size_t)length + 1);
    if (!text || fread(text, 1, (size_t)length, f) != (size_t)length) { puts("Cannot read the batch file."); return 1; }
    fclose(f);
    size = (size_t)length;
    text[size] = 0;
    for (int i = 1; i < argc && arg_count < 64; ++i) args[arg_count++] = argv[i];
    char raw[LINE_LIMIT], line[LINE_LIMIT * 4];
    char prompt[300];
    while (ip < size && !stop) {
        size_t n = 0;
        while (ip < size && text[ip] != '\n') {
            if (text[ip] != '\r' && n + 1 < sizeof(raw)) raw[n++] = text[ip];
            ++ip;
        }
        if (ip < size) ++ip;
        raw[n] = 0;
        if (n && raw[n - 1] == 26) raw[--n] = 0;
        char *start = blanks(raw);
        if (*start == '@') start = blanks(start + 1);
        if (!strncasecmp(start, "FOR", 3) && (start[3] == ' ' || start[3] == '\t')) snprintf(line, sizeof(line), "%s", raw);
        else expand(raw, line, sizeof(line), 0, 0);
        char *p = blanks(line);
        if (!*p || *p == ':') continue;
        if (echo_on && *p != '@') {
            if (dosxc_sys_fullpath(".", prompt, sizeof(prompt)) < 0) strcpy(prompt, "?");
            printf("%s>%s\n", prompt, p);
        }
        command(p, 0);
    }
    return errorlevel;
}
