;
; DOS-XC /boot/stage2.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
org 0

start:
    cli
    ; EAX is the boot image's absolute CD LBA from stage one.
    mov bx, 0x1000
    mov ds, bx
    mov [image_lba], eax
    mov [boot_drive], dl
    sti
    mov ebx, eax
    add ebx, KERNEL_LBA
    mov [kernel_dap+8], ebx
    mov si, kernel_dap
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov eax, [image_lba]
    add eax, 65
    mov [runtime_dap+8], eax
    mov si, runtime_dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov ax, 0x9000
    mov es, ax
    mov dl, [boot_drive]
    mov [es:8], dl
    mov eax, [image_lba]
    mov [es:9], eax
    mov ebx, eax
    add ebx, CORE_LBA
    mov [core_dap+8], ebx
    mov ebx, eax
    add ebx, SHELL_LBA
    mov [shell_dap+8], ebx
    mov si, core_dap
    call read_module
    mov si, shell_dap
    call read_module
    xor ax, ax
    mov es, ax
    mov dword [es:0x7000], 'BDFS'
    mov byte [es:0x7008], 1
    mov dword [es:0x7010], 'CDVF'
    mov dl, [boot_drive]
    mov [es:0x7014], dl
    mov dword [es:0x7018], 2048
    mov eax, [image_lba]
    mov [es:0x7020], eax
    mov eax, [filesystem_lba]
    mov [es:0x701c], eax
    add eax, 512
    mov [volume_dap+8], eax
    mov si, volume_dap
    call read_module
    xor ax, ax
    mov es, ax
    cmp word [es:0x700c], 0xb007
    je .skip_setup
    mov byte [es:0x7028], 5
    mov byte [es:0x702c], 0
    mov dword [es:0x7060], 0
    mov eax, [es:0x7c00+0x1c0]
    shr eax, 2
    add eax, [es:0x701c]
    mov [setup_dap+8], eax
    mov ax, [es:0x7c00+0x1c4]
    mov [setup_dap+2], ax
    mov si, setup_dap
    call read_module
    mov dl, [boot_drive]
    jmp 0x5000:0
.skip_setup:
    mov word [es:0x700c], 0
    cli
    in al, 0x92
    and al, 0xfe
    or al, 2
    out 0x92, al
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:(0x10000 + protected_mode)

disk_error:
    mov [disk_status], ah
    mov si, disk_message
.print:
    lodsb
    test al, al
    jz .status
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    jmp .print
.status:
    mov al, [disk_status]
    shr al, 4
    call .hex
    mov al, [disk_status]
    and al, 15
    call .hex
.halt:
    cli
    hlt
    jmp .halt
.hex:
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    ret


read_module:
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    ret

bits 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    xor edi, edi               ; BIOS has no firmware boot_info pointer
%ifdef DOS_XC_64
    call enable_long_mode
    jmp 0x18:0x20000
%else
    mov eax, 0x20000
    jmp eax
%endif

%ifdef DOS_XC_64
enable_long_mode:
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov eax, 0x10000 + page_table
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

align 4096
page_table:
    dq 0x10000 + page_directory + 0x03
    times 511 dq 0
page_directory:
    dq 0x10000 + page_table_2 + 0x03
    times 511 dq 0
page_table_2:
%assign i 0
%rep 512
    dd (i * 0x200000) | 0x83
    dd 0
%assign i i + 1
%endrep
%endif

gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
    dq 0x00af9a000000ffff
    dq 0x00009a090000ffff        ; 16-bit transition code at 0x90000
    dq 0x000092000000ffff        ; 16-bit transition data
gdt_descriptor:
    dw $ - gdt - 1
    dd 0x10000 + gdt

align 4
kernel_dap:
    db 0x10, 0
    dw KERNEL_SECTORS
    dw 0
    dw 0x2000
    dq KERNEL_LBA

image_lba dd 0
boot_drive db 0
align 4
runtime_dap:
    db 0x10, 0
    dw DOS_SECTORS, 0, 0x9000
    dq 0
core_dap:
    db 0x10, 0
    dw CORE_SECTORS, 0, 0x3000
    dq CORE_LBA
shell_dap:
    db 0x10, 0
    dw SHELL_SECTORS, 0, 0x6000
    dq SHELL_LBA

volume_dap:
    db 16,0
    dw 1,0x7c00,0
    dq 0
setup_dap:
    db 16,0
    dw 0,0,0x5000
    dq 0

    db 'FSIM'
filesystem_lba dd 0

disk_status db 0
disk_message db 13,10,'DOS-XC: BIOS media read failed, status ',0
