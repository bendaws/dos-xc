/*
 * DOS-XC /os/shell/command.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <ctype.h>
#include <dosxc/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "logo.h"

#define LINE_MAX_SHELL 1024
#define HISTORY 32

static int errorlevel;
static int interactive;
static int start_desktop(int ask);
static char system_drive = 'C';
static int exit_requested;

/* ---- small helpers ------------------------------------------------------ */

static void say(const char *text) { fputs(text, stdout); }

static int same(const char *a, const char *b) { return !strcasecmp(a, b); }

static int starts(const char *text, const char *prefix) { return !strncasecmp(text, prefix, strlen(prefix)); }

static char *skip_blanks(char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

static int env_get(const char *name, char *buffer, size_t size) {
    int r = dosxc_sys_environ(DOSXC_ENV_GET, name, buffer, size);
    if (r < 0) buffer[0] = 0;
    return r;
}

static int env_set(const char *name, const char *value) {
    return dosxc_sys_environ(DOSXC_ENV_SET, name, (char *)value, 0);
}

/* "C:\DIR" for the current directory. */
static void current_directory(char *out, size_t size) {
    if (dosxc_sys_fullpath(".", out, size) < 0) snprintf(out, size, "%c:\\", system_drive);
}

static int is_directory(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int is_file(const char *path) {
    struct stat st;
    return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

#define DESKTOP_FOLDER "\\Programs\\DOS-XC Desktop"
static int find_desktop_file(const char *relative, char *out, size_t size) {
    snprintf(out, size, "%c:" DESKTOP_FOLDER "\\%s", system_drive, relative);
    return is_file(out);
}

/* ---- welcome ------------------------------------------------------------ */

static void decimal_tenths(char *out, size_t size, unsigned long long value, unsigned long long unit) {
    unsigned long long tenths = (value * 10 + unit / 2) / unit;
    snprintf(out, size, "%llu.%llu", tenths / 10, tenths % 10);
}

static void welcome(void) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    dosxc_sys_console(DOSXC_CONSOLE_CLEAR, 0, 0);
    say("\n");
    for (int i = 0; i < LOGO_LINES; ++i) {
        say("  ");
        say(logo[i]);
        say("\n");
    }
    unsigned long long total = info.memory_total >> 20, used = (info.memory_total - info.memory_free) >> 20;
    const char *version = info.version;
    if (!strncmp(version, "DOS-XC v", 8)) version += 8;
    printf("\n  DOS-XC %s\n\n", version);
    struct dosxc_user me;
    if (dosxc_sys_user(DOSXC_USER_ENABLED, 0, 0) > 0 && !dosxc_sys_user(DOSXC_USER_WHOAMI, 0, &me) && me.name[0])
        printf("  Logged in as %s\n", me.nickname);
    printf("  Memory: %llu MB/%llu MB\n", used, total);
    struct dosxc_volume volumes[26];
    int count = dosxc_sys_volumes(volumes, 26);
    for (int i = 0; i < count && i < 26; ++i) {
        if (!(volumes[i].flags & DOSXC_VOLUME_SYSTEM)) continue;
        char a[32], b[32];
        decimal_tenths(a, sizeof(a), volumes[i].total_bytes - volumes[i].free_bytes, 1000000000ull);
        decimal_tenths(b, sizeof(b), volumes[i].total_bytes, 1000000000ull);
        printf("  Storage: %s GB/%s GB\n", a, b);
    }
    if (info.live) say("\n  To run DOS-XC Setup, type SETUP and press ENTER.\n");
    say("\n");
}

static void build_path(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%c:\\SYSTEM", system_drive);
    char programs[64];
    snprintf(programs, sizeof(programs), "%c:\\Programs", system_drive);
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 256);
    if (!entries) return;
    int count = (int)dosxc_sys_listdir(programs, entries, 256);
    for (int i = 0; i < count && i < 256; ++i) {
        if (!(entries[i].attributes & DOSXC_ATTR_DIRECTORY)) continue;
        char inf[600];
        snprintf(inf, sizeof(inf), "%s\\%s\\PACKAGE.INF", programs, entries[i].name);
        FILE *f = fopen(inf, "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (starts(line, "Env=") && strchr(line + 4, '=')) {
                char value[600], folder[300];
                snprintf(folder, sizeof(folder), "%c:/Programs/%s", system_drive, entries[i].name);
                char *equals = strchr(line + 4, '=');
                *equals = 0;
                size_t n = 0;
                for (const char *v = equals + 1; *v && n + 1 < sizeof(value);) {
                    if (starts(v, "%PACKAGE%")) {
                        for (const char *f = folder; *f && n + 1 < sizeof(value); ++f) value[n++] = *f;
                        v += 9;
                    } else value[n++] = *v++;
                }
                value[n] = 0;
                env_set(line + 4, value);
                continue;
            }
            if (!starts(line, "Path=")) continue;
            char *p = line + 5;
            while (*p) {
                char part[256];
                size_t n = 0;
                while (*p && *p != ';') {
                    if (n + 1 < sizeof(part)) part[n++] = *p;
                    ++p;
                }
                part[n] = 0;
                if (*p) ++p;
                if (!n) continue;
                char dir[600];
                if (!strcmp(part, ".")) snprintf(dir, sizeof(dir), "%s\\%s", programs, entries[i].name);
                else snprintf(dir, sizeof(dir), "%s\\%s\\%s", programs, entries[i].name, part);
                if (strlen(path) + strlen(dir) + 2 < sizeof(path)) {
                    strcat(path, ";");
                    strcat(path, dir);
                }
            }
        }
        fclose(f);
    }
    free(entries);
    env_set("PATH", path);
}

static char history[HISTORY][LINE_MAX_SHELL];
static int history_count;

static void remember(const char *line) {
    if (!*line) return;
    if (history_count && !strcmp(history[(history_count - 1) % HISTORY], line)) return;
    strncpy(history[history_count % HISTORY], line, LINE_MAX_SHELL - 1);
    ++history_count;
}

static int show_notices(void) {
    char text[100];
    int any = 0;
    while (dosxc_sys_drive(DOSXC_DRIVE_NOTICE, 0, text, sizeof(text)) > 0) {
        printf("%s\n", text);
        any = 1;
    }
    if (any) fflush(stdout);
    return any;
}

/* The next key; a drive notice arriving meanwhile is printed above the
   line being edited (*NOTICE is set so the caller redraws it). */
static int key(int *notice) {
    for (unsigned idle = 0;; ++idle) {
        int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 0, 0);
        if (k >= 0) return k;
        if (notice && idle % 8 == 0) {
            char text[100];
            int n = dosxc_sys_drive(DOSXC_DRIVE_NOTICE, 0, text, sizeof(text));
            if (n > 0) {
                printf("\r\n%s\n", text);
                show_notices();
                *notice = 1;
                return -1;
            }
        }
        dosxc_sys_sleep(5000000);
    }
}

static void redraw(const char *line, size_t length, size_t cursor, size_t *shown) {
    printf("\r");
    char prompt_text[300];
    extern void prompt_string(char *, size_t);
    prompt_string(prompt_text, sizeof(prompt_text));
    fputs(prompt_text, stdout);
    fwrite(line, 1, length, stdout);
    size_t total = strlen(prompt_text) + length;
    if (*shown > total) {
        for (size_t i = total; i < *shown; ++i) putchar(' ');
        for (size_t i = total; i < *shown; ++i) putchar('\b');
    }
    for (size_t i = cursor; i < length; ++i) putchar('\b');
    *shown = total;
    fflush(stdout);
}

/* Complete the word before the cursor with a matching file name. */
static void complete(char *line, size_t *length, size_t *cursor) {
    size_t start = *cursor;
    int quoted = 0;
    for (size_t i = 0; i < *cursor; ++i) if (line[i] == '"') quoted = !quoted;
    while (start && (quoted ? line[start - 1] != '"' : line[start - 1] != ' ')) --start;
    char word[LINE_MAX_SHELL], folder[LINE_MAX_SHELL], stem[LINE_MAX_SHELL];
    size_t n = *cursor - start;
    memcpy(word, line + start, n);
    word[n] = 0;
    char *slash = 0;
    for (char *p = word; *p; ++p) if (*p == '\\' || *p == '/' || (*p == ':' && p == word + 1)) slash = p;
    if (slash) {
        memcpy(folder, word, (size_t)(slash - word) + 1);
        folder[slash - word + 1] = 0;
        strcpy(stem, slash + 1);
    } else {
        strcpy(folder, "");
        strcpy(stem, word);
    }
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 512);
    if (!entries) return;
    int count = (int)dosxc_sys_listdir(*folder ? folder : ".", entries, 512);
    char best[260] = "";
    int matches = 0, best_dir = 0;
    for (int i = 0; i < count && i < 512; ++i) {
        if (strncasecmp(entries[i].name, stem, strlen(stem))) continue;
        if (!matches++) {
            strcpy(best, entries[i].name);
            best_dir = (entries[i].attributes & DOSXC_ATTR_DIRECTORY) != 0;
        } else {
            size_t k = 0;
            while (best[k] && tolower((unsigned char)best[k]) == tolower((unsigned char)entries[i].name[k])) ++k;
            best[k] = 0;
            best_dir = 0;
        }
    }
    free(entries);
    if (!matches || strlen(best) < strlen(stem)) return;
    char replacement[LINE_MAX_SHELL];
    snprintf(replacement, sizeof(replacement), "%s%s%s", folder, best, matches == 1 && best_dir ? "\\" : "");
    size_t add = strlen(replacement);
    if (*length - n + add + 1 >= LINE_MAX_SHELL) return;
    memmove(line + start + add, line + *cursor, *length - *cursor);
    memcpy(line + start, replacement, add);
    *length = *length - n + add;
    *cursor = start + add;
}

static int read_line(char *line) {
    size_t length = 0, cursor = 0, shown = 0;
    int browsing = history_count;
    char saved[LINE_MAX_SHELL] = "";
    fflush(stdout);
    char prompt_text[300];
    extern void prompt_string(char *, size_t);
    prompt_string(prompt_text, sizeof(prompt_text));
    shown = strlen(prompt_text);
    for (;;) {
        int notice = 0;
        int k = key(&notice);
        if (notice) {
            shown = 0;
            redraw(line, length, cursor, &shown);
            continue;
        }
        if (k == '\n' || k == '\r') break;
        if (k == 3) {
            say("^C\n");
            line[0] = 0;
            return 0;
        }
        if (k == 27) { length = cursor = 0; }
        else if (k == '\b') {
            if (!cursor) continue;
            memmove(line + cursor - 1, line + cursor, length - cursor);
            --cursor;
            --length;
        } else if (k == 0x108) {                       /* Delete */
            if (cursor == length) continue;
            memmove(line + cursor, line + cursor + 1, length - cursor - 1);
            --length;
        } else if (k == 0x104) { if (cursor) --cursor; else continue; }
        else if (k == 0x103) { if (cursor < length) ++cursor; else continue; }
        else if (k == 0x105) cursor = 0;
        else if (k == 0x106) cursor = length;
        else if (k == 0x101 || k == 0x102) {           /* history */
            int first = history_count > HISTORY ? history_count - HISTORY : 0;
            if (browsing == history_count) { memcpy(saved, line, length); saved[length] = 0; }
            if (k == 0x101 && browsing > first) --browsing;
            else if (k == 0x102 && browsing < history_count) ++browsing;
            else continue;
            const char *text = browsing == history_count ? saved : history[browsing % HISTORY];
            length = cursor = strlen(text);
            memcpy(line, text, length);
        } else if (k == '\t') complete(line, &length, &cursor);
        else if (k >= 32 && k < 256 && k != 127) {
            if (length + 1 >= LINE_MAX_SHELL) continue;
            memmove(line + cursor + 1, line + cursor, length - cursor);
            line[cursor++] = (char)k;
            ++length;
            if (cursor == length) {
                putchar(k);
                fflush(stdout);
                ++shown;
                continue;
            }
        } else continue;
        redraw(line, length, cursor, &shown);
    }
    line[length] = 0;
    say("\n");
    return 1;
}

/* ---- prompt ---------------------------------------------------------------- */

void prompt_string(char *out, size_t size) {
    char format[256], cwd[300];
    if (env_get("PROMPT", format, sizeof(format)) < 0 || !*format) strcpy(format, "$P$G");
    current_directory(cwd, sizeof(cwd));
    size_t n = 0;
    for (const char *p = format; *p && n + 1 < size; ++p) {
        const char *insert = 0;
        char one[2] = {0, 0};
        if (*p != '$' || !p[1]) { out[n++] = *p; continue; }
        switch (toupper((unsigned char)*++p)) {
        case 'P': insert = cwd; break;
        case 'N': one[0] = cwd[0]; insert = one; break;
        case 'G': insert = ">"; break;
        case 'L': insert = "<"; break;
        case 'B': insert = "|"; break;
        case 'Q': insert = "="; break;
        case '$': insert = "$"; break;
        case '_': insert = "\n"; break;
        case 'E': insert = "\033"; break;
        case 'V': insert = "DOS-XC"; break;
        case 'S': insert = " "; break;
        default: insert = ""; break;
        }
        while (*insert && n + 1 < size) out[n++] = *insert++;
    }
    out[n] = 0;
}

/* ---- command lines ---------------------------------------------------------- */

/* Replace %NAME% with environment values (%% is a percent sign). */
static void expand(const char *in, char *out, size_t size) {
    size_t n = 0;
    while (*in && n + 1 < size) {
        if (*in != '%') { out[n++] = *in++; continue; }
        const char *end = strchr(in + 1, '%');
        if (in[1] == '%') { out[n++] = '%'; in += 2; continue; }
        if (!end || end == in + 1) { out[n++] = *in++; continue; }
        char name[256], value[4096];
        size_t length = (size_t)(end - in - 1);
        if (length >= sizeof(name)) { out[n++] = *in++; continue; }
        memcpy(name, in + 1, length);
        name[length] = 0;
        if (same(name, "ERRORLEVEL")) snprintf(value, sizeof(value), "%d", errorlevel);
        else if (same(name, "CD")) current_directory(value, sizeof(value));
        else if (env_get(name, value, sizeof(value)) < 0) { out[n++] = *in++; continue; }
        for (const char *v = value; *v && n + 1 < size; ++v) out[n++] = *v;
        in = end + 1;
    }
    out[n] = 0;
}

/* Split off the first word (quotes group; they are removed). */
static char *word(char *s, char *out, size_t size) {
    s = skip_blanks(s);
    size_t n = 0;
    int quoted = 0;
    while (*s && (quoted || (*s != ' ' && *s != '\t'))) {
        if (*s == '"') { quoted = !quoted; ++s; continue; }
        if (n + 1 < size) out[n++] = *s;
        ++s;
    }
    out[n] = 0;
    return skip_blanks(s);
}

static int has_extension(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext);
    return a >= b && same(name + a - b, ext);
}

static int try_candidates(const char *base, char *found) {
    static const char *const extensions[] = {"", ".COM", ".EXE", ".BAT"};
    const char *dot = strrchr(base, '.');
    const char *sep = strrchr(base, '\\');
    if (!sep || (strrchr(base, '/') && strrchr(base, '/') > sep)) sep = strrchr(base, '/');
    int has_ext = dot && (!sep || dot > sep);
    for (int i = has_ext ? 0 : 1; i < 4; ++i) {
        if (strlen(base) + strlen(extensions[i]) >= 300) continue;
        snprintf(found, 300, "%s%s", base, extensions[i]);
        if (is_file(found)) return 1;
        if (has_ext) break;
    }
    return 0;
}

/* Find NAME in the current folder, then on PATH. */
static int locate(const char *name, char *found) {
    if (try_candidates(name, found)) return 1;
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) return 0;
    char path[4096];
    if (env_get("PATH", path, sizeof(path)) < 0) return 0;
    char *p = path;
    while (*p) {
        char dir[300];
        size_t n = 0;
        while (*p && *p != ';') {
            if (n + 1 < sizeof(dir)) dir[n++] = *p;
            ++p;
        }
        dir[n] = 0;
        if (*p) ++p;
        if (!n) continue;
        char base[600];
        snprintf(base, sizeof(base), "%s%s%s", dir, dir[n - 1] == '\\' || dir[n - 1] == '/' ? "" : "\\", name);
        if (strlen(base) < 290 && try_candidates(base, found)) return 1;
    }
    return 0;
}

static int wait_status_level(int status) {
    if (status & 0x7f) {
        if ((status & 0x7f) == 2) say("Program interrupted.\n");
        else if ((status & 0x7f) == 6) say("Abnormal program termination.\n");
        return 128 + (status & 0x7f);
    }
    return (status >> 8) & 0xff;
}

static int run_command(char *line);

/* Programs inside a package with "StartIn=program" (16-bit games, mostly)
   run with their own folder as the current directory. */
static int starts_in_program_folder(const char *program) {
    char full[300], inf[400];
    if (dosxc_sys_fullpath(program, full, sizeof(full)) < 0) return 0;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%c:\\Programs\\", system_drive);
    if (!starts(full, prefix)) return 0;
    const char *name = full + strlen(prefix), *end = strchr(name, '\\');
    if (!end) return 0;
    snprintf(inf, sizeof(inf), "%.*s\\PACKAGE.INF", (int)(end - full), full);
    FILE *f = fopen(inf, "r");
    if (!f) return 0;
    char text[512];
    int result = 0;
    while (fgets(text, sizeof(text), f))
        if (starts(text, "StartIn=program")) result = 1;
    fclose(f);
    return result;
}

static int run_program(const char *found, const char *name, char *arguments) {
    unsigned char magic[4] = {0};
    FILE *f = fopen(found, "rb");
    if (!f) { printf("%s: cannot open the program file.\n", name); return 1; }
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    if (has_extension(found, ".BAT")) {
        char batch[300];
        if (!locate("BATCH.EXE", batch)) {
            say("The batch processor is not installed.  Install the batch package with PKG.\n");
            return 1;
        }
        char *tail = malloc(strlen(found) + strlen(arguments) + 4);
        if (!tail) return 1;
        sprintf(tail, "\"%s\" %s", found, arguments);
        int r = run_program(batch, "BATCH", tail);
        free(tail);
        return r;
    }
    if (got == 4 && !memcmp(magic, "\177ELF", 4)) {
        char *argv[256];
        char *copy = strdup(arguments);
        char program[300];
        if (!copy) return 1;
        if (dosxc_sys_resolve(found, program, sizeof(program)) < 0) strcpy(program, found);
        int argc = 0;
        argv[argc++] = program;
        char *p = copy, *out = copy;
        while (*(p = skip_blanks(p)) && argc < 255) {
            argv[argc++] = out;
            int quoted = 0;
            while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
                if (*p == '"') { quoted = !quoted; ++p; continue; }
                if (*p == '\\' && p[1] == '"') ++p;
                *out++ = *p++;
            }
            if (*p) ++p;
            *out++ = 0;
        }
        argv[argc] = 0;
        fflush(stdout);
        fflush(stderr);
        int status = dosxc_sys_spawn(found, argv, 0);
        free(copy);
        if (status < 0) {
            printf("%s: %s.\n", name, status == -DOSXC_ENOMEM ? "not enough memory" :
                   status == -DOSXC_ENOEXEC ? "not a valid program for this edition of DOS-XC" :
                   status == -DOSXC_EAGAIN ? "too many nested programs" : "cannot start program");
            return 1;
        }
        return wait_status_level(status);
    }
    if (got >= 2 && (has_extension(found, ".COM") || (magic[0] == 'M' && magic[1] == 'Z'))) {
        fflush(stdout);
        char saved[300] = "";
        int folder = starts_in_program_folder(found);
        if (folder) {
            if (!getcwd(saved, sizeof(saved))) saved[0] = 0;
            char directory[300];
            dosxc_sys_fullpath(found, directory, sizeof(directory));
            *strrchr(directory, '\\') = 0;
            if (strlen(directory) == 2) strcat(directory, "\\");
            chdir(directory);
        }
        int r;
        char vm[64];
        if (dosxc_sys_console(DOSXC_CONSOLE_VIRTUAL, 0, 0) == 1 && find_desktop_file("Apps\\DOSVM.EXE", vm, sizeof(vm))) {
            char *argv[4] = {"DOSVM.EXE", (char *)found, arguments, 0};
            int status = dosxc_sys_spawn(vm, argv, 0);
            r = status < 0 ? status : wait_status_level(status);
            if (status < 0) printf("%s: the DOS virtual machine could not be started.\n", name);
        } else {
            r = dosxc_sys_rundos(found, arguments);
            if (r == -DOSXC_ENOSYS) { say("16-bit programs need BIOS (legacy) boot.\n"); r = 1; }
            else if (r == -DOSXC_EXDEV) { printf("16-bit programs must be on drive %c:.\n", system_drive); r = 1; }
            else if (r == -DOSXC_E2BIG) { say("The command line or path is too long for a 16-bit program.\n"); r = 1; }
            else if (r < 0) { printf("%s: cannot start the 16-bit program.\n", name); r = 1; }
        }
        if (folder && *saved) chdir(saved);
        return r;
    }
    printf("%s: not a program DOS-XC can run.\n", name);
    return 1;
}

/* ---- built-in commands ---------------------------------------------------- */

static int builtin_cd(char *arguments) {
    char target[300];
    if (!strcmp(arguments, "/?")) { say("CD [/D] [drive:][path]   Shows or changes the current folder. CD .. moves up.\n"); return 0; }
    if (starts(arguments, "/D")) arguments = skip_blanks(arguments + 2);
    word(arguments, target, sizeof(target));
    if (!*target) {
        char cwd[300];
        current_directory(cwd, sizeof(cwd));
        printf("%s\n", cwd);
        return 0;
    }
    if (strlen(target) == 2 && target[1] == ':') {
        char cwd[300];
        current_directory(cwd, sizeof(cwd));
        if (toupper((unsigned char)target[0]) == cwd[0]) { printf("%s\n", cwd); return 0; }
        strcat(target, "\\");
    }
    if (!is_directory(target) || chdir(target)) {
        say("The system cannot find the path specified.\n");
        return 1;
    }
    return 0;
}

static int builtin_drive(const char *name) {
    char target[4] = {name[0], ':', '\\', 0};
    if (!is_directory(target) || chdir(target)) {
        say("The drive is not ready or does not exist.\n");
        return 1;
    }
    return 0;
}

static int builtin_set(char *arguments) {
    if (!strcmp(arguments, "/?")) {
        say("SET [name=[value]]   Shows, sets or (with no value) removes environment variables.\n");
        return 0;
    }
    if (!*arguments || !strchr(arguments, '=')) {
        int size = dosxc_sys_environ(DOSXC_ENV_LIST, 0, 0, 0);
        char *text = malloc((size_t)size + 1);
        if (!text || dosxc_sys_environ(DOSXC_ENV_LIST, 0, text, (size_t)size + 1) < 0) { free(text); return 1; }
        int shown = 0;
        for (char *p = text; *p; p += strlen(p) + 1)
            if (!*arguments || starts(p, arguments)) { printf("%s\n", p); ++shown; }
        free(text);
        if (*arguments && !shown) { printf("Environment variable %s not defined\n", arguments); return 1; }
        return 0;
    }
    char *equals = strchr(arguments, '=');
    *equals = 0;
    char *name = arguments;
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ') name[--n] = 0;
    if (!n) { say("SET name=value\n"); return 1; }
    int r = env_set(name, equals[1] ? equals + 1 : 0);
    if (r < 0) { say(r == -DOSXC_ENOMEM ? "Out of environment space.\n" : "Invalid variable name.\n"); return 1; }
    return 0;
}

static int builtin_path(char *arguments) {
    if (!strcmp(arguments, "/?")) { say("PATH [folder;folder...]   PATH ; clears it.\n"); return 0; }
    if (!*arguments) {
        char path[4096];
        if (env_get("PATH", path, sizeof(path)) < 0) say("No Path\n");
        else printf("PATH=%s\n", path);
        return 0;
    }
    if (!strcmp(arguments, ";")) return env_set("PATH", 0) < 0;
    return env_set("PATH", arguments) < 0;
}

static int echo_on = 1;
static int builtin_echo(char *arguments, char *raw) {
    if (same(arguments, "ON")) { echo_on = 1; return 0; }
    if (same(arguments, "OFF")) { echo_on = 0; return 0; }
    if (!*arguments && raw[4] != '.') { printf("ECHO is %s.\n", echo_on ? "on" : "off"); return 0; }
    printf("%s\n", raw[4] == '.' ? raw + 5 : arguments);
    return 0;
}

static int builtin_history(void) {
    int first = history_count > HISTORY ? history_count - HISTORY : 0;
    for (int i = first; i < history_count; ++i) printf("%4d  %s\n", i + 1, history[i % HISTORY]);
    return 0;
}

/* One simple command: no pipes or chaining, redirections already applied. */
static int simple_command(char *text) {
    char name[300];
    char *raw = skip_blanks(text);
    if (*raw == '@') raw = skip_blanks(raw + 1);
    if (!*raw) return errorlevel;
    char *arguments = word(raw, name, sizeof(name));
    size_t end = strlen(arguments);
    while (end && (arguments[end - 1] == ' ' || arguments[end - 1] == '\t')) arguments[--end] = 0;
    if (strlen(name) == 2 && name[1] == ':' && isalpha((unsigned char)name[0]) && !*arguments) return builtin_drive(name);
    if (same(name, "CD") || same(name, "CHDIR") || starts(name, "CD\\") || starts(name, "CD..")) {
        if (!same(name, "CD") && !same(name, "CHDIR")) return builtin_cd(raw + 2);
        return builtin_cd(arguments);
    }
    if (same(name, "SET")) return builtin_set(arguments);
    if (same(name, "PATH")) return builtin_path(arguments);
    if (same(name, "ECHO") || starts(name, "ECHO.")) return builtin_echo(arguments, raw);
    if (same(name, "REM")) return 0;
    if (same(name, "HISTORY")) return builtin_history();
    if (same(name, "LOGOUT") || same(name, "LOGOFF")) {
        if (!dosxc_sys_user(DOSXC_USER_ENABLED, 0, 0)) { say("User accounts are off: there is nobody to log out.\n"); return 1; }
        if (dosxc_sys_getppid() != 0) { say("Type EXIT until you are back at the first command prompt, then LOGOUT.\n"); return 1; }
        exit_requested = 1;
        return 0;
    }
    if (same(name, "EXIT")) {
        exit_requested = 1;
        return *arguments ? atoi(starts(arguments, "/B") ? arguments + 2 : arguments) : errorlevel;
    }
    char found[300];
    if (!locate(name, found)) {
        printf("'%s' is not a command, program or batch file.\n", name);
        return 9009 & 0xff;
    }
    return run_program(found, name, arguments);
}

/* Apply <, >, >>, 2>, 2>&1 in TEXT (removed from it).  Returns -1 on error. */
struct saved { int fd, copy; };
static int redirect(char *text, struct saved *saved, int *count) {
    int quoted = 0;
    for (char *p = text; *p; ++p) {
        if (*p == '"') quoted = !quoted;
        if (quoted || (*p != '<' && *p != '>')) continue;
        char *start = p;
        int fd = *p == '<' ? 0 : 1, flags;
        if (*p == '>' && p > text && p[-1] == '2' && (p - 1 == text || p[-2] == ' ')) { fd = 2; start = p - 1; }
        if (*p == '<') { flags = O_RDONLY; ++p; }
        else if (p[1] == '>') { flags = O_WRONLY | O_CREAT | O_APPEND; p += 2; }
        else { flags = O_WRONLY | O_CREAT | O_TRUNC; ++p; }
        char target[300];
        int dup_stdout = 0;
        if (fd == 2 && !strncmp(p, "&1", 2)) { dup_stdout = 1; p += 2; target[0] = 0; }
        else p = word(p, target, sizeof(target));
        memmove(start, p, strlen(p) + 1);
        p = start - 1;
        if (!dup_stdout && !*target) { say("The syntax of the command is incorrect.\n"); return -1; }
        int opened = dup_stdout ? dup(1) : open(target, flags, 0666);
        if (opened < 0) { printf("%s: %s\n", target, strerror(errno)); return -1; }
        fflush(stdout);
        fflush(stderr);
        saved[*count].fd = fd;
        saved[*count].copy = dup(fd);
        ++*count;
        dup2(opened, fd);
        close(opened);
    }
    return 0;
}

static void restore(struct saved *saved, int count) {
    fflush(stdout);
    fflush(stderr);
    while (count--) {
        dup2(saved[count].copy, saved[count].fd);
        close(saved[count].copy);
    }
}

static int redirected_command(char *text) {
    struct saved saved[8];
    int count = 0;
    int r = redirect(text, saved, &count) ? 1 : simple_command(text);
    restore(saved, count);
    return r;
}

/* Split TEXT at an unquoted SEPARATOR (one character); NULL when absent. */
static char *split(char *text, char separator, int doubled) {
    int quoted = 0;
    for (char *p = text; *p; ++p) {
        if (*p == '"') quoted = !quoted;
        if (quoted || *p != separator) continue;
        if (doubled ? p[1] != separator : (p[1] == separator || (p > text && p[-1] == separator))) continue;
        if (!doubled && separator == '&' && p > text && p[-1] == '>') continue;
        *p = 0;
        return p + (doubled ? 2 : 1);
    }
    return 0;
}

/* A pipeline: each command's output becomes the next one's input.  Programs
   run one after another, so the pipe holds the whole output in between. */
static int pipeline(char *text) {
    char *rest = split(text, '|', 0);
    if (!rest) return redirected_command(text);
    int fds[2];
    if (pipe(fds)) { say("Cannot create a pipe.\n"); return 1; }
    fflush(stdout);
    int saved_out = dup(1);
    dup2(fds[1], 1);
    close(fds[1]);
    redirected_command(text);
    fflush(stdout);
    dup2(saved_out, 1);
    close(saved_out);
    int saved_in = dup(0);
    dup2(fds[0], 0);
    close(fds[0]);
    int r = pipeline(rest);
    dup2(saved_in, 0);
    close(saved_in);
    return r;
}

static int run_command(char *line) {
    char text[LINE_MAX_SHELL * 2];
    expand(line, text, sizeof(text));
    char *rest;
    char *current = text;
    for (;;) {
        char *and = split(current, '&', 1), *or = and ? 0 : split(current, '|', 1);
        char *seq = and || or ? 0 : split(current, '&', 0);
        rest = and ? and : or ? or : seq;
        errorlevel = pipeline(current);
        if (exit_requested || !rest) break;
        if (and && errorlevel) break;
        if (or && !errorlevel) break;
        current = rest;
    }
    return errorlevel;
}

#define LOGGED_OUT 3

/* Returns the desktop's exit status, or -1 when it did not run. */
static int start_desktop(int ask) {
    char program[64];
    if (!find_desktop_file("DESKTOP.EXE", program, sizeof(program))) return -1;
    for (int i = 0; ask && i < 12; ++i) {
        int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 0, 0);
        if (k == 27) { say("Staying at the command prompt.\n"); return -1; }
        if (k < 0) dosxc_sys_sleep(20000000);
    }
    char *argv[2] = {program, 0};
    fflush(stdout);
    int status = dosxc_sys_spawn(program, argv, 0);
    return status < 0 ? -1 : (status >> 8) & 0xff;
}

static void read_secret(char *out, size_t size) {
    size_t n = 0;
    for (;;) {
        int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
        if (k == '\r' || k == '\n') break;
        if ((k == '\b' || k == 127) && n) { --n; fputs("\b \b", stdout); }
        else if (k >= 32 && k < 127 && n + 1 < size) { out[n++] = (char)k; putchar('*'); }
        fflush(stdout);
    }
    out[n] = 0;
    putchar('\n');
}

/* At the first command prompt with user accounts on: nobody goes further
   until someone has logged in, on the desktop's login screen when the
   desktop is installed, else here. */
static void log_in(void) {
    char program[64];
    if (find_desktop_file("DESKTOP.EXE", program, sizeof(program))) {
        char *argv[3] = {program, "/LOGON", 0};
        fflush(stdout);
        dosxc_sys_spawn(program, argv, 0);
        struct dosxc_user me;
        dosxc_sys_user(DOSXC_USER_WHOAMI, 0, &me);
        if (me.name[0]) return;
    }
    for (;;) {
        say("\nUsers of this computer:\n");
        struct dosxc_user u;
        long count = dosxc_sys_user(DOSXC_USER_LIST, 0, &u);
        for (long i = 0; i < count; ++i) {
            dosxc_sys_user(DOSXC_USER_LIST, (void *)i, &u);
            printf("  %-20s %s%s\n", u.name, u.nickname, u.admin ? " (administrator)" : "");
        }
        char name[64], password[64];
        fputs("\nUser name: ", stdout);
        fflush(stdout);
        if (!read_line(name) || !name[0]) continue;
        fputs("Password: ", stdout);
        fflush(stdout);
        read_secret(password, sizeof(password));
        if (!dosxc_sys_user(DOSXC_USER_LOGIN, name, password)) break;
        say("The user name or password is not right.\n");
    }
}

/* ---- main ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    system_drive = info.drive;
    if (argc > 1 && (same(argv[1], "/C") || same(argv[1], "/K"))) {
        char line[LINE_MAX_SHELL * 2] = "";
        for (int i = 2; i < argc; ++i) {
            if (strlen(line) + strlen(argv[i]) + 4 >= sizeof(line)) break;
            if (i > 2) strcat(line, " ");
            if (strchr(argv[i], ' ') && argc > 3) { strcat(line, "\""); strcat(line, argv[i]); strcat(line, "\""); }
            else strcat(line, argv[i]);
        }
        int r = run_command(line);
        if (same(argv[1], "/C")) return r;
    } else if (argc > 1 && !strcmp(argv[1], "/?")) {
        say("COMMAND [/C line]   Starts the DOS-XC command shell, or runs one command line.\n");
        return 0;
    }
    interactive = 1;
    setvbuf(stdout, 0, _IOLBF, 0);
    char drive_root[4] = {system_drive, ':', '\\', 0};
    if (argc == 1 && dosxc_sys_getppid() == 0) {
        chdir(drive_root);
        build_path();
        int accounts = dosxc_sys_user(DOSXC_USER_ENABLED, 0, 0) > 0;
        struct dosxc_user me;
        if (accounts) {
            log_in();
            dosxc_sys_user(DOSXC_USER_WHOAMI, 0, &me);
            chdir(me.home);
        }
        welcome();
        char autoexec[32];
        snprintf(autoexec, sizeof(autoexec), "%c:\\AUTOEXEC.BAT", system_drive);
        if (is_file(autoexec)) {
            char line[64];
            snprintf(line, sizeof(line), "\"%s\"", autoexec);
            run_command(line);
        }
        char marker[8];
        if (accounts ? me.start == DOSXC_USER_START_DESKTOP : env_get("DESKTOPSTARTED", marker, sizeof(marker)) < 0) {
            if (!accounts) env_set("DESKTOPSTARTED", "1");
            if (start_desktop(!accounts) == LOGGED_OUT) return 0;
        }
    }
    char line[LINE_MAX_SHELL];
    while (!exit_requested) {
        show_notices();
        char prompt_text[300];
        prompt_string(prompt_text, sizeof(prompt_text));
        fputs(prompt_text, stdout);
        fflush(stdout);
        if (!read_line(line)) continue;
        remember(line);
        run_command(line);
        /* EXIT in the main shell ends it too: the kernel's init module
           starts a fresh one (welcome screen and AUTOEXEC.BAT included). */
        if (exit_requested && dosxc_sys_getppid() == 0) say("Restarting the command shell...\n");
    }
    return errorlevel;
}
