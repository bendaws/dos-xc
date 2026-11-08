/* DOS-XC C library - .init/.fini epilogues (x86-64) */
	.section .init, "ax", @progbits
	pop %rax
	ret
	.section .fini, "ax", @progbits
	pop %rax
	ret
	.section .note.GNU-stack,"",@progbits
