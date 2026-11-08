/*
 * DOS-XC /setup/install.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

 #include "install.h"
#define PARTITION_START 2048u
struct installer {
    const struct fat32_volume *source;
    struct fat32_volume target;
    void *disk;
    fat32_read_sector_fn read;
    fat32_write_sector_fn write;
    install_progress_fn progress;
    void *progress_context;
    unsigned files;
};
static struct installer state;
static unsigned char sector[512],verification[512],boot[512],mbr[512];
static void zero(void *p,unsigned size){unsigned char *b=p;while(size--)*b++=0;}
static void text_copy(char *d,const char *s){while((*d++=*s++)){} }
static unsigned text_length(const char *s){unsigned n=0;while(s[n])++n;return n;}
static void put16(unsigned char *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void put32(unsigned char *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=n>>(i*8);}
static uint32_t get32(const unsigned char *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int __attribute__((sysv_abi)) target_read(void *ctx,uint32_t lba,void *p){(void)ctx;return state.read(state.disk,lba,p);}
static int __attribute__((sysv_abi)) target_write(void *ctx,uint32_t lba,const void *p){
    (void)ctx;if(state.write(state.disk,lba,p)||state.read(state.disk,lba,verification))return -1;
    const unsigned char *bytes=p;for(unsigned i=0;i<512;++i)if(bytes[i]!=verification[i])return -1;return 0;
}
static void progress(const char *phase,unsigned percent){if(state.progress)state.progress(state.progress_context,phase,percent);}
struct tree_context { char path[260];unsigned depth;int failed; };
static int copy_tree(const char *,unsigned);
static int copy_entry(void *ctx,const struct fat32_info *info){
    struct tree_context *tree=ctx;
    if(info->name[0]=='.'&&(!info->name[1]||(info->name[1]=='.'&&!info->name[2])))return 0;
    char path[260];text_copy(path,tree->path);unsigned n=text_length(path);if(n>1)path[n++]='\\';
    unsigned size=text_length(info->name);if(n+size>=260){tree->failed=1;return 1;}text_copy(path+n,info->name);
    if(info->attributes&16){if(fat32_mkdir(&state.target,path)||copy_tree(path,tree->depth+1)){tree->failed=1;return 1;}}
    else{if(fat32_copy_to(state.source,path,&state.target,path)){tree->failed=1;return 1;}
        ++state.files;progress(path,0);}
    return 0;
}
static int copy_tree(const char *path,unsigned depth){
    if(depth>8)return -1;struct tree_context tree;text_copy(tree.path,path);tree.depth=depth;tree.failed=0;
    if(fat32_list(state.source,path,copy_entry,&tree)||tree.failed)return -1;return 0;
}
int install_system(const struct fat32_volume *source,void *disk,uint32_t sectors,
                   fat32_read_sector_fn read,fat32_write_sector_fn write,
                   install_progress_fn callback,void *context){
    if(!source||!read||!write||sectors<131072)return -1;
    zero(&state,sizeof(state));state.source=source;state.disk=disk;state.read=read;state.write=write;state.progress=callback;state.progress_context=context;
    /* Validate installation resources before the first target write. */
    struct fat32_info info;if(fat32_stat(source,"\\SYSTEM\\FAT32.BIN",&info)||info.size!=512||
       fat32_read(source,"\\SYSTEM\\FAT32.BIN",boot,512)||fat32_read(source,"\\SYSTEM\\MBR.BIN",mbr,512))return -1;
    if(source->read_sector(source->context,source->partition_lba,sector))return -1;
    boot[0x1ab]=sector[0x1ab];
    uint32_t total=sectors-PARTITION_START;
    unsigned spc=total<1048576?1:total<16777216?8:total<67108864?32:64;
    uint32_t fat=1,clusters=0;
    for(unsigned i=0;i<32;++i){clusters=(total-32-2*fat)/spc;uint32_t needed=(clusters+2+127)/128;
        if(needed<=fat)break;fat=needed;}
    clusters=(total-32-2*fat)/spc;if(clusters<65525||clusters>=0x0ffffff0u)return -1;
    zero(boot+11,79);put16(boot+11,512);boot[13]=spc;put16(boot+14,32);boot[16]=2;boot[21]=0xf8;
    put16(boot+24,63);put16(boot+26,255);put32(boot+28,PARTITION_START);put32(boot+32,total);
    put32(boot+36,fat);put32(boot+44,2);put16(boot+48,1);put16(boot+50,6);boot[64]=0x80;boot[66]=0x29;
    uint32_t serial=get32(mbr+440)^sectors^0x13579bdf;put32(boot+67,serial);
    const char *label="DOS-XC     FAT32   ";for(unsigned i=0;i<19;++i)boot[71+i]=label[i];
    boot[0x1aa]=0;put32(boot+0x1b0,PARTITION_START);
    progress("Formatting full-drive FAT32 filesystem",0);
    zero(sector,512);
    /* Clear old primary GPT metadata and all reserved/FAT sectors, preserving
       the old MBR until the complete bootable filesystem has been verified. */
    uint32_t data=PARTITION_START+32+2*fat;
    for(uint32_t lba=1;lba<data+spc;++lba){if(target_write(0,lba,sector))return -1;
        if(!(lba&255))progress("Formatting full-drive FAT32 filesystem",lba*100/data);}
    if(target_write(0,PARTITION_START,boot)||target_write(0,PARTITION_START+6,boot))return -1;
    zero(sector,512);put32(sector,0x41615252);put32(sector+484,0x61417272);
    put32(sector+488,clusters-1);put32(sector+492,3);put32(sector+508,0xaa550000);
    if(target_write(0,PARTITION_START+1,sector)||target_write(0,PARTITION_START+7,sector))return -1;
    zero(sector,512);put32(sector,0x0ffffff8);put32(sector+4,0xffffffff);put32(sector+8,0x0fffffff);
    if(target_write(0,PARTITION_START+32,sector)||target_write(0,PARTITION_START+32+fat,sector))return -1;
    state.target.context=0;state.target.read_sector=target_read;state.target.write_sector=target_write;
    if(fat32_mount(&state.target,PARTITION_START)||copy_tree("\\",0))return -1;
    const char installed[]="SETUP=0\r\n";if(fat32_write(&state.target,"\\SYSTEM\\SETUP.CFG",installed,sizeof(installed)-1))return -1;
    uint32_t loader,size;if(fat32_file_lba(&state.target,"\\SYSTEM\\LOADER.BIN",&loader,&size))return -1;
    put32(boot+0x1a4,loader);put16(boot+0x1a8,(size+511)/512);put32(boot+0x1ac,sectors);
    if(fat32_file_lba(&state.target,"\\SYSTEM\\SETUP.CFG",&loader,&size))return -1;put32(boot+0x1b4,loader);
    if(fat32_file_lba(&state.target,"\\SYSTEM\\DOSDATA.BIN",&loader,&size))return -1;put32(boot+0x1b8,loader);
    if(target_write(0,PARTITION_START,boot)||target_write(0,PARTITION_START+6,boot))return -1;
    /* Invalidate the old backup GPT header on a previously GPT-formatted disk. */
    if(read(disk,sectors-1,sector))return -1;
    if(get32(sector)==0x20494645&&get32(sector+4)==0x54524150){zero(sector,512);if(target_write(0,sectors-1,sector))return -1;}
    zero(mbr+446,64);mbr[446]=0x80;mbr[447]=mbr[448]=mbr[449]=0xff;mbr[450]=0xef;mbr[451]=mbr[452]=mbr[453]=0xff;
    put32(mbr+454,PARTITION_START);put32(mbr+458,total);put32(mbr+440,serial);
    if(target_write(0,0,mbr))return -1;
    progress("Installation complete",100);return 0;
}
