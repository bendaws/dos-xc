/*
 * DOS-XC C library - getopt, getopt_long (in its own object so a program
 * may supply its own getopt, as libiberty does)
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

char *optarg;
int optind = 1, opterr = 1, optopt;
static int optpos;

int getopt(int argc, char *const argv[], const char *options) {
    if (optind == 0) {
        optind = 1;
        optpos = 0;
    }
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1]) return -1;
    if (!strcmp(argv[optind], "--")) {
        ++optind;
        return -1;
    }
    if (!optpos) optpos = 1;
    int c = (unsigned char)argv[optind][optpos++];
    const char *spec = c == ':' ? 0 : strchr(options + (*options == '+' || *options == '-'), c);
    int colon = options[*options == '+' || *options == '-'] == ':';
    if (!spec) {
        optopt = c;
        if (opterr && !colon) fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        if (!argv[optind][optpos]) ++optind, optpos = 0;
        return '?';
    }
    if (spec[1] == ':') {
        if (argv[optind][optpos]) {
            optarg = argv[optind] + optpos;
        } else if (spec[2] == ':') {
            optarg = 0;
        } else if (optind + 1 < argc) {
            optarg = argv[++optind];
        } else {
            optopt = c;
            ++optind;
            optpos = 0;
            if (colon) return ':';
            if (opterr) fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
            return '?';
        }
        ++optind;
        optpos = 0;
    } else if (!argv[optind][optpos]) {
        ++optind;
        optpos = 0;
    }
    return c;
}

static int long_option(int argc, char *const argv[], const char *options, const struct option *longs, int *index, int only) {
    if (optind == 0) optind = 1;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1]) return -1;
    const char *arg = argv[optind];
    if (!optpos && (arg[1] == '-' || only) && strcmp(arg, "--")) {
        const char *name = arg + (arg[1] == '-' ? 2 : 1);
        size_t n = strcspn(name, "=");
        int found = -1, ambiguous = 0;
        for (int i = 0; longs[i].name; ++i) {
            if (strncmp(longs[i].name, name, n)) continue;
            if (strlen(longs[i].name) == n) { found = i; ambiguous = 0; break; }
            if (found >= 0) ambiguous = 1;
            else found = i;
        }
        if (found >= 0 && !ambiguous) {
            const struct option *o = &longs[found];
            ++optind;
            optarg = 0;
            if (name[n] == '=') {
                if (o->has_arg == no_argument) {
                    if (opterr) fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], o->name);
                    optopt = o->val;
                    return '?';
                }
                optarg = (char *)name + n + 1;
            } else if (o->has_arg == required_argument) {
                if (optind >= argc) {
                    if (opterr) fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], o->name);
                    optopt = o->val;
                    return options[0] == ':' ? ':' : '?';
                }
                optarg = argv[optind++];
            }
            if (index) *index = found;
            if (o->flag) {
                *o->flag = o->val;
                return 0;
            }
            return o->val;
        }
        if (arg[1] == '-') {
            if (opterr) fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], arg);
            ++optind;
            optopt = 0;
            return '?';
        }
    }
    return getopt(argc, argv, options);
}

int getopt_long(int argc, char *const argv[], const char *options, const struct option *longs, int *index) {
    return long_option(argc, argv, options, longs, index, 0);
}
int getopt_long_only(int argc, char *const argv[], const char *options, const struct option *longs, int *index) {
    return long_option(argc, argv, options, longs, index, 1);
}

