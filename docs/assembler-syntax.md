# Assembler-Syntax

> **Stand 0.1:** geplant (Meilenstein 0.2). Das Dokument ist die Spezifikation für Assembler, Encoder und den
> Quelltext-Modus des Disassemblers (`--asm`); beide Seiten müssen genau diese Syntax sprechen.

Die Syntax folgt der üblichen MOS-/Commodore-Schreibweise und nimmt zusätzlich die Pseudobefehle der Schwesterprojekte an
(`ORG`, `EQU`, `DB`, `DW`, `DS`). Sie bildet keinen bestimmten Fremd-Assembler vollständig nach; die Kreuzprüfung mit
`64tass`/`ca65` beschränkt sich auf Schreibweisen, die alle gleich übersetzen ([testing.md](testing.md)).

## Zeilenformat

```
[label[:]]  [befehl [operand]]  [; kommentar]
```

- Ein Label beginnt in Spalte 1 (dann ist der Doppelpunkt optional) oder endet mit `:`.
- **Lokale Labels** beginnen mit `.` und gehören zum letzten globalen Label (`.loop` unter `print` heißt intern
  `print.loop`).
- Groß-/Kleinschreibung: Befehle, Register und Pseudobefehle sind gleichgültig; Labels unterscheiden sie.
- Kommentare mit `;`; eine Zeile mit `*` in Spalte 1, gefolgt von einem Leerzeichen und Text, ist ebenfalls ein Kommentar
  – **außer** `*=` bzw. `* =` (Adresse setzen).

## Operanden

| Art | Schreibweise | Beispiel |
|---|---|---|
| implizit / Akkumulator | – / `A` | `INX`, `ASL`, `ASL A` |
| unmittelbar | `#ausdruck` | `LDA #$01`, `LDA #<text`, `LDA #'A'` |
| Zero Page / absolut | `ausdruck` | `LDA $FB`, `STA $D020` |
| indiziert | `ausdruck,X` / `ausdruck,Y` | `LDA table,X` |
| indirekt | `(ausdruck)` | `JMP ($0314)` |
| indiziert indirekt | `(ausdruck,X)` | `LDA ($20,X)` |
| indirekt indiziert | `(ausdruck),Y` | `STA ($FB),Y` |
| relativ | `ausdruck` | `BNE loop` |

Ein Operand in Klammern, der **nicht** zu einer indirekten Form passt (`LDA (1+2)*3`), ist ein Ausdruck – Klammern um den
ganzen Operanden bedeuten bei `JMP` immer indirekt. Für Rechenklammern sind auch eckige Klammern erlaubt:
`LDA [1+2]*3`.

## Wahl der Befehlsform

- **Zero Page oder absolut** wählt der Assembler selbst: Ist der Wert < `$100` und gibt es die Zero-Page-Form, wird sie
  genommen. Bei Vorwärtsreferenzen beginnt er klein und wächst über mehrere Durchläufe ([architecture.md](architecture.md#assembler)).
- **Erzwingen:** Suffix `.B` (Zero Page) oder `.W` (absolut) am Befehl: `LDA.W $0012,X` (3 Byte, z. B. für Zeitschleifen
  oder selbstmodifizierenden Code). `.B` mit einem Wert ≥ `$100` ist ein Fehler.
- **Mehrdeutige Opcodes:** Es gibt eine **Vorzugsform** (z. B. `NOP` = `$EA`, `SBC #` = `$E9`). Die anderen Opcodes
  schreibt man mit ihrem eigenen Namen (`USBC #`, `NOP #$00` = `$80`, `NOP $10` = `$04`, …) oder als `.BYTE`.
- **Undokumentierte Opcodes** sind standardmäßig erlaubt (stabile); `.CPU 6502` bzw. die Option `--no-illegal` verbietet
  sie, `.CPU 6510X` bzw. `--unstable` erlaubt auch die instabilen ([instruction-set.md](instruction-set.md)). Synonyme
  (`DCM`, `INS`, `ASO`, `LSE`, …) werden angenommen; der Disassembler schreibt immer die Hauptnamen.
- Verzweigungen außerhalb von −128…+127 sind ein Fehler (mit Abstand in der Meldung).

## Zahlen und Ausdrücke

| Schreibweise | Bedeutung |
|---|---|
| `123` | dezimal |
| `$1F`, `0x1F`, `1Fh` | hexadezimal |
| `%1010`, `0b1010` | binär (`%` nur am Anfang eines Operanden; sonst Modulo) |
| `'A'` | Zeichencode (ASCII) |
| `*` | aktuelle Adresse (am Anfang eines Ausdrucks; `**2` = aktuelle Adresse mal 2) |
| `<ausdruck`, `>ausdruck` | Low-Byte, High-Byte |
| `LOW(…)`, `HIGH(…)` | dasselbe als Funktion |
| `+ - * / % << >> & \| ^ ~`, `( )`, `[ ]` | Operatoren mit üblichem Vorrang; `<`/`>` binden schwächer als alle Rechenoperatoren (`#>table+1` = High-Byte von `table+1`) |

Ergebnisse werden mit 32 Bit gerechnet und bei der Ausgabe geprüft: Byte-Operanden −128…255, Wort-Operanden −32768…65535.

## Pseudobefehle

| Pseudobefehl | Bedeutung |
|---|---|
| `* = adresse`, `ORG adresse`, `.ORG` | Adresse setzen (mehrfach erlaubt; Lücken werden in der PRG-Ausgabe aufgefüllt, im Raw-Format ebenfalls) |
| `name = wert`, `name EQU wert` | Konstante |
| `.BYTE`, `.BYT`, `DB`, `DFB` | Bytes; Strings in `"…"` als ASCII-Bytes |
| `.WORD`, `DW` | 16-Bit-Wörter, Little-Endian |
| `.DBYTE` | 16-Bit-Wörter, **High-Byte zuerst** |
| `.TEXT "…"` | wie `.BYTE` mit String |
| `.PETSCII "…"` | String nach PETSCII gewandelt (Klein-/Großschreibung des Zeichensatzes) |
| `.SCREEN "…"` | String als Bildschirmcodes (für direktes Schreiben in den Bildschirmspeicher) |
| `.NULL "…"` | String mit angehängtem `0` |
| `.LOHI tabelle…` / `.LO …` / `.HI …` | getrennte Low-/High-Byte-Tabellen aus einer Adressliste |
| `.FILL n[,wert]`, `.DS n`, `DS n` | `n` Bytes mit `wert` (Standard 0) |
| `.ALIGN n[,wert]` | auf ein Vielfaches von `n` auffüllen |
| `.BASIC [zeile][,ziel]` | **BASIC-Starter**: `zeile SYS ziel` als tokenisierte BASIC-Zeile (Standard: Zeile 10, Ziel = direkt dahinter) |
| `.CPU 6502\|6510\|6510X` | erlaubte Opcodes (siehe oben) |
| `END` | Ende des Quelltexts (Rest wird ignoriert) |

## Ausgabe und Fehler

- **PRG** (Standard für `.prg`): zwei Byte Ladeadresse (die niedrigste belegte Adresse), dann die Daten bis zur höchsten.
- **Raw** (`--raw`): nur die Daten.
- **Listing** (`-l`), **Symboltabelle** (`-s`, `name = $XXXX`), **VICE-Labeldatei** (`--vice-labels`, `al C:XXXX .name`).
- Fehlermeldungen mit Datei, Zeilennummer und Spalte; alle Fehler eines Laufs werden gesammelt; bei Fehlern wird keine
  Ausgabedatei geschrieben.

## Nicht unterstützt (vorerst)

- Makros, `INCLUDE`/`.BINARY`, bedingte Assemblierung, Segmente/Linker (siehe [Roadmap](roadmap.md#danach-ideen))
- anonyme Labels (`+`, `-`, `:`) und weitere Dialekte (ACME `!byte`, ca65 `.res`, KickAssembler-Skripte)
- 65C02-/65816-Befehle
