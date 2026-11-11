/*
 * DOS-XC /os/libc/include/spawn.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SPAWN_H
#define _SPAWN_H
#define __NEED_pid_t
#define __NEED_mode_t
#define __NEED_sigset_t
#include <sys/_types.h>
#define POSIX_SPAWN_RESETIDS 1
#define POSIX_SPAWN_SETPGROUP 2
#define POSIX_SPAWN_SETSIGDEF 4
#define POSIX_SPAWN_SETSIGMASK 8
typedef struct { int __flags; pid_t __pgroup; sigset_t __mask, __default; } posix_spawnattr_t;
struct __dosxc_spawn_action;
typedef struct { int __count, __capacity; struct __dosxc_spawn_action *__actions; } posix_spawn_file_actions_t;
__BEGIN_DECLS
int posix_spawn(pid_t *__restrict, const char *__restrict, const posix_spawn_file_actions_t *,
                const posix_spawnattr_t *__restrict, char *const *__restrict, char *const *__restrict);
int posix_spawnp(pid_t *__restrict, const char *__restrict, const posix_spawn_file_actions_t *,
                 const posix_spawnattr_t *__restrict, char *const *__restrict, char *const *__restrict);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *__restrict, int, const char *__restrict, int, mode_t);
int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict, short *__restrict);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_setpgroup(posix_spawnattr_t *, pid_t);
int posix_spawnattr_setsigmask(posix_spawnattr_t *__restrict, const sigset_t *__restrict);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *__restrict, const sigset_t *__restrict);
/* DOS-XC: start PATH beside the caller with only MAP's descriptors (COUNT pairs of
   the caller's fd and the child's fd).  0 or an errno value, like posix_spawn. */
int dosxc_spawn_fds(pid_t *pid, const char *path, char *const argv[], char *const envp[], const int *map, int count);
/* DOS-XC: give up DOSXC_SANDBOX_* rights (<dosxc/abi.h>) for good, for this
   process and the programs it starts.  0, or -1 with errno set. */
int dosxc_sandbox(unsigned rights);
__END_DECLS
#endif
