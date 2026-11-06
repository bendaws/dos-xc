/*
 * BenDOS /programs/commands.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "util.h"
#include "../kernel/bootinfo.h"
static struct command_context *c;
static char first[260],second[260],path[260],destination[260];
static int error(const char *message){c->write(message);c->write("\r\n");return 1;}
static int ask(const char *message){c->write(message);for(;;){int k=c->read_key();if(k=='y'||k=='Y'){c->write("Y\r\n");return 1;}if(k=='n'||k=='N'||k==27){c->write("N\r\n");return 0;}}}
static int resolved(const char *name,char *out){return c->resolve(name,out);}
static void show_path(const char *p){c->putc(c->drive);c->write(":");c->write(p);}
#if COMMAND_ID == 1
int program_main(const char *args,struct command_context *ctx){c=ctx;(void)args;
 c->write("Built-in commands: \r\n\r\n");
 c->write("DIR [/B] [/W] [/P] [/S] [/A[:D|-D]] [/O:N|S|-N|-S] [path]   List files\r\n");
 c->write("TYPE file...         Display text      EDIT file       Edit/save text\r\n");
 c->write("COPY [/Y] src dst    Copy file         MOVE [/Y] src dst\r\n");
 c->write("REN src name         Rename            DEL [/P] [/Q] pattern\r\n");
 c->write("MD path              Make directory    RD path         Remove empty dir\r\n");
 c->write("ECHO [ON|OFF|text]   CLS   COLOR [BGFG]   VER   BEEP [hz] [ms]\r\n");
 c->write("WRITE [/A] file text   SET [name=value]   PATH [directories]\r\n");
 c->write("SPEED [1..100]       16-bit CPU duty percent (100 = unthrottled)\r\n");
 c->write("CD [/D] [path]       Change shell directory (built in)\r\n");
 c->write("Type a program name to run .EXE, .COM or .BAT from disk.\r\n");
 c->write("Use command /? for help. Batch scripts use \\SYSTEM\\BATCH\\BATCH.EXE.\r\n");return 0;}
#elif COMMAND_ID == 2
static struct fat32_info entries[256];
static char queue[64][260],pattern[260];
static unsigned count,overflow,bare,wide,paging,recursive,attr,order,reverse,lines,files,dirs;static uint64_t bytes;
static int collect(void *context,const struct fat32_info *info){(void)context;
 if(equal(info->name,".")||equal(info->name,".."))return 0;
 if(count==256){overflow=1;return 1;}entries[count++]=*info;return 0;}
static int compare(const struct fat32_info *a,const struct fat32_info *b){
 if(order==1 && a->size!=b->size)return a->size>b->size?1:-1;
 unsigned i=0;while(a->name[i]&&b->name[i]){char x=upper(a->name[i]),y=upper(b->name[i]);if(x!=y)return x>y?1:-1;++i;}return (int)(unsigned char)a->name[i]-(unsigned char)b->name[i];}
static void newline(void){c->write("\r\n");if(paging&&++lines==20){c->write("Press any key to continue . . .");while(c->read_key()<0){}c->write("\r\n");lines=0;}}
static void two(unsigned n){c->putc('0'+(n/10)%10);c->putc('0'+n%10);}
int program_main(const char *args,struct command_context *ctx){c=ctx;copy(pattern,"*");
 const char *p=args;while(*p){p=token(p,first,sizeof(first));if(first[0]=='/'){
  if(equal(first,"/?"))return error("DIR [path][pattern] [/B] [/W] [/P] [/S] [/A[:D|-D]] [/O:N|S|-N|-S]");
  if(equal(first,"/B"))bare=1;else if(equal(first,"/W"))wide=1;else if(equal(first,"/P"))paging=1;else if(equal(first,"/S"))recursive=1;
  else if(equal(first,"/A")||equal(first,"/A:A"))attr=0;else if(equal(first,"/A:D"))attr=1;else if(equal(first,"/A:-D"))attr=2;
  else if(prefix(first,"/O:")){const char *o=first+3;if(*o=='-'){reverse=1;++o;}if(equal(o,"S"))order=1;else if(!equal(o,"N"))return error("Unsupported DIR sort order.");}
  else return error("Invalid DIR switch. Type DIR /?.");
 }else if(resolved(first,path))return error("Invalid path or drive.");}
 if(!*path)copy(path,c->cwd);
 struct fat32_info info;if(c->stat(c->volume,path,&info)||!(info.attributes&16)){
  unsigned split=length(path);while(split&&path[split-1]!='\\')--split;copy(pattern,path+split);path[split?split-1:0]=0;if(!*path)copy(path,"\\");
 }
 copy(queue[0],path);unsigned queued=1;
 for(unsigned q=0;q<queued;++q){count=0;overflow=0;if(c->list(c->volume,queue[q],collect,0))return error("The system cannot find the path specified.");
  if(!bare){c->write(" Directory of ");show_path(queue[q]);newline();newline();}
  for(unsigned i=1;i<count;++i){unsigned j=i;while(j&&((compare(&entries[j-1],&entries[j])>0)^!!reverse)){struct fat32_info temp=entries[j-1];entries[j-1]=entries[j];entries[j]=temp;--j;}}
  unsigned column=0;
  for(unsigned i=0;i<count;++i){struct fat32_info *f=&entries[i];
   if(recursive&&(f->attributes&16)){if(queued==64)return error("DIR recursion limit reached.");copy(queue[queued],queue[q]);if(length(queue[queued])>1)append(queue[queued],"\\");if(length(queue[queued])+length(f->name)>=259)return error("Path too long.");append(queue[queued++],f->name);}
   if(!wildcard(pattern,f->name)||(attr==1&&!(f->attributes&16))||(attr==2&&(f->attributes&16)))continue;
   if(f->attributes&16)++dirs;else{++files;bytes+=f->size;}
   if(bare){if(recursive){show_path(queue[q]);if(length(queue[q])>1)c->putc('\\');}c->write(f->name);newline();continue;}
   if(wide){if(f->attributes&16)c->putc('[');c->write(f->name);if(f->attributes&16)c->putc(']');unsigned width=length(f->name)+(f->attributes&16?2:0);while(width++<18)c->putc(' ');if(++column==4){newline();column=0;}continue;}
   if(f->date){two((f->date>>5)&15);c->putc('/');two(f->date&31);c->putc('/');decimal(c,1980+(f->date>>9));c->write("  ");two(f->time>>11);c->putc(':');two((f->time>>5)&63);}else c->write("----/--/--  --:--");
   c->write("  ");if(f->attributes&16)c->write("       <DIR>");else sized(c,f->size,12);
   c->write("  ");c->putc(f->attributes&1?'R':'-');c->putc(f->attributes&2?'H':'-');c->putc(f->attributes&4?'S':'-');c->putc(f->attributes&32?'A':'-');c->write("  ");c->write(f->name);newline();
  }
  if(wide&&column)newline();if(overflow)return error("Directory has more than 256 entries; narrow the selection.");
 }
 if(!bare){sized(c,files,8);c->write(" File(s) ");sized(c,bytes,14);c->write(" bytes");newline();sized(c,dirs,8);c->write(" Dir(s)  ");sized(c,c->free_bytes(c->volume),14);c->write(" bytes free");newline();}return 0;
}
#elif COMMAND_ID == 3
static char buffer[32769];
int program_main(const char *args,struct command_context *ctx){c=ctx;if(!*args||equal(args,"/?"))return error("TYPE filename [filename ...]");while(*args){args=token(args,first,sizeof(first));if(resolved(first,path))return error("Invalid path.");struct fat32_info info;if(c->stat(c->volume,path,&info)||info.attributes&16)return error("File not found.");if(info.size>32768)return error("TYPE currently supports files up to 32768 bytes.");if(c->read(c->volume,path,buffer,info.size))return error("Read error.");buffer[info.size]=0;c->write(buffer);}return 0;}
#elif COMMAND_ID == 4
int program_main(const char *args,struct command_context *ctx){c=ctx;if(equal(args,"/?"))return error("ECHO [ON|OFF|message]     ECHO. prints an empty line. Redirection: > or >>");if(equal(args,"ON")){c->echo=1;return 0;}if(equal(args,"OFF")){c->echo=0;return 0;}if(!*args){c->write(c->echo?"ECHO is on.\r\n":"ECHO is off.\r\n");return 0;}if(!equal(args,"."))c->write(args);c->write("\r\n");return 0;}
#elif COMMAND_ID == 5
int program_main(const char *args,struct command_context *ctx){c=ctx;if(*args)return error("CLS     Clear the screen.");c->clear();return 0;}
#elif COMMAND_ID == 6
static int digit(char n){n=upper(n);if(n>='0'&&n<='9')return n-'0';if(n>='A'&&n<='F')return n-'A'+10;return -1;}
int program_main(const char *args,struct command_context *ctx){c=ctx;if(!*args){c->color(7);return 0;}if(length(args)!=2||digit(args[0])<0||digit(args[1])<0)return error("COLOR [background foreground]     Example: COLOR 1F");c->color((digit(args[0])<<4)|digit(args[1]));return 0;}
#elif COMMAND_ID == 7
int program_main(const char *args,struct command_context *ctx){c=ctx;(void)args;c->write(BENDOS_LABEL "\r\nSystem directory: ");c->putc(c->drive);c->write(":\\SYSTEM\r\n");return 0;}
#elif COMMAND_ID == 8
static unsigned integer(const char *s){unsigned n=0;while(*s>='0'&&*s<='9')n=n*10+(*s++-'0');return n;}
int program_main(const char *args,struct command_context *ctx){c=ctx;args=token(args,first,260);token(args,second,260);if(equal(first,"/?"))return error("BEEP [frequency-hz] [duration-ms]");unsigned hz=*first?integer(first):880,ms=*second?integer(second):100;if(hz<20||hz>20000||ms>3000)return error("BEEP: frequency 20..20000 Hz; duration 0..3000 ms.");c->beep(hz,ms);return 0;}
#elif COMMAND_ID == 9
static char buffer[32768];
int program_main(const char *args,struct command_context *ctx){c=ctx;int add=0;args=token(args,first,260);if(equal(first,"/A")){add=1;args=token(args,first,260);}if(!*first||equal(first,"/?"))return error("WRITE [/A] filename text     /A appends to an existing file.");if(resolved(first,path))return error("Invalid path.");unsigned n=0;struct fat32_info info;if(add&&!c->stat(c->volume,path,&info)){n=info.size;if(n>32768||c->read(c->volume,path,buffer,n))return error("Cannot append to file.");}unsigned amount=length(args);if(n+amount+2>32768)return error("File exceeds the 32768-byte text limit.");for(unsigned i=0;i<amount;++i)buffer[n++]=args[i];buffer[n++]='\r';buffer[n++]='\n';if(c->save(c->volume,path,buffer,n))return error("Unable to write file.");c->write("Written.\r\n");return 0;}
#elif COMMAND_ID == 10 || COMMAND_ID == 11 || COMMAND_ID == 12
int program_main(const char *args,struct command_context *ctx){c=ctx;int overwrite=0;args=token(args,first,260);if(equal(first,"/Y")){overwrite=1;args=token(args,first,260);}args=token(args,second,260);if(!*first||!*second||*args)return error("COPY [/Y] source destination | MOVE [/Y] source destination | REN source new-name");if(resolved(first,path)||resolved(second,destination))return error("Invalid path.");struct fat32_info from,to;if(c->stat(c->volume,path,&from))return error("File not found.");
#if COMMAND_ID == 12
 if(!(*second=='\\'||*second=='/'||(second[0]&&second[1]==':'))){copy(destination,path);unsigned n=length(destination);while(n&&destination[n-1]!='\\')--n;destination[n]=0;if(n+length(second)>=260)return error("Path too long.");append(destination,second);}
#else
 if(!c->stat(c->volume,destination,&to)&&(to.attributes&16)){if(length(destination)>1)append(destination,"\\");unsigned n=length(path);while(n&&path[n-1]!='\\')--n;if(length(destination)+length(path+n)>=260)return error("Path too long.");append(destination,path+n);}
#endif
 if(equal(path,destination))return error("Source and destination are the same.");
 if(!c->stat(c->volume,destination,&to)){if(to.attributes&16)return error("Destination already exists.");if(!overwrite&&!ask("Overwrite existing file (Y/N)? "))return 1;
#if COMMAND_ID != 10
 if(c->remove(c->volume,destination))return error("Unable to replace destination.");
#endif
}
#if COMMAND_ID == 10
 if(from.attributes&16)return error("COPY does not copy directories.");if(c->copy_file(c->volume,path,destination))return error("Copy failed.");c->write("        1 file(s) copied.\r\n");return 0;
#else
 if(c->rename(c->volume,path,destination))return error("Move or rename failed.");return 0;
#endif
}
#elif COMMAND_ID == 13
static char names[128][260],pattern[260];static unsigned count;
static int collect(void *context,const struct fat32_info *info){(void)context;if(!(info->attributes&16)&&wildcard(pattern,info->name)){if(count==128)return 1;copy(names[count++],info->name);}return 0;}
int program_main(const char *args,struct command_context *ctx){c=ctx;int prompt=0,quiet=0;while(*args){args=token(args,first,260);if(equal(first,"/P"))prompt=1;else if(equal(first,"/Q"))quiet=1;else if(first[0]=='/')return error("DEL [/P] [/Q] filename-or-pattern");else if(resolved(first,path))return error("Invalid path.");}if(!*path)return error("DEL [/P] [/Q] filename-or-pattern");unsigned split=length(path);while(split&&path[split-1]!='\\')--split;copy(pattern,path+split);path[split?split-1:0]=0;if(!*path)copy(path,"\\");if(c->list(c->volume,path,collect,0)||!count)return error("File not found.");if(!prompt&&!quiet&&(equal(pattern,"*")||equal(pattern,"*.*"))&&!ask("Delete all matching files (Y/N)? "))return 1;
 for(unsigned i=0;i<count;++i){copy(destination,path);if(length(destination)>1)append(destination,"\\");append(destination,names[i]);if(prompt){show_path(destination);if(!ask(" (Y/N)? "))continue;}if(c->remove(c->volume,destination))return error("Unable to delete file.");}return 0;}
#elif COMMAND_ID == 14 || COMMAND_ID == 15
int program_main(const char *args,struct command_context *ctx){c=ctx;if(!*args||equal(args,"/?"))return error("MD directory [directory ...] | RD empty-directory");while(*args){args=token(args,first,260);if(resolved(first,path))return error("Invalid path.");
#if COMMAND_ID == 14
 if(c->mkdir(c->volume,path))return error("Unable to create directory.");
#else
 if(equal(path,c->cwd)||equal(path,"\\"))return error("Cannot remove the current or root directory.");if(c->rmdir(c->volume,path))return error("Directory is not empty or cannot be removed.");
#endif
 }return 0;}
#elif COMMAND_ID == 16
static char buffer[32769],saved[65536];static unsigned used,position,top;static int dirty,save_status;
static unsigned line_start(unsigned at){while(at&&buffer[at-1]!='\n')--at;return at;}
static void render(void){
 c->text_color(0x17);c->clear();c->write(" EDIT  ");show_path(path);if(dirty)c->write(" *");c->write("\n");
 unsigned row=1,col=0,at=top,caret_row=1,caret_col=0;
 while(at<used&&row<23){if(at==position){caret_row=row;caret_col=col;}char ch=buffer[at++];if(ch=='\r')continue;if(ch=='\n'){c->write("\n");++row;col=0;}else if(ch=='\t'){unsigned n=8-col%8;while(n--){c->putc(' ');if(++col==80){++row;col=0;}}}else{c->putc(ch);if(++col==80){++row;col=0;}}}
 if(at==position&&row<23){caret_row=row;caret_col=col;}
 c->cursor(24,0);c->text_color(0x70);c->write(save_status<0?" SAVE FAILED - changes remain unsaved.  F2=Retry  ESC=Exit ":save_status>0?" Saved.  F2/Ctrl+S=Save   ESC=Exit   Arrows=Move ":" F2/Ctrl+S=Save   ESC=Exit   Arrows=Move   Delete/Backspace=Erase ");c->text_color(0x17);c->cursor(caret_row,caret_col);
}
static int save(void){unsigned n=0;for(unsigned i=0;i<used;++i){if(buffer[i]=='\n'&&(i==0||buffer[i-1]!='\r'))saved[n++]='\r';saved[n++]=buffer[i];}
 if(c->save(c->volume,path,saved,n)){save_status=-1;return 1;}dirty=0;save_status=1;return 0;}
int program_main(const char *args,struct command_context *ctx){c=ctx;args=token(args,first,260);if(!*first||*args||equal(first,"/?"))return error("EDIT filename     F2 or Ctrl+S saves; ESC exits.");if(resolved(first,path))return error("Invalid path.");struct fat32_info info;if(!c->stat(c->volume,path,&info)){if(info.attributes&16||info.size>32768)return error("EDIT supports text files up to 32768 bytes.");used=info.size;if(c->read(c->volume,path,buffer,used))return error("Read error.");unsigned n=0;for(unsigned i=0;i<used;++i){if(buffer[i]=='\r'&&i+1<used&&buffer[i+1]=='\n')continue;buffer[n++]=buffer[i];}used=n;}
 for(;;){render();int k;do{k=c->read_key();}while(k<0);if(k==27){if(dirty&&!ask("\r\nDiscard unsaved changes (Y/N)? "))continue;break;}if(k==KEY_F2||k==19){save();continue;}
 if(k==KEY_LEFT){if(position)--position;}else if(k==KEY_RIGHT){if(position<used)++position;}
 else if(k==KEY_HOME)position=line_start(position);else if(k==KEY_END){while(position<used&&buffer[position]!='\n')++position;}
 else if(k==KEY_UP){unsigned start=line_start(position),column=position-start;if(start){unsigned prev=line_start(start-1);position=prev+column;if(position>=start)position=start-1;}}
 else if(k==KEY_DOWN){unsigned start=line_start(position),column=position-start,end=position;while(end<used&&buffer[end]!='\n')++end;if(end<used){position=end+1;while(column--&&position<used&&buffer[position]!='\n')++position;}}
 else if(k==8&&position){--position;for(unsigned i=position;i+1<used;++i)buffer[i]=buffer[i+1];--used;dirty=1;}
 else if(k==KEY_DELETE&&position<used){for(unsigned i=position;i+1<used;++i)buffer[i]=buffer[i+1];--used;dirty=1;}
 else if((k>=32&&k<127)||k=='\n'||k=='\t'){if(used<32768){for(unsigned i=used;i>position;--i)buffer[i]=buffer[i-1];buffer[position++]=(char)k;++used;dirty=1;}}
 if(position<top)top=line_start(position);unsigned row=0;for(unsigned i=top;i<position;++i)if(buffer[i]=='\n')++row;while(row>=21){while(top<used&&buffer[top++]!='\n'){}--row;}
 }
 c->text_color(7);c->clear();return 0;}
#elif COMMAND_ID == 17 || COMMAND_ID == 19
int program_main(const char *args,struct command_context *ctx){c=ctx;
#if COMMAND_ID == 19
 if(!*args){for(unsigned i=0;i<16;++i)if(equal(c->environment[i].name,"PATH")){c->write(c->environment[i].value);c->write("\r\n");return 0;}return 0;}
 copy(first,"PATH");copy(second,args);
#else
 if(equal(args,"/?"))return error("SET [name=[value]]     SET displays variables; SET name= removes one.");
 if(!*args){for(unsigned i=0;i<16;++i)if(*c->environment[i].name){c->write(c->environment[i].name);c->putc('=');c->write(c->environment[i].value);c->write("\r\n");}return 0;}
 unsigned n=0;while(args[n]&&args[n]!='='&&n<31){first[n]=upper(args[n]);++n;}first[n]=0;if(args[n]!='=')return error("SET name=value");if(length(args+n+1)>=128)return error("Environment value is too long.");copy(second,args+n+1);
#endif
 if(length(second)>=128)return error("Environment value is too long.");int free=-1;for(unsigned i=0;i<16;++i){if(equal(first,c->environment[i].name)){if(!*second)c->environment[i].name[0]=0;else copy(c->environment[i].value,second);return 0;}if(!*c->environment[i].name)free=(int)i;}if(free<0)return error("Environment is full.");if(*second){copy(c->environment[free].name,first);copy(c->environment[free].value,second);}return 0;}
#elif COMMAND_ID == 18
int program_main(const char *args,struct command_context *ctx){c=ctx;if(!*args){c->write("16-bit CPU duty: ");decimal(c,c->speed);c->write("%\r\n");return 0;}unsigned value=0;while(*args>='0'&&*args<='9')value=value*10+(*args++-'0');if(*skip(args)||value<1||value>100)return error("SPEED [1..100]     CPU duty percent; 100 disables throttling.");c->speed=value;return 0;}
#elif COMMAND_ID == 20
int program_main(const char *args,struct command_context *ctx){c=ctx;if(equal(args,"/?"))return error("EXIT [/B] [errorlevel]     Exit a batch script.");if(!c->batch_depth)return error("The primary DOS shell remains active. Use your computer's power control to shut down.");c->exit_requested=1;if(prefix(args,"/B"))args=skip(args+2);unsigned n=0;while(*args>='0'&&*args<='9')n=n*10+(*args++-'0');return (int)n;}
#endif
