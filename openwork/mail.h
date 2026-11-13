/*
 * DOS-XC /openwork/mail.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "office.h"
#include <time.h>

enum { SECURITY_SSL, SECURITY_STARTTLS, SECURITY_NONE };
enum { PROTOCOL_IMAP, PROTOCOL_POP3 };

struct account {
    char name[80], address[120];
    int protocol;
    char in_host[120], user[120], password[120], out_host[120];
    int in_port, in_security, out_port, out_security;
    int out_auth;                         /* sign in to the outgoing server too */
};
int account_load(struct account *a);      /* 0 when %HOME%\OPENMAIL.CFG has an account */
int account_save(const struct account *a);
void account_guess(struct account *a);    /* fills empty server settings from the address */

enum { FOLDER_INBOX, FOLDER_DRAFTS, FOLDER_SENT, FOLDER_TRASH, FOLDER_JUNK, FOLDER_ARCHIVE, FOLDER_OTHER };
struct folder { char name[160], label[64]; int kind, unread; };

struct message {
    unsigned uid;                         /* IMAP UID, or the POP3 message number */
    int seen, answered, flagged, attachment;
    long size;
    time_t when;
    char from[120], address[120], to[300], cc[300], subject[240], message_id[160], references[400];
};

/* A connection to the incoming server; the program waits in mail_idle while the network is slow. */
struct mailbox {
    struct account account;
    void *conn;
    char selected[160];
    int exists;
    char error[200];
    unsigned tag;
};
extern void (*mail_idle)(void);
extern char mail_ca_file[260];            /* the trusted root certificates */
extern int mail_tls;                      /* 0 when built without TLS */

int mail_folders(struct mailbox *m, struct folder **out, int *count);
int mail_list(struct mailbox *m, const char *folder, int newest, struct message **out, int *count);
int mail_fetch(struct mailbox *m, const char *folder, const struct message *msg, struct text *raw);
int mail_set_seen(struct mailbox *m, const char *folder, const struct message *msg, int seen);
int mail_delete(struct mailbox *m, const char *folder, const struct message *msg, const char *trash);
int mail_append(struct mailbox *m, const char *folder, const char *raw, size_t n);   /* IMAP only */
void mail_disconnect(struct mailbox *m);
int smtp_send(const struct account *a, const char *raw, size_t n, char recipients[][120], int count, char *error, size_t size);

/* MIME (mime.c): text inside the program is Latin-1. */
size_t header_size(const char *raw, size_t n);
void header_get(const char *raw, size_t n, const char *name, char *out, size_t size);
time_t mail_date(const char *s);          /* RFC 5322 or IMAP INTERNALDATE; 0 if unreadable */
void address_name(const char *field, char *name, size_t name_size, char *address, size_t address_size);
int address_list(const char *field, char out[][120], int max);
struct attachment { char name[128], type[64]; size_t offset, length; int encoding; };
struct parsed { struct text body; struct attachment *attachments; int attachment_count; };
void mime_parse(const char *raw, size_t n, struct parsed *out);
void mime_free(struct parsed *p);
int attachment_decode(const char *raw, const struct attachment *a, struct text *out);
void base64_encode(struct text *t, const unsigned char *data, size_t n, int wrap);
/* A message ready to send: TEXT is Latin-1; FILES are attachment paths, '\n'-separated. */
void mail_compose(struct text *out, const struct account *a, const char *to, const char *cc, const char *subject,
                  const char *text, const char *files, const char *in_reply_to, const char *references);
