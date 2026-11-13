/* Host doubles for the seven OS calls PKG uses; never execute int 0x80. */
#pragma once
#include "../../../os/libc/include/dosxc/abi.h"
int dosxc_sys_spawn(const char *,char *const[],char *const[]);
dosxc_ssize dosxc_sys_listdir(const char *,struct dosxc_dirent *,dosxc_size);
int dosxc_sys_console(int,long,long);
int dosxc_sys_sysinfo(struct dosxc_sysinfo *);
int dosxc_sys_environ(int,const char *,char *,dosxc_size);
int dosxc_sys_net(int,long,void *);
int dosxc_sys_driver(int,long,void *,dosxc_size);
