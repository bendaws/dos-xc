/* DOS-XC C library - .init/.fini epilogues (i386) */
	.section .init, "ax", @progbits
	add $12, %esp
	ret
	.section .fini, "ax", @progbits
	add $12, %esp
	ret
	.section .note.GNU-stack,"",@progbits
