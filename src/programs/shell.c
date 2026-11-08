/*
 * DOS-XC /programs/shell.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

 #include "util.h"
#include "../kernel/bootinfo.h"
#include "../kernel/console.h"
#include "../drivers/keyboard.h"
#include "../drivers/pcspeaker.h"
extern void enter_dos(void) __attribute__((noreturn));
static struct command_context context;
static int dos_available,batch_loaded;
static char redirect_path[260],redirect_data[65536];
static unsigned redirect_size;
static int redirect_active,redirect_failed;
static struct runtime_header *runtime;
static void output(const char *s){if(!redirect_active){console_write(s);return;}while(*s){if(redirect_size==sizeof(redirect_data)){redirect_failed=1;return;}redirect_data[redirect_size++]=*s++;}}
static void character(char ch){char s[2]={ch,0};output(s);}
static int resolve(const char *input,char *out){
 input=skip(input);if(input[0]&&input[1]==':'){if(upper(input[0])!=context.drive)return -1;input+=2;}
 if(*input=='\\'||*input=='/')copy(out,"\\");else copy(out,context.cwd);
 while(*input){while(*input=='/'||*input=='\\')++input;if(!*input)break;char part[260];unsigned n=0;
  while(*input&&*input!='/'&&*input!='\\'){if(n==259)return -1;part[n++]=*input++;}part[n]=0;
  if(equal(part,"."))continue;if(equal(part,"..")){unsigned at=length(out);while(at>1&&out[at-1]!='\\')--at;if(at>1)--at;out[at]=0;continue;}
  if(length(out)+n+2>=260)return -1;if(length(out)>1)append(out,"\\");append(out,part);
 }return 0;
}
static void prompt(void){console_putc(context.drive);console_putc(':');console_write(context.cwd);console_putc('>');}
static int is_file(const char *path){struct fat32_info info;return context.volume&&!fat32_stat(context.volume,path,&info)&&!(info.attributes&16);}
static int extension(const char *name,const char *suffix){unsigned a=length(name),b=length(suffix);return a>=b&&equal(name+a-b,suffix);}
static int candidate(const char *name,char *found){
 static const char *extensions[]={"", ".COM", ".EXE", ".BAT"};
 for(unsigned i=0;i<4;++i){if(length(name)+length(extensions[i])>=260)continue;copy(found,name);append(found,extensions[i]);if(is_file(found))return 1;}return 0;
}
static int locate(const char *name,char *found){
 char full[260];if(resolve(name,full))return 0;if(candidate(full,found))return 1;
 for(const char *s=name;*s;++s)if(*s=='\\'||*s=='/'||*s==':')return 0;
 for(unsigned v=0;v<16;++v)if(equal(context.environment[v].name,"PATH")){
  const char *p=context.environment[v].value;while(*p){char dir[260];unsigned n=0;while(*p&&*p!=';'){if(n<259)dir[n++]=*p;++p;}dir[n]=0;if(*p)++p;
   if(resolve(dir,full))continue;if(length(full)+length(name)+2>=260)continue;if(length(full)>1)append(full,"\\");append(full,name);if(candidate(full,found))return 1;
  }
 }return 0;
}
static int native_program(const char *file,const char *arguments,int batch){
 unsigned base=batch?BATCH_BASE:PROGRAM_BASE,limit=batch?0x80000u:PROGRAM_LIMIT;
 if(!batch||!batch_loaded||!context.batch_depth){
  struct fat32_info info;if(fat32_stat(context.volume,file,&info)||info.size<32||info.size>limit||fat32_read(context.volume,file,(void *)(__UINTPTR_TYPE__)base,info.size))return 1;
  unsigned *header=(unsigned *)(__UINTPTR_TYPE__)base;
  if(header[0]!=0x4d435042||header[1]!=1||header[2]!=sizeof(void *)*8||header[3]!=base||header[4]<base+32||header[4]>=base+info.size||header[5]<base||header[6]<header[5]||header[6]>base+limit){output("Invalid or incompatible program image.\r\n");return 1;}
  for(unsigned at=header[5];at<header[6];++at)*(char *)(__UINTPTR_TYPE__)at=0;
  if(batch)batch_loaded=1;
 }
 unsigned *header=(unsigned *)(__UINTPTR_TYPE__)base;
 return ((program_entry)(__UINTPTR_TYPE__)header[4])(arguments,&context);
}
static int run_dos(const char *file,const char *arguments){
 if(!dos_available){output("16-bit execution requires BIOS boot in this version.\r\n");return 1;}
 if(length(file)>=128||length(arguments)>126){output("DOS command line is too long.\r\n");return 1;}
 volatile char *name=(volatile char *)0x9000e,*tail=(volatile char *)0x9008e;
 for(unsigned i=0;i<128;++i){name[i]=0;tail[i]=0;}
 for(unsigned i=0;file[i];++i)name[i]=file[i];for(unsigned i=0;arguments[i];++i)tail[i]=arguments[i];
 volatile char *cwd=(volatile char *)0x90118;
 for(unsigned i=0;i<128;++i)cwd[i]=0;
 if(length(context.cwd)>=128){output("DOS directory path is too long.\r\n");return 1;}
 for(unsigned i=0;context.cwd[i];++i)cwd[i]=context.cwd[i];
 *(volatile unsigned char *)0x90112=context.speed;
 *(volatile unsigned char *)0x7028=context.speed;
 memcpy((void *)0x4200000,&context,sizeof(context));
 enter_dos();
}
static void load_runtime(void){
 runtime=0;struct fat32_info info;const char *path="\\SYSTEM\\ELFAPI.DLL";
 if(!context.volume||fat32_stat(context.volume,path,&info)||info.size<32||info.size>RUNTIME_LIMIT||
    fat32_read(context.volume,path,(void *)(__UINTPTR_TYPE__)RUNTIME_BASE,info.size))return;
 struct runtime_header *header=(struct runtime_header *)(__UINTPTR_TYPE__)RUNTIME_BASE;
 if(header->magic!=RUNTIME_MAGIC||header->version!=1||header->bits!=sizeof(void *)*8||header->base!=RUNTIME_BASE||
    header->init<RUNTIME_BASE+32||header->init>=RUNTIME_BASE+info.size||header->run<RUNTIME_BASE+32||header->run>=RUNTIME_BASE+info.size||
    header->bss_start<RUNTIME_BASE+32||header->bss_end<header->bss_start||header->bss_end>RUNTIME_BASE+RUNTIME_LIMIT)return;
 for(unsigned at=header->bss_start;at<header->bss_end;++at)*(char *)(__UINTPTR_TYPE__)at=0;
 if(((runtime_init_fn)(__UINTPTR_TYPE__)header->init)(&context))return;
 runtime=header;
}
static int run_elf(const char *file,const char *arguments,const char *output_path,int append){
 if(!runtime){output("32/64-bit programs require \\SYSTEM\\ELFAPI.DLL.\r\n");return 1;}
 return ((runtime_run_fn)(__UINTPTR_TYPE__)runtime->run)(file,arguments,output_path,append);
}
static int execute(const char *original){
 char line[512],name[260],found[260];if(length(original)>=sizeof(line))return 1;copy(line,original);
 char *input=line;if(*input=='@')++input;input=(char *)skip(input);if(!*input)return 0;
 char *redirection=0;int quote=0,append_mode=0;
 for(char *p=input;*p;++p){if(*p=='"')quote=!quote;if(!quote&&*p=='>'){redirection=p;break;}}
 if(redirection){append_mode=redirection[1]=='>';char target[260];token(redirection+1+append_mode,target,260);*redirection=0;
  if(resolve(target,redirect_path)||!*target){console_write("Invalid redirection target.\r\n");return 1;}
  redirect_size=0;redirect_failed=0;if(append_mode){struct fat32_info info;if(!fat32_stat(context.volume,redirect_path,&info)){if(info.size>sizeof(redirect_data)||fat32_read(context.volume,redirect_path,redirect_data,info.size))return 1;redirect_size=info.size;}}
  redirect_active=1;
 }
 unsigned end=length(input);while(end&&(input[end-1]==' '||input[end-1]=='\t'))input[--end]=0;
 const char *arguments=token(input,name,sizeof(name));int status=1;
 if(equal(name,"CD")||equal(name,"CHDIR")){
  if(equal(arguments,"/?"))output("CD [/D] [drive:][path]   CD .. moves to the parent directory.\r\n");
  else if(!*arguments){output(context.cwd);output("\r\n");status=0;}
  else{if(prefix(arguments,"/D "))arguments=skip(arguments+2);char target[260];token(arguments,target,260);struct fat32_info info;
   if(!resolve(target,found)&&!fat32_stat(context.volume,found,&info)&&(info.attributes&16)){copy(context.cwd,found);status=0;}else output("The system cannot find the path or drive specified.\r\n");}
 }else if(locate(name,found)){
  if(extension(found,".BAT")){
   char batch[512],processor[260];copy(batch,"\"");append(batch,found);append(batch,"\" ");append(batch,arguments);
   if(resolve("\\SYSTEM\\BATCH\\BATCH.EXE",processor)||!is_file(processor))output("Batch processor is not installed.\r\n");else status=native_program(processor,batch,1);
  }else{
   unsigned signature=0;fat32_read(context.volume,found,&signature,4);
   if(signature==0x464c457f){
    /* ELF programs write redirected output straight to the file. */
    if(redirection){redirect_active=0;status=run_elf(found,arguments,redirect_path,append_mode);redirection=0;}
    else status=run_elf(found,arguments,0,0);
   }
   else if(signature==0x4d435042)status=native_program(found,arguments,0);
   else if(extension(found,".EXE")||extension(found,".COM"))status=run_dos(found,arguments);
   else output("Invalid executable format.\r\n");
  }
 }else output("No such program, command, or action.\r\n");
 if(redirection){redirect_active=0;if(redirect_failed||fat32_write(context.volume,redirect_path,redirect_data,redirect_size)){console_write("Unable to save redirected output.\r\n");status=1;}}
 context.errorlevel=status;return status;
}
void shell_run(struct fat32_volume *volume,int native_dos_available,char drive,uint64_t memory_base,uint64_t memory_size){
 int returning=native_dos_available && *(volatile unsigned char *)0x702c==1;
 if(returning){memcpy(&context,(void *)0x4200000,sizeof(context));context.errorlevel=*(volatile unsigned char *)0x702d;}
 context.bios_available=native_dos_available;context.abi=1;context.volume=volume;context.drive=drive;if(!returning){copy(context.cwd,"\\");context.echo=1;}context.speed=*(volatile unsigned char *)0x7028;if(context.speed<1||context.speed>100)context.speed=5;
 if(!returning){copy(context.environment[0].name,"PATH");context.environment[0].value[0]=drive;copy(context.environment[0].value+1,":\\SYSTEM\\CMD;");
  append(context.environment[0].value,(char[]){drive,0});append(context.environment[0].value,":\\GCC\\BIN");
  copy(context.environment[1].name,"TMPDIR");context.environment[1].value[0]=drive;copy(context.environment[1].value+1,":\\TEMP");}
 context.write=output;context.putc=character;context.read_key=keyboard_read;context.clear=console_clear;context.color=console_set_color;context.cursor=console_set_cursor;context.beep=pcspeaker_beep;
 context.resolve=resolve;context.execute=execute;context.stat=fat32_stat;context.list=fat32_list;context.read=fat32_read;context.save=fat32_write;context.mkdir=fat32_mkdir;context.remove=fat32_delete;context.rmdir=fat32_rmdir;context.rename=fat32_rename;context.free_bytes=fat32_free_bytes;context.copy_file=fat32_copy;context.text_color=console_set_text_color;
 context.read_at=fat32_read_at;context.memory_base=memory_base;context.memory_size=memory_size;
 dos_available=native_dos_available;
 load_runtime();
 if(returning){console_resume();console_write("\r\n");*(volatile unsigned char *)0x702c=0;
  if(context.batch_depth){unsigned *batch=(unsigned *)BATCH_BASE;if(batch[0]==0x4d435042&&batch[7]){batch_loaded=1;((int (*)(struct command_context *))(__UINTPTR_TYPE__)batch[7])(&context);}}
 }else{console_clear();console_write(DOSXC_LABEL "\r\n");}
 if(native_dos_available && *(volatile unsigned int *)0x7060==0x54534e49)execute("\\SYSTEM\\INSTALL.EXE");
 static char line[512];unsigned used=0;prompt();
 for(;;){int key=keyboard_read();if(key<0)continue;if(key=='\n'){line[used]=0;console_write("\r\n");execute(line);used=0;prompt();}
  else if(key=='\b'&&used){--used;console_write("\b \b");}else if(key>=32&&key<127&&used+1<sizeof(line)){line[used++]=(char)key;console_putc((char)key);}}
}
