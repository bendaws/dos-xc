/*
 * DOS-XC /vdm/vdm.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * DOSVM.EXE: a virtual DOS machine, so a 16-bit program can run in a
 * terminal window beside the other programs instead of taking over the
 * computer.  cpu.c interprets the 386 in real mode; video.c is a VGA;
 * bios.c the BIOS, timer and keyboard; dos.c the DOS services.  host.c
 * joins it to DOS-XC and the terminal window that shows it.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

/* ---- the processor ------------------------------------------------------ */
union reg { u32 d; u16 w; struct { u8 l, h; } b; };
enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
enum { ES, CS, SS, DS, FS, GS };
enum { F_CF = 1, F_PF = 4, F_AF = 0x10, F_ZF = 0x40, F_SF = 0x80, F_TF = 0x100, F_IF = 0x200, F_DF = 0x400, F_OF = 0x800 };

struct cpu {
    union reg r[8];
    u16 seg[6];
    u32 base[6];
    u32 ip, flags;
    int halted;                 /* HLT: waits for an interrupt */
    int idle;                   /* a BIOS call is waiting: stop this slice */
    u64 count;                  /* instructions executed */
};
extern struct cpu cpu;
extern u8 *mem;                 /* 1 MiB */

#define AX cpu.r[EAX].w
#define BX cpu.r[EBX].w
#define CX cpu.r[ECX].w
#define DX cpu.r[EDX].w
#define SP cpu.r[ESP].w
#define BP cpu.r[EBP].w
#define SI cpu.r[ESI].w
#define DI cpu.r[EDI].w
#define AL cpu.r[EAX].b.l
#define AH cpu.r[EAX].b.h
#define BL cpu.r[EBX].b.l
#define BH cpu.r[EBX].b.h
#define CL cpu.r[ECX].b.l
#define CH cpu.r[ECX].b.h
#define DL cpu.r[EDX].b.l
#define DH cpu.r[EDX].b.h

void cpu_reset(void);
void cpu_set_seg(int segment, u16 value);
void cpu_run(int instructions);
void cpu_interrupt(int vector);
void cpu_push16(u16 value);

u8 mem_read8(u32 address);
u16 mem_read16(u32 address);
void mem_write8(u32 address, u8 value);
void mem_write16(u32 address, u16 value);
static inline u32 linear(u16 segment, u16 offset) { return ((u32)segment << 4) + offset; }

/* ---- the hardware and BIOS ---------------------------------------------- */
#define VDM_INSTRUCTION_NS 100u  /* the machine's clock: ten million instructions a second */
void bios_init(void);
void bios_trap(int vector);      /* a BIOS or DOS interrupt reached its handler */
void bios_tick(void);            /* timer and keyboard, between slices */
u64 bios_next_event(void);       /* virtual ns of the next timer interrupt */
u32 port_in(u16 port, int size);
void port_out(u16 port, u32 value, int size);
int bios_interrupt_ready(void);  /* an interrupt can be taken now */
void bios_deliver(void);
void bios_set_return_flag(u32 flag, int on);   /* in the FLAGS an IRET will restore */
void bios_wait(void);            /* repeat the current BIOS call */
int bios_key_available(void);
u16 bios_key_read(void);
extern u64 virtual_ns;
extern int vdm_exit_code, vdm_finished;

/* ---- video -------------------------------------------------------------- */
void video_init(void);
void video_set_mode(int mode);
void video_int10(void);
u8 video_read(u32 address);
void video_write(u32 address, u8 value);
u32 video_port_in(u16 port);
void video_port_out(u16 port, u32 value);
void video_put_char(int ch);     /* the console: claimed screen or the terminal */
int video_claimed(void);
int video_cursor_row(void);
int video_cursor_col(void);
void video_present(void);        /* publish a changed screen */
int video_mouse_scale_x(void);
int video_is_graphics(void);
int video_cols(void);
extern u8 bios_font8[256 * 8];

/* ---- DOS ---------------------------------------------------------------- */
int dos_start(const char *program, const char *tail);
void dos_int21(void);
void dos_terminate(int code);
void dos_flush(void);

/* ---- DOS-XC, or a test program on another system ------------------------ */
struct vdm_dirent { char name[260]; u32 attributes; u32 size; i64 mtime; };
u64 host_ns(void);
void host_sleep(u64 nanoseconds);
void host_yield(void);
i64 host_time(void);
int host_key(void);              /* -1 when none */
void host_mouse(int *x, int *y, int *buttons);
void host_speaker(unsigned divisor);   /* 0 silences it */
void host_output(const char *text, size_t size);   /* the terminal, before a screen is claimed */
void host_publish(int graphics, int width, int height, const u32 *pixels, const u8 *text,
                  int cursor_row, int cursor_col, int cursor_on);
void host_frame(void);           /* the published screen has changed */
int host_listdir(const char *dos_path, struct vdm_dirent *entries, int capacity);
int host_fullpath(const char *dos_path, char *out, size_t size);   /* "C:\DIR\FILE" */
void host_native_path(const char *dos_path, char *out, size_t size);
const char *host_environment(void);   /* "NAME=value\0...\0\0" */
unsigned long long host_disk_free(int drive);
