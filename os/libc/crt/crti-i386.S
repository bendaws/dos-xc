/*
 * DOS-XC /os/libc/crt/crti-i386.S
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

	.section .init, "ax", @progbits
	.globl _init
	.type _init, @function
_init:
	sub $12, %esp
	.section .fini, "ax", @progbits
	.globl _fini
	.type _fini, @function
_fini:
	sub $12, %esp
	.section .note.GNU-stack,"",@progbits
