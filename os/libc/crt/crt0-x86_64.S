/*
 * DOS-XC /os/libc/crt/crt0-x86_64.S
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "note.inc"
	.text
	.globl _start
	.type _start, @function
_start:
	xor %ebp, %ebp
	mov %rsp, %rdi
	lea __dosxc_start_info(%rip), %rsi
	lea main(%rip), %rdx
	and $-16, %rsp
	call __libc_start_main@PLT
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
