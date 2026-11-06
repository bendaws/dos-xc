/*
 * BenDOS /setup/program.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

 #include "../programs/util.h"
#include "../kernel/bootinfo.h"
#include "../drivers/storage.h"
#include "install.h"
static struct command_context *console;
static void progress(void *ctx,const char *phase,unsigned percent){
    (void)ctx;console->cursor(8,6);console->write("                                                                    ");
    console->cursor(8,6);console->write(phase);console->cursor(10,6);
    if(percent){decimal(console,percent);console->write("%        ");}
}
int program_main(const char *args,struct command_context *ctx){
    (void)args;console=ctx;
    if(!ctx->bios_available||*(volatile uint32_t *)0x7060!=0x54534e49){ctx->write("Boot the installation media to start Setup.\r\n");return 1;}
    unsigned target=*(volatile unsigned char *)0x7064;
    uint32_t sectors=*(volatile uint32_t *)0x7068;
    *(volatile uint32_t *)0x7060=0;
    if(target==*(volatile unsigned char *)0x7014||target<0x80||target>=0x90){ctx->write("Invalid installation target.\r\n");return 1;}
    ctx->text_color(0x17);ctx->clear();ctx->cursor(1,1);ctx->write(BENDOS_LABEL " Setup");
    ctx->cursor(2,0);ctx->write("================");ctx->text_color(0x1f);
    ctx->cursor(24,0);ctx->text_color(0x70);ctx->write("                                                                               ");ctx->cursor(24,2);ctx->write("Installing - please wait");ctx->text_color(0x1f);
    ctx->cursor(4,6);ctx->write("Installing Ben's DOS using the full internal drive.");
    ctx->cursor(6,6);ctx->write("Do not turn off your computer. Every write is verified.");
    int result=install_system(ctx->volume,(void *)(__UINTPTR_TYPE__)target,sectors,storage_disk_read,storage_disk_write,progress,0);
    ctx->cursor(15,6);
    if(result){ctx->write("Installation failed. The target is incomplete.\r\n");ctx->write("Press any key to return to the external drive.");while(ctx->read_key()<0){}ctx->text_color(7);ctx->clear();return 1;}
    ctx->cursor(24,2);ctx->text_color(0x70);ctx->write("ENTER=Restart             ");ctx->text_color(0x1f);ctx->cursor(15,6);
    ctx->write("Setup has finished installing Ben's DOS.");ctx->cursor(17,6);ctx->write("System directory: C:\\SYSTEM");
    ctx->cursor(19,6);ctx->write("Remove the setup media, then press ENTER to restart.");
    while(ctx->read_key()!='\n'){}
    __asm__ volatile("outb %0,$0x64"::"a"((unsigned char)0xfe));
    for(;;)__asm__ volatile("hlt");
}
