;
; DOS-XC /os/dos/bios.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
; Protected-mode request block at 0:0x7040:
;   +1 function: 0x42 read, 0x43 write (one sector), 0xC2 read [0x704C] sectors
;      (2048-byte blocks on the boot CD), 0xC3 write [0x704C] sectors,
;      0x48 drive parameters (INT 13h AH=48h) to 0x78000,
;      0xE8 E820 memory map to 0x78000,
;      0x10 INT 10h: ax, bx, cx, di from 0x7050.. with es = 0x7800; result = ax
;   +2 drive, +4 LBA, +8 result (0 or -1; E820 entry count)
bios_service:
    xor ax, ax
    mov es, ax
    cmp byte [es:0x7041], 0xe8
    je memory_map
    cmp byte [es:0x7041], 0x10
    je video_service
    cmp byte [es:0x7041], 0x48
    je drive_parameters
    mov eax, [es:0x7044]
    mov word [bios_dap+2], 1
    cmp byte [es:0x7041], 0xc3
    jne .read_many
    mov cx, [es:0x704c]
    mov [bios_dap+2], cx
    mov byte [es:0x7041], 0x43
    jmp .single
.read_many:
    cmp byte [es:0x7041], 0xc2
    jne .single
    mov cx, [es:0x704c]
    mov [bios_dap+2], cx
    mov byte [es:0x7041], 0x42
.single:
    mov dl, [es:0x7042]
    cmp dl, [es:0x7014]
    jne .physical
    cmp dword [es:0x7010], 'CDVF'
    jne .physical
    cmp byte [es:0x7041], 0x43
    je .readonly
    shr eax, 2
    add eax, [es:0x701c]
.physical:
    mov [bios_dap+8], eax
    mov dl, [es:0x7042]
    mov ah, [es:0x7041]
    mov si, bios_dap
    xor al, al
    int 0x13
    mov eax, 0
    jnc .result
.readonly:
    mov eax, -1
.result:
    xor bx, bx
    mov es, bx
    mov [es:0x7048], eax
    mov byte [es:0x7040], 0
    cli
    xor ax, ax
    mov ds, ax
    o32 lgdt [0x70a0]
    mov eax, [0x7090]
    mov cr4, eax
    mov eax, [0x708c]
    mov cr3, eax
    mov eax, [0x7088]
    and eax, 0x7fffffff
    or eax, 1
    mov cr0, eax
    jmp dword 8:(0x90000+bios_protected)
bits 32
bios_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    cmp word [0x70b4], 0x18
    jne .legacy
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1<<8
    wrmsr
.legacy:
    mov eax, [0x7088]
    mov cr0, eax
    lidt [0x70c0]
    jmp far [0x70b0]
bits 16
video_service:
    mov ax, [es:0x7050]
    mov bx, [es:0x7052]
    mov cx, [es:0x7054]
    mov di, [es:0x7056]
    mov dx, 0x7800
    mov es, dx
    xor dx, dx
    int 0x10
    movzx eax, ax
    jmp bios_service.result
memory_map:
    mov ax, 0x7800
    mov es, ax
    xor di, di
    xor ebx, ebx
    xor bp, bp
.next:
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 24
    mov dword [es:di+20], 1
    int 0x15
    jc .done
    cmp eax, 0x534d4150
    jne .done
    inc bp
    add di, 24
    test ebx, ebx
    jz .done
    cmp bp, 1024
    jb .next
.done:
    movzx eax, bp
    jmp bios_service.result
drive_parameters:
    push ds
    mov ax, 0x7800
    mov ds, ax
    xor si, si
    mov cx, 15
.clear:
    mov word [si], 0
    add si, 2
    loop .clear
    xor si, si
    mov word [si], 30
    mov dl, [es:0x7042]
    mov ah, 0x48
    int 0x13
    pop ds
    mov eax, 0
    jnc bios_service.result
    mov eax, -1
    jmp bios_service.result
align 4
bios_dap: db 16,0
          dw 1,0,0x7800
          dq 0
