/*
 * DOS-XC /qt/mkspecs/dosxc-g++/qplatformdefs.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef QPLATFORMDEFS_H
#define QPLATFORMDEFS_H

#include "qglobal.h"
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include "../common/posix/qplatformdefs.h"

#undef QT_SOCKLEN_T
#define QT_SOCKLEN_T socklen_t
#define QT_SNPRINTF ::snprintf
#define QT_VSNPRINTF ::vsnprintf

#endif
