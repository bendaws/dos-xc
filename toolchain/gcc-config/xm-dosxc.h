/* Host configuration for GCC running on DOS-XC.
   Copyright (C) 2026 Ben Daws.

   This file is part of GCC, distributed under the GNU General Public
   License version 3 or later.  */

/* Every DOS-XC program file is named *.EXE.  */
#define HOST_EXECUTABLE_SUFFIX ".exe"

/* File names are case-insensitive: FOO.C is C, not C++ (see gcc.cc).  */
#define GCC_CASE_INSENSITIVE_SUFFIXES 1
