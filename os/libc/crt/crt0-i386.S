/*
 * DOS-XC /os/libc/crt/crt0-i386.S
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "note.inc"
	.text
	.globl _start
	.type _start, @function
_start:
	xor %ebp, %ebp
	mov %esp, %eax
	call 1f
1:	pop %ebx
	add $_GLOBAL_OFFSET_TABLE_+(.-1b), %ebx
	lea __dosxc_start_info@GOTOFF(%ebx), %ecx
	lea main@GOTOFF(%ebx), %edx
	and $-16, %esp
	sub $4, %esp
	push %edx
	push %ecx
	push %eax
	call __libc_start_main@PLT
	ud2
	.size _start, . - _start

	.weak _init, _fini
	.section .data.rel.ro, "aw", @progbits
	.p2align 2
	.hidden __dosxc_start_info
__dosxc_start_info:
	.long __preinit_array_start, __preinit_array_end
	.long __init_array_start, __init_array_end
	.long __fini_array_start, __fini_array_end
	.long _init, _fini
