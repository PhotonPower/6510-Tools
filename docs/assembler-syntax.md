# Assembler-Syntax

> **Stand: umgesetzt (Meilenstein 0.2).** Dieses Dokument ist die Spezifikation für Assembler, Encoder und den
> Quelltext-Modus des Disassemblers (`--asm`); beide Seiten sprechen genau diese Syntax, was der exhaustive Roundtrip-Test
> prüft ([testing.md](testing.md)).

Die Syntax folgt der üblichen MOS-/Commodore-Schreibweise und nimmt zusätzlich die Pseudobefehle der Schwesterprojekte an
(`ORG`, `EQU`, `DB`, `DW`, `DS`). Sie bildet keinen bestimmten Fremd-Assembler vollständig nach; die Kreuzprüfung mit
`ca65` beschränkt sich auf Schreibweisen, die beide gleich übersetzen ([testing.md](testing.md#kreuzprüfung)).

## Zeilenformat

```
[label[:]]  [befehl [operand]]  [; kommentar]
```

- Ein Label endet mit `:` oder beginnt in Spalte 1 (dann ist der Doppelpunkt optional). Ein Wort in Spalte 1, das ein
  Befehl oder Pseudobefehl ist (`NOP`, `.BYTE`, `ORG`), ist kein Label, sondern die Anweisung selbst.
- **Lokale Labels** beginnen mit `.` und gehören zum letzten globalen Label (`.loop` unter `print` heißt intern und in der
  Symboltabelle `print.loop`). Ein lokales Label vor dem ersten globalen ist ein Fehler.
- **Namen** bestehen aus Buchstaben, Ziffern und `_` und beginnen mit einem Buchstaben oder `_`. Reserviert sind die
  Register `A`, `X`, `Y` (in jeder Schreibweise), alle Befehle und Pseudobefehle sowie `LOW` und `HIGH`.
- Groß-/Kleinschreibung: Befehle, Register und Pseudobefehle sind gleichgültig; Labels unterscheiden sie.
- Kommentare mit `;` (nicht innerhalb von Strings und Zeichenkonstanten). Eine Zeile mit `*` in Spalte 1 ist ebenfalls ein
  Kommentar – **außer** `*=` bzw. `* =` (Adresse setzen, auch eingerückt).

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
| relativ | `ausdruck` (Zieladresse) | `BNE loop`, `BEQ *+4` |

Ein Operand in Klammern, der **nicht** zu einer indirekten Form passt (`LDA (1+2)*3`, oder `LDA (5)` bei einem Befehl
ohne indirekte Form), ist ein Ausdruck – bei `JMP` bedeuten Klammern um den ganzen Operanden immer indirekt. Für
Rechenklammern sind auch eckige Klammern erlaubt: `LDA [1+2]*3`.

## Wahl der Befehlsform

- **Zero Page oder absolut** wählt der Assembler selbst: Ist der Wert < `$100` und gibt es die Zero-Page-Form, wird sie
  genommen. Bei Vorwärtsreferenzen beginnt er mit der Zero-Page-Form und wächst über mehrere Durchläufe; Formen schrumpfen
  nie ([architecture.md](architecture.md#assembler)).
- **Erzwingen:** Suffix `.B` (Zero Page) oder `.W` (absolut) am Befehl: `LDA.W $0012,X` (3 Byte, z. B. für Zeitschleifen
  oder selbstmodifizierenden Code). `.B` mit einem Wert ≥ `$100` ist ein Fehler. Der Disassembler schreibt `.W` überall
  dort, wo ein absoluter Befehl einen Zero-Page-Wert hat.
- Nur `LDX`, `STX`, `LAX` und `SAX` haben eine `zp,Y`-Form; `LDA $12,Y` ist deshalb immer absolut (3 Byte).
- **Mehrdeutige Opcodes:** Es gibt eine **Vorzugsform** (z. B. `NOP` = `$EA`, `SBC #` = `$E9`, `ANC #` = `$0B`,
  `JAM` = `$02`). Andere Opcodes schreibt man mit ihrem eigenen Namen, wo es einen gibt (`USBC #` = `$EB`, `NOP #$00` = `$80`,
  `NOP $10` = `$04`, …), sonst als `.BYTE`. Der Disassembler macht im Quelltext-Modus genau das.
- **Undokumentierte Opcodes** sind standardmäßig erlaubt (die stabilen). `.CPU 6502` bzw. `--no-illegal` verbietet sie;
  `.CPU 6510X` bzw. `--unstable` erlaubt zusätzlich die instabilen (`ANE`, `LXA`, `SHA`, `SHX`, `SHY`, `TAS`, `LAS`) und
  `JAM`; `.CPU 6510` stellt den Standard wieder her ([instruction-set.md](instruction-set.md)). Synonyme werden
  angenommen: `ASO`→`SLO`, `LSE`→`SRE`, `AAX`→`SAX`, `DCM`→`DCP`, `ISB`/`INS`→`ISC`, `ASR`→`ALR`, `XAA`→`ANE`,
  `AHX`/`AXA`→`SHA`, `SHS`/`XAS`→`TAS`, `SXA`→`SHX`, `SYA`→`SHY`, `LAR`→`LAS`, `KIL`/`HLT`→`JAM`, `ATX`→`LXA`.
  Der Disassembler schreibt immer die Hauptnamen. (`AXS` ist mehrdeutig – mal `SAX`, mal `SBX` – und wird deshalb nicht
  angenommen.)
- Verzweigungen außerhalb von −128…+127 sind ein Fehler (mit Abstand in der Meldung); der Programmzähler läuft bei 64 KiB um.

## Zahlen und Ausdrücke

| Schreibweise | Bedeutung |
|---|---|
| `123` | dezimal |
| `$1F`, `0x1F`, `1Fh` | hexadezimal |
| `%1010`, `0b1010` | binär (`%` nur dort, wo ein Wert erwartet wird; sonst Modulo) |
| `'A'` | Zeichencode (ASCII, genau ein Zeichen) |
| `*` | aktuelle Adresse (wo ein Wert erwartet wird; `**2` = aktuelle Adresse mal 2) |
| `<ausdruck`, `>ausdruck` | Low-Byte, High-Byte des **ganzen** folgenden Ausdrucks |
| `LOW(…)`, `HIGH(…)` | dasselbe als Funktion |
| `+ - * / % << >> & \| ^ ~`, `( )`, `[ ]` | Operatoren mit dem Vorrang von C; `<`/`>` binden schwächer als alle (`#>table+1` = High-Byte von `table+1`) |

Gerechnet wird mit 64 Bit; geprüft wird bei der Ausgabe: Byte-Operanden −128…255, Adressen 0…`$FFFF`, `.WORD` −32768…65535.

## Pseudobefehle

| Pseudobefehl | Bedeutung |
|---|---|
| `* = adresse`, `ORG adresse`, `.ORG` | Adresse setzen (mehrfach erlaubt; Lücken werden in PRG- und Raw-Ausgabe mit Nullen gefüllt). Der Wert muss bekannt sein (frühere Definition oder Wert aus dem vorigen Durchlauf) |
| `name = wert`, `name EQU wert` | Konstante |
| `.BYTE`, `.BYT`, `DB`, `DFB`, `.TEXT` | Bytes; Strings in `"…"` als ASCII-Bytes (ohne Escapes; `"` kann nicht im String stehen) |
| `.NULL "…"` | wie `.BYTE`, mit angehängtem `0` |
| `.WORD`, `DW` | 16-Bit-Wörter, Little-Endian |
| `.DBYTE` | 16-Bit-Wörter, **High-Byte zuerst** |
| `.PETSCII "…"`, `.PET` | String nach PETSCII: Kleinbuchstaben → `$41–$5A`, Großbuchstaben → `$C1–$DA` (Konvention von ACME/64tass: im Groß-/Kleinschrift-Zeichensatz erscheint der Text wie geschrieben; im Standard-Zeichensatz schreibt man Text klein) |
| `.SCREEN "…"`, `.SCR` | String als Bildschirmcodes: `@` = 0, Kleinbuchstaben 1–26, Großbuchstaben `$41–$5A`, Leerzeichen/Ziffern/Satzzeichen `$20–$3F` |
| `.LO a,b,…` / `.HI a,b,…` | Low- bzw. High-Bytes einer Adressliste |
| `.LOHI a,b,…` | erst alle Low-, dann alle High-Bytes (getrennte Tabellen) |
| `.FILL n[,wert]`, `.DS`, `DS`, `.RES` | `n` Bytes (höchstens 65536) mit `wert` (Standard 0) |
| `.ALIGN n[,wert]` | auf ein Vielfaches von `n` auffüllen |
| `.BASIC [zeile][,ziel]` | **BASIC-Starter** `zeile SYSziel` als tokenisierte BASIC-Zeile mit Endemarke (Standard: Zeile 10, Ziel = direkt dahinter; bei `$0801` also `10 SYS2061`) |
| `.CPU 6502\|6510\|6510X` | erlaubte Opcodes (siehe oben) |
| `END`, `.END` | Ende des Quelltexts (Rest wird ignoriert) |

## Ausgabe und Fehler

- **PRG** (Standard): zwei Byte Ladeadresse (die niedrigste belegte Adresse), dann die Daten bis zur höchsten.
- **Raw** (`--raw`): nur die Daten.
- **Listing** (`-l`): Zeilennummer, Adresse, bis zu vier Bytes, Quelltextzeile.
- **Symboltabelle** (`-s`): `name = $XXXX`, nach Namen sortiert (vom Disassembler mit `--symbols` lesbar).
- **VICE-Labeldatei** (`--vice-labels`): `al C:xxxx .name` (Punkte in lokalen Namen werden zu `_`).
- Fehlermeldungen als `datei:zeile: error: text`; alle Fehler eines Laufs werden gesammelt; bei Fehlern wird keine
  Ausgabedatei geschrieben (das Listing schon).

## Nicht unterstützt (vorerst)

- Makros, `INCLUDE`/`.BINARY`, bedingte Assemblierung, Segmente/Linker (siehe [Roadmap](roadmap.md#danach-ideen))
- anonyme Labels (`+`, `-`, `:`) und weitere Dialekte (ACME `!byte`, ca65 `.res`-Semantik, KickAssembler-Skripte)
- Escapes in Strings
- 65C02-/65816-Befehle
