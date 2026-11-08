;
; DOS-XC /dos/runtime.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

%include "version.inc"
bits 32
org 0
    jmp transition
    times 8-($-$$) db 0
boot_drive db 0
image_lba dd 0
media_mode db 2
program_name times 128 db 0
program_tail times 128 db 0
    dw files, DOS_FILE_COUNT
cpu_duty db 5
    db 0
    times 280-($-$$) db 0
dos_cwd times 128 db 0
    times 416-($-$$) db 0

transition:
    cli
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    test edi, edi
    jz .legacy
    mov ecx, 0xc0000080
    rdmsr
    and eax, 0xfffffeff
    wrmsr
.legacy:
    jmp 0x20:leave_protected
bits 16
leave_protected:
    mov ax, 0x28
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0xfff0
    mov eax, cr0
    and eax, 0xfffffffe
    mov cr0, eax
    jmp 0x9000:real_mode
real_mode:
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xdff0
    lidt [real_idt]
    cld
    sti
    xor ax, ax
    mov es, ax
    cmp byte [es:0x7040], 1
    je bios_service
    mov ax, 3
    int 0x10
    call init_filesystem
    ; Install DOS vectors, leaving BIOS video, keyboard, timer and sound intact.
    xor ax, ax
    mov es, ax
    mov word [es:0x20*4], terminate20
    mov [es:0x20*4+2], cs
    mov word [es:0x21*4], int21
    mov [es:0x21*4+2], cs
    mov word [es:0x2f*4], int2f
    mov [es:0x2f*4+2], cs
    ; Environment and PSP.
    mov ax, 0x800
    mov es, ax
    xor di, di
    mov si, environment
    mov cx, environment_end-environment
    rep movsb
    mov ax, 0x1000
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 128
    rep stosw
    mov word [es:0], 0x20cd
    mov word [es:2], 0x9000
    mov word [es:0x2c], 0x800
    mov word [es:0x50], 0x21cd
    mov byte [es:0x52], 0xcb
    mov si, program_tail
    mov di, 0x81
    xor cx, cx
.tail:
    lodsb
    test al, al
    jz .tail_done
    cmp cx, 126
    jae .tail_done
    stosb
    inc cx
    jmp .tail
.tail_done:
    mov [es:0x80], cl
    mov al, 13
    stosb
    mov ax, 0x0fff
    mov es, ax
    mov byte [es:0], 'Z'
    mov word [es:1], 0x1000
    mov word [es:3], 0x8000
    mov dx, program_name
    call open_file
    jc missing_program
    mov [program_handle], ax
    mov bx, ax
    mov dx, mz_header
    mov cx, 512
    call read_file
    jc load_error
    cmp word [mz_header], 0x5a4d
    jne load_com
    ; Validate and size the MZ load image separately from appended overlays.
    mov bx, [program_handle]
    call get_handle
    mov edx, [di+18]
    movzx eax, word [mz_header+4]
    test eax, eax
    jz load_error
    shl eax, 9
    movzx ebx, word [mz_header+2]
    cmp ebx, 512
    jae load_error
    test ebx, ebx
    jz .declared
    sub eax, 512
    add eax, ebx
.declared:
    cmp eax, edx
    ja load_error
    movzx edx, word [mz_header+8]
    shl edx, 4
    cmp edx, 32
    jb load_error
    sub eax, edx
    jc load_error
    cmp eax, 0x7ff00
    ja load_error
    mov [image_remaining], eax
    mov [image_size], eax
    movzx ebx, word [mz_header+6]
    shl ebx, 2
    movzx ecx, word [mz_header+24]
    mov [relocation_offset], ecx
    add ebx, ecx
    cmp ebx, edx
    ja load_error
    add eax, 15
    shr eax, 4
    add eax, 16
    mov ecx, eax
    movzx edx, word [mz_header+10]
    add ecx, edx
    cmp ecx, 0x8000
    ja load_error
    movzx edx, word [mz_header+12]
    cmp dx, [mz_header+10]
    jb load_error
    add eax, edx
    cmp eax, 0x8000
    jae .allocation_ready
    mov bx, ax
    mov ax, 0x0fff
    mov es, ax
    mov byte [es:0], 'M'
    mov [es:3], bx
    mov ax, 0x1000
    add ax, bx
    mov es, ax
    mov byte [es:0], 'Z'
    mov word [es:1], 0
    mov dx, 0x8fff
    sub dx, ax
    mov [es:3], dx
    mov ax, 0x1000
    mov es, ax
    add bx, ax
    mov [es:2], bx
.allocation_ready:
    movzx eax, word [mz_header+8]
    shl eax, 4
    mov si, [program_handle]
    shl si, 3
    mov [handles+si+2], eax
    mov ax, 0x1010
    mov ds, ax
    xor dx, dx
.load:
    mov ecx, [cs:image_remaining]
    test ecx, ecx
    jz .relocate
    cmp ecx, 0x8000
    jbe .read_count
    mov cx, 0x8000
.read_count:
    mov bx, [cs:program_handle]
    call read_file
    jc load_error
    cmp ax, cx
    jne load_error
    movzx eax, ax
    sub [cs:image_remaining], eax
    mov bx, ds
    add bx, 0x800
    mov ds, bx
    jmp .load
.relocate:
    push cs
    pop ds
    mov cx, [mz_header+6]
.fixup:
    test cx, cx
    jz .launch
    push cx
    mov si, [program_handle]
    shl si, 3
    mov eax, [relocation_offset]
    mov [handles+si+2], eax
    mov bx, [program_handle]
    mov dx, disk_buffer
    mov cx, 4
    call read_file
    jc load_error
    cmp ax, 4
    jne load_error
    mov bx, [disk_buffer]
    movzx eax, word [disk_buffer+2]
    shl eax, 4
    movzx edx, bx
    add eax, edx
    add eax, 2
    cmp eax, [image_size]
    ja load_error
    mov ax, [disk_buffer+2]
    add ax, 0x1010
    cmp ax, 0x9000
    jae load_error
    mov es, ax
    add word [es:bx], 0x1010
    add dword [relocation_offset], 4
    pop cx
    dec cx
    jmp .fixup
.launch:
    call throttle_start
    mov ax, [mz_header+22]
    add ax, 0x1010
    mov [entry+2], ax
    mov ax, [mz_header+20]
    mov [entry], ax
    mov ax, [mz_header+14]
    add ax, 0x1010
    cli
    mov ss, ax
    mov sp, [mz_header+16]
    mov ax, 0x1000
    mov ds, ax
    mov es, ax
    xor ax, ax
    sti
    jmp far [cs:entry]

load_com:
    mov si, program_name
    xor bx, bx
.extension:
    lodsb
    test al, al
    jz .check_com
    cmp al, '.'
    jne .extension
    mov bx, si
    jmp .extension
.check_com:
    test bx, bx
    jz load_error
    mov al, [bx]
    and al, 0xdf
    cmp al, 'C'
    jne load_error
    mov al, [bx+1]
    and al, 0xdf
    cmp al, 'O'
    jne load_error
    mov al, [bx+2]
    and al, 0xdf
    cmp al, 'M'
    jne load_error
    cmp byte [bx+3], 0
    jne load_error
    mov bx, [program_handle]
    call get_handle
    jc load_error
    cmp dword [cs:di+18], 0xff00
    ja load_error
    mov dword [cs:handles+si+2], 0
    mov ax, 0x1000
    mov ds, ax
    mov dx, 0x100
    mov cx, 0xff00
    call read_file
    jc load_error
    push cs
    pop ds
    call throttle_start
    cli
    mov ax, 0x1000
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xfffe
    mov word [ss:0xfffe], 0
    sti
    jmp 0x1000:0x100
missing_program:
    mov byte [cs:exit_code], 1
    push cs
    pop ds
    mov si, missing_message
    call print
    jmp return_shell
load_error:
    mov byte [cs:exit_code], 1
    push cs
    pop ds
    mov si, load_message
    call print
    jmp return_shell
terminate20:
    xor ax, ax
terminate:
    cmp ah, 0x4c
    je .has_code
    xor al, al
.has_code:
    mov [cs:exit_code], al
    cli
    mov ax, cs
    mov ds, ax
    mov ss, ax
    mov sp, 0xdff0
    sti
return_shell:
    call throttle_stop
    mov ah, 0x0f
    int 0x10
    cmp al, 3
    je .text_mode
    mov ax, 3
    int 0x10
.text_mode:
    xor ax, ax
    mov es, ax
    mov word [es:0x700c], 0xb007
    mov byte [es:0x702c], 1
    mov al, [exit_code]
    mov [es:0x702d], al
    cmp dword [es:0x7010], 'CDVF'
    je .cd
    cmp byte [media_mode], 2
    je .cd
    mov eax, 2048
    mov word [boot_dap+2], 1
    jmp .boot
.cd:
    mov eax, [image_lba]
    mov word [boot_dap+2], 1
.boot:
    mov [boot_dap+8], eax
    mov si, boot_dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .halt
    mov dl, [boot_drive]
    jmp 0:0x7c00
.halt:
    cli
    hlt
    jmp .halt

print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    jmp print
.done:
    ret

open_file:
    push ds
    push es
    push si
    push di
    push bx
    push cx
    cmp byte [cs:media_mode], 1
    jne .legacy_lookup
    call fs_lookup
    jc .not_found
    jmp .descriptor_ready
.legacy_lookup:
    mov si, dx
    mov di, dx
.scan:
    lodsb
    test al, al
    jz .basename
    cmp al, '\'
    je .slash
    cmp al, ':'
    jne .scan
.slash:
    mov di, si
    jmp .scan
.basename:
    mov bx, files
    mov cx, [cs:272]
.next:
    jcxz .not_found
    push cx
    mov si, di
    xor cx, cx
.compare:
    mov al, [si]
    cmp al, 'a'
    jb .upper
    cmp al, 'z'
    ja .upper
    sub al, 32
.upper:
    cmp al, [cs:bx]
    jne .different
    inc si
    inc bx
    inc cx
    test al, al
    jz .found
    cmp cx, 13
    jb .compare
.different:
    sub bx, cx
    add bx, 32
    pop cx
    loop .next
.not_found:
    mov ax, 2
    stc
    jmp .done
.found:
    sub bx, cx
    pop cx
.descriptor_ready:
    mov si, 5*8
.find_handle:
    cmp word [cs:handles+si], 0
    je .allocate
    add si, 8
    cmp si, 16*8
    jb .find_handle
    mov ax, 4
    stc
    jmp .done
.allocate:
    mov [cs:handles+si], bx
    mov dword [cs:handles+si+2], 0
    mov ax, si
    shr ax, 3
    clc
.done:
    pop cx
    pop bx
    pop di
    pop si
    pop es
    pop ds
    ret

get_handle:
    cmp bx, 5
    jb .bad
    cmp bx, 16
    jae .bad
    mov si, bx
    shl si, 3
    mov di, [cs:handles+si]
    test di, di
    jz .bad
    clc
    ret
.bad:
    mov ax, 6
    stc
    ret

read_file:
    push bp
    mov bp, sp
    sub sp, 12
    
    mov [ss:bp-2], ds
    mov [ss:bp-4], dx
    mov [ss:bp-6], cx
    mov word [ss:bp-8], 0
    push bx
    push cx
    push dx
    push ds
    push es
    push si
    push di
    call get_handle
    jc .done
.next:
    movzx ecx, word [ss:bp-6]
    test cx, cx
    jz .success
    mov eax, [cs:di+18]
    sub eax, [cs:handles+si+2]
    jbe .success
    cmp eax, ecx
    jae .bounded
    mov cx, ax
.bounded:
    mov ax, [cs:di+22]
    test ax, ax
    jnz .overlay
    mov eax, [cs:handles+si+2]
    mov dx, ax
    and dx, 2047
    cmp byte [cs:media_mode], 1
    je .fat_address
    shr eax, 11
    add eax, [cs:di+14]
    cmp byte [cs:media_mode], 2
    je .cd_address
    shl eax, 2
.cd_address:
    add eax, [cs:image_lba]
    jmp .address_ready
.fat_address:
    and dx, 511
    call file_cluster_sector
    jc .io_error
.address_ready:
    mov [cs:dap+8], eax
    mov bx, 2048
    cmp byte [cs:media_mode], 1
    jne .block_size
    mov bx, 512
.block_size:
    sub bx, dx
    cmp cx, bx
    jbe .chunk
    mov cx, bx
.chunk:
    pushad
    push ds
    push cs
    pop ds
    cmp byte [media_mode], 1
    je .filesystem_read
    mov word [dap+2], 1
    cmp byte [media_mode], 2
    je .read_disk
    mov word [dap+2], 4
.read_disk:
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jmp .read_done
.filesystem_read:
    mov eax, [dap+8]
    mov bx, disk_buffer
    mov cx, 1
    call fs_read
.read_done:
    pop ds
    popad
    jc .io_error
    mov bx, disk_buffer
    add bx, dx
    jmp .copy
.overlay:
    mov bx, ax
    add bx, [cs:handles+si+2]
.copy:
    movzx eax, cx
    add [cs:handles+si+2], eax
    sub [ss:bp-6], cx
    add [ss:bp-8], cx
    push si
    push di
    mov si, bx
    mov es, [ss:bp-2]
    mov di, [ss:bp-4]
    push cs
    pop ds
    rep movsb
    mov [ss:bp-4], di
    pop di
    pop si
    jmp .next
.io_error:
    mov ax, 30
    stc
    jmp .done
.success:
    mov ax, [ss:bp-8]
    clc
.done:
    pop di
    pop si
    pop es
    pop ds
    pop dx
    pop cx
    pop bx
    mov sp, bp
    pop bp
    ret

int21:
    push bp
    mov bp, sp
    push ds
    push es
    push si
    push di
    cld
    and word [ss:bp+6], 0xfffe
%ifdef DOS_TRACE
    push ax
    mov al, ah
    call trace_byte
    pop ax
%endif
    cmp ah, 0
    je terminate
    cmp ah, 0x4c
    je terminate
    cmp ah, 0x30
    je .version
    cmp ah, 0x09
    je .print_string
    cmp ah, 0x02
    je .putchar
    cmp ah, 0x01
    je .getchar
    cmp ah, 0x07
    je .getchar
    cmp ah, 0x08
    je .getchar
    cmp ah, 0x0b
    je .key_status
    cmp ah, 0x0e
    je .set_drive
    cmp ah, 0x19
    je .get_drive
    cmp ah, 0x1a
    je .set_dta
    cmp ah, 0x2f
    je .get_dta
    cmp ah, 0x25
    je .set_vector
    cmp ah, 0x35
    je .get_vector
    cmp ah, 0x2a
    je .date
    cmp ah, 0x2c
    je .time
    cmp ah, 0x33
    je .break
    cmp ah, 0x36
    je .disk_space
    cmp ah, 0x3b
    je .success
    cmp ah, 0x3c
    je .create
    cmp ah, 0x3d
    je .open
    cmp ah, 0x3e
    je .close
    cmp ah, 0x3f
    je .read
    cmp ah, 0x40
    je .write
    cmp ah, 0x42
    je .seek
    cmp ah, 0x43
    je .attributes
    cmp ah, 0x44
    je .ioctl
    cmp ah, 0x47
    je .cwd
    cmp ah, 0x48
    je .allocate
    cmp ah, 0x49
    je .free
    cmp ah, 0x4a
    je .resize
    cmp ah, 0x4d
    je .success
    cmp ah, 0x51
    je .psp
    cmp ah, 0x62
    je .psp
    cmp ah, 0x57
    je .file_time
    cmp ah, 0x58
    je .strategy
    cmp ah, 0x67
    je .success
    
    mov ax, 1
.error:
    or word [ss:bp+6], 1
    jmp .done
.success:
    xor ax, ax
.done:
    pop di
    pop si
    pop es
    pop ds
    pop bp
    iret
.version:
    mov ax, (DOSXC_DOS_COMPAT_MINOR << 8) | DOSXC_DOS_COMPAT_MAJOR
    xor bx, bx
    xor cx, cx
    jmp .done
.print_string:
    mov si, dx
.print_next:
    lodsb
    cmp al, '$'
    je .done
    push bx
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    pop bx
    jmp .print_next
.putchar:
    mov al, dl
    push bx
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    pop bx
    jmp .done
.getchar:
    xor ah, ah
    int 0x16
    jmp .done
.key_status:
    mov ah, 1
    int 0x16
    mov al, 0
    jz .done
    dec al
    jmp .done
.set_drive:
    mov al, 3
    jmp .done
.get_drive:
    mov al, 2
    jmp .done
.set_dta:
    mov [cs:dta], dx
    mov [cs:dta+2], ds
    jmp .done
.get_dta:
    mov bx, [cs:dta]
    mov ax, [cs:dta+2]
    mov [ss:bp-4], ax
    jmp .done
.set_vector:
    xor ah, ah
    mov si, ax
    shl si, 2
    xor ax, ax
    mov es, ax
    cli
    mov [es:si], dx
    mov [es:si+2], ds
    sti
    cmp si, 0x1b*4
    jne .done
    call crt_patch                       ; Turbo Pascal's CRT unit starts up here
    jmp .done
.get_vector:
    xor ah, ah
    mov si, ax
    shl si, 2
    xor ax, ax
    mov es, ax
    mov bx, [es:si]
    mov ax, [es:si+2]
    mov [ss:bp-4], ax
    jmp .done
.date:
    mov cx, 2026
    mov dx, 0x0101
    mov al, 4
    jmp .done
.time:
    push bx
    xor ax, ax
    int 0x1a
    mov ax, dx
    xor dx, dx
    mov bx, 18
    div bx
    xor dx, dx
    mov bx, 60
    div bx
    mov dh, dl
    xor dl, dl
    xor cx, cx
    mov cl, al
    pop bx
    jmp .done
.break:
    xor dl, dl
    jmp .done
.disk_space:
    mov ax, 1
    mov bx, 16384
    mov cx, 512
    mov dx, 32768
    jmp .done
.open:
    call open_file
    jc .error
    jmp .done
.create:
    call open_file
    jc .error
    push bx
    mov bx, ax
    push ax
    call get_handle
    mov dword [cs:di+18], 0
    pop ax
    pop bx
    jmp .done
.close:
    call get_handle
    jc .error
    mov word [cs:handles+si], 0
    jmp .success
.read:
    cmp bx, 0
    je .stdin
    call read_file
    jc .error
    jmp .done
.stdin:
    xor ax, ax
    int 0x16
    mov si, dx
    mov [si], al
    mov ax, 1
    jmp .done
.write:
    cmp bx, 2
    ja .write_file
    mov si, dx
    push cx
    push bx
    mov bx, 7
.stdout:
    jcxz .stdout_done
    lodsb
    mov ah, 0x0e
    int 0x10
    loop .stdout
.stdout_done:
    pop bx
    pop ax
    jmp .done
.write_file:
    call write_file
    jc .error
    jmp .done
.seek:
    push ax
    call get_handle
    pop ax
    jc .error
    push ax
    movzx eax, cx
    shl eax, 16
    movzx edx, dx
    or edx, eax
    pop ax
    cmp al, 0
    je .seek_set
    cmp al, 1
    je .seek_current
    cmp al, 2
    jne .invalid_seek
    add edx, [cs:di+18]
    jmp .seek_set
.seek_current:
    add edx, [cs:handles+si+2]
.seek_set:
    test edx, edx
    js .invalid_seek
    mov [cs:handles+si+2], edx
    mov ax, dx
    shr edx, 16
    jmp .done
.invalid_seek:
    mov ax, 1
    jmp .error
.attributes:
    cmp al, 0
    jne .success
    call open_file
    jc .error
    push bx
    mov bx, ax
    call get_handle
    mov word [cs:handles+si], 0
    pop bx
    xor cx, cx
    jmp .done
.ioctl:
    cmp al, 0
    jne .success
    xor dx, dx
    cmp bx, 5
    jae .done
    mov dx, 0x80d3
    jmp .done
.cwd:
    mov byte [si], 0
    jmp .success
.psp:
    mov bx, 0x1000
    jmp .done
.file_time:
    xor cx, cx
    mov dx, 0x5c21
    jmp .done
.strategy:
    xor ax, ax
    jmp .done
.allocate:
    mov ax, 0x0fff
    xor dx, dx
.alloc_next:
    mov es, ax
    cmp word [es:1], 0
    jne .alloc_skip
.merge_free:
    cmp byte [es:0], 'Z'
    je .check_capacity
    mov cx, ax
    add cx, [es:3]
    inc cx
    mov ds, cx
    cmp word [1], 0
    jne .check_capacity
    mov cx, [3]
    inc cx
    add [es:3], cx
    mov cl, [0]
    mov [es:0], cl
    jmp .merge_free
.check_capacity:
    cmp dx, [es:3]
    jae .check_request
    mov dx, [es:3]
.check_request:
    cmp [es:3], bx
    jae .alloc_found
.alloc_skip:
    cmp byte [es:0], 'Z'
    je .no_memory
    add ax, [es:3]
    inc ax
    jmp .alloc_next
.alloc_found:
    mov dx, [es:3]
    sub dx, bx
    jz .alloc_whole
    dec dx
    mov cx, ax
    add cx, bx
    inc cx
    mov ds, cx
    mov cl, [es:0]
    mov [0], cl
    mov word [1], 0
    mov [3], dx
    mov byte [es:0], 'M'
    mov [es:3], bx
.alloc_whole:
    mov word [es:1], 0x1000
    inc ax
    jmp .done
.no_memory:
    mov bx, dx
    mov ax, 8
    jmp .error
.free:
    mov ax, es
    dec ax
    mov es, ax
    mov word [es:1], 0
    jmp .success
.resize:
    mov ax, es
    dec ax
    mov es, ax
    mov dx, [es:3]
    cmp bx, dx
    ja .no_memory
    je .success
    mov dx, [es:3]
    sub dx, bx
    dec dx
    mov cx, ax
    add cx, bx
    inc cx
    mov ds, cx
    mov cl, [es:0]
    mov [0], cl
    mov word [1], 0
    mov [3], dx
    mov byte [es:0], 'M'
    mov [es:3], bx
    jmp .success

write_file:
    cmp byte [cs:media_mode], 1
    je fs_write_existing
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push es
    call get_handle
    jc .done
    movzx eax, cx
    add eax, [cs:handles+si+2]
    cmp eax, 16384
    ja .full
    mov bx, [cs:di+22]
    test bx, bx
    jnz .copy
    
    cmp eax, [cs:di+18]
    jae .size
    mov eax, [cs:di+18]
.size:
    add eax, 511
    and eax, 0xfffffe00
    mov bx, [cs:overlay_top]
    add ax, bx
    jc .full
    cmp ax, 0xc000
    ja .full
    mov [cs:overlay_top], ax
    sub ax, bx
    mov [cs:di+24], ax
    
    pushad
    push ds
    mov eax, [cs:handles+si+2]
    push eax
    mov dword [cs:handles+si+2], 0
    mov dx, bx
    mov cx, [cs:di+18]
    mov bx, si
    shr bx, 3
    push cs
    pop ds
    call read_file
    pop eax
    mov [cs:handles+si+2], eax
    pop ds
    popad
    jc .io_error
    mov [cs:di+22], bx
.copy:
    movzx eax, cx
    add eax, [cs:handles+si+2]
    movzx edx, word [cs:di+24]
    cmp eax, edx
    ja .full
    cmp eax, [cs:di+18]
    jbe .not_growing
    mov [cs:di+18], eax
.not_growing:
    mov ax, cx
    add bx, [cs:handles+si+2]
    movzx edx, cx
    add [cs:handles+si+2], edx
    mov di, bx
    mov si, [ss:bp-6]
    push cs
    pop es
    rep movsb
    clc
    jmp .done
.full:
    mov ax, 112
    stc
    jmp .done
.io_error:
    mov ax, 30
    stc
    jmp .done
.done:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

%include "filesystem.asm"
%include "throttle.asm"
%include "crtdelay.asm"
%include "bios.asm"

int2f:
    iret
trace_byte:
    push ax
    shr al, 4
    call .digit
    pop ax
    and al, 15
    call .digit
    mov al, ' '
    out 0xe9, al
    ret
.digit:
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    out 0xe9, al
    ret

align 4
boot_dap: db 16,0
          dw 1,0x7c00,0
          dq 0
real_idt: dw 0x3ff
          dd 0
entry: dd 0
dta: dw 0x80, 0x1000
program_handle dw 0
image_remaining dd 0
image_size dd 0
relocation_offset dd 0
exit_code db 0
overlay_top dw 0x5000
handles: times 16*8 db 0
align 4
dap: db 16, 0
     dw 1, disk_buffer, 0x9000
     dq 0
missing_message db 'Program not found.',13,10,0
load_message db 'DOS executable load failed.',13,10,0

environment db 'COMSPEC=C:\SYSTEM\SHELL.DLL',0,'PATH=C:\SYSTEM\CMD',0,0,1,0,'C:\PROGRAM.EXE',0
environment_end:
%include "dos-files.inc"
%if ($-$$) > 0x3800
%error "DOS runtime overlaps its disk buffer"
%endif
times 0x3800-($-$$) db 0
cd_buffer: times 2048 db 0
disk_buffer: times 2048 db 0
mz_header: times 2048 db 0
; The module ends at 20 KiB; the upper segment is a bounded heap/stack,
; not a disk load buffer. Never overwrite the BIOS EBDA near 0x9fc00.
