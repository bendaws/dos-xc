/*
 * BenDOS /programs/util.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "api.h"
static unsigned length(const char *s){unsigned n=0;while(s[n])++n;return n;}
static char upper(char c){return c>='a'&&c<='z'?c-32:c;}
static int equal(const char *a,const char *b){while(*a&&*b){if(upper(*a++)!=upper(*b++))return 0;}return !*a&&!*b;}
static int prefix(const char *a,const char *b){while(*b){if(upper(*a++)!=upper(*b++))return 0;}return 1;}
static void copy(char *d,const char *s){while((*d++=*s++)){} }
static void append(char *d,const char *s){copy(d+length(d),s);}
static const char *skip(const char *s){while(*s==' '||*s=='\t')++s;return s;}
static const char *token(const char *s,char *out,unsigned cap){s=skip(s);unsigned n=0;int quote=0;
 while(*s){if(*s=='"'){quote=!quote;++s;continue;}if(!quote&&(*s==' '||*s=='\t'))break;if(n+1<cap)out[n++]=*s;++s;}out[n]=0;return skip(s);}
static int wildcard(const char *pattern,const char *text){
 if(equal(pattern,"*.*"))pattern="*";
 while(*pattern){if(*pattern=='*'){do{if(wildcard(pattern+1,text))return 1;}while(*text++);return 0;}
 if(!*text||(*pattern!='?'&&upper(*pattern)!=upper(*text)))return 0;++pattern;++text;}return !*text;}
static void decimal(struct command_context *c,unsigned value){char b[16];unsigned n=0;do{b[n++]=(char)('0'+value%10);value/=10;}while(value);while(n)c->putc(b[--n]);}
static void sized(struct command_context *c,uint64_t value,unsigned width){char b[32];unsigned n=0,count=0;do{
 uint64_t quotient=0;unsigned remainder=0;for(int bit=63;bit>=0;--bit){remainder=(remainder<<1)|(unsigned)((value>>bit)&1);if(remainder>=10){remainder-=10;quotient|=(uint64_t)1<<bit;}}
 if(count&&count%3==0)b[n++]=',';b[n++]=(char)('0'+remainder);value=quotient;++count;
 }while(value);while(width>n){c->putc(' ');--width;}while(n)c->putc(b[--n]);}

void *memcpy(void *destination,const void *source,__SIZE_TYPE__ size){char *d=destination;const char *s=source;while(size--)*d++=*s++;return destination;}
void *memset(void *destination,int value,__SIZE_TYPE__ size){unsigned char *d=destination;while(size--)*d++=(unsigned char)value;return destination;}
