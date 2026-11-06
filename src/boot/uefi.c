/*
 * BenDOS /boot/uefi.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "efi.h"
#include "../setup/install.h"

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
static void text(const char *s) {
 CHAR16 out[2]={0,0};
 while(*s) { if(*s=='\n'){out[0]='\r';st->out->write(st->out,out);}char c=*s++;out[0]=(U8)c;st->out->write(st->out,out);__asm__ volatile("outb %0, $0xe9"::"a"((U8)c));if(c=='\n')for(int i=0;i<indent;++i){out[0]=' ';st->out->write(st->out,out);} }
}
static void at(UINTN x,UINTN y,const char *s) { st->out->cursor(st->out,x,y);text(s); }
static void number(U64 n) { char b[24];int i=0;do{b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i){char s[2]={b[--i],0};text(s);} }
static struct KEY key(void) {
 struct KEY k;while(st->in->read(st->in,&k))st->services->stall(1000);return k;
}
static void screen(const char *body) {
 st->out->attribute(st->out,0x17);st->out->clear(st->out);
 at(1,1,BENDOS_LABEL " Setup");at(0,2,"================");
 st->out->attribute(st->out,0x70);at(0,24,"                                                                               ");
 at(2,24,"ENTER=Continue  F1=Help  F3=Exit");st->out->attribute(st->out,0x1f);
 at(6,4,body);
}
static void fatal(const char *s) { screen(s);text("\n\nPress any key to stop.");key();for(;;)__asm__ volatile("hlt"); }
static int read_block(struct BLOCK *disk,U64 lba,UINTN bytes,void *p) {
 return disk->read(disk,disk->media->id,lba,bytes,p)!=0;
}
static int write_block(struct BLOCK *disk,U64 lba,UINTN bytes,const void *p) {
 return disk->write(disk,disk->media->id,lba,bytes,p)!=0;
}
static int file_read(const char *path,void *p,UINTN *size) {
 CHAR16 name[128];unsigned i=0;while(path[i] && i<127){name[i]=(U8)path[i];++i;}name[i]=0;
 struct FILE *f;if(root->open(root,&f,name,1,0))return -1;
 EFI_STATUS result=f->read(f,size,p);f->close(f);return result?-1:0;
}
static void help(void) {
 screen("Setup Help\n\nENTER continues. F3 leaves Setup.\n\nSelect a physical internal disk of at least 64 MiB.\nSetup replaces its partition table and creates a full-drive FAT32\nsystem partition. Existing partitions will be lost.\n\nNo disk is written until you type ERASE and press ENTER.\n\nPress any key to return.");key();
}
static int discover(void) {
 /* A removable boot path may leave internal storage controllers disconnected. */
 HANDLE *all;UINTN all_count;
 if(!st->services->handles(0,0,0,&all_count,&all)){
  typedef EFI_STATUS (*CONNECT)(HANDLE,HANDLE *,void *,U8);
  CONNECT connect=(CONNECT)st->services->connect;
  for(UINTN i=0;i<all_count;++i)connect(all[i],0,0,1);
  st->services->free_pool(all);
 }
 HANDLE *handles;UINTN count;
 if(st->services->handles(2,&block_guid,0,&count,&handles))return -1;
 target_count=0;source=0;
 for(UINTN i=0;i<count;++i) {
  struct BLOCK *disk;
  if(st->services->protocol(handles[i],&block_guid,(void **)&disk))continue;
  struct MEDIA *m=disk->media;
  if(!m->present || m->logical || m->block_size!=512)continue;
  if(!read_block(disk,0,512,buffer) && get32(buffer+440)==source_serial){source=disk;continue;}
  if(m->removable || m->readonly || m->last<131071 || m->last>=0xffffffffull || target_count==9)continue;
  targets[target_count++]=disk;
 }
 st->services->free_pool(handles);return 0;
}
static int __attribute__((sysv_abi)) sector_read(void *,U32,void *);
static int __attribute__((sysv_abi)) sector_write(void *,U32,const void *);
static void install_progress(void *ctx,const char *phase,unsigned percent){
 (void)ctx;at(6,8,"                                                                    ");at(6,8,phase);
 at(6,10,"                    ");if(percent){at(6,10,"");number(percent);text("%");}
}
static int install(struct BLOCK *disk) {
 screen("Installing system files...\n\nCreating C:\\SYSTEM and copying the system components.\nEvery copied block is read back and verified.\n\nDo not turn off your computer.");
 struct fat32_volume source_filesystem;
 source_filesystem.context=volume;source_filesystem.read_sector=sector_read;source_filesystem.write_sector=0;
 if(disk->media->last>=0xffffffffull||fat32_mount(&source_filesystem,0))return -1;
 if(install_system(&source_filesystem,disk,(U32)(disk->media->last+1),sector_read,sector_write,install_progress,0))return -1;
 if(disk->flush&&disk->flush(disk))return -1;return 0;
}

static void setup(void) {
welcome:
 for(;;) {
  screen("Welcome to Setup.\n\nSetup installs Ben's DOS on an internal drive.\nSystem files will be stored in C:\\SYSTEM.\n\nFor more information, press F1.\n\nTo choose an installation drive, press ENTER.\n\nTo leave Setup without installing, press F3.");
  struct KEY k=key();if(k.scan==11){help();continue;}if(k.scan==13)return;if(k.unicode==13)break;
 }
 if(discover()) {screen("The physical setup drive could not be identified.\n\nSetup has not written any disk.\nPress any key to return.");key();goto welcome;}
 if(!target_count){screen("No eligible internal disk was found.\n\nAttach a writable 512-byte-sector disk of at least 64 MiB.\nPress any key to return.");key();goto welcome;}
 UINTN selected=0;
 for(;;) {
  screen("Choose the internal drive for your installation.\n\nUse arrows or a number, then press ENTER.\nThe external setup drive is excluded.");
  for(UINTN i=0;i<target_count;++i){at(6,9+i,i==selected?"> ":"  ");number(i+1);text(". Internal disk   ");number((targets[i]->media->last+1)/2048);text(" MiB");}
  struct KEY k=key();if(k.scan==13)return;if(k.scan==11){help();continue;}
  if(k.scan==1 && selected)--selected;if(k.scan==2 && selected+1<target_count)++selected;
  if(k.unicode>='1' && k.unicode<'1'+target_count)selected=k.unicode-'1';
  if(k.unicode!=13)continue;
  screen("Confirm installation\n\nSelected destination: internal disk ");number(selected+1);text("  (");number((targets[selected]->media->last+1)/2048);text(" MiB)\n\nWARNING: All existing partitions on this drive will be lost.\nSetup replaces its partition table and uses all available\nspace for a FAT32 system partition.\n\nType ERASE and press ENTER to install.\nPress ESC to choose another drive, or F3 to exit.\n\nConfirmation: ");
  char answer[6];unsigned used=0;int confirmed=0;
  for(;;) {
   k=key();if(k.scan==13)return;if(k.unicode==27)break;
   if(k.unicode==13){answer[used]=0;if(used==5 && answer[0]=='E'&&answer[1]=='R'&&answer[2]=='A'&&answer[3]=='S'&&answer[4]=='E')confirmed=1;break;}
   if(k.unicode==8 && used){--used;text("\b \b");continue;}
   U16 c=k.unicode;if(c>='a'&&c<='z')c-=32;
   if(c>='A'&&c<='Z'&&used<5){answer[used++]=(char)c;char s[2]={(char)c,0};text(s);}
  }
  if(!confirmed)continue;
  if(install(targets[selected])){screen("Setup could not read, write, or verify the selected drive.\nInstallation is incomplete. Do not boot this disk.\n\nPress any key to return.");key();goto welcome;}
  screen("Setup has finished installing Ben's DOS.\n\nSystem directory: C:\\SYSTEM\n\nRemove the external setup drive, then press ENTER\nto restart from the internal drive.");
  while(key().unicode!=13){}
  typedef void (*RESET)(U32,EFI_STATUS,UINTN,void *);
  RESET reset=*(RESET *)((U8 *)st->runtime+104);reset(0,0,0,0);
  for(;;)__asm__ volatile("hlt");
 }
}
static int __attribute__((sysv_abi)) sector_read(void *ctx,U32 lba,void *out) {return read_block(ctx,lba,512,out)?-1:0;}
static int __attribute__((sysv_abi)) sector_write(void *ctx,U32 lba,const void *in) {return write_block(ctx,lba,512,in)?-1:0;}
static void __attribute__((sysv_abi)) output(void *ctx,char c) {(void)ctx;char s[2]={c,0};text(s);}
static void __attribute__((sysv_abi)) clear(void *ctx) {(void)ctx;st->out->clear(st->out);}
static void __attribute__((sysv_abi)) color(void *ctx,U8 c) {(void)ctx;st->out->attribute(st->out,c&127);}
static void __attribute__((sysv_abi)) cursor(void *ctx,unsigned row,unsigned col){(void)ctx;st->out->cursor(st->out,col,row);}
static int __attribute__((sysv_abi)) input(void *ctx) {
 (void)ctx;struct KEY k;if(st->in->read(st->in,&k))return -1;
 if(k.unicode==13)return '\n';if(k.scan)return 0x100+k.scan;return k.unicode?k.unicode:-1;
}
static void boot_kernel(void) {
 indent=0;st->out->visible(st->out,1);
 U64 address=0x2000000;
 if(st->services->pages(2,2,768,&address))fatal("Cannot reserve memory for the system components.");
 U64 programs=0x4000000;if(st->services->pages(2,2,768,&programs))fatal("Cannot reserve command program memory.");
 UINTN size=65536;
 if(file_read("\\SYSTEM\\KERNELU.BIN",(void *)0x2000000,&size))fatal("Cannot load SYSTEM\\KERNELU.BIN.");
 size=65536;if(file_read("\\SYSTEM\\COREU.DLL",(void *)0x2100000,&size))fatal("Cannot load SYSTEM\\COREU.DLL.");
 size=65536;if(file_read("\\SYSTEM\\SHELLU.DLL",(void *)0x2200000,&size))fatal("Cannot load SYSTEM\\SHELLU.DLL.");
 U32 *core=(U32 *)0x2100000,*shell=(U32 *)0x2200000;
 if(core[0]!=0x4c4c4442||shell[0]!=0x4c4c4442||core[1]!=1||shell[1]!=1||core[2]!=64||shell[2]!=64)fatal("Invalid system module ABI.");
 struct fat32_volume filesystem;filesystem.context=volume;filesystem.read_sector=sector_read;filesystem.write_sector=volume->media->readonly?0:sector_write;
 if(fat32_mount(&filesystem,0))fatal("The system FAT filesystem could not be mounted.");
 struct boot_info info;
 info.read_key=input;info.keyboard_context=0;info.write_char=output;info.console_context=0;
 info.volume=&filesystem;info.clear_screen=clear;info.set_color=color;info.drive_letter=external?'X':'C';info.set_cursor=cursor;
 st->out->attribute(st->out,7);st->out->clear(st->out);
 typedef void (__attribute__((sysv_abi)) *ENTRY)(struct boot_info *);
 ((ENTRY)0x2000000)(&info);
}
EFI_STATUS efi_main(HANDLE image,struct TABLE *table) {
 st=table;st->out->visible(st->out,0);struct LOADED *loaded;struct FS *fs;
 if(st->services->protocol(image,&loaded_guid,(void **)&loaded)||st->services->protocol(loaded->device,&fs_guid,(void **)&fs)||fs->open(fs,&root)||st->services->protocol(loaded->device,&block_guid,(void **)&volume))return 1;
 for(UINTN i=0;i<(UINTN)st->out->state->max;++i){UINTN w,h;if(!st->out->query(st->out,i,&w,&h)&&w==80&&h==25){st->out->mode(st->out,i);break;}}
 U64 work=0;if(st->services->pages(0,2,16,&work))return 1;buffer=(U8 *)work;verify_buffer=buffer+32768;
 if(read_block(volume,0,512,buffer)||get32(buffer+0x1a0)!=0x53464442)fatal("Invalid BenDOS system volume.");
 source_serial=get32(buffer+((buffer[22]||buffer[23])?39:67));copy_end=get32(buffer+0x1ac);config_lba=get32(buffer+0x1b4);external=buffer[0x1aa]!=0;
 if(buffer[0x1ab]!=64)fatal("Use the x64 setup image for UEFI installation.");
 if(external)setup();boot_kernel();return 0;
}
