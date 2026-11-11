;
; DOS-XC /os/dos/crtdelay.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
; Called from int21 with BP as its frame; [ss:bp+4] is the caller's CS.
crt_patch:
    pushad
    push es
    push ds
    mov es, [ss:bp+4]
    push cs
    pop ds
    xor di, di
.search:
    cmp di, 0x10000 - crt_signature_size
    ja .done
    mov si, crt_signature
    mov bx, di
    mov cx, crt_signature_size
    repe cmpsb
    mov di, bx
    je .found
    inc di
    jnz .search
    jmp .done
.found:
    cmp dword [tsc_per_ms], 0
    jne .patch
    call crt_calibrate
    cmp dword [tsc_per_ms], 0
    je .done
.patch:
    mov byte [es:di], 0xea               ; jmp far crt_delay
    mov word [es:di+1], crt_delay
    mov [es:di+3], cs
.done:
    pop ds
    pop es
    popad
    ret

; Time stamp counter ticks per millisecond, measured over 10 ms of PIT
; channel 2 (nothing is using the speaker yet).
crt_calibrate:
    cli
    in al, 0x61
    mov bl, al
    and al, 0xfd
    or al, 1
    out 0x61, al
    mov al, 0xb0
    out 0x43, al
    mov al, 11932 & 0xff
    out 0x42, al
    mov al, 11932 >> 8
    out 0x42, al
    rdtsc
    mov esi, eax
    mov ecx, 50000000
.wait:
    in al, 0x61
    test al, 0x20
    jnz .elapsed
    dec ecx
    jnz .wait
    mov al, bl
    out 0x61, al
    sti
    ret                                  ; no timer output: leave CRT as it is
.elapsed:
    rdtsc
    sub eax, esi
    mov ecx, eax
    mov al, bl
    out 0x61, al
    sti
    mov eax, ecx
    xor edx, edx
    mov ecx, 10
    div ecx
    mov [tsc_per_ms], eax
    ret

; procedure Delay(ms: word), far, argument on the stack.
crt_delay:
    mov bx, sp
    movzx ecx, word [ss:bx+4]
    jcxz .return
    push esi
    push edi
    mov eax, [cs:tsc_per_ms]
    mul ecx
    mov esi, eax
    mov edi, edx
    rdtsc
    add esi, eax
    adc edi, edx
.wait:
    pause
    rdtsc
    sub eax, esi
    sbb edx, edi
    js .wait
    pop edi
    pop esi
.return:
    retf 2

tsc_per_ms dd 0
crt_signature:                           ; Delay up to its use of the loop count
    db 0x8b, 0xdc, 0x36, 0x8b, 0x57, 0x04, 0x0b, 0xd2, 0x74, 0x13, 0x33, 0xff
    db 0x8e, 0xc7, 0x26, 0x8a, 0x05, 0x8b, 0x1e
crt_signature_size equ $ - crt_signature
