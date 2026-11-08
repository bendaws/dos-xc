/*
 * DOS-XC C library - crt0 for x86-64 programs
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * __libc_start(stack, info): info lists the program's own constructor and
 * destructor arrays, which the C library cannot see when it is a shared
 * library (libc.so).
 */
#include "note.inc"
	.text
	.globl _start
	.type _start, @function
_start:
	xor %ebp, %ebp
	mov %rsp, %rdi
	lea __dosxc_start_info(%rip), %rsi
	and $-16, %rsp
	call __libc_start@PLT
	ud2
	.size _start, . - _start

	.weak _init, _fini
	.section .data.rel.ro, "aw", @progbits
	.p2align 3
	.hidden __dosxc_start_info
__dosxc_start_info:
	.quad __preinit_array_start, __preinit_array_end
	.quad __init_array_start, __init_array_end
	.quad __fini_array_start, __fini_array_end
	.quad _init, _fini
