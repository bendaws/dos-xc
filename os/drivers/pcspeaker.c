/*
 * DOS-XC /os/drivers/pcspeaker.c
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
    /* Time the tone with the refresh-request bit (port 0x61 bit 4), which
       every PC toggles about every 15.085 microseconds whatever the CPU
       speed: 66.29 toggles per millisecond. */
    if (milliseconds > 60000) milliseconds = 60000;
    unsigned toggles = milliseconds * 66 + milliseconds * 29 / 100;
    unsigned char last = inb(0x61) & 0x10;
    for (unsigned seen = 0, spins = 0; seen < toggles; ) {
        unsigned char now = inb(0x61) & 0x10;
        if (now != last) { last = now; ++seen; spins = 0; }
        else if (++spins > 1000000) break;
    }
    outb(0x61, state & 0xfc);
}
