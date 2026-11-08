# Anleitung: vom Programm zum kommentierten Listing

> **Stand 0.5:** Die Schritte 1–4 und 6b sind gegen das echte Werkzeug geprüft (die Ausgaben unten stammen daraus; die Tests
> `agent` und `cli_*` führen dieselben Schritte aus). Schritt 6 mit einem echten Modell ist – wie bei den Schwesterprojekten –
> noch nicht erprobt; Abschnitt 7 wird mit den Erfahrungen gefüllt. Vorbild: [walkthrough.md im MC68000-Projekt](https://github.com/PhotonPower/MC68000/blob/main/docs/walkthrough.md).

## 1. Bauen und prüfen

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## 2. Das Beispiel übersetzen und starten

[`examples/c64/hello_c64.asm`](../examples/c64/hello_c64.asm) enthält alle Fälle, die die Analyse kennen muss: BASIC-Starter,
`CHROUT`-Ausgabe, einen PETSCII-String als Zeiger in A/Y, eine `RTS`-Sprungtabelle, getrennte Low-/High-Byte-Tabellen hinter
`JMP (vektor)`, eine Zero-Page-Zeigerschleife, selbstmodifizierenden Code, Bildschirmcodes, die in den Bildschirmspeicher
kopiert werden, ein Sprite und einen Raster-Interrupt-Handler.

```sh
./build/m6510tool asm examples/c64/hello_c64.asm -o hello_c64.prg
printf '%s\n' '{"cmd":"machine","type":"c64"}' '{"cmd":"load_file","file":"hello_c64.prg"}' \
              '{"cmd":"run_program"}' '{"cmd":"console"}' '{"cmd":"screen"}' | ./build/m6510tool serve --allow-files
```

```
{"ok":true,"machine":"c64","kernal":"emulated","memory":"$A000:RAM $D000:I/O $E000:KERNAL"}
{"ok":true,"start":"0x0801","end":"0x093F","basic":"10 SYS2061","entry":"0x080D"}
{"ok":true,"stop":"exit","instructions":219,"cycles":887,"pc":"0xFFF6",…,"message":"program returned to BASIC",…}
{"ok":true,"text":"HELLO, C64!\n"}
{"ok":true,"lines":["HELLO, C64!","",…,"THE WALKTHROUGH",…],"mode":"text","border":2,"background":6,…}
```

Mit `{"cmd":"screenshot"}` kommt derselbe Bildschirm als PNG (Base64); im Monitor schreibt `shot bild.png` ihn in eine Datei.

## 3. Die Analyse ansehen

```sh
python3 agent/m6510_annotate.py --prg hello_c64.prg --dry-run
```

```
program loaded: 0x0801-0x093F, entry 0x080D (10 SYS2061)
analysed 69 instructions, 0x0801-0x0940, 10 routines, 2 jump tables, 1 interrupt handler(s)
3 data regions marked: 1 screen, 1 sprites, 1 text

== dry run: no model is contacted ==
10 routines, 10 still without documentation. Processing order (leaves first):
    1. sub_082D               0x082D     17 B     9 instr  calls  1  called by  1  KERNAL calls 1  ~1912 tokens
    2. sub_0854               0x0854      6 B     3 instr  calls  0  called by  0  via jump table  ~1824 tokens
    …
    7. sub_0898               0x0898     15 B     5 instr  calls  1  called by  0  interrupt handler (0x0314)  ~1851 tokens
    …
   10. sub_080D               0x080D     32 B    13 instr  calls  5  called by  0  ~1961 tokens
jump table (rts) at 0x08A7: 3 entries
jump table (lohi) at 0x08AD: 3 entries
KERNAL calls: CHROUT x1
chip registers used: BGCOL0, EXTCOL, SP0X, SP0Y, SPENA, VICIRQ
estimate: about 18799 prompt tokens IN TOTAL for the first messages of all 10 routines …
```

Was hier zu sehen ist: Der Einstieg ist die `SYS`-Adresse des BASIC-Starters. Beide Sprungtabellen sind gefunden, ihre drei
Fälle sind Routinen („via jump table“). Den Raster-Handler trägt das Programm per `LDA #<irq / STA $0314 / LDA #>irq /
STA $0315` ein; die Analyse erkennt ihn daran. Text, Bildschirmcodes und das Sprite (über den Sprite-Zeiger `$07F8`) sind
als Daten markiert. Die Reihenfolge beginnt bei den Blättern: Wenn das Modell zu einer Routine kommt, sind die Routinen,
die sie aufruft, schon benannt.

## 4. Das Listing ohne Modell

```sh
printf '%s\n' '{"cmd":"machine","type":"c64"}' '{"cmd":"load_file","file":"hello_c64.prg"}' '{"cmd":"analyze"}' \
              '{"cmd":"data_regions","apply":true}' '{"cmd":"listing"}' | ./build/m6510tool serve --allow-files
```

Die letzte Antwort enthält das Listing mit `"verified":true` (es wird neu assembliert und Byte für Byte verglichen):

```
        .CPU 6510X

SP0X            = $D000                 ; chip register
…
EXTCOL          = $D020                 ; chip register
BGCOL0          = $D021                 ; chip register
CHROUT          = $FFD2                 ; KERNAL

        * = $0801
        .BASIC 10,sub_080D

; ------------------------------------------------------------
; sub_080D (not documented yet)
sub_080D:
        LDA #<data_08B3
        LDY #>data_08B3
        JSR sub_082D
…
sub_083E:
        LDA data_08AA,X
        PHA
        LDA data_08A7,X
        PHA
        RTS
…
data_08A7:
        .LO sub_0854-1,sub_085A-1,sub_086A-1
…
data_08C0:
        .SCREEN "the walkthrough"
```

Alle Routinen heißen noch `sub_XXXX`, Sprungziele `loc_XXXX`, Daten `data_XXXX` (`unnamed` zählt sie). Namen und Kommentare
vergibt das Modell – oder ein Mensch mit `ann_label`, `ann_routine`, … ([agent-protocol.md](agent-protocol.md)).

## 5. Ollama vorbereiten

```sh
ollama pull qwen3-vl:8b-instruct
```

Ein Modell mit Bildverständnis sieht die Screenshots, die `run_routine` auf Wunsch anhängt; ein Textmodell bekommt
stattdessen den Bildschirminhalt als Text (das Orchestrator-Skript erkennt das an den Fähigkeiten, die Ollama meldet).

## 6. Der erste Lauf

```sh
python3 agent/m6510_annotate.py --prg hello_c64.prg --project hello.m6510proj.json --out hello_kommentiert.asm \
        --lang Deutsch --log hello.log.jsonl --report hello_luecken.md
```

Das Skript arbeitet die Routinen in der Reihenfolge aus Schritt 3 ab. Für jede bekommt das Modell das Listing der Routine
(mit Adressen als Kommentar), was sie aufruft, welche Bausteinregister und Zero-Page-Adressen sie benutzt und worauf ihre
Operanden zeigen. Es testet die Routine mit `run_routine` (Register und Speicher vorgeben; das Ergebnis zeigt Register,
geänderten Speicher, Schreibzugriffe auf die Bausteine, KERNAL-Aufrufe, den mit `CHROUT` ausgegebenen Text und die
Bildschirmänderung), vergibt Namen und Kommentare und ruft `finish_routine` auf. Das prüft, ob noch Platzhalter, unbenannte
Speicherstellen oder zu wenige Kommentare übrig sind, und schickt das Modell einmal zurück. Interrupt-Handler laufen dabei mit
dem Stapelrahmen des KERNAL-IRQ-Einsprungs und kehren mit `RTI` zurück.

Nach jeder Routine wird die Projektdatei gespeichert; ein abgebrochener Lauf setzt dort wieder ein. Mit `--refine-model`
prüft ein zweites (langsameres) Modell die Routinen, die noch Lücken haben. Am Ende steht das verifizierte Listing in
`hello_kommentiert.asm` und die verbleibenden Lücken in `hello_luecken.md`.

## 6b. Ohne Ollama: ein Agent oder ein Mensch übernimmt die Rolle des Modells

`agent/m6510_drive.py` bietet dieselben Werkzeuge, ein Schritt je Aufruf; der Zustand liegt im Arbeitsverzeichnis.

```sh
python3 agent/m6510_drive.py --work work init --prg hello_c64.prg
python3 agent/m6510_drive.py --work work status
python3 agent/m6510_drive.py --work work show 0x082D
python3 agent/m6510_drive.py --work work tool run_routine '{"addr":"0x082D","registers":{"A":"0xB3","Y":"0x08"},"screenshot":true}'
```

```
stop: returned  returned: True  instructions: 90  cycles: 442
registers after: A=00 X=00 Y=0C S=FD P=26 flags=..-..IZ.
memory changed (3 ranges; the stack page is omitted; small ranges first):
  0x00FB len 2: 0000 -> B308
  0x00D1 len 6: 000000000000 -> 280400000001
  0x0400 len 11: 2020202020202020202020 -> 08050C0C0F2C2003363421
KERNAL calls made: 12
  CHROUT A=48 X=00 Y=00 -> A=48 X=00 Y=00 flags=..-..I.. (the JSR is at 0x0837)
  …
text printed with CHROUT: 'HELLO, C64!\n'
screen: 125 pixels changed, columns 0-10, rows 0-0 of 40x25
  row  0: 'HELLO, C64!'
(image attached: columns 0-12, rows 0-1, zoom 3x)
```

```sh
python3 agent/m6510_drive.py --work work tool document_routine \
        '{"addr":"0x082D","name":"print_string","summary":"Gibt einen nullterminierten Text aus.","inputs":"A/Y = Adresse","outputs":"keine","clobbers":"A Y","confidence":"high"}'
python3 agent/m6510_drive.py --work work tool name_variable '{"addr":"0xFB","name":"text_ptr","size":2}'
python3 agent/m6510_drive.py --work work finish 0x082D
python3 agent/m6510_drive.py --work work listing --out hello_kommentiert.asm
```

`finish` nimmt eine Routine erst an, wenn nichts mehr fehlt (`--accept-gaps`: die Regel des Orchestrators, der zweite Aufruf
gilt). `report` zeigt, was offen ist.

## 7. Woran man ein Problem erkennt

**Blindtest (2026-11-08):** Ein Claude-Agent hat mit `m6510_drive.py` die Rolle des Modells übernommen – nur mit dem PRG,
ohne Quelltext. Er hat alle 10 Routinen richtig erkannt und dokumentiert (beide Sprungtabellen, den Raster-Handler, den
selbstmodifizierenden Vergleich, das Sprite), das Listing war verifiziert; gebraucht hat er rund 115 Werkzeugaufrufe, 6 bis
13 je Routine. Seine Befunde zu den Werkzeugen sind behoben (CHANGELOG): uneinheitliche Namen in Ausschnitten, verlorene
Kommentare an Daten, ein Zähler in einem `.FILL`, fehlende Hinweise auf Sprungtabellen, Sprites und KERNAL-Adressen.

Die Erprobung mit einem echten lokalen Modell steht noch aus. Bekannt aus den Schwesterprojekten und schon abgefangen:

- **Falsches `returned`:** `run_routine` meldet `returned: True` nur, wenn der Stapelzeiger wieder stimmt. Ein Interrupt-
  Handler, als Unterprogramm aufgerufen, kehrt nicht zurück – deshalb läuft er automatisch mit Interrupt-Rahmen.
- **Ins Leere gesprungen:** `stop: halted` (ein `JAM`-Opcode) heißt fast immer, dass ein Zeiger oder Vektor nicht vorbereitet
  war. `setup` (die Initialisierung des Programms) oder `memory` helfen.
- **Tabellen als Code, Strings als Routinen:** `data_regions` und die Sprungtabellen-Erkennung laufen vor dem ersten Modell-
  aufruf; was trotzdem falsch ist, korrigiert `mark_data` (Datenbereiche überdecken nie analysierten Code) bzw.
  `remove_annotation`.
- **Zahl oder Zeiger?** `LDA #<x / LDY #>x` wird als Zeiger geschrieben, wenn das Ziel im Programm liegt; ist es eine Zahl,
  macht `mark_constant` das rückgängig.
