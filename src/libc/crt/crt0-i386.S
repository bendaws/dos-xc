/*
 * DOS-XC C library - crt0 for i386 programs
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "note.inc"
	.text
	.globl _start
	.type _start, @function
_start:
	xor %ebp, %ebp
	mov %esp, %eax
	and $-16, %esp
	sub $12, %esp
	push %eax
	call __libc_start
	ud2
	.size _start, . - _start
