/*
 * DOS-XC /os/libc/crt/crtn-i386.S
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

	.section .init, "ax", @progbits
	add $12, %esp
	ret
	.section .fini, "ax", @progbits
	add $12, %esp
	ret
	.section .note.GNU-stack,"",@progbits
