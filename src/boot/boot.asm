;
; DOS-XC /boot/boot.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
org 0x7c00

    jmp start
    times 8-($-$$) db 0
boot_info_table:
    times 56 db 0

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    sti
    mov [boot_drive], dl
    ; xorriso fills in the boot image's absolute CD LBA at offset 12.
    ; INT 13h addresses the CD in 2048-byte blocks, not 512-byte sectors.
    mov eax, [boot_info_table + 4]
    inc eax
    mov [dap + 8], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov dl, [boot_drive]
    mov eax, [boot_info_table + 4]
    jmp 0x1000:0

disk_error:
    mov si, error_message
.print:
    lodsb
    test al, al
    jz $
    mov ah, 0x0e
    int 0x10
    jmp .print

boot_drive db 0
error_message db "DOS-XC: disk read error", 0
align 4
dap:
    db 0x10, 0
    dw 16                       ; 32 KiB: avoid firmware 64 KiB transfer limits
    dw 0
    dw 0x1000
    dq 0                        ; absolute stage-two CD LBA, filled above

times 510-($-$$) db 0
dw 0xaa55
times 2048-($-$$) db 0           ; reserve one complete CD block
