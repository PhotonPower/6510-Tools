# Befehlssatz-Überblick MOS 6510

> Nachschlagewerk für die Umsetzung (Meilensteine 0.2 und 0.3). Werte, die nicht ohnehin aus vielen Quellen bekannt sind,
> werden beim Schreiben der Opcode-Tabelle gegen die Testsätze ([testing.md](testing.md)) geprüft.

Der 6510 ist ein NMOS-6502 mit einem eingebauten 6-Bit-I/O-Port an den Adressen `$00/$01`; der Befehlssatz ist identisch.

## Programmiermodell

| Register | Breite | Bedeutung |
|---|---|---|
| `A` | 8 | Akkumulator |
| `X`, `Y` | 8 | Indexregister |
| `S` | 8 | Stapelzeiger; der Stapel liegt bei `$0100 + S`, wächst nach unten, `PHA` schreibt und dekrementiert danach |
| `PC` | 16 | Programmzähler |
| `P` | 8 | Status `N V - B D I Z C` |

| Bit | Flag | Bedeutung |
|---|---|---|
| 7 | `N` | negativ (Bit 7 des Ergebnisses) |
| 6 | `V` | Überlauf (vorzeichenbehaftet); `BIT` kopiert Bit 6 des Operanden |
| 5 | – | nicht vorhanden, liest sich als 1 |
| 4 | `B` | nur in der gestapelten Kopie: 1 bei `BRK`/`PHP`, 0 bei IRQ/NMI |
| 3 | `D` | Dezimalmodus für `ADC`/`SBC` |
| 2 | `I` | IRQ gesperrt |
| 1 | `Z` | null |
| 0 | `C` | Übertrag; bei `SBC`/`CMP` ist C = 1 „kein Borgen“ |

Nach RESET: `I` = 1, `S` wird um 3 vermindert (drei Schein-Pushes ohne Schreiben), `PC` aus `$FFFC/$FFFD`; `A`, `X`, `Y`,
die übrigen Flags und insbesondere `D` sind **unbestimmt** (der Emulator setzt sie auf feste, dokumentierte Werte).

## Speicher

- 64 KiB, **Little-Endian**, keine Ausrichtungsregeln.
- **Zero Page** `$0000–$00FF`: kürzere und schnellere Adressierung, Zeiger für `(zp),Y` / `(zp,X)`.
- **Stapel** `$0100–$01FF`.
- **Vektoren:** NMI `$FFFA`, RESET `$FFFC`, IRQ/BRK `$FFFE`.
- **Prozessorport (nur 6510):** `$00` Richtungsregister (1 = Ausgang), `$01` Daten. Auf dem C64: Bit 0 `LORAM`, Bit 1
  `HIRAM`, Bit 2 `CHAREN` (Speicherkonfiguration, siehe [c64.md](c64.md)), Bit 3 Kassette Schreiben, Bit 4 Kassette Taste
  (Eingang), Bit 5 Kassette Motor. Eingänge ohne Last behalten ihren Wert eine Weile („fading“), was manche Kopierschutz-
  Routinen prüfen – nicht nachgebildet.

## Adressierungsarten

| Art | Schreibweise | Bytes | Beispiel | Hinweis |
|---|---|---|---|---|
| implizit | – | 1 | `INX` | |
| Akkumulator | `A` (oder nichts) | 1 | `ASL A` | |
| unmittelbar | `#nn` | 2 | `LDA #$01` | |
| Zero Page | `zp` | 2 | `LDA $FB` | |
| Zero Page,X | `zp,X` | 2 | `LDA $10,X` | Adresse läuft **innerhalb der Zero Page** um |
| Zero Page,Y | `zp,Y` | 2 | `LDX $10,Y` | nur `LDX`, `STX`, `LAX`, `SAX` |
| absolut | `abs` | 3 | `JMP $C000` | |
| absolut,X | `abs,X` | 3 | `LDA $0400,X` | +1 Zyklus bei Seitenüberschreitung (lesend) |
| absolut,Y | `abs,Y` | 3 | `LDA $0400,Y` | ebenso |
| indirekt | `(abs)` | 3 | `JMP ($0314)` | nur `JMP`; **Fehler:** `JMP ($xxFF)` holt das High-Byte von `$xx00` |
| indiziert indirekt | `(zp,X)` | 2 | `LDA ($20,X)` | Zeiger aus der Zero Page, umlaufend |
| indirekt indiziert | `(zp),Y` | 2 | `LDA ($FB),Y` | Zeiger bei `$FF` holt das High-Byte von `$00`; +1 Zyklus bei Seitenüberschreitung |
| relativ | `ziel` | 2 | `BNE loop` | −128…+127 ab dem folgenden Befehl; genommen +1, über eine Seite +2 Zyklen |

## Opcodes

**151 dokumentierte** Opcodes (56 Befehle) und **105 undokumentierte**, davon 12 `JAM` (auch `KIL`/`HLT`: halten den
Prozessor an, bis RESET): `$02 $12 $22 $32 $42 $52 $62 $72 $92 $B2 $D2 $F2`.

Aufbau des Opcode-Bytes `aaabbbcc`: `cc` wählt die Gruppe (01: `ORA AND EOR ADC STA LDA CMP SBC` mit acht
Adressierungsarten; 10: Schiebe-/Rotier-/`STX`/`LDX`/`DEC`/`INC`; 00: Sprünge, Verzweigungen, Flags, `BIT`, `STY`/`LDY`/
`CPY`/`CPX`; 11: nur undokumentiert – im Wesentlichen „01 und 10 gleichzeitig“), `bbb` die Adressierungsart, `aaa` die
Operation. Für die Tabelle ist das eine Hilfe zum Prüfen, nicht die Implementierung (siehe [architecture.md](architecture.md)).

### Dokumentierte Befehle (Auswahl der Eigenheiten)

- `ADC`/`SBC` im **Dezimalmodus** (NMOS): Ergebnis und `C` sind korrekte BCD-Arithmetik für gültige BCD-Eingaben; `Z` richtet
  sich nach dem **binären** Ergebnis, `N` und `V` nach einem Zwischenergebnis. Ungültige BCD-Ziffern liefern definierte, aber
  „seltsame“ Ergebnisse – der Emulator rechnet genau das nach (Klaus Dormanns Dezimaltest prüft alle Kombinationen).
- `BRK` ist 2 Byte lang (das zweite wird übersprungen): gestapelt wird `PC+2` und `P` mit `B` = 1.
- `JSR` stapelt die Adresse **des letzten Bytes** des `JSR` (Rücksprungadresse − 1); `RTS` addiert 1. Darauf beruht der
  **`RTS`-Trick**: Adresse−1 mit `PHA` stapeln, dann `RTS` – eine berechnete Sprungtabelle.
- `RTI` holt `P` und `PC` (ohne +1).
- Read-Modify-Write-Befehle (`ASL LSR ROL ROR INC DEC` auf Speicher, die undokumentierten `SLO RLA SRE RRA DCP ISC`) schreiben
  den **alten Wert noch einmal**, bevor sie den neuen schreiben. Bei I/O-Registern sichtbar (`INC $D019` quittiert einen
  VIC-Interrupt); das Schreibprotokoll des C64 zeigt beide Zugriffe.
- `CLI`/`SEI`/`PLP` wirken auf die Interrupt-Annahme erst **nach dem nächsten Befehl**; `RTI` sofort.

### Undokumentierte Befehle

Namen nach „No More Secrets“ (Synonyme in Klammern, vom Assembler angenommen):

| Befehl | Wirkung | Opcodes (Adressierungsarten) |
|---|---|---|
| `SLO` (`ASO`) | `ASL` Speicher, dann `ORA` | 7 Formen |
| `RLA` | `ROL` Speicher, dann `AND` | 7 Formen |
| `SRE` (`LSE`) | `LSR` Speicher, dann `EOR` | 7 Formen |
| `RRA` | `ROR` Speicher, dann `ADC` | 7 Formen |
| `SAX` (`AXS`, `AAX`) | speichert `A & X` | zp, zp,Y, abs, (zp,X) |
| `LAX` | lädt `A` und `X` | zp, zp,Y, abs, abs,Y, (zp,X), (zp),Y; `#imm` siehe `LXA` |
| `DCP` (`DCM`) | `DEC` Speicher, dann `CMP` | 7 Formen |
| `ISC` (`ISB`, `INS`) | `INC` Speicher, dann `SBC` | 7 Formen |
| `ANC` | `AND #`, `C` = `N` | `$0B`, `$2B` |
| `ALR` (`ASR`) | `AND #`, dann `LSR A` | `$4B` |
| `ARR` | `AND #`, dann `ROR A` mit eigenen Flag-Regeln (und Dezimalmodus!) | `$6B` |
| `SBX` (`AXS`) | `X = (A & X) − #`, Flags wie `CMP` | `$CB` |
| `USBC` | wie `SBC #` | `$EB` |
| `NOP` | verschiedene Längen und Adressierungsarten (lesen ihren Operanden) | 27 Opcodes außer `$EA` |
| `JAM` | hält an | 12 Opcodes |

**Instabil** (Ergebnis hängt von der Hardware ab; Emulatoren legen sich fest, Programme benutzen sie selten):
`ANE`/`XAA` (`$8B`), `LXA`/`LAX #` (`$AB`), `SHA`/`AHX` (`$93`, `$9F`), `SHX` (`$9E`), `SHY` (`$9C`), `TAS`/`SHS` (`$9B`),
`LAS`/`LAR` (`$BB`, stabil, aber selten). Das Verhalten im Emulator wird in [emulator.md](emulator.md) festgehalten
(offene Frage in der [Roadmap](roadmap.md#offene-fragen)).

## Interrupts

| Quelle | Vektor | Gestapelt | Bemerkung |
|---|---|---|---|
| RESET | `$FFFC` | nichts (S − 3) | `I` = 1 |
| NMI | `$FFFA` | `PC`, `P` (B = 0) | flankengesteuert, nicht maskierbar; C64: RESTORE-Taste, CIA 2 |
| IRQ | `$FFFE` | `PC`, `P` (B = 0) | pegelgesteuert, maskierbar mit `I`; C64: CIA 1, VIC-II |
| `BRK` | `$FFFE` | `PC+2`, `P` (B = 1) | |

Alle setzen `I` = 1; `D` bleibt auf dem NMOS-Prozessor **unverändert**. Ein NMI während der Annahme eines `BRK`/IRQ kann
dessen Vektor „übernehmen“ (bekannte Eigenheit; nachgebildet, wenn ein Testsatz es prüft).

## Zyklen (Auswahl)

| Art | Zyklen |
|---|---|
| implizit, unmittelbar | 2 |
| Zero Page / Zero Page,X/Y / absolut / absolut,X/Y | 3 / 4 / 4 / 4 (+1) |
| `(zp,X)` / `(zp),Y` | 6 / 5 (+1) |
| Read-Modify-Write zp / zp,X / abs / abs,X | 5 / 6 / 6 / 7 |
| `JMP abs` / `JMP (abs)` / `JSR` / `RTS` / `RTI` | 3 / 5 / 6 / 6 / 6 |
| `BRK`, IRQ, NMI | 7 |
| Verzweigung | 2, genommen +1, über eine Seite +2 |

Schreibende indizierte Befehle (`STA abs,X`, …) brauchen immer den Zusatzzyklus. Die vollständige Tabelle steht in der
Opcode-Tabelle (Meilenstein 0.2) und wird gegen Tom Hartes Testsatz geprüft.

## Was der Dekoder als „ungültig“ behandeln kann

Es gibt keine ungültigen Opcodes – jedes Byte tut etwas. Der Dekoder kennt deshalb **Optionen**:

| Option | Wirkung |
|---|---|
| nur dokumentierte | undokumentierte Opcodes werden als `.BYTE` ausgegeben und beenden einen Weg der Ablaufanalyse |
| stabile undokumentierte (Standard) | zusätzlich `SLO … SBX`, `NOP`-Varianten, `USBC`; instabile und `JAM` wie oben |
| alle | auch instabile Opcodes; `JAM` beendet einen Weg |
