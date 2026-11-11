/*
 * DOS-XC /os/drivers/keyboard.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "keyboard.h"

static void *external_context;
static int (*external_read_key)(void *context);
static unsigned char shifts, caps_lock, extended, control;

static unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(unsigned short port,unsigned char value){__asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port));}
static void acknowledge_keyboard(void){
    /* Protected-mode input polls the controller with interrupts disabled.
       Acknowledge its pending PIC edge too, so a later BIOS disk call cannot
       replay the already-consumed scancode through the firmware IRQ handler.
       Mask other IRQs temporarily; polling must not consume a timer/disk IRQ. */
    unsigned char mask=inb(0x21);
    outb(0x21,0xfd);outb(0x20,0x0c);
    unsigned char pending=inb(0x20);
    if(pending==0x81)outb(0x20,0x20);
    outb(0x21,mask);
}

void keyboard_init(void) { shifts = caps_lock = extended = control = 0; }

void keyboard_set_boot_info(void *context, int (*read_key)(void *context)) {
    external_context = context;
    external_read_key = read_key;
}

int keyboard_read(void) {
    static const char normal[] =
        "\0\x1b" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char shifted[] =
        "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    if (external_read_key) return external_read_key(external_context);
    volatile unsigned char *flags=(volatile unsigned char *)0x417;
    shifts=((*flags&2)?1:0)|((*flags&1)?2:0);
    caps_lock=!!(*flags&64);control=!!(*flags&4);
    volatile unsigned short *head=(volatile unsigned short *)0x41a;
    volatile unsigned short *tail=(volatile unsigned short *)0x41c;
    if(*head!=*tail && *head>=0x1e && *head<0x3e) {
        unsigned short key=*(volatile unsigned short *)(__UINTPTR_TYPE__)(0x400+*head);
        *head=(*head+2>=0x3e)?0x1e:*head+2;
        if((key&255)=='\r')return '\n';
        if(key&255)return key&255;
        unsigned scan=key>>8;
        if(scan==0x48)return 0x101;if(scan==0x50)return 0x102;
        if(scan==0x4d)return 0x103;if(scan==0x4b)return 0x104;
        if(scan==0x47)return 0x105;if(scan==0x4f)return 0x106;
        if(scan==0x53)return 0x108;if(scan==0x52)return 0x107;if(scan==0x49)return 0x109;if(scan==0x51)return 0x10a;
        if(scan>=0x3b&&scan<=0x44)return 0x10b+scan-0x3b;if(scan==0x85)return 0x115;if(scan==0x86)return 0x116;
        return -1;
    }
    if (!(inb(0x64) & 1)) return -1;
    unsigned char code = inb(0x60);
    acknowledge_keyboard();
    if (code == 0xe0 || code == 0xe1) { extended = 1; return -1; }
    if (extended) {
        extended=0;if(code&128)return -1;
        if(code==0x48)return 0x101;if(code==0x50)return 0x102;
        if(code==0x4d)return 0x103;if(code==0x4b)return 0x104;
        if(code==0x47)return 0x105;if(code==0x4f)return 0x106;
        if(code==0x53)return 0x108;if(code==0x52)return 0x107;
        if(code==0x49)return 0x109;if(code==0x51)return 0x10a;return -1;
    }
    unsigned char key = code & 0x7f;
    if (key == 0x2a || key == 0x36) {
        unsigned char bit = key == 0x2a ? 1 : 2;
        if (code & 0x80) shifts &= ~bit;
        else shifts |= bit;
        unsigned char bios_bit=key==0x2a?2:1;
        if(code&128)*flags&=~bios_bit;else *flags|=bios_bit;
        return -1;
    }
    if(key==0x1d){control=!(code&128);if(control)*flags|=4;else *flags&=~4;return -1;}
    if (code & 0x80) return -1;
    if(key>=0x3b&&key<=0x44)return 0x10b+key-0x3b;if(key==0x57)return 0x115;if(key==0x58)return 0x116;
    if (key == 0x3a) { caps_lock ^= 1; *flags^=64; return -1; }
    if (key >= sizeof(normal) - 1 || !normal[key]) return -1;
    char value = normal[key];
    if (value >= 'a' && value <= 'z') {
        if (!!shifts != caps_lock) value -= 'a' - 'A';
    } else if (shifts) value = shifted[key];
    if(control&&((value>='a'&&value<='z')||(value>='A'&&value<='Z')))value&=31;
    return value;
}
