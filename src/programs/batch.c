/*
 * DOS-XC /programs/batch.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "util.h"
struct frame { char text[65536],argv[10][260],all[512];unsigned ip,returns[8],return_count; };
static struct frame frames[4];
static void integer_string(int value,char *out){char temp[16];unsigned n=0;if(value<0){*out++='-';value=-value;}do{temp[n++]=(char)('0'+value%10);value/=10;}while(value);while(n)*out++=temp[--n];*out=0;}
static int expand(struct frame *f,struct command_context *c,const char *input,char *out){unsigned used=0;
 while(*input){if(*input!='%'){if(used==511)return -1;out[used++]=*input++;continue;}++input;const char *value="";char temporary[260];
  if(*input>='0'&&*input<='9')value=f->argv[*input++-'0'];else if(*input=='*'){value=f->all;++input;}else if(*input=='%'){value="%";++input;}
  else{unsigned n=0;while(*input&&*input!='%'&&n<259)temporary[n++]=*input++;temporary[n]=0;if(*input=='%')++input;
   if(equal(temporary,"ERRORLEVEL")){integer_string(c->errorlevel,temporary);value=temporary;}
   else if(equal(temporary,"CD")){temporary[0]=c->drive;temporary[1]=':';copy(temporary+2,c->cwd);value=temporary;}
   else for(unsigned i=0;i<16;++i)if(equal(temporary,c->environment[i].name)){value=c->environment[i].value;break;}
  }
  while(*value){if(used==511)return -1;out[used++]=*value++;}
 }out[used]=0;return 0;}
static unsigned label(struct frame *f,const char *wanted){unsigned p=0;while(f->text[p]){unsigned at=p;while(f->text[p]&&f->text[p]!='\n')++p;unsigned end=p;if(f->text[p])++p;while(at<end&&(f->text[at]==' '||f->text[at]=='\t'))++at;
 if(f->text[at]==':'&&f->text[at+1]!=':'){char name[260];unsigned n=0;++at;while(at<end&&f->text[at]!='\r'&&n<259)name[n++]=f->text[at++];name[n]=0;if(equal(name,wanted))return p;}}
 return 0xffffffffu;}
static int process(struct frame *f,struct command_context *c){
 char path[260];struct fat32_info info;
 while(f->text[f->ip]){char raw[512],line[512],word[260];unsigned n=0;int too_long=0;
  while(f->text[f->ip]&&f->text[f->ip]!='\n'){char ch=f->text[f->ip++];if(ch=='\r')continue;if(n<511)raw[n++]=ch;else too_long=1;}if(f->text[f->ip]=='\n')++f->ip;raw[n]=0;
  if(too_long||expand(f,c,raw,line)){c->write("Batch line exceeds 511 characters.\r\n");c->errorlevel=1;break;}
  char *p=(char *)skip(line);int silent=*p=='@';if(silent)p=(char *)skip(p+1);if(!*p||*p==':')continue;
  const char *rest=token(p,word,260);if(equal(word,"REM"))continue;
  if(c->echo&&!silent){c->putc(c->drive);c->write(":");c->write(c->cwd);c->write(">");c->write(p);c->write("\r\n");}
  if(equal(word,"GOTO")){token(rest,word,260);if(equal(word,":EOF")||equal(word,"EOF")){if(f->return_count){f->ip=f->returns[--f->return_count];continue;}break;}unsigned target=label(f,*word==':'?word+1:word);if(target==0xffffffffu){c->write("Batch label not found.\r\n");c->errorlevel=1;break;}f->ip=target;continue;}
  if(equal(word,"CALL")){
   if(*rest==':'){token(rest+1,word,260);unsigned target=label(f,word);if(target==0xffffffffu||f->return_count==8){c->errorlevel=1;break;}f->returns[f->return_count++]=f->ip;f->ip=target;continue;}
   c->errorlevel=c->execute(rest);
  }else if(equal(word,"IF")){
   int negate=0,condition=0;rest=token(rest,word,260);if(equal(word,"NOT")){negate=1;rest=token(rest,word,260);}
   if(equal(word,"EXIST")){rest=token(rest,word,260);condition=!c->resolve(word,path)&&!c->stat(c->volume,path,&info);}
   else if(equal(word,"ERRORLEVEL")){rest=token(rest,word,260);unsigned value=0;for(unsigned i=0;word[i]>='0'&&word[i]<='9';++i)value=value*10+word[i]-'0';condition=c->errorlevel>=(int)value;}
   else{char *split=word;while(*split&&!(split[0]=='='&&split[1]=='='))++split;if(!*split){c->write("IF supports EXIST, ERRORLEVEL and string==string.\r\n");c->errorlevel=1;break;}*split=0;condition=equal(word,split+2);}
   if(condition!=negate)c->errorlevel=c->execute(rest);
  }else if(equal(word,"EXIT")){if(f->return_count){f->ip=f->returns[--f->return_count];continue;}if(prefix(rest,"/B"))rest=skip(rest+2);if(*rest){int value=0;while(*rest>='0'&&*rest<='9')value=value*10+(*rest++-'0');c->errorlevel=value;}break;
  }else c->errorlevel=c->execute(p);
  if(c->exit_requested){if(f->return_count){f->ip=f->returns[--f->return_count];c->exit_requested=0;}else break;}
 }
 --c->batch_depth;c->exit_requested=0;return c->errorlevel;
}
int program_main(const char *arguments,struct command_context *c){
 if(c->batch_depth>=4){c->write("Batch nesting limit (4) reached.\r\n");return 1;}
 struct frame *f=&frames[c->batch_depth];memset(f,0,sizeof(*f));
 arguments=token(arguments,f->argv[0],260);if(length(arguments)>=sizeof(f->all))return 1;copy(f->all,arguments);for(unsigned i=1;i<10&&*arguments;++i)arguments=token(arguments,f->argv[i],260);
 char path[260];struct fat32_info info;if(c->resolve(f->argv[0],path)||c->stat(c->volume,path,&info)||info.size>=65536||c->read(c->volume,path,f->text,info.size)){c->write("Cannot read batch script (maximum 65535 bytes).\r\n");return 1;}f->text[info.size]=0;
 ++c->batch_depth;c->exit_requested=0;
 return process(f,c);
}


int program_resume(struct command_context *c){while(c->batch_depth)process(&frames[c->batch_depth-1],c);return c->errorlevel;}
