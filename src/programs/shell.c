/*
 * DOS-XC /programs/shell.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The kernel's init module.  It loads \SYSTEM\ELFAPI.DLL (the 32/64-bit
 * program runtime) and keeps the command shell, \SYSTEM\COMMAND.EXE,
 * running.  A 16-bit DOS program replaces the whole protected-mode system
 * while it runs; afterwards the boot loader restarts the kernel and init
 * hands control back to the runtime, which resumes the program that
 * started the DOS program.
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
 }else console_clear();
 if(!runtime&&load_runtime())halt("DOS-XC cannot start: \\SYSTEM\\ELFAPI.DLL is missing or damaged.\r\n");
 for(unsigned failures=0;;){
  int status=((runtime_run_fn)(__UINTPTR_TYPE__)runtime->run)("\\SYSTEM\\COMMAND.EXE","");
  if(status<0){if(++failures==3)halt("DOS-XC cannot start the command shell, \\SYSTEM\\COMMAND.EXE.\r\n");}
  else failures=0;
 }
}

/* UEFI hands its disk and reset services to init through boot_info. */
void shell_firmware(disk_count_fn count,disk_info_fn info,disk_io_fn io,void (__attribute__((sysv_abi)) *reset)(void),
                    unsigned (__attribute__((sysv_abi)) *rescan)(void),
                    int (__attribute__((sysv_abi)) *pointer)(int *,int *,int *,unsigned *),
                    void (__attribute__((sysv_abi)) *poweroff)(void),uint64_t rsdp,
                    int (__attribute__((sysv_abi)) *display)(int,unsigned,unsigned *,unsigned *,unsigned *,uint64_t *)){
 context.disk_count=count;context.disk_info=info;context.disk_io=io;firmware_reboot=reset;
 context.disk_rescan=rescan;context.pointer=pointer;context.poweroff=poweroff;context.acpi_rsdp=rsdp;context.display=display;
}
