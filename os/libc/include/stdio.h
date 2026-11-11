/*
 * DOS-XC /os/libc/include/stdio.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _STDIO_H
#define _STDIO_H
#define __NEED_size_t
#define __NEED_ssize_t
#define __NEED_off_t
#define __NEED_FILE
#define __NEED_va_list
#include <sys/_types.h>

#define EOF (-1)
#define BUFSIZ 8192
#define FOPEN_MAX 64
#define FILENAME_MAX 260
#define L_tmpnam 64
#define L_ctermid 16
#define TMP_MAX 238328
#define P_tmpdir "/TEMP"
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

__BEGIN_DECLS
typedef struct { off_t __position; unsigned __state[2]; } fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#define stdin stdin
#define stdout stdout
#define stderr stderr

int remove(const char *);
int rename(const char *, const char *);
FILE *tmpfile(void);
char *tmpnam(char *);
char *tempnam(const char *, const char *);
int fclose(FILE *);
int fflush(FILE *);
FILE *fopen(const char *__restrict, const char *__restrict);
FILE *freopen(const char *__restrict, const char *__restrict, FILE *__restrict);
FILE *fdopen(int, const char *);
FILE *fmemopen(void *__restrict, size_t, const char *__restrict);
FILE *open_memstream(char **, size_t *);
FILE *popen(const char *, const char *);
int pclose(FILE *);
void setbuf(FILE *__restrict, char *__restrict);
int setvbuf(FILE *__restrict, char *__restrict, int, size_t);
void setbuffer(FILE *, char *, size_t);
void setlinebuf(FILE *);

int fprintf(FILE *__restrict, const char *__restrict, ...) __printflike(2, 3);
int printf(const char *__restrict, ...) __printflike(1, 2);
int sprintf(char *__restrict, const char *__restrict, ...) __printflike(2, 3);
int snprintf(char *__restrict, size_t, const char *__restrict, ...) __printflike(3, 4);
int dprintf(int, const char *__restrict, ...) __printflike(2, 3);
int asprintf(char **, const char *, ...) __printflike(2, 3);
int vfprintf(FILE *__restrict, const char *__restrict, __dosxc_va_list) __printflike(2, 0);
int vprintf(const char *__restrict, __dosxc_va_list) __printflike(1, 0);
int vsprintf(char *__restrict, const char *__restrict, __dosxc_va_list) __printflike(2, 0);
int vsnprintf(char *__restrict, size_t, const char *__restrict, __dosxc_va_list) __printflike(3, 0);
int vdprintf(int, const char *__restrict, __dosxc_va_list) __printflike(2, 0);
int vasprintf(char **, const char *, __dosxc_va_list) __printflike(2, 0);
int fscanf(FILE *__restrict, const char *__restrict, ...) __scanflike(2, 3);
int scanf(const char *__restrict, ...) __scanflike(1, 2);
int sscanf(const char *__restrict, const char *__restrict, ...) __scanflike(2, 3);
int vfscanf(FILE *__restrict, const char *__restrict, __dosxc_va_list) __scanflike(2, 0);
int vscanf(const char *__restrict, __dosxc_va_list) __scanflike(1, 0);
int vsscanf(const char *__restrict, const char *__restrict, __dosxc_va_list) __scanflike(2, 0);

int fgetc(FILE *);
char *fgets(char *__restrict, int, FILE *__restrict);
int fputc(int, FILE *);
int fputs(const char *__restrict, FILE *__restrict);
int getc(FILE *);
int getchar(void);
char *gets(char *) __deprecated;
int putc(int, FILE *);
int putchar(int);
int puts(const char *);
int ungetc(int, FILE *);
int getw(FILE *);
int putw(int, FILE *);
ssize_t getline(char **__restrict, size_t *__restrict, FILE *__restrict);
ssize_t getdelim(char **__restrict, size_t *__restrict, int, FILE *__restrict);
size_t fread(void *__restrict, size_t, size_t, FILE *__restrict);
size_t fwrite(const void *__restrict, size_t, size_t, FILE *__restrict);
int fgetpos(FILE *__restrict, fpos_t *__restrict);
int fseek(FILE *, long, int);
int fseeko(FILE *, off_t, int);
int fsetpos(FILE *, const fpos_t *);
long ftell(FILE *);
off_t ftello(FILE *);
void rewind(FILE *);
void clearerr(FILE *);
int feof(FILE *);
int ferror(FILE *);
void perror(const char *);
int fileno(FILE *);
char *ctermid(char *);

void flockfile(FILE *);
int ftrylockfile(FILE *);
void funlockfile(FILE *);
int getc_unlocked(FILE *);
int getchar_unlocked(void);
int putc_unlocked(int, FILE *);
int putchar_unlocked(int);
int fgetc_unlocked(FILE *);
int fputc_unlocked(int, FILE *);
int fputs_unlocked(const char *__restrict, FILE *__restrict);
char *fgets_unlocked(char *__restrict, int, FILE *__restrict);
size_t fread_unlocked(void *__restrict, size_t, size_t, FILE *__restrict);
size_t fwrite_unlocked(const void *__restrict, size_t, size_t, FILE *__restrict);
int fflush_unlocked(FILE *);
void clearerr_unlocked(FILE *);
int feof_unlocked(FILE *);
int ferror_unlocked(FILE *);
int fileno_unlocked(FILE *);
__END_DECLS
#endif
