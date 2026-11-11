/*
 * DOS-XC /os/libc/crt/crti-x86_64.S
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

	.section .init, "ax", @progbits
	.globl _init
	.type _init, @function
_init:
	push %rax
	.section .fini, "ax", @progbits
	.globl _fini
	.type _fini, @function
_fini:
	push %rax
	.section .note.GNU-stack,"",@progbits
