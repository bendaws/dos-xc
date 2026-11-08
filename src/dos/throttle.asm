;
; DOS-XC /dos/throttle.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 16
throttle_start:
    cmp byte [cpu_duty], 100
    jae .done
    cmp byte [cpu_duty], 1
    jae .valid
    mov byte [cpu_duty], 1
.valid:
    cli
    xor ax, ax
    mov es, ax
    mov eax, [es:8*4]
    mov [old_timer], eax
    mov word [es:8*4], timer_interrupt
    mov [es:8*4+2], cs
    mov byte [timer_installed], 1
    mov al, 0x36
    out 0x43, al
    mov ax, 5966
    out 0x40, al
    mov al, ah
    out 0x40, al
    sti
.done:
    ret
throttle_stop:
    cmp byte [cs:timer_installed], 0
    je .done
    cli
    xor ax, ax
    mov es, ax
    mov eax, [cs:old_timer]
    mov [es:8*4], eax
    mov al, 0x36
    out 0x43, al
    xor ax, ax
    out 0x40, al
    out 0x40, al
    mov byte [cs:timer_installed], 0
    sti
.done:
    ret
timer_interrupt:
    push ax
    push ds
    push cs
    pop ds
    add word [timer_phase], 5966
    jc .bios_tick
    mov al, 0x20
    out 0x20, al
    jmp .duty
.bios_tick:
    pushf
    call far [old_timer]
.duty:
    ; Programs that play music on the PC speaker time it with loops
    ; calibrated against the full-speed CPU: running in 5 ms bursts would
    ; slow the music down and chop it up.  While the speaker is in use (and
    ; 200 ms after), the CPU runs at full speed.
    in al, 0x61
    test al, 2
    jz .quiet
    mov byte [speaker_hold], 40
.quiet:
    cmp byte [speaker_hold], 0
    je .throttle
    dec byte [speaker_hold]
    mov word [duty_budget], 100
    jmp .done
.throttle:
    movzx ax, byte [cpu_duty]
    add [duty_budget], ax
    cmp byte [timer_waiting], 0
    jne .done
    cmp word [duty_budget], 100
    jae .resume
    mov byte [timer_waiting], 1
.wait:
    sti
    hlt
    cli
    cmp word [duty_budget], 100
    jb .wait
    mov byte [timer_waiting], 0
.resume:
    sub word [duty_budget], 100
.done:
    pop ds
    pop ax
    iret
old_timer dd 0
timer_phase dw 0
duty_budget dw 0
timer_waiting db 0
timer_installed db 0
speaker_hold db 0
