/*
 * DOS-XC C library - crt0 for i386 programs
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * __libc_start(stack, info): see crt0-x86_64.S.  The call goes through the
 * PLT when libc is a shared library, so %ebx holds the GOT address.
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
	and $-16, %esp
	sub $8, %esp
	push %ecx
	push %eax
	call __libc_start@PLT
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
