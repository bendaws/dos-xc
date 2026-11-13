/* DOS paths translated only in the host integration executable. */
#pragma once
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
FILE *pkg_host_fopen(const char *,const char *);
int pkg_host_open(const char *,int,...);
int pkg_host_stat(const char *,struct stat *);
int pkg_host_mkdir(const char *,mode_t);
int pkg_host_unlink(const char *);
int pkg_host_rmdir(const char *);
int pkg_host_remove(const char *);
int pkg_host_rename(const char *,const char *);
int pkg_host_access(const char *,int);
#ifndef PKG_HOST_IMPLEMENTATION
#define fopen(a,b) pkg_host_fopen(a,b)
#define open(...) pkg_host_open(__VA_ARGS__)
#define stat(a,b) pkg_host_stat(a,b)
#define mkdir(a,b) pkg_host_mkdir(a,b)
#define unlink(a) pkg_host_unlink(a)
#define rmdir(a) pkg_host_rmdir(a)
#define remove(a) pkg_host_remove(a)
#define rename(a,b) pkg_host_rename(a,b)
#define access(a,b) pkg_host_access(a,b)
#endif
