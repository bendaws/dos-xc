;
; BenDOS /boot/mbr.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
org 0x600
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    mov si, 0x7c00
    mov di, 0x600
    mov cx, 256
    rep movsw
    jmp 0:relocated
relocated:
    sti
    mov [drive], dl
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc error
    mov dl, [drive]
    jmp 0:0x7c00
error:
    mov si, message
.next:
    lodsb
    test al, al
    jz $
    mov ah, 0x0e
    int 0x10
    jmp .next
message db 'BenDOS: cannot read system partition',0
drive db 0
align 4
dap: db 16,0
     dw 1,0x7c00,0
     dq 2048
times 440-($-$$) db 0
dd 0
dw 0
times 64 db 0
dw 0xaa55
