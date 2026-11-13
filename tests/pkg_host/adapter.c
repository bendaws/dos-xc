/* Filesystem and process adapters for running the actual PKG installer on a host. */
#define PKG_HOST_IMPLEMENTATION
#include "fs.h"
#undef fopen
#undef open
#undef stat
#undef mkdir
#undef unlink
#undef rmdir
#undef remove
#undef rename
#undef access
#include "dosxc/syscall.h"
#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <string.h>
#include <sys/wait.h>
extern char **environ;

static const char *path(const char *source,char target[2048]) {
    const char *root=getenv("DOSXC_TEST_ROOT");
    if (!root) abort();
    int drive=source[0] && source[1]==':';
    int n=snprintf(target,2048,"%s%s",drive ? root : "",drive ? source+2 : source);
    if (n<0 || n>=2048) abort();
    for (char *p=target;*p;++p) if (*p=='\\') *p='/';
    return target;
}
FILE *pkg_host_fopen(const char *p,const char *m) { char b[2048]; return fopen(path(p,b),m); }
int pkg_host_open(const char *p,int flags,...) {
    mode_t mode=0; if (flags&O_CREAT) { va_list args; va_start(args,flags); mode=(mode_t)va_arg(args,int); va_end(args); }
    char b[2048]; return open(path(p,b),flags,mode);
}
int pkg_host_stat(const char *p,struct stat *s) { char b[2048]; return stat(path(p,b),s); }
int pkg_host_mkdir(const char *p,mode_t m) { char b[2048]; return mkdir(path(p,b),m); }
int pkg_host_unlink(const char *p) { char b[2048]; return unlink(path(p,b)); }
int pkg_host_rmdir(const char *p) { char b[2048]; return rmdir(path(p,b)); }
int pkg_host_remove(const char *p) { char b[2048]; return remove(path(p,b)); }
int pkg_host_rename(const char *a,const char *b) { char x[2048],y[2048]; return rename(path(a,x),path(b,y)); }
int pkg_host_access(const char *p,int m) { char b[2048]; return access(path(p,b),m); }
int dosxc_sys_sysinfo(struct dosxc_sysinfo *s) {
    memset(s,0,sizeof(*s)); s->drive='C'; s->bits=getenv("DOSXC_TEST_BITS") ? atoi(getenv("DOSXC_TEST_BITS")) : 64; return 0;
}
int dosxc_sys_console(int operation,long a,long b) { (void)operation;(void)a;(void)b; return 'y'; }
int dosxc_sys_driver(int operation,long a,void *b,dosxc_size size) { (void)operation;(void)a;(void)b;(void)size; return 0; }
int dosxc_sys_net(int operation,long a,void *b) { (void)operation;(void)a;(void)b; return 0; }
int dosxc_sys_environ(int operation,const char *name,char *buffer,dosxc_size size) {
    if (operation==DOSXC_ENV_SET) return buffer ? setenv(name,buffer,1) : unsetenv(name);
    const char *v=getenv(name); if (!v || strlen(v)>=size) return -1;
    strcpy(buffer,v); return (int)strlen(v);
}
dosxc_ssize dosxc_sys_listdir(const char *p,struct dosxc_dirent *entries,dosxc_size capacity) {
    char b[2048]; DIR *d=opendir(path(p,b)); if (!d) return -1;
    struct dirent *e; int count=0;
    while ((e=readdir(d))) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if ((dosxc_size)count<capacity) {
            struct dosxc_dirent *out=&entries[count]; memset(out,0,sizeof(*out));
            snprintf(out->name,sizeof(out->name),"%s",e->d_name);
            char full[4096]; snprintf(full,sizeof(full),"%s/%s",b,e->d_name);
            struct stat st; if (!stat(full,&st)) { out->size=st.st_size; if (S_ISDIR(st.st_mode)) out->attributes=DOSXC_ATTR_DIRECTORY; }
        }
        ++count;
    }
    closedir(d); return count;
}
int dosxc_sys_spawn(const char *program,char *const argv[],char *const envp[]) {
    (void)program;
    char *args[40]; int count=0;
    for (;argv[count] && count<39;++count) {
        char translated[2048]; const char *v=argv[count];
        if (count && (!strcmp(argv[count-1],"--output") || !strcmp(argv[count-1],"--cacert"))) v=path(v,translated);
        args[count]=strdup(v);
    }
    args[count]=0; pid_t pid; int status=0;
    int error=posix_spawnp(&pid,"curl",0,0,args,envp ? envp : environ);
    if (!error) while (waitpid(pid,&status,0)<0 && errno==EINTR) {}
    for (int i=0;i<count;++i) free(args[i]);
    return error ? -error : status;
}
