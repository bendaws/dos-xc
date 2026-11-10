/*
 * DOS-XC /runtime/random.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The system's random numbers (DOSXC_SYS_RANDOM).  One SHA-256 chaining
 * value serves as the pool.  Every system call adds the time-stamp counter,
 * so disk, network and keyboard timing accumulate from boot onward, and
 * RDRAND is added where the processor has it.  Output is the pool compressed
 * with a counter, and the pool moves on after every request, so earlier
 * output cannot be recovered from a later state.
 */

#include "runtime.h"

#define RANDOM_MAX_REQUEST 65536u

static const u32 round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static u32 pool[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
static u32 samples[16];
static unsigned sample_count;
static u64 output_counter;
static int rdrand_state = -1;

static u32 rotate(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void compress(u32 state[8], const u32 message[16]) {
    u32 w[64], v[8];
    for (int i = 0; i < 16; ++i) w[i] = message[i];
    for (int i = 16; i < 64; ++i) {
        u32 s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 8; ++i) v[i] = state[i];
    for (int i = 0; i < 64; ++i) {
        u32 s1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25);
        u32 choice = (v[4] & v[5]) ^ (~v[4] & v[6]);
        u32 t1 = v[7] + s1 + choice + round_constants[i] + w[i];
        u32 s0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22);
        u32 majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + s0 + majority;
    }
    for (int i = 0; i < 8; ++i) state[i] += v[i];
}

static u32 counter_low(void) {
    u32 low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return low ^ (high << 16);
}

static int have_rdrand(void) {
    if (rdrand_state < 0) {
        u32 a = 1, b, c = 0, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
        rdrand_state = (c >> 30) & 1;
    }
    return rdrand_state;
}

static u32 hardware_word(void) {
    if (!have_rdrand()) return 0;
    u32 value = 0;
    unsigned char ok = 0;
    for (int tries = 0; tries < 10 && !ok; ++tries) __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=q"(ok));
    return ok ? value : 0;
}

void random_stir(void) {
    samples[sample_count++] ^= counter_low();
    if (sample_count == 16) {
        compress(pool, samples);
        sample_count = 0;
    }
}

long random_syscall(void *buffer, dosxc_size size) {
    unsigned char *out = buffer;
    u32 block[16];
    if (size > RANDOM_MAX_REQUEST) size = RANDOM_MAX_REQUEST;
    long result = (long)size;

    for (int i = 0; i < 16; ++i) block[i] = samples[i] ^ counter_low() ^ hardware_word();
    compress(pool, block);

    while (size) {
        u32 output[8];
        for (int i = 0; i < 8; ++i) output[i] = pool[i];
        for (int i = 0; i < 16; ++i) block[i] = 0;
        block[0] = (u32)output_counter;
        block[1] = (u32)(output_counter >> 32);
        block[2] = 0x4f555450;               /* output, as opposed to the step below */
        ++output_counter;
        compress(output, block);
        for (unsigned i = 0; i < 32 && size; ++i, --size) *out++ = (unsigned char)(output[i / 4] >> ((i % 4) * 8));
    }

    for (int i = 0; i < 16; ++i) block[i] = 0;
    block[0] = (u32)output_counter;
    block[1] = (u32)(output_counter >> 32);
    block[2] = 0x4e455854;                   /* the next pool */
    compress(pool, block);
    return result;
}
