# Emulator

> **Stand:** Der Prozessorkern (`m6510::Cpu`) ist umgesetzt und geprüft (Meilenstein 0.3, erster Teil): alle 2,44 Millionen
> Einzelbefehls-Tests von Tom Harte und Klaus Dormanns Funktionstest bestehen. Snapshots, Sitzung und C64-Maschine folgen.

`m6510::Cpu` (`include/m6510/cpu.hpp`) ist ein **befehlsgenauer** NMOS-6510-Kern: Für jeden Befehl stimmen Register, Flags,
Speicherinhalt und die **Zahl der Zyklen**; einzelne Buszyklen werden nicht modelliert (Entscheidung in der
[Roadmap](roadmap.md#entscheidungen)).

## Umfang

- **Alle 256 Opcodes**: dokumentierte, stabile und instabile undokumentierte; `JAM` hält den Prozessor an
  (`halted()`, nur RESET oder `set PC` macht weiter). Welche Opcodes als Befehl gelten, entscheidet dieselbe Tabelle wie
  beim Disassembler; für die Ausführung gibt es keine „unerlaubten“ Opcodes.
- **Dezimalmodus** nach NMOS-Art, auch für `ARR`, `RRA`, `ISC` und `USBC`, inklusive der Flags bei ungültigen BCD-Ziffern.
- **Seiteneigenheiten:** `JMP ($xxFF)`, Zero-Page-Umlauf bei `zp,X`/`zp,Y`/`(zp,X)`/`(zp),Y` (`Memory::read16Wrapped`).
- **Dummy-Zugriffe**, soweit sie für Geräte am wichtigsten sind: das doppelte Schreiben der Read-Modify-Write-Befehle
  (alter Wert, dann neuer). Nicht nachgebildet: das Lesen der falschen Adresse bei indizierten Zugriffen mit
  Seitenüberschreitung und die übrigen Scheinlesezugriffe (sichtbar nur für Lese-Hooks auf I/O-Register).
- **`JSR`** holt das High-Byte des Ziels erst nach dem Stapeln der Rücksprungadresse (wie die Hardware; wichtig, wenn das
  Stapeln das eigene Operandenbyte überschreibt).
- **Zyklen** pro Befehl: Grundzyklen + Seitenüberschreitung (lesend indiziert) + genommene Verzweigung (+1, über eine
  Seite +2); Gesamtzähler im Zustand. Interrupt-Annahme 7 Zyklen.
- **Interrupts:** IRQ (Pegel, maskierbar mit `I`), NMI (Flanke), RESET, `BRK`; Verzögerung der Annahme nach `CLI`/`SEI`/
  `PLP` um einen Befehl; `D` bleibt bei Interrupts unverändert (NMOS).
- **Prozessorport** `$00/$01` (abschaltbar = reiner 6502): Richtungs- und Datenregister, Eingänge mit festem Wert; jede
  Änderung der Ausgänge meldet die `Cpu` an die Maschine (Speicherkonfiguration des C64).

## Bedienung

Die Namen folgen den Schwesterprojekten, damit Sitzung, Monitor und Orchestrator gleich aufgebaut werden können:

| Aufruf | Wirkung |
|---|---|
| `powerOn()` | Einschaltwerte (`A` = `X` = `Y` = 0, `P` = `$24`, `S` = `$FD` nach dem Reset), Zähler auf 0, dann `reset()` |
| `reset()` | wie die RESET-Leitung: `I` = 1, `S` − 3, `PC` aus `$FFFC`, Prozessorport auf Eingang |
| `step()` | ein Befehl (oder eine anstehende Interrupt-Annahme); liefert die Zyklen |
| `run(n)` | höchstens `n` Befehle; stoppt an Breakpoints, Watchpoints, `JAM`; der Befehl am Startpunkt läuft auch, wenn dort ein Breakpoint sitzt |
| `runUntil(adresse, n)` | bis `pc == adresse` (vor dem Befehl geprüft) |
| `runUntilReturn(n)` | bis die laufende Routine zurückkehrt (Aufruftiefe: `JSR`/Interrupts zählen, `RTS`/`RTI` zählen ab) |
| `stepOver(n)` | wie `step()`, aber `JSR` läuft bis zur Rückkehr |
| `call(einstieg, rücksprung, n)` | stapelt `rücksprung − 1` (wie `JSR`) und läuft bis `pc == rücksprung`; `Returned` nur, wenn `S` dann wieder den Wert vor dem Aufruf hat (sonst `Reached`) |
| `setIrq(aktiv)`, `nmi()` | IRQ-Leitung (Pegel) bzw. NMI-Flanke |
| `portHook`, `setPortEnabled`, `setPortInputs` | Prozessorport: Rückruf bei Änderung der Ausgänge, abschalten (reiner 6502), Wert der Eingänge (Standard `$17`) |
| `addBreakpoint`, `addWatchpoint(start, länge, lesen, schreiben)` | Haltepunkte; ein Watchpoint hält **nach** dem Befehl, der zugreift |
| `traceHook`, `accessHook` | Rückrufe vor jedem Befehl bzw. bei jedem Datenzugriff |
| `executeHook`, `hookLow`/`hookHigh` | statt eines Befehls aufgerufen, wenn der PC im Bereich liegt (KERNAL-Emulation, [c64.md](c64.md)); `returnFromSubroutine()` und `requestStop(text)` für den Haken |
| Bus (`read`/`write`) | Zugriffe gehen über die Maschine (RAM/ROM/I/O); `peek()` liest ohne Seiteneffekte |
| `registers()`, `state()`, `setState()` | Zustand (Register, Zähler für Befehle/Zyklen/Interrupts, Aufruftiefe, Interrupt-Leitungen, Prozessorport) – Grundlage der Snapshots |
| `push()`, `pull()`, `addCycles()` | Hilfen für Hooks und die Sitzung |

Die Ergebnisse von `run…` nennen den Grund (`StopReason`): Limit, Breakpoint, Watchpoint, Reached, Returned, Halted (JAM),
Requested.

## Genauigkeit

**Register, Speicher und Zyklen pro Befehl zählen.** Nicht modelliert:

- der Zeitpunkt einzelner Buszugriffe innerhalb eines Befehls (wichtig für Raster-Effekte, Sprite-Multiplexer, „stabilen
  Raster“) und die Zyklen, die der VIC-II dem Prozessor stiehlt (Badlines, Sprite-DMA);
- die Zustände der Geräte über das hinaus, was [c64.md](c64.md#geräte-register-mit-schreibprotokoll) beschreibt;
- das Verhalten der instabilen Opcodes bei echter Hardware-Streuung: der Kern legt sich auf **eine** Variante fest
  (Tabelle unten, nach der Umsetzung ausgefüllt).

| Fall | Verhalten |
|---|---|
| `ANE #imm` (`$8B`) | `A = (A \| $EE) & X & imm` (die Konstante `$EE` erwartet Tom Hartes Testsatz; echte Chips streuen) |
| `LXA #imm` (`$AB`) | `A = X = (A \| $EE) & imm` |
| `LAS abs,Y` | `A = X = S = Wert & S` |
| `SHA`, `SHX`, `SHY`, `TAS` | Wert `& (High-Byte der Zieladresse + 1)`; bei Seitenüberschreitung wird auch das High-Byte der Adresse verändert |
| `JAM` | hält an; `PC` bleibt auf dem Opcode |
| Register nach dem Einschalten (`powerOn`) | `A`, `X`, `Y` = 0, `P` = `$24` (I gesetzt, Bit 5), `S` = `$FD` |
| Schreiben auf `$00/$01` | Wert geht in den Port **und** ins RAM darunter |
| Lesen von `$01` | `(Daten & Richtung) \| (Eingänge & ~Richtung)`, Eingänge `$17`; nach dem KERNAL-Start (`$00 = $2F`, `$01 = $37`) liest sich `$37`. Das „Verblassen“ nicht angesteuerter Bits wird nicht nachgebildet |
| NMI während `BRK`/IRQ | keine Übernahme des Vektors (vereinfacht) |

### Wie der Kern geprüft wird

Wie bei den Schwesterprojekten (ZEXDOC/ZEXALL, Musashi) gegen unabhängige Quellen, alle zur Laufzeit geholt und nicht ins
Repository aufgenommen ([testing.md](testing.md)):

1. **Tom Hartes Einzelbefehls-Tests** (SingleStepTests/65x02, Satz „6502“, MIT; `tools/harte_runner.cpp`, CI-Job
   `singlestep`): je Opcode 10 000 Fälle mit Anfangs- und Endzustand. Verglichen werden alle Register, jedes aufgeführte
   Speicherbyte und die **Zahl der Zyklen**; die Reihenfolge der einzelnen Buszugriffe nicht. Ergebnis: **2 440 000 Fälle,
   0 Abweichungen** (alle 244 Opcodes außer den 12 `JAM`, also auch alle undokumentierten, Dezimalmodus inklusive). Der Test
   fand einen Fehler, der behoben ist: die Reihenfolge, in der `JSR` sein Ziel-High-Byte holt.
2. **Klaus Dormanns Funktionstest** (`6502_functional_test.bin`, GPL-3.0, zur Laufzeit geholt; `tools/dormann_runner.cpp`,
   CI-Job `functional`): endet nach 30,6 Millionen Befehlen an der Erfolgsadresse `$3469`. Der Dezimaltest liegt nur als
   Quelltext vor; den Dezimalmodus decken die Einzelbefehls-Tests ab (160 000 Fälle für `ADC`/`SBC`, dazu `ARR`, `RRA`, `ISC`).
3. **Testsuite von Wolfgang Lorenz** (C64-Programme je Befehl/Thema) auf der C64-Maschine mit KERNAL-Emulation – geplant,
   sobald die Maschine steht.
4. **Differenztest** gegen eine unabhängige Referenz (Kandidat: perfect6502) – geplant; durch den vollständigen Einzelbefehls-
   Testsatz weniger dringend als beim MC68000.
5. **Unit-Tests** (`tests/test_cpu.cpp`, rund 350 Prüfungen): von Hand gerechnete Ergebnisse und Flags, Seitenfehler,
   Zyklen, Dezimalmodus, Stapel, `BRK`/IRQ/NMI mit der Verzögerung nach `CLI`, `JAM`, Breakpoints/Watchpoints/`runUntil`/
   `stepOver`/`call` (auch das falsche „returned“), Hooks (auch das doppelte Schreiben von `INC $D019`), Prozessorport,
   Zustand speichern/zurückspielen – und ein Abgleich mit dem Dekoder für alle Opcodes (Länge und Ablaufart).

## Plattformen

Der Kern kennt keine bestimmte Maschine. Zielplattform ist der **Commodore 64** ([c64.md](c64.md)): `m6510::C64` bildet die
Speicherkarte mit Banking, die Bausteinregister mit Schreibprotokoll und den Rasterzähler nach; `m6510::Kernal` die
KERNAL-Aufrufe.

## Snapshots

`capture(machine)` / `restore(machine, snapshot)` sichern und setzen Prozessor- und Maschinenzustand; `serialize()` /
`deserializeSnapshot()` schreiben/lesen die Datei (`M6510SN1`, [formats.md](formats.md#snapshot-m6510sn1)). Das
Zurückspielen muss **bit-identisch** sein: ein erneuter Lauf liefert dieselben Register, Zyklen und denselben Speicher
(Test). Haltepunkte und Hooks gehören nicht zum Zustand.
