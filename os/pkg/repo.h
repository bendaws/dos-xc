/* DOS-XC /os/pkg/repo.h - package catalog protocol, shared with PKGMGR. */
#pragma once
#include "dsp.h"
#define PKG_REPOSITORY "https://dos-packages.bendaws.net"
#define REPO_MAX_PACKAGES 128
#define REPO_MAX_DOWNLOAD (256UL * 1024 * 1024)
struct repo_entry { struct dsp_info info; unsigned long id, size; char sha256[65]; };
/* exact is set when maximum is the known size of the file, not just a bound. */
typedef int (*repo_transfer)(const char *url, const char *output, unsigned long maximum, int exact, void *context);
struct repo { char origin[256], work[300]; int bits; repo_transfer transfer; void *context; };
int repo_init(struct repo *repo, const char *origin, int allow_http, int bits, const char *work, repo_transfer transfer, void *context);
int repo_read_index(const char *file, struct repo_entry *out, int capacity, int *next);
int repo_resolve(struct repo *repo, const char *name, struct repo_entry *out);
int repo_search(struct repo *repo, const char *query, struct repo_entry *out, int capacity);
int repo_fetch(struct repo *repo, const struct repo_entry *entry, char *file, size_t size);
int repo_write_index(const char *file, const struct repo_entry *entries, int count);
