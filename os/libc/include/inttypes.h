/*
 * DOS-XC /os/libc/include/inttypes.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>
#define __NEED_wchar_t
#include <sys/_types.h>

#if __SIZEOF_LONG__ == 8
#define __PRI64 "l"
#define __PRIPTR "l"
#define __PRIFAST "l"
#else
#define __PRI64 "ll"
#define __PRIPTR ""
#define __PRIFAST ""
#endif

#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 __PRI64 "d"
#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 __PRI64 "i"
#define PRIo8 "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 __PRI64 "o"
#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 __PRI64 "u"
#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 __PRI64 "x"
#define PRIX8 "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 __PRI64 "X"
#define PRIdLEAST8 PRId8
#define PRIdLEAST16 PRId16
#define PRIdLEAST32 PRId32
#define PRIdLEAST64 PRId64
#define PRIiLEAST8 PRIi8
#define PRIiLEAST16 PRIi16
#define PRIiLEAST32 PRIi32
#define PRIiLEAST64 PRIi64
#define PRIoLEAST8 PRIo8
#define PRIoLEAST16 PRIo16
#define PRIoLEAST32 PRIo32
#define PRIoLEAST64 PRIo64
#define PRIuLEAST8 PRIu8
#define PRIuLEAST16 PRIu16
#define PRIuLEAST32 PRIu32
#define PRIuLEAST64 PRIu64
#define PRIxLEAST8 PRIx8
#define PRIxLEAST16 PRIx16
#define PRIxLEAST32 PRIx32
#define PRIxLEAST64 PRIx64
#define PRIXLEAST8 PRIX8
#define PRIXLEAST16 PRIX16
#define PRIXLEAST32 PRIX32
#define PRIXLEAST64 PRIX64
#define PRIdFAST8 "d"
#define PRIdFAST16 __PRIFAST "d"
#define PRIdFAST32 __PRIFAST "d"
#define PRIdFAST64 PRId64
#define PRIiFAST8 "i"
#define PRIiFAST16 __PRIFAST "i"
#define PRIiFAST32 __PRIFAST "i"
#define PRIiFAST64 PRIi64
#define PRIoFAST8 "o"
#define PRIoFAST16 __PRIFAST "o"
#define PRIoFAST32 __PRIFAST "o"
#define PRIoFAST64 PRIo64
#define PRIuFAST8 "u"
#define PRIuFAST16 __PRIFAST "u"
#define PRIuFAST32 __PRIFAST "u"
#define PRIuFAST64 PRIu64
#define PRIxFAST8 "x"
#define PRIxFAST16 __PRIFAST "x"
#define PRIxFAST32 __PRIFAST "x"
#define PRIxFAST64 PRIx64
#define PRIXFAST8 "X"
#define PRIXFAST16 __PRIFAST "X"
#define PRIXFAST32 __PRIFAST "X"
#define PRIXFAST64 PRIX64
#define PRIdMAX PRId64
#define PRIiMAX PRIi64
#define PRIoMAX PRIo64
#define PRIuMAX PRIu64
#define PRIxMAX PRIx64
#define PRIXMAX PRIX64
#define PRIdPTR __PRIPTR "d"
#define PRIiPTR __PRIPTR "i"
#define PRIoPTR __PRIPTR "o"
#define PRIuPTR __PRIPTR "u"
#define PRIxPTR __PRIPTR "x"
#define PRIXPTR __PRIPTR "X"

#define SCNd8 "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 __PRI64 "d"
#define SCNi8 "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 __PRI64 "i"
#define SCNo8 "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNo64 __PRI64 "o"
#define SCNu8 "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 __PRI64 "u"
#define SCNx8 "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 __PRI64 "x"
#define SCNdMAX SCNd64
#define SCNiMAX SCNi64
#define SCNoMAX SCNo64
#define SCNuMAX SCNu64
#define SCNxMAX SCNx64
#define SCNdPTR __PRIPTR "d"
#define SCNiPTR __PRIPTR "i"
#define SCNoPTR __PRIPTR "o"
#define SCNuPTR __PRIPTR "u"
#define SCNxPTR __PRIPTR "x"

__BEGIN_DECLS
typedef struct { intmax_t quot, rem; } imaxdiv_t;
intmax_t imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
intmax_t strtoimax(const char *__restrict, char **__restrict, int);
uintmax_t strtoumax(const char *__restrict, char **__restrict, int);
__END_DECLS
#endif
