/* DOS-XC package download integrity (SHA-256, FIPS 180-4). */
#pragma once
#include <stddef.h>
#include <stdint.h>
struct pkg_sha256 { uint32_t h[8]; uint64_t bytes; unsigned used; unsigned char block[64]; };
void pkg_sha256_init(struct pkg_sha256 *s);
void pkg_sha256_update(struct pkg_sha256 *s, const void *data, size_t size);
void pkg_sha256_final(struct pkg_sha256 *s, char hex[65]);
