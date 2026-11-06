/*
 * BenDOS /drivers/pcspeaker.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "pcspeaker.h"

static void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void pcspeaker_beep(unsigned frequency, unsigned milliseconds) {
    unsigned divisor = 1193180u / (frequency ? frequency : 1);
    outb(0x43, 0xb6);
    outb(0x42, divisor & 0xff);
    outb(0x42, divisor >> 8);
    unsigned char state = inb(0x61);
    outb(0x61, state | 3);
    for (volatile unsigned i = 0; i < milliseconds * 1000; ++i) __asm__ volatile ("pause");
    outb(0x61, state & 0xfc);
}
