/* The DOS-XC ABI note: name "DOS-XC", type 1, {abi_version 2, flags 0}.
   Version 2 programs make system calls with int $0x80. */
	.section .note.dosxc,"a",@note
	.p2align 2
	.long 7, 8, 1
	.asciz "DOS-XC"
	.p2align 2
	.long 2, 0
	.section .note.GNU-stack,"",@progbits
