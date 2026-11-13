/*
 * DOS-XC /dosoffice/office.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "office.h"

/* Google Drive's .gdoc/.gsheet/.gslides shortcuts are JSON with a "url" (or a "doc_id"). */
void open_in_browser(const char *path) {
    size_t size;
    char *data = read_whole(path, &size);
    if (!data) { message_box("DOS Office", "The shortcut could not be read.", path); return; }
    char url[512] = "";
    const char *key = strstr(data, "\"url\"");
    if (key && (key = strchr(key + 5, '"'))) {
        size_t n = 0;
        for (++key; *key && *key != '"' && n + 1 < sizeof(url); ++key) {
            if (*key == '\\' && key[1]) ++key;
            url[n++] = *key;
        }
        url[n] = 0;
    } else if ((key = strstr(data, "\"doc_id\"")) && (key = strchr(key + 8, '"'))) {
        const char *kind = strcasecmp(extension_of(path), "gsheet") == 0 ? "spreadsheets" : strcasecmp(extension_of(path), "gslides") == 0 ? "presentation" : "document";
        const char *end = strchr(++key, '"');
        if (end) snprintf(url, sizeof(url), "https://docs.google.com/%s/d/%.*s/edit", kind, (int)(end - key), key);
    }
    free(data);
    if (strncmp(url, "https://", 8)) { message_box("DOS Office", "This Google Docs shortcut has no web address.", path); return; }
    char browser[64];
    default_program("browser", browser, sizeof(browser));
    if (!browser[0] || program_start(browser, url))
        message_box("DOS Office", "Google Docs files open in a web browser.", "Choose a default browser in the Control Panel.");
}
