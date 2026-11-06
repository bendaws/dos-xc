;
; BenDOS /boot/volume.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
org 0x7c00
    jmp short start
    nop
    db 'BENDOS  '
    dw 512
    db 4
    dw 4
    db 2
%ifdef FAT32
    dw 0
%else
    dw 512
%endif
    dw 0
    db 0xf8
%ifdef FAT32
    dw 0
%else
    dw 128
%endif
    dw 63,255
    dd 2048,129024
%ifdef FAT32
    dd 0
    dw 0,0
    dd 2
    dw 1,6
    times 12 db 0
    db 0x80,0,0x29
    dd 0
    db 'BENDOS     '
    db 'FAT32   '
%else
    db 0x80,0,0x29
    dd 0
    db 'BENDOS     '
    db 'FAT16   '
%endif
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    sti
    mov [drive], dl
    mov eax, [loader_lba]
    mov [dap+8], eax
    mov ax, [loader_sectors]
    mov [dap+2], ax
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc error
    mov dl, [drive]
    jmp 0x1000:0
error:
    mov si, message
.next:
    lodsb
    test al, al
    jz $
    mov ah, 0x0e
    int 0x10
    jmp .next
message db 'BenDOS: LOADER.BIN read failed',0
drive db 0
align 4
dap: db 16,0
     dw 0,0,0x1000
     dq 0
times 0x1a0-($-$$) db 0
    db 'BDFS'
loader_lba dd 0
loader_sectors dw 0
setup_flag db 1
    db 0
copy_sectors dd 0
    dd 2048
    dd 0                       ; SETUP.CFG first sector
    dd 0                       ; DOSDATA.BIN first sector
    dd 0                       ; format version
    times 510-($-$$) db 0
    dw 0xaa55
