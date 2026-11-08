;
; DOS-XC /boot/fsloader.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
org 0
start:
    cli
    mov ax, cs
    mov ds, ax
    mov ss, ax
    mov sp, 0xfff0
    cld
    sti
    mov [drive], dl
    xor ax, ax
    mov es, ax
    mov ax, [es:0x700c]
    mov [skip_setup], ax
    cmp ax, 0xb007
    je .keep_speed
    mov byte [es:0x7028], 5
    mov byte [es:0x702c], 0
    mov dword [es:0x7060], 0
.keep_speed:
    mov word [es:0x700c], 0
    mov eax, [es:0x7c00+28]
    mov [partition], eax
    cmp word [es:0x7c00+11], 512
    jne error
    mov al, [es:0x7c00+13]
    mov [sectors_per_cluster], al
    mov eax, [partition]
    movzx ebx, word [es:0x7c00+14]
    add eax, ebx
    mov [fat_lba], eax
    movzx ebx, word [es:0x7c00+22]
    mov byte [fat_type], 16
    mov dword [root_cluster], 0
    test ebx, ebx
    jnz .fat_size
    mov byte [fat_type], 32
    mov ebx, [es:0x7c00+36]
    mov ecx, [es:0x7c00+44]
    mov [root_cluster], ecx
.fat_size:
    movzx ecx, byte [es:0x7c00+16]
    imul ebx, ecx
    add eax, ebx
    mov [root_lba], eax
    movzx ebx, word [es:0x7c00+17]
    add ebx, 15
    shr ebx, 4
    mov [root_sectors], ebx
    add eax, ebx
    mov [data_lba], eax
    mov eax, [es:0x7c00+39]
    cmp byte [fat_type], 32
    jne .serial
    mov eax, [es:0x7c00+67]
.serial:
    mov dword [es:0x7000], 'BDFS'
    mov [es:0x7004], eax
    mov al, [es:0x7c00+0x1aa]
    mov [es:0x7008], al
    mov si, system_name
    xor eax, eax
    call find_name
    mov eax, [cluster]
    mov [system_cluster], eax
boot_system:
    mov si, kernel_name
    mov ax, 0x2000
    call load_file
    mov si, core_name
    mov ax, 0x3000
    call load_file
    mov ax, 0x3000
    call validate_module
    mov si, shell_name
    mov ax, 0x6000
    call load_file
    mov ax, 0x6000
    call validate_module
    mov si, dos_name
    mov ax, 0x9000
    call load_file
    mov ax, 0x9000
    mov es, ax
    mov dl, [drive]
    mov [es:8], dl
    xor ax, ax
    mov es, ax
    mov eax, [es:0x7c00+0x1b8]
    sub eax, 97*4
    mov bx, 0x9000
    mov es, bx
    mov [es:9], eax
    mov byte [es:13], 0
    ; Filesystem boot information for the integrated DOS services.
    xor ax, ax
    mov es, ax
    mov dword [es:0x7010], 'FATB'
    mov al, [drive]
    mov [es:0x7014], al
    mov eax, [partition]
    mov [es:0x7018], eax
    cli
    in al, 0x92
    and al, 0xfe
    or al, 2
    out 0x92, al
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 8:(0x10000+protected_mode)

validate_module:
    mov es, ax
    cmp dword [es:0], 'BDLL'
    jne error
    cmp dword [es:4], 1
    jne error
%ifdef DOS_XC_64
    cmp dword [es:8], 64
%else
    cmp dword [es:8], 32
%endif
    jne error
    ret

; EAX=LBA, CX=count, ES:BX=destination.
read:
    pushad
    mov [dap+8], eax
    mov [dap+2], cx
    mov [dap+4], bx
    mov [dap+6], es
    mov si, dap
    mov dl, [drive]
    mov ah, 0x42
    int 0x13
    jc error
    popad
    ret

; Find an 8.3 entry by traversing FAT16/FAT32 directory chains.
; EAX=directory cluster, or zero for the root. SI=name.
find_name:
    mov [wanted], si
    mov [search_cluster], eax
    mov dword [chain_limit], 65536
    test eax, eax
    jnz .cluster
    cmp byte [fat_type], 32
    je .fat32_root
    mov eax, [root_lba]
    mov [search_lba], eax
    mov eax, [root_sectors]
    mov [search_left], eax
    jmp .next_sector
.fat32_root:
    mov eax, [root_cluster]
    mov [search_cluster], eax
.cluster:
    mov eax, [search_cluster]
    call cluster_sector
    mov [search_lba], eax
    movzx eax, byte [sectors_per_cluster]
    mov [search_left], eax
.next_sector:
    dec dword [chain_limit]
    jz error
    xor ax, ax
    mov es, ax
    mov bx, 0x8000
    mov eax, [search_lba]
    mov cx, 1
    call read
    mov di, 0x8000
.next_entry:
    cmp byte [es:di], 0
    je error
    cmp byte [es:di+11], 15
    je .skip
    mov si, [wanted]
    push di
    mov cx, 11
    repe cmpsb
    pop di
    je .found
.skip:
    add di, 32
    cmp di, 0x8200
    jb .next_entry
    inc dword [search_lba]
    dec dword [search_left]
    jnz .next_sector
    mov eax, [search_cluster]
    test eax, eax
    jz error
    call next_cluster
    mov [search_cluster], eax
    jmp .cluster
.found:
    xor eax, eax
    cmp byte [fat_type], 32
    jne .low_cluster
    mov ax, [es:di+20]
    shl eax, 16
.low_cluster:
    mov ax, [es:di+26]
    mov [cluster], eax
    mov eax, [es:di+28]
    mov [file_size], eax
    ret
cluster_sector:
    cmp eax, 2
    jb error
    sub eax, 2
    movzx ebx, byte [sectors_per_cluster]
    imul eax, ebx
    add eax, [data_lba]
    ret
next_cluster:
    mov ebx, eax
    cmp byte [fat_type], 32
    je .fat32
    cmp eax, 0xfff8
    jae error
    and bx, 255
    shl bx, 1
    shr eax, 8
    jmp .sector
.fat32:
    cmp eax, 0x0ffffff8
    jae error
    and bx, 127
    shl bx, 2
    shr eax, 7
.sector:
    add eax, [fat_lba]
    push bx
    xor bx, bx
    mov es, bx
    mov bx, 0x8000
    mov cx, 1
    call read
    pop bx
    cmp byte [fat_type], 32
    je .entry32
    movzx eax, word [es:bx+0x8000]
    cmp eax, 0xfff8
    jae error
    ret
.entry32:
    mov eax, [es:bx+0x8000]
    and eax, 0x0fffffff
    cmp eax, 0x0ffffff8
    jae error
    ret
load_file:
    mov [destination], ax
    mov eax, [system_cluster]
    call find_name
    mov eax, [file_size]
    cmp eax, 65536
    ja error
    cmp word [destination], 0x9000
    jne .size_ok
    cmp eax, 0x6000
    ja error
.size_ok:
    mov [file_left], eax
.next_cluster:
    cmp dword [file_left], 0
    je .done
    mov eax, [cluster]
    call cluster_sector
    mov [file_lba], eax
    movzx ax, byte [sectors_per_cluster]
    mov [file_sectors], ax
.next_sector:
    mov es, [destination]
    xor bx, bx
    mov eax, [file_lba]
    mov cx, 1
    call read
    add word [destination], 32
    cmp dword [file_left], 512
    jbe .done
    sub dword [file_left], 512
    inc dword [file_lba]
    dec word [file_sectors]
    jnz .next_sector
    mov eax, [cluster]
    call next_cluster
    mov [cluster], eax
    jmp .next_cluster
.done:
    ret

error:
    xor ax, ax
    mov es, ax
    mov si, error_text
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

bits 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    xor edi, edi
%ifdef DOS_XC_64
    mov eax, cr4
    or eax, 1<<5
    mov cr4, eax
    mov eax, 0x10000+pml4
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1<<8
    wrmsr
    mov eax, cr0
    or eax, 1<<31
    mov cr0, eax
    jmp 0x18:0x20000
%else
    mov eax, 0x20000
    jmp eax
%endif
bits 16
align 8
gdt:
    dq 0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff
    dq 0x00009a090000ffff,0x000092000000ffff
gdt_descriptor: dw $-gdt-1
                dd 0x10000+gdt
align 4
dap: db 16,0
     dw 0,0,0
     dq 0
drive db 0
skip_setup dw 0
partition dd 0
fat_lba dd 0
root_lba dd 0
data_lba dd 0
system_cluster dd 0
root_cluster dd 0
root_sectors dd 0
fat_type db 16
sectors_per_cluster db 4
search_cluster dd 0
file_left dd 0
file_lba dd 0
file_sectors dw 0
wanted dw 0
search_lba dd 0
search_left dd 0
cluster dd 0
file_size dd 0
destination dw 0
chain_limit dd 0
system_name db 'SYSTEM     '
kernel_name db 'KERNEL  BIN'
core_name db 'CORE    DLL'
shell_name db 'SHELL   DLL'
dos_name db 'DOSAPI  DLL'
error_text db 13,10,'DOS-XC: missing, invalid or unreadable SYSTEM component.',0
%ifdef DOS_XC_64
align 4096
pml4: dq 0x10000+pdpt+3
      times 511 dq 0
pdpt: dq 0x10000+pd+3
      times 511 dq 0
pd:
%assign i 0
%rep 512
    dq (i*0x200000)|0x83
%assign i i+1
%endrep
%endif
