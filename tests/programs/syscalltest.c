/*
 * DOS-XC /tests/programs/syscalltest.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dosxc/syscall.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (cond) printf("ok   %s\n", #cond);                          \
        else { printf("FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
    } while (0)

/* registers_preserved(number) makes the call by hand with known values in
   every other register (and xmm0, and pi on the x87 stack) and returns 1 if none of them changed. */
int registers_preserved(long number);
#if defined(__x86_64__)
__asm__(".text\n.globl registers_preserved\nregisters_preserved:\n"
        "push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "movabs $0x3ff0000000000000, %rax\n movq %rax, %xmm0\n fldpi\n"
        "mov %rdi, %rax\n"
        "movabs $0x1111111111111111, %rbx\n movabs $0x2222222222222222, %rcx\n"
        "movabs $0x3333333333333333, %rdx\n movabs $0x4444444444444444, %rsi\n"
        "xor %edi, %edi\n movabs $0x5555555555555555, %rbp\n"
        "movabs $0x6666666666666666, %r8\n movabs $0x7777777777777777, %r9\n"
        "movabs $0x8888888888888888, %r10\n movabs $0x9999999999999999, %r11\n"
        "movabs $0xaaaaaaaaaaaaaaaa, %r12\n movabs $0xbbbbbbbbbbbbbbbb, %r13\n"
        "movabs $0xcccccccccccccccc, %r14\n movabs $0xdddddddddddddddd, %r15\n"
        "int $0x80\n"
        "xor %eax, %eax\n"
        "movabs $0x1111111111111111, %rdi\n cmp %rdi, %rbx\n jne 1f\n"
        "movabs $0x2222222222222222, %rdi\n cmp %rdi, %rcx\n jne 1f\n"
        "movabs $0x3333333333333333, %rdi\n cmp %rdi, %rdx\n jne 1f\n"
        "movabs $0x4444444444444444, %rdi\n cmp %rdi, %rsi\n jne 1f\n"
        "movabs $0x5555555555555555, %rdi\n cmp %rdi, %rbp\n jne 1f\n"
        "movabs $0x6666666666666666, %rdi\n cmp %rdi, %r8\n jne 1f\n"
        "movabs $0x7777777777777777, %rdi\n cmp %rdi, %r9\n jne 1f\n"
        "movabs $0x8888888888888888, %rdi\n cmp %rdi, %r10\n jne 1f\n"
        "movabs $0x9999999999999999, %rdi\n cmp %rdi, %r11\n jne 1f\n"
        "movabs $0xaaaaaaaaaaaaaaaa, %rdi\n cmp %rdi, %r12\n jne 1f\n"
        "movabs $0xbbbbbbbbbbbbbbbb, %rdi\n cmp %rdi, %r13\n jne 1f\n"
        "movabs $0xcccccccccccccccc, %rdi\n cmp %rdi, %r14\n jne 1f\n"
        "movabs $0xdddddddddddddddd, %rdi\n cmp %rdi, %r15\n jne 1f\n"
        "movq %xmm0, %rdi\n movabs $0x3ff0000000000000, %rsi\n cmp %rsi, %rdi\n jne 1f\n"
        "fldpi\n fucomip %st(1), %st\n jne 1f\n"
        "mov $1, %eax\n"
        "1: fstp %st(0)\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");
#else
__asm__(".text\n.globl registers_preserved\nregisters_preserved:\n"
        "push %ebx\n push %esi\n push %edi\n push %ebp\n"
        "mov $0x3f800000, %eax\n movd %eax, %xmm0\n fldpi\n"
        "mov 20(%esp), %eax\n"
        "xor %ebx, %ebx\n mov $0x22222222, %ecx\n mov $0x33333333, %edx\n"
        "mov $0x44444444, %esi\n mov $0x55555555, %edi\n mov $0x66666666, %ebp\n"
        "int $0x80\n"
        "xor %eax, %eax\n"
        "test %ebx, %ebx\n jne 1f\n"
        "cmp $0x22222222, %ecx\n jne 1f\n cmp $0x33333333, %edx\n jne 1f\n"
        "cmp $0x44444444, %esi\n jne 1f\n cmp $0x55555555, %edi\n jne 1f\n"
        "cmp $0x66666666, %ebp\n jne 1f\n"
        "movd %xmm0, %ecx\n cmp $0x3f800000, %ecx\n jne 1f\n"
        "fldpi\n fucomip %st(1), %st\n jne 1f\n"
        "mov $1, %eax\n"
        "1: fstp %st(0)\n pop %ebp\n pop %edi\n pop %esi\n pop %ebx\n ret\n");
#endif

int main(void) {
    int pid = dosxc_sys_getpid();
    CHECK(pid > 0);
    CHECK(pid == getpid());
    CHECK(__dosxc_syscall0(9999) == -DOSXC_ENOSYS);
    CHECK(registers_preserved(DOSXC_SYS_GETPID));
    CHECK(registers_preserved(9999));

    long long now = dosxc_sys_time();
    CHECK(now > 1700000000LL);
    unsigned long long t0 = dosxc_sys_nanoseconds();
    dosxc_sys_sleep(20000000ull);
    unsigned long long t1 = dosxc_sys_nanoseconds();
    CHECK(t1 - t0 >= 15000000ull && t1 - t0 < 2000000000ull);

    int fd = dosxc_sys_open("SYSCALL.TMP", DOSXC_O_RDWR | DOSXC_O_CREAT | DOSXC_O_TRUNC, 0666);
    CHECK(fd >= 0);
    CHECK(dosxc_sys_write(fd, "abcdef", 6) == 6);
    CHECK(dosxc_sys_seek(fd, 0, SEEK_END) == 6);
    /* A 64-bit offset round-trips (FAT32 files stop at 4 GiB, so only
       the argument and result encoding matter here). */
    long long far = dosxc_sys_seek(fd, 0x100000002LL, SEEK_SET);
    CHECK(far == 0x100000002LL || far == -DOSXC_EINVAL || far == -DOSXC_EFBIG);
    CHECK(dosxc_sys_seek(fd, 2, SEEK_SET) == 2);
    char buffer[8] = {0};
    CHECK(dosxc_sys_read(fd, buffer, 4) == 4 && !memcmp(buffer, "cdef", 4));
    CHECK(dosxc_sys_ftruncate(fd, 3) == 0);
    struct dosxc_stat st;
    CHECK(dosxc_sys_fstat(fd, &st) == 0 && st.size == 3);
    CHECK(dosxc_sys_close(fd) == 0);
    CHECK(dosxc_sys_close(fd) == -DOSXC_EBADF);
    CHECK(dosxc_sys_unlink("SYSCALL.TMP") == 0);
    CHECK(dosxc_sys_stat("SYSCALL.TMP", &st) == -DOSXC_ENOENT);

    void *block = dosxc_sys_alloc(8192);
    CHECK(block && !((unsigned long)block & 4095) && ((char *)block)[100] == 0);
    CHECK(dosxc_sys_release(block) == 0);

    struct dosxc_sysinfo info;
    CHECK(dosxc_sys_sysinfo(&info) == 0 && info.abi_version == DOSXC_ABI_VERSION &&
          info.bits == sizeof(void *) * 8);

    /* Floating-point state survives a child program run inside a call
       (values are rounded through volatiles so x87 excess precision
       cannot make the comparison fail). */
    volatile double x = 1.0 / 3.0;
    double y = x * 3.0 + 0.25;
    char *argv[] = {"HELLO.EXE", "child", 0};
    int status = dosxc_sys_spawn("HELLO.EXE", argv, 0);
    CHECK(status == 0);
    volatile double kept = y, expected = x * 3.0 + 0.25;
    CHECK(kept == expected && x > 0.33 && x < 0.34);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
