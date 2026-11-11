/*
 * DOS-XC /os/libc/crt/crtn-x86_64.S
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

	.section .init, "ax", @progbits
	pop %rax
	ret
	.section .fini, "ax", @progbits
	pop %rax
	ret
	.section .note.GNU-stack,"",@progbits
