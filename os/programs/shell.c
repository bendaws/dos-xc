/*
 * DOS-XC /os/programs/shell.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "util.h"
#include "../kernel/bootinfo.h"
#include "../kernel/console.h"
#include "../drivers/keyboard.h"
#include "../drivers/pcspeaker.h"
extern void enter_dos(void) __attribute__((noreturn));
static struct command_context context;
static struct runtime_header *runtime;
static void (__attribute__((sysv_abi)) *firmware_reboot)(void);

static void character(char ch){console_putc(ch);}

static void run_dos(const char *file,const char *arguments,const char *cwd){
 if(!context.bios_available||length(file)>=128||length(arguments)>126||length(cwd)>=128)return;
 volatile char *name=(volatile char *)0x9000e,*tail=(volatile char *)0x9008e,*directory=(volatile char *)0x90118;
 for(unsigned i=0;i<128;++i){name[i]=0;tail[i]=0;directory[i]=0;}
 for(unsigned i=0;file[i];++i)name[i]=file[i];
 for(unsigned i=0;arguments[i];++i)tail[i]=arguments[i];
 for(unsigned i=0;cwd[i];++i)directory[i]=cwd[i];
 copy(context.cwd,cwd);
 *(volatile unsigned char *)0x90112=context.speed;
 *(volatile unsigned char *)0x7028=context.speed;
 memcpy((void *)(__UINTPTR_TYPE__)CONTEXT_SAVE,&context,sizeof(context));
 enter_dos();
}

static void reboot(void){
 if(firmware_reboot)firmware_reboot();
 __asm__ volatile("cli");
 for(unsigned n=0;n<100000;++n){unsigned char status;__asm__ volatile("inb $0x64,%0":"=a"(status));if(!(status&2))break;}
 __asm__ volatile("outb %0,$0x64"::"a"((unsigned char)0xfe));
 for(volatile unsigned n=0;n<50000000;++n){}
 /* No keyboard controller (most current PCs): the chipset's reset control register. */
 __asm__ volatile("outb %0,%1"::"a"((unsigned char)2),"Nd"((unsigned short)0xcf9));
 __asm__ volatile("outb %0,%1"::"a"((unsigned char)6),"Nd"((unsigned short)0xcf9));
 for(volatile unsigned n=0;n<50000000;++n){}
 struct __attribute__((packed)){unsigned short limit;unsigned long base;}none={0,0};
 __asm__ volatile("lidt %0;int3"::"m"(none));
 for(;;)__asm__ volatile("hlt");
}

static int valid_header(const struct runtime_header *header,unsigned size){
 return header->magic==RUNTIME_MAGIC&&header->version==RUNTIME_VERSION&&header->bits==sizeof(void *)*8&&header->base==RUNTIME_BASE&&
  header->init>=RUNTIME_BASE+36&&header->init<RUNTIME_BASE+size&&header->run>=RUNTIME_BASE+36&&header->run<RUNTIME_BASE+size&&
  header->resume>=RUNTIME_BASE+36&&header->resume<RUNTIME_BASE+size&&
  header->bss_start>=RUNTIME_BASE+36&&header->bss_end>=header->bss_start&&header->bss_end<=RUNTIME_BASE+RUNTIME_LIMIT;
}

static int load_runtime(void){
 struct fat32_info info;const char *path="\\SYSTEM\\ELFAPI.DLL";
 if(!context.volume||fat32_stat(context.volume,path,&info)||info.size<36||info.size>RUNTIME_LIMIT||
    fat32_read(context.volume,path,(void *)(__UINTPTR_TYPE__)RUNTIME_BASE,info.size))return -1;
 struct runtime_header *header=(struct runtime_header *)(__UINTPTR_TYPE__)RUNTIME_BASE;
 if(!valid_header(header,info.size))return -1;
 for(unsigned at=header->bss_start;at<header->bss_end;++at)*(char *)(__UINTPTR_TYPE__)at=0;
 if(((runtime_init_fn)(__UINTPTR_TYPE__)header->init)(&context))return -1;
 runtime=header;return 0;
}

static void halt(const char *message){
 console_write(message);
 for(;;)__asm__ volatile("hlt");
}

/* System updates (PKG UPDATE of the system package, os/pkg/pkg.c): PKG unpacks
   the new files into \SYSTEM.NEW.  At the next start every file moves into
   \SYSTEM one at a time, and each file it replaces goes to \SYSTEM.OLD; the
   steps can be repeated, so an interrupted update carries on at the next
   start.  \SYSTEM.OLD\TRIAL counts the starts until COMMAND.EXE confirms the
   new version by deleting it; after TRIAL_STARTS the old files come back. */
#define NEW_DIR "\\SYSTEM.NEW"
#define OLD_DIR "\\SYSTEM.OLD"
#define TRIAL OLD_DIR "\\TRIAL"
#define ADDED OLD_DIR "\\ADDED.LST"
#define NOTICE "\\SYSTEM\\UPDATE.MSG"
#define TRIAL_STARTS 3
static int on_trial;
static char added[8192];

static int exists(const char *path){struct fat32_info info;return !fat32_stat(context.volume,path,&info);}
static int join(char *out,const char *a,const char *b,const char *c){
 if(length(a)+length(b)+length(c)+2>=PATH_LIMIT)return -1;
 copy(out,a);append(out,b);if(*c){append(out,"\\");append(out,c);}return 0;
}

struct first_entry{char name[PATH_LIMIT];int directory,found,top;};
static int take_first(void *data,const struct fat32_info *info){
 struct first_entry *f=data;
 if(equal(info->name,".")||equal(info->name,"..")||(f->top&&(equal(info->name,"TRIAL")||equal(info->name,"ADDED.LST"))))return 0;
 copy(f->name,info->name);f->directory=(info->attributes&16)!=0;f->found=1;return 1;
}
/* The first entry of DIRECTORY in F (F->found 0: empty); TOP skips the update's own files. */
static int first(const char *directory,int top,struct first_entry *f){f->found=0;f->top=top;return fat32_list(context.volume,directory,take_first,f);}

static int remove_tree(const char *path){
 struct first_entry f;char child[PATH_LIMIT];
 for(;;){
  if(first(path,0,&f))return -1;
  if(!f.found)return fat32_rmdir(context.volume,path);
  if(join(child,path,"",f.name)||(f.directory?remove_tree(child):fat32_delete(context.volume,child)))return -1;
 }
}

/* REL ("" or "\\LIB") and NAME were not in \SYSTEM before: listed so that going back removes them. */
static int note_added(const char *rel,const char *name){
 struct fat32_info info;unsigned used=0;
 if(!fat32_stat(context.volume,ADDED,&info)){used=info.size;if(used>=sizeof(added)||fat32_read(context.volume,ADDED,added,used))return -1;}
 if(used+length(rel)+length(name)+3>=sizeof(added))return -1;
 added[used]=0;append(added,rel);append(added,"\\");append(added,name);append(added,"\n");
 return fat32_write(context.volume,ADDED,added,length(added));
}

static int apply_tree(const char *rel){
 struct first_entry f;char from[PATH_LIMIT],to[PATH_LIMIT],keep[PATH_LIMIT],sub[PATH_LIMIT];
 struct fat32_volume *v=context.volume;
 for(;;){
  if(join(from,NEW_DIR,rel,"")||first(from,0,&f))return -1;
  if(!f.found)return 0;
  if(join(from,NEW_DIR,rel,f.name)||join(to,"\\SYSTEM",rel,f.name)||join(keep,OLD_DIR,rel,f.name)||join(sub,rel,"",f.name))return -1;
  if(f.directory){
   if(!exists(to)&&(note_added(rel,f.name)||fat32_mkdir(v,to)))return -1;
   if(!exists(keep)&&fat32_mkdir(v,keep))return -1;
   if(apply_tree(sub)||fat32_rmdir(v,from))return -1;
   continue;
  }
  if(exists(to)){if(exists(keep)||fat32_rename(v,to,keep))return -1;}
  else if(!exists(keep)&&note_added(rel,f.name))return -1;
  if(fat32_rename(v,from,to))return -1;
 }
}

static int restore_tree(const char *rel){
 struct first_entry f;char from[PATH_LIMIT],to[PATH_LIMIT],sub[PATH_LIMIT];
 struct fat32_volume *v=context.volume;
 for(;;){
  if(join(from,OLD_DIR,rel,"")||first(from,!*rel,&f))return -1;
  if(!f.found)return 0;
  if(join(from,OLD_DIR,rel,f.name)||join(to,"\\SYSTEM",rel,f.name)||join(sub,rel,"",f.name))return -1;
  if(f.directory){
   if(!exists(to)&&fat32_mkdir(v,to))return -1;
   if(restore_tree(sub)||fat32_rmdir(v,from))return -1;
   continue;
  }
  if((exists(to)&&fat32_delete(v,to))||fat32_rename(v,from,to))return -1;
 }
}

static void save_notice(const char *text){fat32_write(context.volume,NOTICE,text,length(text));}

/* Puts the files in \SYSTEM.OLD back and restarts.  TRIAL is set to the
   limit first, so a start that interrupts this carries on with it. */
static void go_back(const char *why){
 struct fat32_volume *v=context.volume;struct fat32_info info;char path[PATH_LIMIT];unsigned char starts=TRIAL_STARTS;
 console_write(why);console_write("  Going back to the previous version of DOS-XC...\r\n");
 if(exists(OLD_DIR))fat32_write(v,TRIAL,&starts,1);
 if(exists(NEW_DIR)&&remove_tree(NEW_DIR))halt("The update could not be undone.  Start DOS-XC Setup to repair the system.\r\n");
 if(restore_tree(""))halt("The previous version could not be restored.  Start DOS-XC Setup to repair the system.\r\n");
 if(!fat32_stat(v,ADDED,&info)&&info.size<sizeof(added)&&!fat32_read(v,ADDED,added,info.size)){
  added[info.size]=0;
  for(int end=(int)info.size;end>0;){
   int start=end-1;while(start>0&&added[start-1]!='\n')--start;
   added[end-1]=0;
   if(!join(path,"\\SYSTEM",added+start,"")&&fat32_delete(v,path))fat32_rmdir(v,path);
   end=start;
  }
  fat32_delete(v,ADDED);
 }
 fat32_delete(v,TRIAL);
 fat32_rmdir(v,OLD_DIR);
 save_notice(why);
 reboot();
}

/* Before the runtime loads, on a start from the disk (not after a 16-bit program). */
static void system_update(void){
 struct fat32_volume *v=context.volume;unsigned char starts=0;
 if(!v||!v->write_sector||context.live)return;
 if(!fat32_read(v,TRIAL,&starts,1)&&starts>=TRIAL_STARTS)go_back("The updated system did not start.");
 starts=0;
 if(exists(NEW_DIR)){
  console_write("Installing the DOS-XC system update...\r\n");
  if(!exists(OLD_DIR)&&fat32_mkdir(v,OLD_DIR))go_back("The system update could not be started.");
  if(!exists(TRIAL)&&fat32_write(v,TRIAL,&starts,1))go_back("The system update could not be started.");
  if(apply_tree("")||fat32_rmdir(v,NEW_DIR))go_back("The system update could not be installed.");
  reboot();
 }
 if(fat32_read(v,TRIAL,&starts,1))return;
 ++starts;
 if(fat32_write(v,TRIAL,&starts,1))return;
 on_trial=1;
}

void shell_run(struct fat32_volume *volume,int native_dos_available,char drive,uint64_t memory_base,uint64_t memory_size){
 int returning=native_dos_available && *(volatile unsigned char *)0x702c==1;
 if(returning){memcpy(&context,(void *)(__UINTPTR_TYPE__)CONTEXT_SAVE,sizeof(context));context.errorlevel=*(volatile unsigned char *)0x702d;}
 else{copy(context.cwd,"\\");}
 context.abi=2;context.bios_available=native_dos_available;context.volume=volume;context.drive=drive;
 context.speed=*(volatile unsigned char *)0x7028;if(context.speed<1||context.speed>100)context.speed=5;
 context.write=console_write;context.putc=character;context.read_key=keyboard_read;context.clear=console_clear;context.color=console_set_color;
 context.cursor=console_set_cursor;context.beep=pcspeaker_beep;
 context.stat=fat32_stat;context.list=fat32_list;context.read=fat32_read;context.save=fat32_write;context.mkdir=fat32_mkdir;context.remove=fat32_delete;
 context.rmdir=fat32_rmdir;context.rename=fat32_rename;context.free_bytes=fat32_free_bytes;context.copy_file=fat32_copy;context.text_color=console_set_text_color;
 context.read_at=fat32_read_at;context.mount=fat32_mount;context.read_cursor=fat32_read_cursor;context.memory_base=memory_base;context.memory_size=memory_size;
 context.get_cursor=console_get_cursor;context.put_at=console_put_at;context.erase=console_erase;
 context.get_color=console_get_color;context.scroll=console_scroll;
 context.run_dos=native_dos_available?run_dos:0;context.reboot=reboot;context.live=drive=='X';
 context.short_path=fat32_short_path;
 if(returning){
  console_resume();*(volatile unsigned char *)0x702c=0;
  struct runtime_header *resident=(struct runtime_header *)(__UINTPTR_TYPE__)RUNTIME_BASE;
  if(valid_header(resident,RUNTIME_LIMIT)){
   runtime=resident;
   if(((runtime_resume_fn)(__UINTPTR_TYPE__)resident->resume)(&context,context.errorlevel)<0)runtime=0;
  }
 }else{console_clear();system_update();}
 if(!runtime&&load_runtime()){
  if(on_trial)go_back("The updated system could not load \\SYSTEM\\ELFAPI.DLL.");
  halt("DOS-XC cannot start: \\SYSTEM\\ELFAPI.DLL is missing or damaged.\r\n");
 }
 for(unsigned failures=0;;){
  int status=((runtime_run_fn)(__UINTPTR_TYPE__)runtime->run)("\\SYSTEM\\COMMAND.EXE","");
  if(status<0){
   if(++failures<3)continue;
   if(on_trial&&exists(TRIAL))go_back("The updated system could not start \\SYSTEM\\COMMAND.EXE.");
   halt("DOS-XC cannot start the command shell, \\SYSTEM\\COMMAND.EXE.\r\n");
  }
  failures=0;
 }
}

void shell_firmware(disk_count_fn count,disk_info_fn info,disk_io_fn io,void (__attribute__((sysv_abi)) *reset)(void),
                    unsigned (__attribute__((sysv_abi)) *rescan)(void),
                    int (__attribute__((sysv_abi)) *pointer)(int *,int *,int *,unsigned *),
                    void (__attribute__((sysv_abi)) *poweroff)(void),uint64_t rsdp,
                    int (__attribute__((sysv_abi)) *display)(int,unsigned,unsigned *,unsigned *,unsigned *,uint64_t *),
                    unsigned system_pci,uint64_t system_lba,uint64_t ap_page){
 context.disk_count=count;context.disk_info=info;context.disk_io=io;firmware_reboot=reset;
 context.disk_rescan=rescan;context.pointer=pointer;context.poweroff=poweroff;context.acpi_rsdp=rsdp;context.display=display;
 context.system_pci=system_pci;context.system_lba=system_lba;context.ap_page=ap_page;
}
