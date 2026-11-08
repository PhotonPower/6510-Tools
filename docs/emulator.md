# Emulator

> **Stand 0.1:** geplant (Meilenstein 0.3). Dieses Dokument legt Umfang, Schnittstelle und Prüfung fest; nach der
> Umsetzung beschreibt es das tatsächliche Verhalten, einschließlich aller Stellen, an denen der Kern sich bewusst für
> eine Variante entscheidet.

`m6510::Cpu` (`include/m6510/cpu.hpp`) wird ein **befehlsgenauer** NMOS-6510-Kern: Für jeden Befehl stimmen Register, Flags,
Speicherinhalt und die **Zahl der Zyklen**; einzelne Buszyklen werden nicht modelliert (Entscheidung in der
[Roadmap](roadmap.md#entscheidungen-vorschlag--bitte-bestätigen)).

## Umfang

- **Alle 256 Opcodes**: dokumentierte, stabile und instabile undokumentierte; `JAM` hält den Prozessor an
  (`halted()`, nur RESET oder `set PC` macht weiter). Welche Opcodes als Befehl gelten, entscheidet dieselbe Tabelle wie
  beim Disassembler; für die Ausführung gibt es keine „unerlaubten“ Opcodes.
- **Dezimalmodus** nach NMOS-Art, auch für `ARR`, `RRA`, `ISC` und `USBC`, inklusive der Flags bei ungültigen BCD-Ziffern.
- **Seiteneigenheiten:** `JMP ($xxFF)`, Zero-Page-Umlauf bei `zp,X`/`zp,Y`/`(zp,X)`/`(zp),Y` (`Memory::read16Wrapped`).
- **Dummy-Zugriffe**, soweit sie für Geräte sichtbar sind: das doppelte Schreiben der Read-Modify-Write-Befehle, das
  Lesen der falschen Adresse bei indizierten Zugriffen mit Seitenüberschreitung (für Lese-Hooks; Option, Standard aus).
- **Zyklen** pro Befehl: Grundzyklen + Seitenüberschreitung (lesend indiziert) + genommene Verzweigung (+1, über eine
  Seite +2); Gesamtzähler im Zustand. Interrupt-Annahme 7 Zyklen.
- **Interrupts:** IRQ (Pegel, maskierbar mit `I`), NMI (Flanke), RESET, `BRK`; Verzögerung der Annahme nach `CLI`/`SEI`/
  `PLP` um einen Befehl; `D` bleibt bei Interrupts unverändert (NMOS).
- **Prozessorport** `$00/$01` (abschaltbar = reiner 6502): Richtungs- und Datenregister, Eingänge mit festem Wert; jede
  Änderung der Ausgänge meldet die `Cpu` an die Maschine (Speicherkonfiguration des C64).

## Bedienung (geplante Schnittstelle)

Die Namen folgen den Schwesterprojekten, damit Sitzung, Monitor und Orchestrator gleich aufgebaut werden können:

| Aufruf | Wirkung |
|---|---|
| `reset()` | wie die RESET-Leitung: `I` = 1, `S` − 3, `PC` aus `$FFFC` |
| `step()` | ein Befehl (oder eine anstehende Interrupt-Annahme); liefert die Zyklen |
| `run(n)` | höchstens `n` Befehle; stoppt an Breakpoints, Watchpoints, `JAM`; der Befehl am Startpunkt läuft auch, wenn dort ein Breakpoint sitzt |
| `runUntil(adresse, n)` | bis `pc == adresse` (vor dem Befehl geprüft) |
| `runUntilReturn(n)` | bis die laufende Routine zurückkehrt (Aufruftiefe: `JSR`/Interrupts zählen, `RTS`/`RTI` zählen ab) |
| `stepOver(n)` | wie `step()`, aber `JSR` läuft bis zur Rückkehr |
| `call(einstieg, rücksprung, n)` | stapelt `rücksprung − 1` (wie `JSR`) und läuft bis `pc == rücksprung` **und** `S` wieder dem Wert vor dem Aufruf entspricht |
| `irq(level)`, `nmi()` | Interrupt-Leitungen |
| `addBreakpoint`, `addWatchpoint(start, länge, lesen, schreiben)` | Haltepunkte; ein Watchpoint hält **nach** dem Befehl, der zugreift |
| `traceHook`, `accessHook` | Rückrufe vor jedem Befehl bzw. bei jedem Datenzugriff |
| `executeHook`, `hookLow`/`hookHigh` | statt eines Befehls aufgerufen, wenn der PC im Bereich liegt (KERNAL-Emulation, [c64.md](c64.md)); `returnFromSubroutine()` und `requestStop(text)` für den Haken |
| Bus (`read`/`write`) | Zugriffe gehen über die Maschine (RAM/ROM/I/O); `peek()` liest ohne Seiteneffekte |
| `registers()`, `state()`, `setState()` | Zustand (Register, Zähler, anstehende Interrupts, Prozessorport) – Grundlage der Snapshots |

Die Ergebnisse von `run…` nennen den Grund (`StopReason`): Limit, Breakpoint, Watchpoint, Reached, Returned, Halted (JAM),
Requested.

## Genauigkeit

**Register, Speicher und Zyklen pro Befehl zählen.** Nicht modelliert:

- der Zeitpunkt einzelner Buszugriffe innerhalb eines Befehls (wichtig für Raster-Effekte, Sprite-Multiplexer, „stabilen
  Raster“) und die Zyklen, die der VIC-II dem Prozessor stiehlt (Badlines, Sprite-DMA);
- die Zustände der Geräte über das hinaus, was [c64.md](c64.md#geräte-register-mit-schreibprotokoll) beschreibt;
- das Verhalten der instabilen Opcodes bei echter Hardware-Streuung: der Kern legt sich auf **eine** Variante fest
  (Tabelle unten, nach der Umsetzung ausgefüllt).

| Fall | Verhalten (wird festgelegt) |
|---|---|
| `ANE #imm` (`$8B`) | `A = (A \| magic) & X & imm`, `magic` = siehe [offene Fragen](roadmap.md#offene-fragen) |
| `LXA #imm` (`$AB`) | `A = X = (A \| magic) & imm` |
| `SHA`, `SHX`, `SHY`, `TAS` | Wert `& (High-Byte der Zieladresse + 1)`; bei Seitenüberschreitung wird auch das High-Byte der Adresse verändert |
| `JAM` | hält an; `PC` bleibt auf dem Opcode |
| Register nach RESET | `A`, `X`, `Y` = 0, `P` = `$24` (I gesetzt, Bit 5), `S` = `$FD` |
| Schreiben auf `$00/$01` | Wert geht in den Port **und** ins RAM darunter |

### Wie der Kern geprüft wird

Wie bei den Schwesterprojekten (ZEXDOC/ZEXALL, Musashi) gegen unabhängige Quellen, alle zur Laufzeit geholt und nicht ins
Repository aufgenommen ([testing.md](testing.md)):

1. **Tom Hartes Einzelbefehls-Tests** (SingleStepTests, Satz „6502“): je Opcode 10 000 Fälle mit Anfangs- und Endzustand
   (Register, berührter Speicher) und der Liste der Buszyklen – also auch Zyklenzahl und Dummy-Zugriffe. Alle 256 Opcodes,
   auch die undokumentierten.
2. **Klaus Dormanns Funktionstest** (`6502_functional_test`) und **Dezimaltest** (`6502_decimal_test`): ganze Programme,
   die bei Erfolg an einer bekannten Adresse in einer Endlosschleife stehen.
3. **Testsuite von Wolfgang Lorenz** (C64-Programme, eins je Befehl/Thema, inklusive undokumentierter Opcodes und
   Interrupt-Eigenheiten) auf der C64-Maschine mit KERNAL-Emulation.
4. **Differenztest** gegen eine unabhängige Referenz (Kandidat: perfect6502, eine Simulation auf Transistorebene; Lizenz
   vor Einsatz prüfen): zufällige Zustände und Programme, Vergleich nach jedem Befehl – entspricht dem Musashi-Test des
   MC68000-Projekts.
5. **Unit-Tests** (`tests/test_cpu.cpp`): von Hand gerechnete Ergebnisse und Flags, alle Adressierungsarten, Seitenfehler,
   Dezimalmodus, Interrupts, Hooks, Ablaufsteuerung – und ein Abgleich mit dem Dekoder für alle 256 Opcodes (Länge und
   Ablaufart stimmen überein).

## Plattformen

Der Kern kennt keine bestimmte Maschine. Zielplattform ist der **Commodore 64** ([c64.md](c64.md)): `m6510::C64` bildet die
Speicherkarte mit Banking, die Bausteinregister mit Schreibprotokoll und den Rasterzähler nach; `m6510::Kernal` die
KERNAL-Aufrufe.

## Snapshots

`capture(machine)` / `restore(machine, snapshot)` sichern und setzen Prozessor- und Maschinenzustand; `serialize()` /
`deserializeSnapshot()` schreiben/lesen die Datei (`M6510SN1`, [formats.md](formats.md#snapshot-m6510sn1)). Das
Zurückspielen muss **bit-identisch** sein: ein erneuter Lauf liefert dieselben Register, Zyklen und denselben Speicher
(Test). Haltepunkte und Hooks gehören nicht zum Zustand.
