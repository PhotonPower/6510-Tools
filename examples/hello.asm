; hello.asm - minimal Commodore 64 example (6510)
;
;   m6510tool asm examples/hello.asm -l -               assemble, listing on the screen
;   m6510tool asm examples/hello.asm -o hello.prg       PRG file with load address $0801
;   m6510tool dis hello.prg -c examples/hello.comments.txt
;
; LOAD"HELLO",8 and RUN on a C64: the BASIC line 10 SYS2061 starts the machine code, which
; prints the message through the KERNAL routine CHROUT and returns to BASIC.
;
; This file is the reference program of the CLI round-trip test (tests/cli_roundtrip.cmake) and of
; the documentation.

CHROUT      = $FFD2             ; KERNAL: output the character in A

            * = $0801
            .BASIC 10,start     ; BASIC starter: 10 SYS2061

start:      LDX #$00            ; X = index into the message
next:       LDA message,X       ; fetch the next character
            BEQ done            ; zero byte ends the text
            JSR CHROUT          ; print it
            INX
            BNE next            ; at most 255 characters
done:       RTS                 ; back to BASIC

message:    .BYTE "HELLO",$0D,0 ; upper-case ASCII = PETSCII in the default character set
