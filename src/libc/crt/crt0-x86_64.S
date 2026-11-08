/*
 * DOS-XC C library - crt0 for x86-64 programs
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "note.inc"
	.text
	.globl _start
	.type _start, @function
_start:
	xor %ebp, %ebp
	mov %rsp, %rdi
	and $-16, %rsp
	call __libc_start
	ud2
	.size _start, . - _start
