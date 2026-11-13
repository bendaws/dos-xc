/* Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "sha256.h"
#include <string.h>

static uint32_t rotate(uint32_t n, unsigned bits) { return (n >> bits) | (n << (32 - bits)); }
static void transform(struct pkg_sha256 *s) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned char *p = s->block + i * 4;
        w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t a = w[i-15], b = w[i-2];
        w[i] = w[i-16] + (rotate(a,7) ^ rotate(a,18) ^ (a>>3)) + w[i-7] + (rotate(b,17) ^ rotate(b,19) ^ (b>>10));
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t = h + (rotate(e,6)^rotate(e,11)^rotate(e,25)) + ((e&f)^(~e&g)) + k[i] + w[i];
        uint32_t u = (rotate(a,2)^rotate(a,13)^rotate(a,22)) + ((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=d+t; d=c; c=b; b=a; a=t+u;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}
void pkg_sha256_init(struct pkg_sha256 *s) {
    static const uint32_t initial[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, initial, sizeof(initial)); s->bytes=0; s->used=0;
}
void pkg_sha256_update(struct pkg_sha256 *s, const void *data, size_t size) {
    const unsigned char *p=data;
    s->bytes += size;
    while (size) {
        size_t n=64-s->used; if (n>size) n=size;
        memcpy(s->block+s->used,p,n); s->used+=(unsigned)n; p+=n; size-=n;
        if (s->used==64) { transform(s); s->used=0; }
    }
}
void pkg_sha256_final(struct pkg_sha256 *s, char hex[65]) {
    uint64_t bits=s->bytes*8;
    unsigned char padding[128]={0x80};
    size_t n=s->used<56 ? 56-s->used : 120-s->used;
    pkg_sha256_update(s,padding,n);
    for (unsigned i=0;i<8;++i) padding[i]=(unsigned char)(bits >> (56-i*8));
    pkg_sha256_update(s,padding,8);
    static const char digits[]="0123456789abcdef";
    for (unsigned i=0;i<32;++i) { unsigned b=(s->h[i/4] >> (24-(i%4)*8))&255; hex[i*2]=digits[b>>4]; hex[i*2+1]=digits[b&15]; }
    hex[64]=0;
}
