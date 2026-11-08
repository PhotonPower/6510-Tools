; hello_c64 - the example program of docs/walkthrough.md
;
; It contains every case the analysis has to recognise: a BASIC starter, output with CHROUT, a
; PETSCII string passed as a pointer in A/Y, a jump table used with the RTS trick, split tables of
; low and high bytes behind JMP (vector), a zero-page pointer loop, self-modifying code, text in
; screen codes copied into the screen memory, a sprite, a raster interrupt handler and a table of
; plain numbers.

CHROUT  = $FFD2
ptr     = $FB               ; zero-page pointer (2 bytes)
vector  = $FD               ; target of the JMP (vector) dispatcher (2 bytes)

        * = $0801
        .BASIC 10, start

start:  LDA #<greeting
        LDY #>greeting
        JSR print
        LDX #0
        JSR dispatch        ; command 0: set the border colour
        LDX #1
        JSR dispatch        ; command 1: count
        LDX #2
        JSR run_cmd         ; command 2 through the second table: show the title
        JSR sprite_on
        JSR irq_on
        LDA counter
        RTS

; print a zero-terminated PETSCII string: A = low byte, Y = high byte of its address
print:  STA ptr
        STY ptr+1
        LDY #0
loop:   LDA (ptr),Y
        BEQ done
        JSR CHROUT
        INY
        BNE loop
done:   RTS

; command X (0..2) through the RTS trick
dispatch:
        LDA cmd_hi,X
        PHA
        LDA cmd_lo,X
        PHA
        RTS

; command X (0..2) through split tables and JMP (vector)
run_cmd:
        LDA jmp_lo,X
        STA vector
        LDA jmp_hi,X
        STA vector+1
        JMP (vector)

set_border:
        LDA #2
        STA $D020
        RTS

count:  INC counter
        LDA counter
patch:  CMP #$00            ; the operand is changed below: self-modifying code
        BNE count_done
        LDA #5
        STA patch+1
count_done:
        RTS

show_title:
        LDX #0
copy:   LDA title,X
        STA $0400+40*12,X
        INX
        CPX #title_end-title
        BNE copy
        RTS

sprite_on:
        LDA #sprite/64
        STA $07F8
        LDA #1
        STA $D015
        LDA #100
        STA $D000
        STA $D001
        RTS

irq_on: SEI
        LDA #<irq
        STA $0314
        LDA #>irq
        STA $0315
        CLI
        RTS

irq:    INC $D019
        LDX counter
        LDA colors,X
        STA $D021
        JMP $EA31

; ---- data ------------------------------------------------------------------------------------------
cmd_lo: .LO set_border-1, count-1, show_title-1
cmd_hi: .HI set_border-1, count-1, show_title-1
jmp_lo: .LO set_border, count, show_title
jmp_hi: .HI set_border, count, show_title
greeting:
        .BYTE "HELLO, C64!", 13, 0
title:  .SCREEN "the walkthrough"
title_end:
colors: .BYTE 0, 6, 14, 3, 1
counter:
        .BYTE 0
        .ALIGN 64
sprite: .BYTE $00, $7E, $00, $03, $FF, $C0, $07, $FF, $E0, $0F, $FF, $F0
        .BYTE $1F, $FF, $F8, $1F, $FF, $F8, $3F, $FF, $FC, $3F, $FF, $FC
        .BYTE $3F, $FF, $FC, $3F, $FF, $FC, $1F, $FF, $F8, $1F, $FF, $F8
        .BYTE $0F, $FF, $F0, $07, $FF, $E0, $03, $FF, $C0, $00, $7E, $00
        .FILL 16
