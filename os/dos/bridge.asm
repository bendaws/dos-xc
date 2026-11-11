;
; DOS-XC /os/dos/bridge.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
org 0

BASE equ 0x1000
RSTACK equ 0x2ff0                ; real-mode stack top (offset)
PSTACK equ 0x5ff0                ; protected-mode stack top (linear)

    db 'BRDG'
    dw int13, int16, int33, int08
    times 0x10-($-$$) db 0
old13 dd 0
old16 dd 0
old33 dd 0
entry dd 0
page_root dd 0
mode_bits dd 0
cr4_value dd 0
drives times 8 db 0xff
busy db 0
live_drive db 0xff
runtime_active db 1
    times 0x38-($-$$) db 0
r_op  dd 0
r_eax dd 0
r_ebx dd 0
r_ecx dd 0
r_edx dd 0
r_esi dd 0
r_edi dd 0
r_ebp dd 0
r_ds  dw 0
r_es  dw 0
r_flags dw 0
    dw 0
r_result dd 0
saved_ss dw 0
saved_sp dw 0
    times 0x80-($-$$) db 0
gdt:
    dq 0
    dq 0x00cf9a000000ffff        ; 0x08 code 32, flat
    dq 0x00cf92000000ffff        ; 0x10 data 32, flat
    dq 0x00af9a000000ffff        ; 0x18 code 64
    dq 0x00009a001000ffff        ; 0x20 code 16, base 0x1000
    dq 0x000092000000ffff        ; 0x28 data 16
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd BASE + gdt
real_idtr:
    dw 0x3ff
    dd 0

; Store the caller's registers in the request block (op in [r_op] already).
save_registers:
    mov [cs:r_eax], eax
    mov [cs:r_ebx], ebx
    mov [cs:r_ecx], ecx
    mov [cs:r_edx], edx
    mov [cs:r_esi], esi
    mov [cs:r_edi], edi
    mov [cs:r_ebp], ebp
    mov [cs:r_ds], ds
    mov [cs:r_es], es
    ret

load_registers:
    mov eax, [cs:r_eax]
    mov ebx, [cs:r_ebx]
    mov ecx, [cs:r_ecx]
    mov edx, [cs:r_edx]
    mov esi, [cs:r_esi]
    mov edi, [cs:r_edi]
    mov ebp, [cs:r_ebp]
    mov es, [cs:r_es]
    ret

; Call the runtime.  Preserves the real-mode stack and segment registers.
call_runtime:
    pushf
    cli
    push ds
    push es
    push fs
    push gs
    pushad
    mov [cs:saved_ss], ss
    mov [cs:saved_sp], sp
    mov ax, cs
    mov ss, ax
    mov sp, RSTACK
    in al, 0x92                  ; A20 on
    or al, 2
    and al, 0xfe
    out 0x92, al
    o32 lgdt [cs:gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:(BASE + protected)
bits 32
protected:
    cld
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, PSTACK
    cmp dword [BASE + mode_bits], 64
    je .long
    mov eax, [BASE + entry]
    call eax
    jmp leave_protected
.long:
    mov eax, [BASE + cr4_value]
    or eax, 1 << 5
    mov cr4, eax
    mov eax, [BASE + page_root]
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    jmp 0x18:(BASE + long_mode)
bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, PSTACK
    mov eax, [abs BASE + entry]
    call rax
    push 0x08
    mov eax, BASE + compatibility
    push rax
    retfq
bits 32
compatibility:
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    mov ecx, 0xc0000080
    rdmsr
    and eax, ~(1 << 8)
    wrmsr
leave_protected:
    jmp 0x20:protected16
bits 16
protected16:
    mov ax, 0x28
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and eax, 0x7ffffffe
    mov cr0, eax
    jmp 0x0100:real_again
real_again:
    o32 lidt [cs:real_idtr]
    mov ss, [cs:saved_ss]
    mov sp, [cs:saved_sp]
    popad
    pop gs
    pop fs
    pop es
    pop ds
    popf
    ret

; Set or clear CF in the interrupt frame from r_flags, then IRET.
; Entered with the caller's stack: [sp] = ip, cs, flags.
return_flags:
    push bp
    mov bp, sp
    and word [bp+6], 0xfffe
    test word [cs:r_flags], 1
    jz .clear
    or word [bp+6], 1
.clear:
    pop bp
    iret

; ---- INT 13h ----------------------------------------------------------------

int13:
    cmp byte [cs:busy], 0
    jne .chain
    cmp dl, [cs:live_drive]
    jne .usb
    cmp byte [cs:runtime_active], 0
    je live13
.usb:
    push bx
    xor bx, bx
.find:
    cmp dl, [cs:drives+bx]
    je .ours
    inc bx
    cmp bx, 8
    jb .find
    pop bx
.chain:
    jmp far [cs:old13]
.ours:
    pop bx
    mov byte [cs:busy], 1
    mov dword [cs:r_op], 1
    call save_registers
    call call_runtime
    call load_registers
    mov byte [cs:busy], 0
    jmp return_flags

; The live CD while 16-bit code runs: reads get the RAM overlay patched in,
; marked image-sector writes go to it.
live13:
    cmp ah, 0x42
    je .read
    cmp ah, 0x43
    jne int13.chain
    push ds
    push si
    cmp dword [ds:si+12], 0x40000000     ; bit 62 of the LBA: image sectors
    pop si
    pop ds
    jne int13.chain
    mov byte [cs:busy], 1
    mov dword [cs:r_op], 5
    call save_registers
    call call_runtime
    call load_registers
    mov byte [cs:busy], 0
    jmp return_flags
.read:
    mov byte [cs:busy], 1
    push bx
    xor bx, bx
.mine:
    cmp dl, [cs:drives+bx]
    je .native
    inc bx
    cmp bx, 8
    jb .mine
    pop bx
    pushf
    call far [cs:old13]
    jmp .overlay
.native:                         ; the runtime drives the CD itself (storage.c)
    pop bx
    mov dword [cs:r_op], 1
    call save_registers
    call call_runtime
    call load_registers
    test word [cs:r_flags], 1
    jz .read_ok
    stc
    jmp .overlay
.read_ok:
    clc
.overlay:
    mov dword [cs:r_op], 4
    call save_registers
    pushf
    pop word [cs:r_flags]
    jc .failed
    call call_runtime
.failed:
    call load_registers
    mov byte [cs:busy], 0
    jmp return_flags

; Move USB keys into the BIOS buffer (registers preserved).
pump:
    push eax
    mov dword [cs:r_op], 2
    call call_runtime
    pop eax
    ret

int16:
    cmp byte [cs:busy], 0
    jne .chain
    mov byte [cs:busy], 1
    cmp ah, 0x00
    je .wait
    cmp ah, 0x10
    je .wait
    cmp ah, 0x01
    je .pump_chain
    cmp ah, 0x11
    je .pump_chain
    cmp ah, 0x02
    je .pump_chain
    cmp ah, 0x12
    je .pump_chain
    mov byte [cs:busy], 0
.chain:
    jmp far [cs:old16]
.pump_chain:
    call pump
    mov byte [cs:busy], 0
    jmp far [cs:old16]
.wait:
    push ax
.again:
    call pump
    pop ax
    push ax
    inc ah                       ; 00 -> 01, 10 -> 11: is a key there?
    pushf
    call far [cs:old16]
    jnz .key
    sti
    hlt                          ; the timer tick wakes us
    jmp .again
.key:
    pop ax
    mov byte [cs:busy], 0
    jmp far [cs:old16]

; ---- INT 33h ----------------------------------------------------------------

int33:
    cmp byte [cs:busy], 0
    jne .chain
    mov byte [cs:busy], 1
    mov dword [cs:r_op], 3
    call save_registers
    call call_runtime
    cmp dword [cs:r_result], 1
    je .pass
    call load_registers
    mov byte [cs:busy], 0
    iret
.pass:
    mov byte [cs:busy], 0
.chain:
    cmp dword [cs:old33], 0
    je .none
    jmp far [cs:old33]
.none:
    iret

; ---- INT 08h ----------------------------------------------------------------

int08:
    push ax
    push ds
    xor ax, ax
    mov ds, ax
    add word [0x46c], 1          ; ticks since midnight
    adc word [0x46e], 0
    cmp word [0x46e], 0x18
    jb .counted
    cmp word [0x46c], 0xb0
    jb .counted
    mov word [0x46c], 0
    mov word [0x46e], 0
    mov byte [0x470], 1          ; midnight passed
.counted:
    cmp byte [0x440], 0          ; floppy motor timeout
    je .motor
    dec byte [0x440]
    jnz .motor
    and byte [0x43f], 0xf0
    push dx
    mov dx, 0x3f2
    mov al, 0x0c
    out dx, al
    pop dx
.motor:
    int 0x1c                     ; the user timer hook
    mov al, 0x20
    out 0x20, al
    pop ds
    pop ax
    iret

    times 0x1000-($-$$) db 0     ; the image is one page; stacks follow
