/*
 * DOS-XC /os/runtime/sha256.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

struct sha256 { unsigned state[8]; unsigned long long length; unsigned char block[64]; unsigned used; };
void sha256_init(struct sha256 *s);
void sha256_add(struct sha256 *s, const void *data, unsigned long size);
void sha256_finish(struct sha256 *s, unsigned char out[32]);

/* The stored form of PASSWORD: SALT, then many rounds of SHA-256. */
void password_hash(const unsigned char salt[16], const char *password, unsigned char out[32]);
