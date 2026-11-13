/*
 * DOS-XC /openwork/mailnet.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "mail.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "wm.h"
#ifdef HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
int mail_tls = 1;
#else
int mail_tls = 0;
#endif

#define WAIT_MS 100
#define TIMEOUT_MS 45000
#define NEWEST 200

void (*mail_idle)(void);
char mail_ca_file[260];

/* ---- the account: %HOME%\OPENMAIL.CFG ---- */

static void obscure(const char *in, char *out, size_t size) {
    struct text t = {0};
    base64_encode(&t, (const unsigned char *)in, strlen(in), 0);
    snprintf(out, size, "%s", t.data ? t.data : "");
    free(t.data);
}

static void reveal(const char *in, char *out, size_t size) {
    size_t n = 0;
    unsigned value = 0;
    int bits = 0;
    for (const char *p = in; *p && n + 1 < size; ++p) {
        const char *digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", *d = strchr(digits, *p);
        if (!d || *p == '=') continue;
        value = value << 6 | (unsigned)(d - digits);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (char)(value >> bits & 0xff); }
    }
    out[n] = 0;
}

int account_load(struct account *a) {
    memset(a, 0, sizeof(*a));
    a->in_port = 993;
    a->out_port = 465;
    a->out_auth = 1;
    char path[260], line[400];
    home_path(path, sizeof(path), "OPENMAIL.CFG");
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "name")) snprintf(a->name, sizeof(a->name), "%s", v);
        else if (!strcmp(k, "address")) snprintf(a->address, sizeof(a->address), "%s", v);
        else if (!strcmp(k, "protocol")) a->protocol = !strcasecmp(v, "pop3") ? PROTOCOL_POP3 : PROTOCOL_IMAP;
        else if (!strcmp(k, "in_host")) snprintf(a->in_host, sizeof(a->in_host), "%s", v);
        else if (!strcmp(k, "in_port")) a->in_port = atoi(v);
        else if (!strcmp(k, "in_security")) a->in_security = atoi(v);
        else if (!strcmp(k, "user")) snprintf(a->user, sizeof(a->user), "%s", v);
        else if (!strcmp(k, "password")) reveal(v, a->password, sizeof(a->password));
        else if (!strcmp(k, "out_host")) snprintf(a->out_host, sizeof(a->out_host), "%s", v);
        else if (!strcmp(k, "out_port")) a->out_port = atoi(v);
        else if (!strcmp(k, "out_security")) a->out_security = atoi(v);
        else if (!strcmp(k, "out_auth")) a->out_auth = atoi(v);
    }
    fclose(f);
    return a->address[0] && a->in_host[0] ? 0 : -1;
}

int account_save(const struct account *a) {
    char path[260], hidden[200];
    home_path(path, sizeof(path), "OPENMAIL.CFG");
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    obscure(a->password, hidden, sizeof(hidden));
    fprintf(f, "name=%s\naddress=%s\nprotocol=%s\nin_host=%s\nin_port=%d\nin_security=%d\nuser=%s\npassword=%s\n"
               "out_host=%s\nout_port=%d\nout_security=%d\nout_auth=%d\n",
            a->name, a->address, a->protocol == PROTOCOL_POP3 ? "pop3" : "imap", a->in_host, a->in_port, a->in_security, a->user, hidden,
            a->out_host, a->out_port, a->out_security, a->out_auth);
    fclose(f);
    return 0;
}

void account_guess(struct account *a) {
    const char *at = strchr(a->address, '@');
    if (!at || !at[1]) return;
    const char *domain = at + 1;
    static const struct { const char *domain, *in, *out; int out_port, out_security; } known[] = {
        {"gmail.com", "imap.gmail.com", "smtp.gmail.com", 465, SECURITY_SSL},
        {"googlemail.com", "imap.gmail.com", "smtp.gmail.com", 465, SECURITY_SSL},
        {"outlook.com", "outlook.office365.com", "smtp-mail.outlook.com", 587, SECURITY_STARTTLS},
        {"hotmail.com", "outlook.office365.com", "smtp-mail.outlook.com", 587, SECURITY_STARTTLS},
        {"live.com", "outlook.office365.com", "smtp-mail.outlook.com", 587, SECURITY_STARTTLS},
        {"yahoo.com", "imap.mail.yahoo.com", "smtp.mail.yahoo.com", 465, SECURITY_SSL},
        {"icloud.com", "imap.mail.me.com", "smtp.mail.me.com", 587, SECURITY_STARTTLS},
        {"me.com", "imap.mail.me.com", "smtp.mail.me.com", 587, SECURITY_STARTTLS},
        {"aol.com", "imap.aol.com", "smtp.aol.com", 465, SECURITY_SSL},
        {"fastmail.com", "imap.fastmail.com", "smtp.fastmail.com", 465, SECURITY_SSL},
    };
    if (!a->user[0]) snprintf(a->user, sizeof(a->user), "%s", a->address);
    if (a->in_host[0] || a->out_host[0]) return;
    for (size_t i = 0; i < sizeof(known) / sizeof(*known); ++i)
        if (!strcasecmp(domain, known[i].domain)) {
            snprintf(a->in_host, sizeof(a->in_host), "%s", known[i].in);
            snprintf(a->out_host, sizeof(a->out_host), "%s", known[i].out);
            a->protocol = PROTOCOL_IMAP;
            a->in_port = 993;
            a->in_security = SECURITY_SSL;
            a->out_port = known[i].out_port;
            a->out_security = known[i].out_security;
            return;
        }
    snprintf(a->in_host, sizeof(a->in_host), "%s.%s", a->protocol == PROTOCOL_POP3 ? "pop" : "imap", domain);
    snprintf(a->out_host, sizeof(a->out_host), "smtp.%s", domain);
}

/* ---- connections ---- */

struct conn {
    int fd;
    void *ssl;
    char buffer[16384];
    size_t used, at;
    char *error;
    size_t error_size;
};

static void fail(struct conn *c, const char *format, ...) {
    va_list list;
    va_start(list, format);
    vsnprintf(c->error, c->error_size, format, list);
    va_end(list);
}

/* Waits for EVENTS on the socket, keeping the program responsive. */
static int wait_for(struct conn *c, short events) {
    for (int waited = 0; waited < TIMEOUT_MS; waited += WAIT_MS) {
        struct pollfd p = {c->fd, events, 0};
        int r = poll(&p, 1, WAIT_MS);
        if (r > 0) return 0;
        if (r < 0 && errno != EINTR) { fail(c, "The connection failed (error %d).", errno); return -1; }
        if (mail_idle) mail_idle();
    }
    fail(c, "The server stopped answering.");
    return -1;
}

#ifdef HAVE_TLS
static SSL_CTX *tls_context;

static int tls_start(struct conn *c, const char *host) {
    if (!tls_context) {
        tls_context = SSL_CTX_new(TLS_client_method());
        if (!tls_context) { fail(c, "TLS could not start."); return -1; }
        SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION);
        SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, 0);
        if (!mail_ca_file[0] || SSL_CTX_load_verify_locations(tls_context, mail_ca_file, 0) != 1) {
            SSL_CTX_free(tls_context);
            tls_context = 0;
            fail(c, "The trusted certificates (CACERT.PEM) could not be read.");
            return -1;
        }
    }
    SSL *ssl = SSL_new(tls_context);
    if (!ssl) { fail(c, "TLS could not start."); return -1; }
    SSL_set_fd(ssl, c->fd);
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set1_host(ssl, host);
    c->ssl = ssl;
    for (;;) {
        int r = SSL_connect(ssl);
        if (r == 1) return 0;
        int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ) { if (wait_for(c, POLLIN)) return -1; continue; }
        if (e == SSL_ERROR_WANT_WRITE) { if (wait_for(c, POLLOUT)) return -1; continue; }
        long verify = SSL_get_verify_result(ssl);
        if (verify != X509_V_OK) fail(c, "The server's certificate is not trusted: %s. Check the computer's date and time.", X509_verify_cert_error_string(verify));
        else {
            char reason[160];
            ERR_error_string_n(ERR_get_error(), reason, sizeof(reason));
            fail(c, "The secure connection failed: %s", reason);
        }
        return -1;
    }
}
#else
static int tls_start(struct conn *c, const char *host) {
    fail(c, "This OpenMail was built without TLS; choose no encryption.");
    return -1;
}
#endif

static void conn_close(struct conn *c) {
    if (!c) return;
#ifdef HAVE_TLS
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
#endif
    if (c->fd >= 0) close(c->fd);
    free(c);
}

static struct conn *conn_open(const char *host, int port, int tls, char *error, size_t error_size) {
    struct conn *c = calloc(1, sizeof(*c));
    if (!c) { snprintf(error, error_size, "Out of memory."); return 0; }
    c->fd = -1;
    c->error = error;
    c->error_size = error_size;
    char service[8];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints, *list = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &list) || !list) { fail(c, "%s could not be found.", host); conn_close(c); return 0; }
    for (struct addrinfo *ai = list; ai; ai = ai->ai_next) {
        c->fd = socket(ai->ai_family, SOCK_STREAM, 0);
        if (c->fd < 0) continue;
        fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
        int r = connect(c->fd, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && (errno == EINPROGRESS || errno == EAGAIN)) {
            if (!wait_for(c, POLLOUT)) {
                int problem = 0;
                socklen_t len = sizeof(problem);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &problem, &len);
                r = problem ? -1 : 0;
            }
        }
        if (r == 0) break;
        close(c->fd);
        c->fd = -1;
    }
    freeaddrinfo(list);
    if (c->fd < 0) {
        if (!error[0]) fail(c, "OpenMail could not connect to %s on port %d.", host, port);
        conn_close(c);
        return 0;
    }
    if (tls && tls_start(c, host)) { conn_close(c); return 0; }
    return c;
}

static int conn_fill(struct conn *c) {
    if (c->at > 0) {
        memmove(c->buffer, c->buffer + c->at, c->used - c->at);
        c->used -= c->at;
        c->at = 0;
    }
    if (c->used == sizeof(c->buffer)) return 0;
    for (;;) {
#ifdef HAVE_TLS
        if (c->ssl) {
            int r = SSL_read(c->ssl, c->buffer + c->used, (int)(sizeof(c->buffer) - c->used));
            if (r > 0) { c->used += (size_t)r; return 0; }
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_READ) { if (wait_for(c, POLLIN)) return -1; continue; }
            if (e == SSL_ERROR_WANT_WRITE) { if (wait_for(c, POLLOUT)) return -1; continue; }
            fail(c, "The server closed the connection.");
            return -1;
        }
#endif
        ssize_t r = recv(c->fd, c->buffer + c->used, sizeof(c->buffer) - c->used, 0);
        if (r > 0) { c->used += (size_t)r; return 0; }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) { if (wait_for(c, POLLIN)) return -1; continue; }
        fail(c, "The server closed the connection.");
        return -1;
    }
}

static int conn_write(struct conn *c, const char *s, size_t n) {
    while (n) {
#ifdef HAVE_TLS
        if (c->ssl) {
            int r = SSL_write(c->ssl, s, (int)(n > 16384 ? 16384 : n));
            if (r > 0) { s += r; n -= (size_t)r; continue; }
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_READ) { if (wait_for(c, POLLIN)) return -1; continue; }
            if (e == SSL_ERROR_WANT_WRITE) { if (wait_for(c, POLLOUT)) return -1; continue; }
            fail(c, "The server closed the connection.");
            return -1;
        }
#endif
        ssize_t r = send(c->fd, s, n, 0);
        if (r > 0) { s += r; n -= (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) { if (wait_for(c, POLLOUT)) return -1; continue; }
        fail(c, "The server closed the connection.");
        return -1;
    }
    return 0;
}

static int conn_printf(struct conn *c, const char *format, ...) {
    char line[1200];
    va_list list;
    va_start(list, format);
    int n = vsnprintf(line, sizeof(line), format, list);
    va_end(list);
    if (n < 0 || n >= (int)sizeof(line)) { fail(c, "A command was too long."); return -1; }
    return conn_write(c, line, (size_t)n);
}

/* The next line, without its line ending. */
static int conn_line(struct conn *c, struct text *line) {
    line->size = 0;
    if (line->data) line->data[0] = 0;
    for (;;) {
        char *nl = memchr(c->buffer + c->at, '\n', c->used - c->at);
        if (nl) {
            size_t n = (size_t)(nl - (c->buffer + c->at));
            text_add(line, c->buffer + c->at, n && nl[-1] == '\r' ? n - 1 : n);
            c->at += n + 1;
            if (!line->data) text_put(line, "");
            return 0;
        }
        text_add(line, c->buffer + c->at, c->used - c->at);
        c->at = c->used;
        if (conn_fill(c)) return -1;
    }
}

static int conn_bytes(struct conn *c, size_t n, struct text *out) {
    while (n) {
        if (c->at == c->used && conn_fill(c)) return -1;
        size_t take = c->used - c->at < n ? c->used - c->at : n;
        text_add(out, c->buffer + c->at, take);
        c->at += take;
        n -= take;
    }
    return 0;
}

static int starts_with(const char *s, const char *prefix) { return !strncasecmp(s, prefix, strlen(prefix)); }

/* ---- IMAP ---- */

/* Untagged responses of one command: TEXT holds them with their literals, START where each begins. */
struct reply { struct text text; size_t *start; int count; };

static void reply_free(struct reply *r) { free(r->text.data); free(r->start); memset(r, 0, sizeof(*r)); }

static const char *reply_at(const struct reply *r, int i, size_t *n) {
    size_t end = i + 1 < r->count ? r->start[i + 1] : r->text.size;
    *n = end - r->start[i];
    return r->text.data + r->start[i];
}

static void quote(char *out, size_t size, const char *s) {
    size_t n = 0;
    out[n++] = '"';
    for (; *s && n + 3 < size; ++s) {
        if (*s == '"' || *s == '\\') out[n++] = '\\';
        out[n++] = *s;
    }
    out[n++] = '"';
    out[n] = 0;
}

/* Sends a command and collects the reply; with DATA, sends it as the literal the server asks for. */
static int imap(struct mailbox *m, struct reply *r, const char *data, size_t data_size, const char *format, ...) {
    struct conn *c = m->conn;
    char command[1100], tag[16];
    va_list list;
    va_start(list, format);
    vsnprintf(command, sizeof(command), format, list);
    va_end(list);
    snprintf(tag, sizeof(tag), "A%04u", ++m->tag);
    if (conn_printf(c, "%s %s\r\n", tag, command)) return -1;
    struct text line = {0};
    int status = -1, in_response = 0;
    for (;;) {
        if (conn_line(c, &line)) break;
        if (!in_response && !strncmp(line.data, tag, strlen(tag)) && line.data[strlen(tag)] == ' ') {
            const char *rest = line.data + strlen(tag) + 1;
            status = starts_with(rest, "OK") ? 0 : -1;
            if (status) snprintf(m->error, sizeof(m->error), "%s", rest);
            break;
        }
        if (!in_response && line.data[0] == '+') {
            if (data) { if (conn_write(c, data, data_size) || conn_write(c, "\r\n", 2)) break; data = 0; }
            continue;
        }
        if (r && !in_response) {
            size_t *bigger = realloc(r->start, sizeof(*bigger) * (size_t)(r->count + 1));
            if (!bigger) break;
            r->start = bigger;
            r->start[r->count++] = r->text.size;
        }
        if (r) text_add(&r->text, line.data, line.size);
        in_response = 0;
        size_t n = line.size;
        if (n > 2 && line.data[n - 1] == '}') {
            const char *open = strrchr(line.data, '{');
            if (open) {
                size_t literal = strtoul(open + 1, 0, 10);
                struct text bytes = {0};
                if (conn_bytes(c, literal, &bytes)) { free(bytes.data); break; }
                if (r) { text_put(&r->text, "\r\n"); if (bytes.data) text_add(&r->text, bytes.data, bytes.size); }
                free(bytes.data);
                in_response = 1;
            }
        }
        if (r && !in_response) text_put(&r->text, "\r\n");
    }
    free(line.data);
    return status;
}

static int imap_open(struct mailbox *m) {
    if (m->conn) return 0;
    const struct account *a = &m->account;
    m->error[0] = 0;
    m->selected[0] = 0;
    struct conn *c = conn_open(a->in_host, a->in_port, a->in_security == SECURITY_SSL, m->error, sizeof(m->error));
    if (!c) return -1;
    m->conn = c;
    struct text line = {0};
    int ok = !conn_line(c, &line) && starts_with(line.data, "* OK");
    if (!ok && !m->error[0]) snprintf(m->error, sizeof(m->error), "%s did not greet OpenMail as a mail server.", a->in_host);
    if (ok && a->in_security == SECURITY_STARTTLS) {
        ok = !imap(m, 0, 0, 0, "STARTTLS") && !tls_start(c, a->in_host);
        if (!ok && !m->error[0]) snprintf(m->error, sizeof(m->error), "The server does not offer STARTTLS.");
    }
    free(line.data);
    if (ok) {
        char user[260], password[260];
        quote(user, sizeof(user), a->user[0] ? a->user : a->address);
        quote(password, sizeof(password), a->password);
        ok = !imap(m, 0, 0, 0, "LOGIN %s %s", user, password);
        if (!ok) {
            char why[200];
            snprintf(why, sizeof(why), "%s", m->error);
            snprintf(m->error, sizeof(m->error), "Sign-in failed: %s", why);
        }
    }
    if (!ok) { conn_close(m->conn); m->conn = 0; return -1; }
    return 0;
}

static int imap_select(struct mailbox *m, const char *folder) {
    if (!strcmp(m->selected, folder)) return 0;
    char name[340];
    quote(name, sizeof(name), folder);
    struct reply r = {0};
    int status = imap(m, &r, 0, 0, "SELECT %s", name);
    m->exists = 0;
    for (int i = 0; i < r.count; ++i) {
        size_t n;
        const char *s = reply_at(&r, i, &n);
        char *word;
        unsigned long count = strtoul(s + 2, &word, 10);
        if (word != s + 2 && !strncasecmp(word, " EXISTS", 7)) m->exists = (int)count;
    }
    reply_free(&r);
    if (!status) snprintf(m->selected, sizeof(m->selected), "%s", folder);
    return status;
}

/* Runs a command, reconnecting once if the server dropped the connection. */
#define WITH_IMAP(m, body)                                                                                            \
    for (int attempt = 0; attempt < 2; ++attempt) {                                                                  \
        if (imap_open(m)) return -1;                                                                                  \
        int result = (body);                                                                                          \
        if (result == 0) return 0;                                                                                    \
        if (strstr(m->error, "closed") || strstr(m->error, "stopped")) { conn_close(m->conn); m->conn = 0; continue; } \
        return -1;                                                                                                    \
    }                                                                                                                 \
    return -1;

/* Modified UTF-7 folder names as Latin-1. */
static void folder_label(const char *name, char *out, size_t size) {
    struct text t = {0};
    for (const char *p = name; *p;) {
        if (*p != '&') { text_add(&t, p++, 1); continue; }
        const char *end = strchr(p, '-');
        if (!end) { text_put(&t, p); break; }
        if (end == p + 1) { text_put(&t, "&"); p = end + 1; continue; }
        unsigned value = 0, unit = 0;
        int bits = 0;
        for (const char *q = p + 1; q < end; ++q) {
            const char *digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,", *d = strchr(digits, *q);
            if (!d) continue;
            value = value << 6 | (unsigned)(d - digits);
            bits += 6;
            if (bits >= 16) {
                bits -= 16;
                unit = value >> bits & 0xffff;
                char c = (char)(unit < 256 ? unit : '?');
                text_add(&t, &c, 1);
            }
        }
        p = end + 1;
    }
    snprintf(out, size, "%s", t.data ? t.data : "");
    free(t.data);
}

static int folder_order(const void *a, const void *b) {
    const struct folder *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return strcasecmp(x->name, y->name);
}

static const char *parse_string(const char *s, char *out, size_t size) {
    while (*s == ' ') ++s;
    size_t n = 0;
    if (*s == '"') {
        for (++s; *s && *s != '"'; ++s) {
            if (*s == '\\' && s[1]) ++s;
            if (n + 1 < size) out[n++] = *s;
        }
        if (*s) ++s;
    } else if (*s == '{') {
        const char *body = strstr(s, "\r\n");
        size_t len = strtoul(s + 1, 0, 10);
        if (body) { body += 2; n = len < size - 1 ? len : size - 1; memcpy(out, body, n); s = body + len; }
    } else while (*s && *s != ' ' && *s != '\r' && *s != ')') { if (n + 1 < size) out[n++] = *s; ++s; }
    out[n] = 0;
    return s;
}

static int imap_folders(struct mailbox *m, struct folder **out, int *count) {
    struct reply r = {0};
    if (imap(m, &r, 0, 0, "LIST \"\" \"*\"")) { reply_free(&r); return -1; }
    struct folder *list = calloc((size_t)r.count + 1, sizeof(*list));
    int n = 0;
    for (int i = 0; list && i < r.count; ++i) {
        size_t size;
        const char *s = reply_at(&r, i, &size);
        if (!starts_with(s, "* LIST (")) continue;
        const char *flags = s + 8, *close = strchr(flags, ')');
        if (!close) continue;
        char flag_text[200];
        snprintf(flag_text, sizeof(flag_text), "%.*s", (int)(close - flags), flags);
        if (strcasestr(flag_text, "\\Noselect") || strcasestr(flag_text, "\\NonExistent")) continue;
        char delimiter[8], name[160];
        const char *p = parse_string(close + 1, delimiter, sizeof(delimiter));
        parse_string(p, name, sizeof(name));
        struct folder *f = &list[n++];
        snprintf(f->name, sizeof(f->name), "%s", name);
        char label[160];
        folder_label(name, label, sizeof(label));
        const char *leaf = delimiter[0] && strcmp(delimiter, "NIL") ? strrchr(label, delimiter[0]) : 0;
        snprintf(f->label, sizeof(f->label), "%s", leaf ? leaf + 1 : label);
        f->kind = FOLDER_OTHER;
        if (!strcasecmp(name, "INBOX")) { f->kind = FOLDER_INBOX; snprintf(f->label, sizeof(f->label), "Inbox"); }
        else if (strcasestr(flag_text, "\\Sent")) f->kind = FOLDER_SENT;
        else if (strcasestr(flag_text, "\\Drafts")) f->kind = FOLDER_DRAFTS;
        else if (strcasestr(flag_text, "\\Trash")) f->kind = FOLDER_TRASH;
        else if (strcasestr(flag_text, "\\Junk")) f->kind = FOLDER_JUNK;
        else if (strcasestr(flag_text, "\\Archive") || strcasestr(flag_text, "\\All")) f->kind = FOLDER_ARCHIVE;
        else if (!strcasecmp(f->label, "Sent") || !strcasecmp(f->label, "Sent Items") || !strcasecmp(f->label, "Sent Mail") || !strcasecmp(f->label, "Sent Messages")) f->kind = FOLDER_SENT;
        else if (!strcasecmp(f->label, "Drafts")) f->kind = FOLDER_DRAFTS;
        else if (!strcasecmp(f->label, "Trash") || !strcasecmp(f->label, "Deleted Items") || !strcasecmp(f->label, "Deleted Messages") || !strcasecmp(f->label, "Bin")) f->kind = FOLDER_TRASH;
        else if (!strcasecmp(f->label, "Junk") || !strcasecmp(f->label, "Spam") || !strcasecmp(f->label, "Junk E-mail")) f->kind = FOLDER_JUNK;
    }
    reply_free(&r);
    if (!list) return -1;
    qsort(list, (size_t)n, sizeof(*list), folder_order);
    for (int i = 0; i < n; ++i) {
        if (list[i].kind != FOLDER_INBOX) continue;
        struct reply s = {0};
        if (!imap(m, &s, 0, 0, "STATUS \"INBOX\" (UNSEEN)") && s.count) {
            size_t len;
            const char *u = strstr(reply_at(&s, 0, &len), "UNSEEN ");
            if (u) list[i].unread = atoi(u + 7);
        }
        reply_free(&s);
    }
    free(*out);
    *out = list;
    *count = n;
    return 0;
}

static void read_headers(const char *header, size_t n, struct message *msg) {
    char value[400];
    header_get(header, n, "From", value, sizeof(value));
    address_name(value, msg->from, sizeof(msg->from), msg->address, sizeof(msg->address));
    if (!msg->from[0]) snprintf(msg->from, sizeof(msg->from), "(unknown sender)");
    header_get(header, n, "To", msg->to, sizeof(msg->to));
    header_get(header, n, "Cc", msg->cc, sizeof(msg->cc));
    header_get(header, n, "Subject", msg->subject, sizeof(msg->subject));
    header_get(header, n, "Message-ID", msg->message_id, sizeof(msg->message_id));
    header_get(header, n, "References", msg->references, sizeof(msg->references));
    header_get(header, n, "Content-Type", value, sizeof(value));
    msg->attachment = !strncasecmp(value, "multipart/mixed", 15);
    if (!msg->when) {
        header_get(header, n, "Date", value, sizeof(value));
        msg->when = mail_date(value);
    }
}

static int by_date(const void *a, const void *b) {
    const struct message *x = a, *y = b;
    return x->when < y->when ? 1 : x->when > y->when ? -1 : (int)(y->uid - x->uid);
}

static int imap_list(struct mailbox *m, const char *folder, int newest, struct message **out, int *count) {
    if (imap_select(m, folder)) return -1;
    free(*out);
    *out = 0;
    *count = 0;
    if (!m->exists) return 0;
    int first = m->exists > newest ? m->exists - newest + 1 : 1;
    struct reply r = {0};
    if (imap(m, &r, 0, 0, "FETCH %d:%d (UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID REFERENCES CONTENT-TYPE)])",
             first, m->exists)) { reply_free(&r); return -1; }
    struct message *list = calloc((size_t)r.count + 1, sizeof(*list));
    int n = 0;
    for (int i = 0; list && i < r.count; ++i) {
        size_t size;
        const char *s = reply_at(&r, i, &size);
        if (!strstr(s, " FETCH (")) continue;
        struct message *msg = &list[n];
        memset(msg, 0, sizeof(*msg));
        const char *field;
        if ((field = strstr(s, "UID "))) msg->uid = (unsigned)strtoul(field + 4, 0, 10);
        if ((field = strstr(s, "RFC822.SIZE "))) msg->size = strtol(field + 12, 0, 10);
        if ((field = strstr(s, "INTERNALDATE \""))) msg->when = mail_date(field + 14);
        if ((field = strstr(s, "FLAGS ("))) {
            const char *end = strchr(field, ')');
            char flags[200];
            snprintf(flags, sizeof(flags), "%.*s", end ? (int)(end - field) : 0, field);
            msg->seen = strcasestr(flags, "\\Seen") != 0;
            msg->answered = strcasestr(flags, "\\Answered") != 0;
            msg->flagged = strcasestr(flags, "\\Flagged") != 0;
        }
        if ((field = strstr(s, "BODY[HEADER"))) {
            const char *brace = strchr(field, '{');
            const char *body = brace ? strstr(brace, "}\r\n") : 0;
            if (body) {
                size_t len = strtoul(brace + 1, 0, 10);
                body += 3;
                if (body + len <= s + size) read_headers(body, len, msg);
            }
        }
        if (msg->uid) ++n;
    }
    reply_free(&r);
    if (!list) return -1;
    qsort(list, (size_t)n, sizeof(*list), by_date);
    *out = list;
    *count = n;
    return 0;
}

static int imap_fetch(struct mailbox *m, const char *folder, const struct message *msg, struct text *raw) {
    if (imap_select(m, folder)) return -1;
    struct reply r = {0};
    if (imap(m, &r, 0, 0, "UID FETCH %u (BODY.PEEK[])", msg->uid)) { reply_free(&r); return -1; }
    int found = -1;
    for (int i = 0; i < r.count && found; ++i) {
        size_t size;
        const char *s = reply_at(&r, i, &size);
        const char *field = strstr(s, "BODY[]");
        const char *brace = field ? strchr(field, '{') : 0;
        const char *body = brace ? strstr(brace, "}\r\n") : 0;
        if (!body) continue;
        size_t len = strtoul(brace + 1, 0, 10);
        body += 3;
        if (body + len > s + size) continue;
        text_add(raw, body, len);
        found = 0;
    }
    reply_free(&r);
    if (found) snprintf(m->error, sizeof(m->error), "The message is no longer on the server.");
    return found;
}

static int imap_seen(struct mailbox *m, const char *folder, const struct message *msg, int seen) {
    if (imap_select(m, folder)) return -1;
    return imap(m, 0, 0, 0, "UID STORE %u %cFLAGS.SILENT (\\Seen)", msg->uid, seen ? '+' : '-');
}

static int imap_delete(struct mailbox *m, const char *folder, const struct message *msg, const char *trash) {
    if (imap_select(m, folder)) return -1;
    if (trash && trash[0] && strcmp(trash, folder)) {
        char name[340];
        quote(name, sizeof(name), trash);
        if (imap(m, 0, 0, 0, "UID COPY %u %s", msg->uid, name)) return -1;
    }
    if (imap(m, 0, 0, 0, "UID STORE %u +FLAGS.SILENT (\\Deleted)", msg->uid)) return -1;
    return imap(m, 0, 0, 0, "EXPUNGE");
}

static int imap_append(struct mailbox *m, const char *folder, const char *raw, size_t n) {
    char name[340];
    quote(name, sizeof(name), folder);
    return imap(m, 0, raw, n, "APPEND %s (\\Seen) {%lu}", name, (unsigned long)n);
}

/* ---- POP3: one connection for each task ---- */

static int pop_reply(struct conn *c, struct text *line) {
    if (conn_line(c, line)) return -1;
    if (starts_with(line->data, "+OK")) return 0;
    fail(c, "%s", line->data[0] == '-' ? line->data + (starts_with(line->data, "-ERR ") ? 5 : 0) : "The server gave an unexpected answer.");
    return -1;
}

static int pop_multiline(struct conn *c, struct text *out) {
    struct text line = {0};
    for (;;) {
        if (conn_line(c, &line)) { free(line.data); return -1; }
        if (!strcmp(line.data, ".")) break;
        text_add(out, line.data[0] == '.' ? line.data + 1 : line.data, line.data[0] == '.' ? line.size - 1 : line.size);
        text_put(out, "\r\n");
    }
    free(line.data);
    return 0;
}

static struct conn *pop_open(struct mailbox *m) {
    const struct account *a = &m->account;
    m->error[0] = 0;
    struct conn *c = conn_open(a->in_host, a->in_port, a->in_security == SECURITY_SSL, m->error, sizeof(m->error));
    if (!c) return 0;
    struct text line = {0};
    int ok = !pop_reply(c, &line);
    if (ok && a->in_security == SECURITY_STARTTLS) ok = !conn_printf(c, "STLS\r\n") && !pop_reply(c, &line) && !tls_start(c, a->in_host);
    if (ok) ok = !conn_printf(c, "USER %s\r\n", a->user[0] ? a->user : a->address) && !pop_reply(c, &line);
    if (ok) ok = !conn_printf(c, "PASS %s\r\n", a->password) && !pop_reply(c, &line);
    free(line.data);
    if (!ok) {
        char why[200];
        snprintf(why, sizeof(why), "%s", m->error);
        snprintf(m->error, sizeof(m->error), "Sign-in failed: %s", why);
        conn_close(c);
        return 0;
    }
    return c;
}

static void pop_close(struct conn *c) {
    struct text line = {0};
    if (!conn_printf(c, "QUIT\r\n")) pop_reply(c, &line);
    free(line.data);
    conn_close(c);
}

static int pop_list(struct mailbox *m, int newest, struct message **out, int *count) {
    struct conn *c = pop_open(m);
    if (!c) return -1;
    struct text line = {0};
    int total = 0;
    if (conn_printf(c, "STAT\r\n") || pop_reply(c, &line)) { free(line.data); conn_close(c); return -1; }
    total = atoi(line.data + 4);
    int first = total > newest ? total - newest + 1 : 1, n = 0;
    struct message *list = calloc((size_t)(total - first + 2), sizeof(*list));
    for (int i = total; list && i >= first; --i) {
        struct text header = {0};
        if (conn_printf(c, "TOP %d 0\r\n", i) || pop_reply(c, &line) || pop_multiline(c, &header)) { free(header.data); break; }
        struct message *msg = &list[n++];
        msg->uid = (unsigned)i;
        msg->seen = 1;
        if (header.data) read_headers(header.data, header.size, msg);
        free(header.data);
    }
    free(line.data);
    pop_close(c);
    free(*out);
    *out = list;
    *count = n;
    m->exists = total;
    if (list) qsort(list, (size_t)n, sizeof(*list), by_date);
    return list ? 0 : -1;
}

static int pop_fetch(struct mailbox *m, const struct message *msg, struct text *raw) {
    struct conn *c = pop_open(m);
    if (!c) return -1;
    struct text line = {0};
    int r = conn_printf(c, "RETR %u\r\n", msg->uid) || pop_reply(c, &line) || pop_multiline(c, raw) ? -1 : 0;
    free(line.data);
    pop_close(c);
    return r;
}

static int pop_delete(struct mailbox *m, const struct message *msg) {
    struct conn *c = pop_open(m);
    if (!c) return -1;
    struct text line = {0};
    int r = conn_printf(c, "DELE %u\r\n", msg->uid) || pop_reply(c, &line) ? -1 : 0;
    free(line.data);
    pop_close(c);
    return r;
}

/* ---- the public operations ---- */

int mail_folders(struct mailbox *m, struct folder **out, int *count) {
    if (m->account.protocol == PROTOCOL_POP3) {
        free(*out);
        *out = calloc(1, sizeof(**out));
        if (!*out) return -1;
        snprintf((*out)->name, sizeof((*out)->name), "INBOX");
        snprintf((*out)->label, sizeof((*out)->label), "Inbox");
        *count = 1;
        return 0;
    }
    WITH_IMAP(m, imap_folders(m, out, count))
}

int mail_list(struct mailbox *m, const char *folder, int newest, struct message **out, int *count) {
    if (newest <= 0) newest = NEWEST;
    if (m->account.protocol == PROTOCOL_POP3) return pop_list(m, newest, out, count);
    WITH_IMAP(m, imap_list(m, folder, newest, out, count))
}

int mail_fetch(struct mailbox *m, const char *folder, const struct message *msg, struct text *raw) {
    if (m->account.protocol == PROTOCOL_POP3) return pop_fetch(m, msg, raw);
    WITH_IMAP(m, imap_fetch(m, folder, msg, raw))
}

int mail_set_seen(struct mailbox *m, const char *folder, const struct message *msg, int seen) {
    if (m->account.protocol == PROTOCOL_POP3) return 0;
    WITH_IMAP(m, imap_seen(m, folder, msg, seen))
}

int mail_delete(struct mailbox *m, const char *folder, const struct message *msg, const char *trash) {
    if (m->account.protocol == PROTOCOL_POP3) return pop_delete(m, msg);
    WITH_IMAP(m, imap_delete(m, folder, msg, trash))
}

int mail_append(struct mailbox *m, const char *folder, const char *raw, size_t n) {
    if (m->account.protocol == PROTOCOL_POP3) return 0;
    WITH_IMAP(m, imap_append(m, folder, raw, n))
}

void mail_disconnect(struct mailbox *m) {
    if (!m->conn) return;
    imap(m, 0, 0, 0, "LOGOUT");
    conn_close(m->conn);
    m->conn = 0;
    m->selected[0] = 0;
}

/* ---- SMTP ---- */

static int smtp_reply(struct conn *c, int expect, struct text *line, char *extensions, size_t ext_size) {
    if (extensions) extensions[0] = 0;
    for (;;) {
        if (conn_line(c, line)) return -1;
        if (extensions && line->size > 4) {
            size_t used = strlen(extensions);
            snprintf(extensions + used, ext_size - used, "%s\n", line->data + 4);
        }
        if (line->size < 4 || line->data[3] != '-') break;
    }
    if (atoi(line->data) / 100 == expect / 100) return 0;
    fail(c, "The outgoing server said: %s", line->data);
    return -1;
}

int smtp_send(const struct account *a, const char *raw, size_t n, char recipients[][120], int count, char *error, size_t size) {
    error[0] = 0;
    struct conn *c = conn_open(a->out_host, a->out_port, a->out_security == SECURITY_SSL, error, size);
    if (!c) return -1;
    struct text line = {0};
    char extensions[2048];
    int ok = !smtp_reply(c, 220, &line, 0, 0) && !conn_printf(c, "EHLO dosxc\r\n") && !smtp_reply(c, 250, &line, extensions, sizeof(extensions));
    if (ok && a->out_security == SECURITY_STARTTLS)
        ok = !conn_printf(c, "STARTTLS\r\n") && !smtp_reply(c, 220, &line, 0, 0) && !tls_start(c, a->out_host) &&
             !conn_printf(c, "EHLO dosxc\r\n") && !smtp_reply(c, 250, &line, extensions, sizeof(extensions));
    if (ok && a->out_auth) {
        const char *user = a->user[0] ? a->user : a->address;
        if (strcasestr(extensions, "PLAIN")) {
            char plain[300];
            int len = snprintf(plain, sizeof(plain), "%c%s%c%s", 0, user, 0, a->password);
            struct text encoded = {0};
            base64_encode(&encoded, (const unsigned char *)plain, (size_t)len, 0);
            ok = !conn_printf(c, "AUTH PLAIN %s\r\n", encoded.data) && !smtp_reply(c, 235, &line, 0, 0);
            free(encoded.data);
        } else {
            struct text u = {0}, p = {0};
            base64_encode(&u, (const unsigned char *)user, strlen(user), 0);
            base64_encode(&p, (const unsigned char *)a->password, strlen(a->password), 0);
            ok = !conn_printf(c, "AUTH LOGIN\r\n") && !smtp_reply(c, 334, &line, 0, 0) && !conn_printf(c, "%s\r\n", u.data) &&
                 !smtp_reply(c, 334, &line, 0, 0) && !conn_printf(c, "%s\r\n", p.data) && !smtp_reply(c, 235, &line, 0, 0);
            free(u.data);
            free(p.data);
        }
    }
    if (ok) ok = !conn_printf(c, "MAIL FROM:<%s>\r\n", a->address) && !smtp_reply(c, 250, &line, 0, 0);
    for (int i = 0; ok && i < count; ++i) ok = !conn_printf(c, "RCPT TO:<%s>\r\n", recipients[i]) && !smtp_reply(c, 250, &line, 0, 0);
    if (ok) ok = !conn_printf(c, "DATA\r\n") && !smtp_reply(c, 354, &line, 0, 0);
    if (ok) {
        struct text body = {0};
        int line_start = 1;
        for (size_t i = 0; i < n; ++i) {
            if (line_start && raw[i] == '.') text_put(&body, ".");
            text_add(&body, raw + i, 1);
            line_start = raw[i] == '\n';
        }
        if (!line_start) text_put(&body, "\r\n");
        text_put(&body, ".\r\n");
        ok = !conn_write(c, body.data, body.size) && !smtp_reply(c, 250, &line, 0, 0);
        free(body.data);
    }
    if (ok) { conn_printf(c, "QUIT\r\n"); smtp_reply(c, 221, &line, 0, 0); error[0] = 0; }
    free(line.data);
    conn_close(c);
    return ok ? 0 : -1;
}
