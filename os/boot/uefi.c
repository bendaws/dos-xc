/*
 * DOS-XC /os/boot/uefi.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "efi.h"
#include "../kernel/bootinfo.h"

static struct TABLE *st;
static struct FILE *root;
static struct BLOCK *volume,*source;
static struct GUID loaded_guid=GUID_LOADED,fs_guid=GUID_FS,block_guid=GUID_BLOCK;
static struct BLOCK *targets[9];
static UINTN target_count;
static U8 *buffer,*verify_buffer;
static U32 source_serial,copy_end,config_lba;
static int external;
static int indent=6;
void *memset(void *p,int c,UINTN n) { U8 *b=p;while(n--)*b++=c;return p; }
void *memcpy(void *d,const void *s,UINTN n) { U8 *a=d;const U8 *b=s;while(n--)*a++=*b++;return d; }
static U32 get32(U8 *p) { return p[0]|((U32)p[1]<<8)|((U32)p[2]<<16)|((U32)p[3]<<24); }
static void put32(U8 *p,U32 n) { for(int i=0;i<4;++i)p[i]=n>>(i*8); }
/* CP437 block and shade characters (the logo, Setup's progress bar) as Unicode. */
static CHAR16 glyph(U8 c) {
 switch(c){case 0xb0:return 0x2591;case 0xb1:return 0x2592;case 0xb2:return 0x2593;case 0xdb:return 0x2588;
  case 0xdc:return 0x2584;case 0xdd:return 0x258c;case 0xde:return 0x2590;case 0xdf:return 0x2580;}
 return c;
}
static void text(const char *s) {
 CHAR16 out[2]={0,0};
 while(*s) { if(*s=='\n'){out[0]='\r';st->out->write(st->out,out);}char c=*s++;out[0]=glyph((U8)c);st->out->write(st->out,out);__asm__ volatile("outb %0, $0xe9"::"a"((U8)c));if(c=='\n')for(int i=0;i<indent;++i){out[0]=' ';st->out->write(st->out,out);} }
}
static void at(UINTN x,UINTN y,const char *s) { st->out->cursor(st->out,x,y);text(s); }
static void number(U64 n) { char b[24];int i=0;do{b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i){char s[2]={b[--i],0};text(s);} }
static struct KEY key(void) {
 struct KEY k;while(st->in->read(st->in,&k))st->services->stall(1000);return k;
}
static void screen(const char *body) {
 st->out->attribute(st->out,0x17);st->out->clear(st->out);
 at(1,1,DOSXC_LABEL);at(0,2,"==================================");
 st->out->attribute(st->out,0x70);at(0,24,"                                                                               ");
 at(2,24,"ENTER=Continue  F1=Help  F3=Exit");st->out->attribute(st->out,0x1f);
 at(6,4,body);
}
static void fatal(const char *s) { screen(s);text("\n\nPress any key to stop.");key();for(;;)__asm__ volatile("hlt"); }
static U8 block_cache[4096];
static struct BLOCK *cached_disk;
static U64 cached_block=~0ull;
static int read_block(struct BLOCK *disk,U64 lba,UINTN bytes,void *p) {
 U32 size=disk->media->block_size;
 if(size==512)return disk->read(disk,disk->media->id,lba,bytes,p)!=0;
 if(size<512||size>sizeof(block_cache)||size%512)return 1;
 U8 *out=p;U32 per=size/512;
 for(UINTN done=0;done<bytes;done+=512,++lba){
  U64 block=lba/per;
  if(cached_disk!=disk||cached_block!=block){
   if(disk->read(disk,disk->media->id,block,size,block_cache)){cached_disk=0;return 1;}
   cached_disk=disk;cached_block=block;
  }
  memcpy(out+done,block_cache+(lba%per)*512,bytes-done<512?bytes-done:512);
 }
 return 0;
}
static int write_block(struct BLOCK *disk,U64 lba,UINTN bytes,const void *p) {
 if(disk->media->block_size!=512)return 1;
 if(cached_disk==disk)cached_disk=0;
 return disk->write(disk,disk->media->id,lba,bytes,p)!=0;
}
static int file_read(const char *path,void *p,UINTN *size) {
 CHAR16 name[128];unsigned i=0;while(path[i] && i<127){name[i]=(U8)path[i];++i;}name[i]=0;
 struct FILE *f;if(root->open(root,&f,name,1,0))return -1;
 EFI_STATUS result=f->read(f,size,p);f->close(f);return result?-1:0;
}
static struct BLOCK *disks[16];
static HANDLE disk_handles[16];
static unsigned disk_flags[16];
static UINTN disk_total;
static U8 *bounce;
/* The disk is still there: its handle still carries the same Block I/O. */
static int disk_alive(unsigned i) {
 struct BLOCK *b;
 return i<disk_total&&disks[i]&&!st->services->protocol(disk_handles[i],&block_guid,(void **)&b)&&b==disks[i];
}
static void add_disks(int first_time) {
 HANDLE *handles;UINTN count;
 if(st->services->handles(2,&block_guid,0,&count,&handles))return;
 for(UINTN d=0;d<disk_total;++d){
  if(!disks[d])continue;
  int found=0;for(UINTN i=0;i<count;++i)if(handles[i]==disk_handles[d])found=1;
  if(!found||!disk_alive((unsigned)d))disks[d]=0;
 }
 for(UINTN i=0;i<count;++i) {
  int known=0;for(UINTN d=0;d<disk_total;++d)if(disks[d]&&disk_handles[d]==handles[i])known=1;
  if(known)continue;
  struct BLOCK *disk;
  if(st->services->protocol(handles[i],&block_guid,(void **)&disk))continue;
  struct MEDIA *m=disk->media;
  if(m->logical || m->block_size!=512)continue;
  if(first_time&&!m->present)continue;
  unsigned flags=(m->removable?DISK_FLAG_REMOVABLE:0)|(m->readonly?DISK_FLAG_READONLY:0);
  if(first_time&&!read_block(disk,0,512,buffer) && get32(buffer+440)==source_serial)flags|=DISK_FLAG_SYSTEM;
  UINTN slot=disk_total;
  if(slot==16)break;
  ++disk_total;
  disks[slot]=disk;disk_handles[slot]=handles[i];disk_flags[slot]=flags;
 }
 st->services->free_pool(handles);
}
static void collect_disks(void) {
 HANDLE *all;UINTN all_count;
 if(!st->services->handles(0,0,0,&all_count,&all)){
  typedef EFI_STATUS (*CONNECT)(HANDLE,HANDLE *,void *,U8);
  CONNECT connect=(CONNECT)st->services->connect;
  for(UINTN i=0;i<all_count;++i)connect(all[i],0,0,1);
  st->services->free_pool(all);
 }
 add_disks(1);
}
static unsigned __attribute__((sysv_abi)) firmware_disk_count(void){return (unsigned)disk_total;}
static unsigned __attribute__((sysv_abi)) firmware_disk_rescan(void){add_disks(0);return (unsigned)disk_total;}
static int __attribute__((sysv_abi)) firmware_disk_info(unsigned i,U64 *sectors,unsigned *flags){
 if(!disk_alive(i)||!disks[i]->media->present)return -1;
 *sectors=disks[i]->media->last+1;*flags=disk_flags[i]|(disks[i]->media->readonly?DISK_FLAG_READONLY:0);return 0;
}
/* Simple Pointer devices (mice), found again every second. */
struct POINTER_STATE { int x,y,z; U8 left,right; };
struct POINTER_MODE { U64 rx,ry,rz; U8 left,right; };
struct POINTER { void *reset; EFI_STATUS (*state)(struct POINTER *,struct POINTER_STATE *); void *event; struct POINTER_MODE *mode; };
static struct GUID pointer_guid={0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
static struct POINTER *pointers[4];
static UINTN pointer_count,pointer_polls;
static int __attribute__((sysv_abi)) firmware_pointer(int *dx,int *dy,int *wheel,unsigned *buttons){
 static unsigned held;
 if(pointer_polls++%200==0){
  HANDLE *handles;UINTN count;pointer_count=0;
  if(!st->services->handles(2,&pointer_guid,0,&count,&handles)){
   for(UINTN i=0;i<count&&pointer_count<4;++i){struct POINTER *p;
    if(!st->services->protocol(handles[i],&pointer_guid,(void **)&p))pointers[pointer_count++]=p;}
   st->services->free_pool(handles);
  }
 }
 *dx=*dy=*wheel=0;
 for(UINTN i=0;i<pointer_count;++i){
  struct POINTER_STATE s;
  if(pointers[i]->state(pointers[i],&s))continue;
  /* Resolution is in counts per millimetre: about 2 mm per text column
     (the runtime moves one column per 8 units). */
  long long rx=pointers[i]->mode&&pointers[i]->mode->rx?(long long)pointers[i]->mode->rx:2;
  long long ry=pointers[i]->mode&&pointers[i]->mode->ry?(long long)pointers[i]->mode->ry:2;
  *dx+=(int)((long long)s.x*4/rx);*dy+=(int)((long long)s.y*4/ry);
  *wheel+=s.z>0?-1:s.z<0?1:0;
  held=(s.left?1u:0)|(s.right?2u:0);
 }
 *buttons=held;
 return pointer_count>0;
}
static int __attribute__((sysv_abi)) firmware_disk_io(unsigned i,U64 lba,unsigned count,void *data,int write){
 if(!disk_alive(i)||!count||count>64)return -1;
 struct BLOCK *disk=disks[i];UINTN bytes=(UINTN)count*512;
 if(write){
  if(disk->media->readonly)return -1;
  memcpy(bounce,data,bytes);if(write_block(disk,lba,bytes,bounce))return -1;
  if(disk->flush)disk->flush(disk);return 0;
 }
 if(read_block(disk,lba,bytes,bounce))return -1;memcpy(data,bounce,bytes);return 0;
}
static void __attribute__((sysv_abi)) firmware_reset(void){
 typedef void (*RESET)(U32,EFI_STATUS,UINTN,void *);
 RESET reset=*(RESET *)((U8 *)st->runtime+104);reset(0,0,0,0);
 for(;;)__asm__ volatile("hlt");
}
static void __attribute__((sysv_abi)) firmware_poweroff(void){
 typedef void (*RESET)(U32,EFI_STATUS,UINTN,void *);
 RESET reset=*(RESET *)((U8 *)st->runtime+104);reset(2,0,0,0);
}
static U64 acpi_rsdp(void){
 static const U8 acpi20[16]={0x71,0xe8,0x68,0x88,0xf1,0xe4,0xd3,0x11,0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81};
 static const U8 acpi10[16]={0x30,0x2d,0x9d,0xeb,0x88,0x2d,0xd3,0x11,0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d};
 U64 found=0;
 for(UINTN i=0;i<st->table_count;++i){
  const U8 *g=(const U8 *)&st->tables[i].guid;int is20=1,is10=1;
  for(int k=0;k<16;++k){if(g[k]!=acpi20[k])is20=0;if(g[k]!=acpi10[k])is10=0;}
  if(is20)return (U64)st->tables[i].table;
  if(is10)found=(U64)st->tables[i].table;
 }
 return found;
}
static int __attribute__((sysv_abi)) sector_read(void *ctx,U32 lba,void *out) {return read_block(ctx,lba,512,out)?-1:0;}
static int __attribute__((sysv_abi)) sector_write(void *ctx,U32 lba,const void *in) {return write_block(ctx,lba,512,in)?-1:0;}
static void __attribute__((sysv_abi)) output(void *ctx,char c) {(void)ctx;char s[2]={c,0};text(s);}
static void __attribute__((sysv_abi)) clear(void *ctx) {(void)ctx;st->out->clear(st->out);}
static void __attribute__((sysv_abi)) color(void *ctx,U8 c) {(void)ctx;st->out->attribute(st->out,c&127);}
static void __attribute__((sysv_abi)) cursor(void *ctx,unsigned row,unsigned col){(void)ctx;st->out->cursor(st->out,col,row);}
static int __attribute__((sysv_abi)) input(void *ctx) {
 (void)ctx;struct KEY k;if(st->in->read(st->in,&k))return -1;
 if(k.unicode==13)return '\n';if(k.scan==0x17)return 27;if(k.scan)return 0x100+k.scan;return k.unicode?k.unicode:-1;
}
/* The Graphics Output Protocol, for the desktop: 32-bit BGRX modes only. */
struct GOP_INFO { U32 version,width,height,format; U32 masks[4]; U32 pitch; };
struct GOP_MODE { U32 max,mode; struct GOP_INFO *info; UINTN size; U64 base; UINTN base_size; };
struct GOP { EFI_STATUS (*query)(struct GOP *,U32,UINTN *,struct GOP_INFO **); EFI_STATUS (*set)(struct GOP *,U32); void *blt; struct GOP_MODE *mode; };
static struct GUID gop_guid={0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static struct GOP *gop;
static int __attribute__((sysv_abi)) firmware_display(int op,unsigned index,unsigned *w,unsigned *h,unsigned *pitch,U64 *base){
 if(op==3){st->out->mode(st->out,st->out->state->mode);st->out->clear(st->out);return 0;}
 if(!gop){HANDLE *handles;UINTN count;
  if(st->services->handles(2,&gop_guid,0,&count,&handles)||!count||st->services->protocol(handles[0],&gop_guid,(void **)&gop)){gop=0;return op?-1:0;}}
 if(op==0)return (int)gop->mode->max;
 if(op==2){if(gop->set(gop,index))return -1;}
 UINTN size;struct GOP_INFO *info;
 if(gop->query(gop,index,&size,&info)||info->format!=1)return -1;
 *w=info->width;*h=info->height;*pitch=info->pitch*4;*base=gop->mode->base;return 0;
}
static void boot_kernel(void) {
 indent=0;st->out->visible(st->out,1);
 U64 address=0x2000000;
 if(st->services->pages(2,2,768,&address))fatal("Cannot reserve memory for the system components.");
 U64 programs=0x4000000;if(st->services->pages(2,2,4096,&programs))fatal("Cannot reserve command program memory.");
 /* The largest block the firmware will spare becomes the 32/64-bit program
    arena; the rest stays available to the firmware's own drivers. */
 static const U32 arena_mib[]={3072,2048,1536,1024,768,512,384,256,192,128,96,64,48,32};
 U64 arena=0,arena_size=0;
 for(UINTN i=0;i<sizeof(arena_mib)/sizeof(arena_mib[0]);++i){U64 at=0;
  if(!st->services->pages(0,2,(UINTN)arena_mib[i]*256,&at)){arena=at;arena_size=(U64)arena_mib[i]<<20;break;}}
 UINTN size=65536;
 if(file_read("\\SYSTEM\\KERNELU.BIN",(void *)0x2000000,&size))fatal("Cannot load SYSTEM\\KERNELU.BIN.");
 size=65536;if(file_read("\\SYSTEM\\COREU.DLL",(void *)0x2100000,&size))fatal("Cannot load SYSTEM\\COREU.DLL.");
 size=65536;if(file_read("\\SYSTEM\\SHELLU.DLL",(void *)0x2200000,&size))fatal("Cannot load SYSTEM\\SHELLU.DLL.");
 U32 *core=(U32 *)0x2100000,*shell=(U32 *)0x2200000;
 if(core[0]!=0x4c4c4442||shell[0]!=0x4c4c4442||core[1]!=1||shell[1]!=1||core[2]!=64||shell[2]!=64)fatal("Invalid system module ABI.");
 struct fat32_volume filesystem={0};filesystem.context=volume;filesystem.read_sector=sector_read;filesystem.write_sector=volume->media->readonly?0:sector_write;
 if(fat32_mount(&filesystem,0))fatal("The system FAT filesystem could not be mounted.");
 struct boot_info info;
 info.read_key=input;info.keyboard_context=0;info.write_char=output;info.console_context=0;
 info.volume=&filesystem;info.clear_screen=clear;info.set_color=color;info.drive_letter=external?'X':'C';info.set_cursor=cursor;
 info.memory_base=arena;info.memory_size=arena_size;
 collect_disks();
 info.disk_count=firmware_disk_count;info.disk_info=firmware_disk_info;info.disk_io=firmware_disk_io;info.reboot=firmware_reset;
 info.disk_rescan=firmware_disk_rescan;info.pointer=firmware_pointer;
 info.poweroff=firmware_poweroff;info.acpi_rsdp=acpi_rsdp();info.display=firmware_display;
 st->out->attribute(st->out,7);st->out->clear(st->out);
 typedef void (__attribute__((sysv_abi)) *ENTRY)(struct boot_info *);
 ((ENTRY)0x2000000)(&info);
}
EFI_STATUS efi_main(HANDLE image,struct TABLE *table) {
 st=table;st->out->visible(st->out,0);struct LOADED *loaded;struct FS *fs;
 if(st->services->protocol(image,&loaded_guid,(void **)&loaded)||st->services->protocol(loaded->device,&fs_guid,(void **)&fs)||fs->open(fs,&root)||st->services->protocol(loaded->device,&block_guid,(void **)&volume))return 1;
 for(UINTN i=0;i<(UINTN)st->out->state->max;++i){UINTN w,h;if(!st->out->query(st->out,i,&w,&h)&&w==80&&h==25){st->out->mode(st->out,i);break;}}
 U64 work=0;if(st->services->pages(0,2,16,&work))return 1;buffer=(U8 *)work;verify_buffer=buffer+32768;
 if(read_block(volume,0,512,buffer)||get32(buffer+0x1a0)!=0x53464442)fatal("Invalid DOS-XC system volume.");
 source_serial=get32(buffer+((buffer[22]||buffer[23])?39:67));copy_end=get32(buffer+0x1ac);config_lba=get32(buffer+0x1b4);external=buffer[0x1aa]!=0;
 if(buffer[0x1ab]!=64)fatal("This is the x86 edition of DOS-XC; use the x64 edition for UEFI boot.");
 bounce=buffer+32768;(void)config_lba;(void)copy_end;(void)verify_buffer;
 boot_kernel();return 0;
}
