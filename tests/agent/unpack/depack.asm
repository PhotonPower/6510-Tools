; Depacker for lzpack.py: unpacks `packed` to DEST and jumps to ENTRY.
; The test puts "DEST = ...", "ENTRY = ..." and "ORIGIN = ..." in front and appends the packed bytes as .BYTE lines.
src     = $FB
dst     = $FD
cp      = $02

        * = ORIGIN
start:  SEI
        LDA #<packed
        STA src
        LDA #>packed
        STA src+1
        LDA #<DEST
        STA dst
        LDA #>DEST
        STA dst+1
loop:   JSR getbyte
        CMP #$FF
        BEQ done
        CMP #$80
        BCS match
        TAX
        INX
lit:    JSR getbyte
        JSR putbyte
        DEX
        BNE lit
        BEQ loop
match:  AND #$7F
        CLC
        ADC #3
        TAX
        JSR getbyte
        STA dist
        JSR getbyte
        STA dist+1
        SEC
        LDA dst
        SBC dist
        STA cp
        LDA dst+1
        SBC dist+1
        STA cp+1
mcopy:  LDY #0
        LDA (cp),Y
        JSR putbyte
        INC cp
        BNE mnext
        INC cp+1
mnext:  DEX
        BNE mcopy
        BEQ loop
done:   CLI
        JMP ENTRY
getbyte:
        LDY #0
        LDA (src),Y
        INC src
        BNE g1
        INC src+1
g1:     RTS
putbyte:
        LDY #0
        STA (dst),Y
        INC dst
        BNE p1
        INC dst+1
p1:     RTS
dist:   .BYTE 0, 0
packed:
