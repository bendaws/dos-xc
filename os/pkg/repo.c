/* Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "repo.h"
#include "sha256.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int repo_init(struct repo *r, const char *origin, int allow_http, int bits, const char *work, repo_transfer transfer, void *context) {
    if (!origin || !*origin) origin=PKG_REPOSITORY;
    const char *host = !strncmp(origin,"https://",8) ? origin+8 : allow_http && !strncmp(origin,"http://",7) ? origin+7 : 0;
    if (!host || !*host || (bits!=32 && bits!=64) || strlen(origin)>=sizeof(r->origin) || strlen(work)>=sizeof(r->work)) return -1;
    size_t n=strlen(origin);
    if (origin[n-1]=='/') --n;
    for (const char *p=host;p<origin+n;++p)
        if (!(isalnum((unsigned char)*p) || strchr(".-:[]",*p))) return -1;
    if (host>=origin+n) return -1;
    memcpy(r->origin,origin,n); r->origin[n]=0;
    snprintf(r->work,sizeof(r->work),"%s",work); r->bits=bits; r->transfer=transfer; r->context=context;
    return 0;
}

static int number(const char *text, unsigned long *value) {
    unsigned long n=0;
    if (!*text) return -1;
    for (;*text;++text) {
        if (*text<'0' || *text>'9' || n>214748364UL || (n==214748364UL && *text>'7')) return -1;
        n=n*10+(unsigned)(*text-'0');
    }
    *value=n; return 0;
}

int repo_read_index(const char *file, struct repo_entry *out, int capacity, int *next) {
    FILE *f=fopen(file,"rb");
    if (!f) return -1;
    char line[1024], metadata[4096]; size_t used=0,total=0; int inside=0,count=0,failed=0,seen_next=0;
    struct repo_entry entry; unsigned fields=0; *next=0;
    while (fgets(line,sizeof(line),f)) {
        size_t n=strlen(line); total+=n;
        if (total>512*1024 || !n || line[n-1]!='\n') { failed=1; break; }
        while (n && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n]=0;
        if (!strcmp(line,"[Package]")) {
            if (inside) { failed=1; break; }
            inside=1; used=0; fields=0; memset(&entry,0,sizeof(entry)); continue;
        }
        if (!strcmp(line,"[/Package]")) {
            if (!inside || fields!=15 || dsp_parse_info(metadata,used,&entry.info) || !entry.id || !entry.size || entry.size>REPO_MAX_DOWNLOAD) { failed=1; break; }
            if (count>=capacity) { failed=1; break; }
            out[count++]=entry; inside=0; continue;
        }
        if (!inside) {
            unsigned long page;
            if (seen_next || strncmp(line,"NextPage=",9) || number(line+9,&page) || page>1000000) { failed=1; break; }
            *next=(int)page; seen_next=1; continue;
        }
        unsigned bit=0;
        if (!strncmp(line,"Id=",3)) { bit=1; if (number(line+3,&entry.id)) failed=1; }
        else if (!strncmp(line,"Size=",5)) { bit=2; if (number(line+5,&entry.size)) failed=1; }
        else if (!strncmp(line,"SHA256=",7)) {
            bit=4;
            if (strlen(line+7)!=64) failed=1;
            else {
                for (unsigned i=0;i<64;++i) { if (!isxdigit((unsigned char)line[i+7])) failed=1; entry.sha256[i]=(char)tolower((unsigned char)line[i+7]); }
            }
        } else if (!strncmp(line,"Download=",9)) {
            char expected[100]; bit=8;
            snprintf(expected,sizeof(expected),"/api/v1/packages/%lu/download",entry.id);
            if (strcmp(line+9,expected)) failed=1;
        } else {
            if (used+n+1>=sizeof(metadata)) failed=1;
            else { memcpy(metadata+used,line,n); used+=n; metadata[used++]='\n'; }
        }
        if (bit && (fields&bit)) failed=1;
        fields|=bit;
        if (failed) break;
    }
    if (ferror(f) || inside || !seen_next) failed=1;
    fclose(f);
    if (failed) { puts("Invalid response from the package catalog."); return -1; }
    return count;
}

static int encode(const char *text, char *out, size_t capacity) {
    static const char digits[]="0123456789ABCDEF";
    size_t n=0;
    for (const unsigned char *p=(const unsigned char *)text;*p;++p) {
        int plain=isalnum(*p) || strchr("-._~",*p);
        if (n+(plain?1:3)>=capacity) return -1;
        if (plain) out[n++]=(char)*p;
        else { out[n++]='%'; out[n++]=digits[*p>>4]; out[n++]=digits[*p&15]; }
    }
    out[n]=0; return 0;
}

static int request_index(struct repo *r, const char *path, struct repo_entry *out, int capacity, int *next) {
    char url[1200],file[340];
    if (snprintf(url,sizeof(url),"%s%s",r->origin,path)>=(int)sizeof(url)) return -1;
    snprintf(file,sizeof(file),"%s/index.inf",r->work);
    int count=-1;
    if (!r->transfer(url,file,512*1024,0,r->context)) count=repo_read_index(file,out,capacity,next);
    remove(file); return count;
}

int repo_resolve(struct repo *r, const char *name, struct repo_entry *out) {
    char encoded[256],path[400]; int next;
    if (!dsp_valid_name(name) || encode(name,encoded,sizeof(encoded))) { puts("Invalid package name."); return -1; }
    snprintf(path,sizeof(path),"/api/v1/resolve?format=inf&arch=%d&name=%s",r->bits,encoded);
    if (request_index(r,path,out,1,&next)!=1 || strcasecmp(name,out->info.name) || (out->info.arch!=16 && out->info.arch!=r->bits)) return -1;
    return 0;
}

int repo_search(struct repo *r, const char *query, struct repo_entry *out, int capacity) {
    char encoded[512],path[700]; int page=1,count=0;
    if (encode(query,encoded,sizeof(encoded))) return -1;
    struct repo_entry *batch=malloc(25*sizeof(*batch));
    if (!batch) return -1;
    for (int requests=0;requests<100 && count<capacity;++requests) {
        snprintf(path,sizeof(path),"/api/v1/packages?format=inf&arch=%d&compatible=1&q=%s&page=%d",r->bits,encoded,page);
        int next,n=request_index(r,path,batch,25,&next);
        if (n<0 || (next && next<=page)) { free(batch); return -1; }
        for (int i=0;i<n && count<capacity;++i) {
            if (batch[i].info.arch!=16 && batch[i].info.arch!=r->bits) continue;
            int at=0;
            while (at<count && strcasecmp(out[at].info.name,batch[i].info.name)) ++at;
            if (at==count) out[count++]=batch[i];
            else {
                int version=dsp_compare_versions(batch[i].info.version,out[at].info.version);
                if (version>0 || (!version && batch[i].info.arch==r->bits)) out[at]=batch[i];
            }
        }
        if (!next) break;
        page=next;
    }
    free(batch); return count;
}

int repo_fetch(struct repo *r, const struct repo_entry *entry, char *file, size_t capacity) {
    char url[400];
    if (snprintf(file,capacity,"%s/package.dsp",r->work)>=(int)capacity) return -1;
    snprintf(url,sizeof(url),"%s/api/v1/packages/%lu/download",r->origin,entry->id);
    if (r->transfer(url,file,entry->size,1,r->context)) { remove(file); return -1; }
    FILE *f=fopen(file,"rb"); struct pkg_sha256 hash; char actual[65]; unsigned char buffer[8192]; unsigned long size=0;
    int failed=!f; pkg_sha256_init(&hash);
    if (f) {
        size_t n;
        while ((n=fread(buffer,1,sizeof(buffer),f))) {
            if (n>entry->size-size) { failed=1; break; }
            size+=(unsigned long)n; pkg_sha256_update(&hash,buffer,n);
        }
        if (ferror(f)) failed=1;
        fclose(f);
    }
    pkg_sha256_final(&hash,actual);
    struct dsp_info info; char error[160];
    if (failed || size!=entry->size || strcmp(actual,entry->sha256) || dsp_read_info(file,&info,error,sizeof(error)) ||
        strcasecmp(info.name,entry->info.name) || strcmp(info.version,entry->info.version) || info.arch!=entry->info.arch) {
        puts("The downloaded package failed its size, SHA-256 or metadata check."); remove(file); return -1;
    }
    return 0;
}

int repo_write_index(const char *file, const struct repo_entry *entries, int count) {
    FILE *f=fopen(file,"wb");
    if (!f) return -1;
    int failed=fputs("NextPage=0\n",f)<0;
    for (int i=0;i<count && !failed;++i) {
        char text[4096];
        if (dsp_format_info(&entries[i].info,text,sizeof(text))<0) { failed=1; break; }
        if (fprintf(f,"[Package]\n%sId=%lu\nSize=%lu\nSHA256=%s\nDownload=/api/v1/packages/%lu/download\n[/Package]\n",
                    text,entries[i].id,entries[i].size,entries[i].sha256,entries[i].id)<0) failed=1;
    }
    if (fclose(f)) failed=1;
    if (failed) remove(file);
    return failed ? -1 : 0;
}
