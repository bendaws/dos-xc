;
; DOS-XC /dos/filesystem.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
init_filesystem:
    xor ax, ax
    mov es, ax
    cmp dword [es:0x7010], 'FATB'
    je .fat
    cmp dword [es:0x7010], 'CDVF'
    jne .done
.fat:
    mov byte [media_mode], 1
    mov eax, [es:0x7018]
    mov [fs_partition], eax
    mov bx, disk_buffer
    mov cx, 1
    call fs_read
    jc load_error
    cmp word [disk_buffer+11], 512
    jne load_error
    mov al, [disk_buffer+13]
    mov [fs_spc], al
    mov cl, 9
.shift:
    cmp al, 1
    je .shift_done
    test al, 1
    jnz load_error
    shr al, 1
    inc cl
    cmp cl, 16
    ja load_error
    jmp .shift
.shift_done:
    mov [fs_cluster_shift], cl
    movzx eax, word [disk_buffer+14]
    add eax, [fs_partition]
    mov [fs_fat], eax
    movzx ebx, word [disk_buffer+22]
    mov byte [fs_type], 16
    mov dword [fs_root_cluster], 0
    test ebx, ebx
    jnz .fat_size
    mov byte [fs_type], 32
    mov ebx, [disk_buffer+36]
    mov ecx, [disk_buffer+44]
    mov [fs_root_cluster], ecx
.fat_size:
    movzx ecx, byte [disk_buffer+16]
    imul ebx, ecx
    add eax, ebx
    mov [fs_root], eax
    movzx ebx, word [disk_buffer+17]
    add ebx, 15
    shr ebx, 4
    mov [fs_root_sectors], ebx
    add eax, ebx
    mov [fs_data], eax
    mov word [272], 0
    mov di, files
    mov cx, 64*16
    xor ax, ax
    push cs
    pop es
    rep stosw

.done:
    ret

; EAX=logical 512-byte LBA, CX=count (1 or 4), CS:BX=destination.
fs_read:
    pushad
    push ds
    push es
    push cs
    pop ds
    mov [fs_dap+8], eax
    mov [fs_dap+2], cx
    mov [fs_dap+4], bx
    mov [fs_destination], bx
    mov bx, ax
    and bx, 3
    shl bx, 9
    mov [fs_subsector], bx
    xor bx, bx
    mov es, bx
    cmp dword [es:0x7010], 'CDVF'
    jne .disk
    shr eax, 2
    add eax, [es:0x701c]
    mov [fs_dap+8], eax
    mov word [fs_dap+2], 1
    cmp cx, 4
    je .disk
    mov word [fs_dap+4], cd_buffer
.disk:
    mov si, fs_dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .done
    xor ax, ax
    mov es, ax
    cmp dword [es:0x7010], 'CDVF'
    jne .success
    cmp word [fs_dap+4], cd_buffer
    jne .success
    push cs
    pop es
    mov si, cd_buffer
    add si, [fs_subsector]
    mov di, [fs_destination]
    mov cx, 256
    rep movsw
.success:
    clc
.done:
    pop es
    pop ds
    popad
    ret
; Input DI=file descriptor, EAX=position. Return physical cluster sector in EAX.
file_cluster_sector:
    push ebx
    push ecx
    push edx
    push esi
    push ds
    push cs
    pop ds
    mov edx, eax
    shr edx, 9
    movzx ecx, byte [fs_spc]
    dec ecx
    and edx, ecx
    mov cl, [fs_cluster_shift]
    shr eax, cl
    mov esi, eax
    movzx eax, word [di+16]
    shl eax, 16
    mov ax, [di+26]
.next:
    cmp eax, 2
    jb .bad
    test esi, esi
    jz .found
    call fs_next_cluster
    jc .bad
    dec esi
    jmp .next
.found:
    sub eax, 2
    movzx ebx, byte [fs_spc]
    imul eax, ebx
    add eax, [fs_data]
    add eax, edx
    clc
    jmp .done
.bad:
    stc
.done:
    pop ds
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret
fs_next_cluster:
    push ebx
    push ecx
    push edx
    mov ebx, eax
    cmp byte [fs_type], 32
    je .fat32
    and bx, 255
    shl bx, 1
    shr eax, 8
    jmp .sector
.fat32:
    and bx, 127
    shl bx, 2
    shr eax, 7
.sector:
    mov dx, bx
    add eax, [fs_fat]
    mov bx, disk_buffer
    mov cx, 1
    call fs_read
    jc .done
    mov bx, dx
    cmp byte [fs_type], 32
    je .entry32
    movzx eax, word [disk_buffer+bx]
    cmp eax, 0xfff8
    jae .bad
    jmp .valid
.entry32:
    mov eax, [disk_buffer+bx]
    and eax, 0x0fffffff
    cmp eax, 0x0ffffff8
    jae .bad
.valid:
    cmp eax, 2
    jb .bad
    clc
    jmp .done
.bad:
    stc
.done:
    pop edx
    pop ecx
    pop ebx
    ret
align 4
fs_dap: db 16,0
        dw 0,0,0x9000
        dq 0
fs_partition dd 0
fs_fat dd 0
fs_root dd 0
fs_data dd 0
fs_index dd 0
fs_root_cluster dd 0
fs_root_sectors dd 0
fs_spc db 4
fs_cluster_shift db 11
fs_type db 16
fs_destination dw 0
fs_subsector dw 0

; Resolve an ordinary DOS 8.3 path on FAT16. BX returns the cached descriptor.
fs_lookup:
    push ds
    push es
    push si
    push di
    push cx
    push dx
    mov si, dx
    mov di, fs_path
    push cs
    pop es
    cmp byte [si+1], ':'
    jne .drive_done
    add si, 2
.drive_done:
    cmp byte [si], '\'
    je .absolute
    cmp byte [si], '/'
    je .absolute
    push si
    push ds
    push cs
    pop ds
    mov si, dos_cwd
.cwd:
    lodsb
    test al, al
    jz .cwd_done
    stosb
    jmp .cwd
.cwd_done:
    mov al, '\'
    stosb
    pop ds
    pop si
.absolute:
    mov cx, 255
.path:
    lodsb
    stosb
    test al, al
    jz .parse
    loop .path
    jmp .bad
.parse:
    push cs
    pop ds
    mov dword [fs_directory], 0
    mov si, fs_path
.component:
    cmp byte [si], '\'
    je .separator
    cmp byte [si], '/'
    jne .name
.separator:
    inc si
    jmp .component
.name:
    cmp byte [si], 0
    je .bad
    mov di, fs_short
    mov cx, 11
    mov al, ' '
    rep stosb
    mov di, fs_short
    xor bx, bx
.character:
    lodsb
    test al, al
    jz .search
    cmp al, '\'
    je .search
    cmp al, '/'
    je .search
    cmp al, '.'
    jne .letter
    cmp bx, 0
    jne .extension
    mov byte [fs_short], '.'
    cmp byte [si], '.'
    jne .dot_done
    inc si
    mov byte [fs_short+1], '.'
.dot_done:
    cmp byte [si], '\'
    je .dot_end
    cmp byte [si], '/'
    je .dot_end
    cmp byte [si], 0
    jne .bad
.dot_end:
    lodsb
    jmp .search
.extension:
    mov bx, 8
    jmp .character
.letter:
    cmp al, 'a'
    jb .uppercase
    cmp al, 'z'
    ja .uppercase
    sub al, 32
.uppercase:
    cmp bx, 11
    jae .bad
    mov [fs_short+bx], al
    inc bx
    jmp .character
.search:
    mov [fs_path_cursor], si
    mov [fs_path_ended], al
    cmp dword [fs_directory], 0
    jne .find
    cmp byte [fs_short], '.'
    je .continue_path
.find:
    call fs_find_entry
    jc .bad
    cmp byte [fs_path_ended], 0
    je .file
    test byte [fs_found+11], 16
    jz .bad
    movzx eax, word [fs_found+20]
    shl eax, 16
    mov ax, [fs_found+26]
    mov [fs_directory], eax
.continue_path:
    mov si, [fs_path_cursor]
    jmp .component
.file:
    test byte [fs_found+11], 0x18
    jnz .bad
    xor cx, cx
.cached:
    cmp cx, [272]
    jae .new
    mov bx, cx
    shl bx, 5
    add bx, files
    mov eax, [fs_found_lba]
    cmp [bx+28], eax
    jne .cache_next
    mov ax, [fs_found_offset]
    cmp [bx+14], ax
    je .success
.cache_next:
    inc cx
    jmp .cached
.new:
    cmp cx, 64
    jae .bad
    mov bx, cx
    shl bx, 5
    add bx, files
    inc word [272]
    mov di, bx
    xor ax, ax
    mov cx, 16
    rep stosw
    mov eax, [fs_found+28]
    mov [bx+18], eax
    mov ax, [fs_found+20]
    mov [bx+16], ax
    mov ax, [fs_found+26]
    mov [bx+26], ax
    mov eax, [fs_found_lba]
    mov [bx+28], eax
    mov ax, [fs_found_offset]
    mov [bx+14], ax
.success:
    clc
    jmp .done
.bad:
    mov ax, 2
    stc
.done:
    pop dx
    pop cx
    pop di
    pop si
    pop es
    pop ds
    ret
fs_find_entry:
    mov eax, [fs_directory]
    mov [fs_search_cluster], eax
    test eax, eax
    jnz .cluster
    cmp byte [fs_type], 32
    je .root32
    mov eax, [fs_root]
    mov [fs_search_lba], eax
    mov ax, [fs_root_sectors]
    mov [fs_search_count], ax
    jmp .sector
.root32:
    mov eax, [fs_root_cluster]
    mov [fs_search_cluster], eax
.cluster:
    mov eax, [fs_search_cluster]
    sub eax, 2
    movzx ebx, byte [fs_spc]
    imul eax, ebx
    add eax, [fs_data]
    mov [fs_search_lba], eax
    movzx ax, byte [fs_spc]
    mov [fs_search_count], ax
.sector:
    mov eax, [fs_search_lba]
    mov bx, disk_buffer
    mov cx, 1
    call fs_read
    jc .missing
    mov di, disk_buffer
.entry:
    cmp byte [di], 0
    je .missing
    cmp byte [di], 0xe5
    je .next
    cmp byte [di+11], 15
    je .next
    mov si, fs_short
    push di
    mov cx, 11
    repe cmpsb
    pop di
    je .found
.next:
    add di, 32
    cmp di, disk_buffer+512
    jb .entry
    inc dword [fs_search_lba]
    dec word [fs_search_count]
    jnz .sector
    mov eax, [fs_search_cluster]
    test eax, eax
    jz .missing
    call fs_next_cluster
    jc .missing
    mov [fs_search_cluster], eax
    jmp .cluster
.found:
    mov ax, di
    sub ax, disk_buffer
    mov [fs_found_offset], ax
    mov eax, [fs_search_lba]
    mov [fs_found_lba], eax
    mov si, di
    mov di, fs_found
    mov cx, 16
    rep movsw
    clc
    ret
.missing:
    stc
    ret
fs_path times 384 db 0
fs_short times 11 db 0
fs_found times 32 db 0
fs_directory dd 0
fs_path_cursor dw 0
fs_path_ended db 0
fs_search_cluster dd 0
fs_search_count dw 0
fs_search_lba dd 0
fs_found_lba dd 0
fs_found_offset dw 0

; CF clear when the live CD has a RAM overlay (the USB bridge at 0x1000 keeps
; it, see bridge.asm): writes on the CD go there instead of failing.
overlay_check:
    push es
    push ax
    xor ax, ax
    mov es, ax
    cmp dword [es:0x1000], 'BRDG'
    jne .none
    cmp byte [es:0x1035], 0xff
    je .none
    pop ax
    pop es
    clc
    ret
.none:
    pop ax
    pop es
    stc
    ret

; Existing FAT files are written through to disk (on the live CD, to its
; RAM overlay).
; This version can extend within an existing cluster chain, but does not allocate
; new clusters through INT 21h. Native CMD programs provide general file creation.
fs_write_existing:
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
    push si
    push di
    push ds
    push es
    push cs
    pop ds
    xor ax, ax
    mov es, ax
    cmp dword [es:0x7010], 'CDVF'
    jne .writable
    call overlay_check
    jc .denied
.writable:
    call get_handle
    jc .done
    cmp word [ss:bp-6], 0
    jne .chunk
    mov eax, [handles+si+2]
    cmp eax, [di+18]
    ja .full
    mov [di+18], eax
    jmp .commit
.chunk:
    mov eax, [handles+si+2]
    call file_cluster_sector
    jc .full
    mov [ss:bp-12], eax
    push si
    push di
    mov bx, disk_buffer
    mov cx, 1
    call fs_read
    pop di
    pop si
    jc .io_error
    mov dx, [handles+si+2]
    and dx, 511
    mov cx, 512
    sub cx, dx
    cmp cx, [ss:bp-6]
    jbe .copy_count
    mov cx, [ss:bp-6]
.copy_count:
    movzx eax, cx
    add [handles+si+2], eax
    add [ss:bp-8], cx
    sub [ss:bp-6], cx
    push si
    push di
    mov di, disk_buffer
    add di, dx
    mov si, [ss:bp-4]
    mov ds, [ss:bp-2]
    push cs
    pop es
    rep movsb
    mov [ss:bp-4], si
    push cs
    pop ds
    pop di
    pop si
    mov eax, [ss:bp-12]
    mov bx, disk_buffer
    mov cx, 1
    call fs_store
    jc .io_error
    mov eax, [handles+si+2]
    cmp eax, [di+18]
    jbe .size_ready
    mov [di+18], eax
.size_ready:
    cmp word [ss:bp-6], 0
    jne .chunk
.commit:
    call fs_update_entry
    jc .io_error
    mov ax, [ss:bp-8]
    clc
    jmp .done
.denied:
    mov ax, 5
    stc
    jmp .done
.full:
    mov ax, 112
    stc
    jmp .done
.io_error:
    mov ax, 30
    stc
.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    mov sp, bp
    pop bp
    ret
fs_update_entry:
    pushad
    mov eax, [di+28]
    mov bx, disk_buffer
    mov cx, 1
    call fs_read
    jc .done
    mov bx, [di+14]
    mov eax, [di+18]
    mov [disk_buffer+bx+28], eax
    mov eax, [di+28]
    mov bx, disk_buffer
    mov cx, 1
    call fs_store
.done:
    popad
    ret
fs_store:
    pushad
    push ds
    push es
    push cs
    pop ds
    xor dx, dx
    mov es, dx
    cmp dword [es:0x7010], 'CDVF'
    jne .store
    call overlay_check
    jc .readonly
    mov [fs_dap+8], eax              ; image sectors, marked for the bridge
    mov dword [fs_dap+12], 0x40000000
    mov [fs_dap+2], cx
    mov [fs_dap+4], bx
    mov si, fs_dap
    mov dl, [boot_drive]
    mov ax, 0x4300
    int 0x13
    mov dword [fs_dap+12], 0
    jmp .done
.store:
    mov [fs_dap+8], eax
    mov [fs_dap+2], cx
    mov [fs_dap+4], bx
    mov si, fs_dap
    mov dl, [boot_drive]
    mov ax, 0x4300
    int 0x13
    jmp .done
.readonly:
    stc
.done:
    pop es
    pop ds
    popad
    ret
