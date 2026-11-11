/*
 * DOS-XC /os/libc/crt/setjmp-x86_64.S
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

	.text
	.globl setjmp, _setjmp, sigsetjmp, longjmp, _longjmp, siglongjmp
	.type setjmp, @function
	.type _setjmp, @function
	.type sigsetjmp, @function
	.type longjmp, @function
	.type _longjmp, @function
	.type siglongjmp, @function
setjmp:
_setjmp:
sigsetjmp:
	mov %rbx, 0(%rdi)
	mov %rbp, 8(%rdi)
	mov %r12, 16(%rdi)
	mov %r13, 24(%rdi)
	mov %r14, 32(%rdi)
	mov %r15, 40(%rdi)
	lea 8(%rsp), %rdx
	mov %rdx, 48(%rdi)
	mov (%rsp), %rdx
	mov %rdx, 56(%rdi)
	xor %eax, %eax
	ret
longjmp:
_longjmp:
siglongjmp:
	push %rdi
	push %rsi
	sub $8, %rsp
	call __dosxc_fault_unwind@PLT
	add $8, %rsp
	pop %rsi
	pop %rdi
	mov %esi, %eax
	test %eax, %eax
	jnz 1f
	inc %eax
1:	mov 0(%rdi), %rbx
	mov 8(%rdi), %rbp
	mov 16(%rdi), %r12
	mov 24(%rdi), %r13
	mov 32(%rdi), %r14
	mov 40(%rdi), %r15
	mov 48(%rdi), %rsp
	jmp *56(%rdi)
	.section .note.GNU-stack,"",@progbits
