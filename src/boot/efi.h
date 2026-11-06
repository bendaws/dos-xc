/*
 * BenDOS /boot/efi.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "../kernel/bootinfo.h"
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef U64 UINTN;
typedef U64 EFI_STATUS;
typedef void *HANDLE;
typedef U16 CHAR16;
struct GUID { U32 a; U16 b,c; U8 d[8]; };
struct KEY { U16 scan,unicode; };
struct INPUT { void *reset; EFI_STATUS (*read)(void *,struct KEY *); void *event; };
struct TEXT_MODE { int max,mode,attribute,column,row; U8 visible; };
struct OUTPUT {
 void *reset; EFI_STATUS (*write)(void *,CHAR16 *); void *test;
 EFI_STATUS (*query)(void *,UINTN,UINTN *,UINTN *);
 EFI_STATUS (*mode)(void *,UINTN); EFI_STATUS (*attribute)(void *,UINTN);
 EFI_STATUS (*clear)(void *); EFI_STATUS (*cursor)(void *,UINTN,UINTN);
 EFI_STATUS (*visible)(void *,U8); struct TEXT_MODE *state;
};
struct MEDIA { U32 id; U8 removable,present,logical,readonly,cache; U32 block_size,align; U64 last; };
struct BLOCK {
 U64 revision; struct MEDIA *media; void *reset;
 EFI_STATUS (*read)(struct BLOCK *,U32,U64,UINTN,void *);
 EFI_STATUS (*write)(struct BLOCK *,U32,U64,UINTN,const void *);
 EFI_STATUS (*flush)(struct BLOCK *);
};
struct FILE {
 U64 revision; EFI_STATUS (*open)(struct FILE *,struct FILE **,CHAR16 *,U64,U64);
 EFI_STATUS (*close)(struct FILE *); void *delete;
 EFI_STATUS (*read)(struct FILE *,UINTN *,void *);
 void *write,*get_position,*set_position,*get_info;
};
struct FS { U64 revision; EFI_STATUS (*open)(struct FS *,struct FILE **); };
struct LOADED { U32 revision; HANDLE parent; void *table; HANDLE device; };
struct SERVICES {
 U8 header[24]; void *raise,*restore;
 EFI_STATUS (*pages)(U32,U32,UINTN,U64 *); void *free_pages,*memory_map;
 EFI_STATUS (*pool)(U32,UINTN,void **); EFI_STATUS (*free_pool)(void *);
 void *create_event,*set_timer,*wait_event,*signal_event,*close_event,*check_event;
 void *install,*reinstall,*uninstall;
 EFI_STATUS (*protocol)(HANDLE,struct GUID *,void **);
 void *reserved,*notify,*locate_handle,*locate_path,*configuration;
 void *load_image,*start_image,*exit,*unload_image,*exit_services,*monotonic;
 EFI_STATUS (*stall)(UINTN); void *watchdog,*connect,*disconnect,*open_protocol,*close_protocol,*open_info,*protocols;
 EFI_STATUS (*handles)(U32,struct GUID *,void *,UINTN *,HANDLE **);
 void *locate_protocol,*install_multiple,*uninstall_multiple,*crc,*copy,*set,*create_event_ex;
};
struct TABLE {
 U8 header[24]; CHAR16 *vendor; U32 revision;
 HANDLE in_handle; struct INPUT *in;
 HANDLE out_handle; struct OUTPUT *out;
 HANDLE error_handle; void *error,*runtime; struct SERVICES *services;
};
_Static_assert(__builtin_offsetof(struct SERVICES,protocol)==152,"EFI HandleProtocol offset");
_Static_assert(__builtin_offsetof(struct SERVICES,handles)==312,"EFI LocateHandleBuffer offset");
_Static_assert(__builtin_offsetof(struct MEDIA,last)==24,"EFI media layout");
#define GUID_LOADED {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define GUID_FS {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define GUID_BLOCK {0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
