/*
 * DOS-XC /os/libc/crt/note.inc
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

	.section .note.dosxc,"a",@note
	.p2align 2
	.long 7, 8, 1
	.asciz "DOS-XC"
	.p2align 2
	.long 2, 0
	.section .note.GNU-stack,"",@progbits
