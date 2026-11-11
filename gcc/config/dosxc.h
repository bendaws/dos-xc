/*
 * DOS-XC /gcc/config/dosxc.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

/* This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3, or (at your option) any later
   version. */

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()		\
  do						\
    {						\
      builtin_define ("__dosxc__");		\
      builtin_define ("__DOSXC__");		\
      builtin_assert ("system=dosxc");	\
    }						\
  while (0)

#undef TARGET_EXECUTABLE_SUFFIX
#define TARGET_EXECUTABLE_SUFFIX ".exe"

#undef TARGET_SUBTARGET64_DEFAULT
#define TARGET_SUBTARGET64_DEFAULT (MASK_128BIT_LONG_DOUBLE | MASK_NO_RED_ZONE)

#undef STARTFILE_SPEC
#define STARTFILE_SPEC "%{!r:%{!nostartfiles:%{!shared:crt0.o%s} crti.o%s crtbeginS.o%s}}"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "%{!r:%{!nostartfiles:crtendS.o%s crtn.o%s}}"

#undef LIB_SPEC
#define LIB_SPEC "-lc"

#undef LINK_GCC_C_SEQUENCE_SPEC
#define LINK_GCC_C_SEQUENCE_SPEC "%{!nolibc:%L} %G %{!nolibc:%L}"

#undef CPP_SPEC
#define CPP_SPEC "%{pthread:-D_REENTRANT}"

#undef LINK_SPEC
#define LINK_SPEC \
  "%{no-pie:%eDOS-XC programs are always position-independent (omit -no-pie)} \
   %{!r:-z text -z max-page-size=4096 -z noseparate-code -z noexecstack \
	%{shared:-shared %{!o*:-o a.so}; \
	  static|static-pie:-static -pie --no-dynamic-linker %{!o*:-o a.exe}; \
	  :-pie -dynamic-linker /SYSTEM/ELFAPI.DLL -z now %{!o*:-o a.exe}}}"

#undef TARGET_ASM_FILE_END
#define TARGET_ASM_FILE_END file_end_indicate_exec_stack
