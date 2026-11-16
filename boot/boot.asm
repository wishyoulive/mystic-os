; ============================================================
;  MYSTIC OS  -  stage 1 boot sector
;  512 bytes. The BIOS loads this, we print, we halt.
; ============================================================

[BITS 16]           ; real mode: 16-bit
[ORG 0x7C00]        ; the BIOS always loads us at this address

start:
    cli             ; interrupts off while we set up
    xor ax, ax
    mov ds, ax      ; all segments start at 0
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00  ; stack grows down, below our code
    sti

    mov ax, 0x0003  ; BIOS mode 3: 80x25 colour text
    int 0x10

    mov si, banner
    call print

hang:
    hlt             ; sleep until next interrupt
    jmp hang

; ------------------------------------------------------------
; print: send the zero-terminated string at DS:SI to the screen
; ------------------------------------------------------------
print:
    lodsb           ; AL = next byte, SI++
    test al, al
    jz .done
    mov ah, 0x0E    ; BIOS teletype
    mov bx, 0x0007  ; grey on black
    int 0x10
    jmp print
.done:
    ret

banner:
    db 'MYSTIC OS', 13, 10
    db 'This is your own boot sector talking to the BIOS.', 13, 10
    db 13, 10
    db 'CPU: real mode, 16-bit, no OS yet. Just you.', 13, 10
    db 0

; 510 bytes of padding, then the magic number every PC looks for
times 510 - ($ - $$) db 0
dw 0xAA55
