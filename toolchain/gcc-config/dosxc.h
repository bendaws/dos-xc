/* Target definitions for DOS-XC (x86 and x86-64).
   Copyright (C) 2026 Ben Daws.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3, or (at your option) any later
   version.

   DOS-XC executables are position-independent ELF images (ET_DYN)
   carrying a ".note.dosxc" ABI note, always named with the .EXE
   extension.  The system loader places each image anywhere in free
   memory and applies its relocations, so nested programs (gcc -> cc1 ->
   as -> collect2 -> ld) coexist in one address space.

   By default programs are linked dynamically: shared libraries (.so,
   made with "-shared", libc.so included) are loaded by the system
   loader, \SYSTEM\ELFAPI.DLL, which shares their code between programs
   and gives every process its own copy of their data.  "-static" links a
   static PIE without PT_INTERP instead.  */

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()		\
  do						\
    {						\
      builtin_define ("__dosxc__");		\
      builtin_define ("__DOSXC__");		\
      builtin_assert ("system=dosxc");	\
    }						\
  while (0)

/* Every executable carries the .exe suffix: "gcc foo.c -o foo" writes
   foo.exe, and the default output is a.exe.  */
#undef TARGET_EXECUTABLE_SUFFIX
#define TARGET_EXECUTABLE_SUFFIX ".exe"

/* Programs run in ring 0 on their own stack, where an interrupt frame is
   pushed without a stack switch.  That would clobber a red zone.  */
#undef TARGET_SUBTARGET64_DEFAULT
#define TARGET_SUBTARGET64_DEFAULT (MASK_128BIT_LONG_DOUBLE | MASK_NO_RED_ZONE)

#undef STARTFILE_SPEC
#define STARTFILE_SPEC "%{!r:%{!nostartfiles:%{!shared:crt0.o%s} crti.o%s crtbeginS.o%s}}"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "%{!r:%{!nostartfiles:crtendS.o%s crtn.o%s}}"

#undef LIB_SPEC
#define LIB_SPEC "-lc"

/* The C library goes first: it holds the threads implementation and
   __emutls_get_address, and every object must use that one copy of the
   emulated thread-local storage rather than libgcc's.  */
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

/* Mark every object as not needing an executable stack, as GNU/Linux does,
   so hand-run "ld" never warns about .note.GNU-stack.  */
#undef TARGET_ASM_FILE_END
#define TARGET_ASM_FILE_END file_end_indicate_exec_stack
