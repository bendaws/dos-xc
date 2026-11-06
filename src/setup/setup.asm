;
; BenDOS /setup/setup.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

%include "version.inc"
bits 16
org 0
start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xfff0
    cld
    sti
    mov [source], dl
    mov ax, 3
    int 0x10
    mov ah, 1
    mov cx, 0x2607
    int 0x10
welcome:
    call screen
    mov si, welcome_text
    call print
    call keys
    cmp ah, 0x3b
    je help
    cmp ah, 0x3d
    je leave
    cmp al, 13
    jne welcome
    call scan_disks
select_disk:
    call screen
    mov si, selection_text
    call print
    cmp byte [disk_count], 0
    je no_disks
    xor bx, bx
.disk:
    mov ax, bx
    add ax, 9
    mov dx, 160
    mul dx
    add ax, 12
    mov [cursor], ax
    mov al, ' '
    cmp bl, [selected]
    jne .not_selected
    mov al, '>'
.not_selected:
    call putc
    mov al, bl
    add al, '1'
    call putc
    mov si, disk_label
    call print
    movzx si, bl
    mov al, [disks+si]
    call hex
    mov si, size_label
    call print
    mov si, bx
    shl si, 2
    mov eax, [capacities+si]
    shr eax, 11
    call decimal
    mov si, mib_text
    call print
    inc bl
    cmp bl, [disk_count]
    jb .disk
.wait:
    call keys
    cmp ah, 0x3d
    je leave
    cmp ah, 0x3b
    je help
    cmp ah, 0x48
    je .up
    cmp ah, 0x50
    je .down
    cmp al, 13
    je confirmation
    sub al, '1'
    cmp al, [disk_count]
    jae .wait
    mov [selected], al
    jmp select_disk
.up:
    cmp byte [selected], 0
    je .wait
    dec byte [selected]
    jmp select_disk
.down:
    mov al, [selected]
    inc al
    cmp al, [disk_count]
    jae .wait
    mov [selected], al
    jmp select_disk
no_disks:
    mov si, no_disk_text
    call print
    call keys
    cmp ah, 0x3d
    je leave
    jmp welcome
confirmation:
    movzx bx, byte [selected]
    mov al, [disks+bx]
    mov [target], al
    call screen
    mov si, confirm_text
    call print
    mov al, [target]
    call hex
    mov si, confirm_tail
    call print
    mov byte [typed], 0
.type:
    call keys
    cmp ah, 0x3d
    je leave
    cmp al, 27
    je select_disk
    cmp al, 8
    je .backspace
    cmp al, 13
    je .validate
    cmp al, 'a'
    jb .upper
    cmp al, 'z'
    ja .upper
    sub al, 32
.upper:
    cmp al, 'A'
    jb .type
    cmp al, 'Z'
    ja .type
    movzx bx, byte [typed]
    cmp bx, 5
    jae .type
    mov [answer+bx], al
    inc byte [typed]
    call putc
    jmp .type
.backspace:
    cmp byte [typed], 0
    je .type
    dec byte [typed]
    sub word [cursor], 2
    mov al, ' '
    call putc
    sub word [cursor], 2
    jmp .type
.validate:
    cmp byte [typed], 5
    jne .type
    mov si, answer
    mov di, erase_word
    mov cx, 5
.compare:
    mov al, [si]
    cmp al, [di]
    jne confirmation
    inc si
    inc di
    loop .compare
    call install
    jmp complete

scan_disks:
    mov byte [disk_count], 0
    mov byte [selected], 0
    mov dl, 0x80
.next:
    cmp dl, [source]
    je .skip
    push dx
    mov word [edd], 30
    mov si, edd
    mov ah, 0x48
    int 0x13
    pop dx
    jc .skip
    cmp word [edd+24], 512
    jne .skip
    test word [edd+2], 4       ; removable media is never an installation target
    jnz .skip
    cmp dword [edd+20], 0
    jne .skip                ; BIOS version supports disks below 2 TiB
    cmp dword [edd+16], 131072
    jb .skip
    movzx bx, byte [disk_count]
    mov [disks+bx], dl
    shl bx, 2
    mov eax, [edd+16]
    mov [capacities+bx], eax
    inc byte [disk_count]
    cmp byte [disk_count], 8
    jae .done
.skip:
    inc dl
    cmp dl, 0x90
    jb .next
.done:
    ret

install:
    ; The native installer can format FAT32 volumes without 16-bit buffer limits.
    ; This request is written only after the user typed ERASE for this target.
    xor ax, ax
    mov es, ax
    mov dword [es:0x7060], 'INST'
    mov al, [target]
    mov [es:0x7064], al
    movzx si, byte [selected]
    shl si, 2
    mov eax, [capacities+si]
    mov [es:0x7068], eax
    jmp leave
complete:
    call screen
    mov si, complete_text
    call print
    call keys
    cmp al, 13
    jne complete
    int 0x19
    jmp $
disk_error:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, 0xfff0
    sti
    push cs
    pop ds
    call screen
    mov si, failure_text
    call print
    call keys
    jmp welcome
help:
    call screen
    mov si, help_text
    call print
    call keys
    jmp welcome
leave:
    xor ax, ax
    mov es, ax
    mov word [es:0x700c], 0xb007
    mov ax, 3
    int 0x10
    mov dl, [source]
    xor ax, ax
    mov es, ax
    mov eax, [es:0x7020]
    jmp 0x1000:0

screen:
    push ax
    push cx
    push di
    mov ax, 0xb800
    mov es, ax
    xor di, di
    mov ax, 0x1720
    mov cx, 80*24
    rep stosw
    mov ax, 0x7020
    mov cx, 80
    rep stosw
    mov word [cursor], 162
    mov si, title
    call print
    mov word [cursor], 320
    mov si, underline
    call print
    mov word [cursor], 24*160+4
    mov byte [attribute], 0x70
    mov si, footer
    call print
    mov byte [attribute], 0x1f
    mov word [cursor], (4*80+6)*2
    pop di
    pop cx
    pop ax
    ret
print:
    lodsb
    test al, al
    jz .done
    cmp al, 10
    jne .character
    push ax
    push dx
    mov ax, [cursor]
    xor dx, dx
    mov di, 160
    div di
    inc ax
    mul di
    add ax, 12
    mov [cursor], ax
    pop dx
    pop ax
    jmp print
.character:
    call putc
    jmp print
.done:
    ret
putc:
    push ax
    push di
    push es
    mov di, [cursor]
    cmp di, 25*160
    jae .done
    mov ah, [attribute]
    mov dx, 0xb800
    mov es, dx
    mov [es:di], ax
    add word [cursor], 2
.done:
    pop es
    pop di
    pop ax
    ret
keys:
    xor ax, ax
    int 0x16
    ret
hex:
    push ax
    shr al, 4
    call .digit
    pop ax
    and al, 15
.digit:
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    call putc
    ret
decimal:
    pushad
    xor cx, cx
    mov ebx, 10
.next:
    xor edx, edx
    div ebx
    push dx
    inc cx
    test eax, eax
    jnz .next
.emit:
    pop ax
    add al, '0'
    call putc
    loop .emit
    popad
    ret

source db 0
target db 0
disk_count db 0
selected db 0
operation db 0
attribute db 0x1f
typed db 0
answer times 5 db 0
erase_word db 'ERASE'
disks times 8 db 0
capacities times 8 dd 0
cursor dw 0
count dw 0
position dd 0
copy_end dd 0
serial dd 0
align 4
edd times 30 db 0
dap: db 16,0
     dw 0,0,0
     dq 0
title db "Ben's DOS v", BENDOS_VERSION, " Setup",0
underline db '================',0
footer db 'ENTER=Continue  F1=Help  F3=Exit',0
welcome_text db 'Welcome to Setup.',10,10,"Setup will help you install Ben's DOS onto your system.",10,10,10,'For help, press F1.',10,10,'To begin Setup, press ENTER.',10,10,'To leave Setup without installing, press F3.',0
selection_text db 'Choose the internal drive for your installation.',10,10,'Use the arrow keys or a number, then press ENTER.',10,'The external setup drive is excluded.',0
disk_label db '. BIOS drive ',0
size_label db 'h    ',0
mib_text db ' MiB',0
no_disk_text db 10,10,'No eligible internal disk was found.',10,'Attach a writable 512-byte-sector disk of at least 64 MiB.',10,10,'Press F3 to leave Setup.',0
confirm_text db 'Confirm installation',10,10,'Selected destination: BIOS drive ',0
confirm_tail db 'h',10,10,'WARNING: All existing partitions on this drive will be lost.',10,'Setup replaces its partition table and creates a',10,'FAT32 system partition using the full disk.',10,10,'Type ERASE and press ENTER to install.',10,'Press ESC to choose another drive, or F3 to exit.',10,10,'Confirmation: ',0
installing_text db "Installing Ben's DOS",10,10,"Setup is copying Ben's DOS to your computer's storage.",10,10,'Do not turn off your computer.',0
percent_text db '% complete   ',0
complete_text db "Setup has finished installing Ben's DOS.",10,10,'Remove the external setup drive, then press ENTER',10,'to restart your computer from the internal drive.',0
failure_text db 'Setup could not read, write, or verify the selected drive.',10,10,'Installation is incomplete. Do not boot this disk.',10,'Check the drive and connections, then try again.',10,10,'Press any key to return to Setup.',0
help_text db 'Setup Help',10,10,'ENTER continues to the next screen.',10,'F3 leaves Setup without starting an installation.',10,'On the drive screen, use arrows or a number to choose.',10,10,'Setup supports BIOS and x64 UEFI boot on 512-byte disks.',10,'The selected disk needs at least 64 MiB of space.',10,'Installation replaces all its existing partitions.',10,10,'Press any key to return to the welcome screen.',0
