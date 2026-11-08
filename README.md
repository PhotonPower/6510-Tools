# 6510 Tools

[![CI](https://github.com/PhotonPower/6510-Tools/actions/workflows/ci.yml/badge.svg)](https://github.com/PhotonPower/6510-Tools/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Assembler, Disassembler, **Emulator** und **Maschinensprache-Monitor** für den MOS 6510 in modernem C++20 – das dritte
Projekt der Familie nach [PhotonPower/Z80](https://github.com/PhotonPower/Z80) und
[PhotonPower/MC68000](https://github.com/PhotonPower/MC68000), mit derselben Architektur: eine Bibliothek ohne
Ein-/Ausgabe (`m6510core`), ein Kommandozeilenwerkzeug (`m6510tool`) und ein Emulator als Ausführungsumgebung für einen
lokalen LLM-Agenten, der Programme analysiert und kommentiert.

> **Zielplattform:** Commodore **64** (MOS 6510, PAL) – siehe [docs/c64.md](docs/c64.md).

> **Status: Meilenstein 0.4.** Assembler und Disassembler (0.2), Emulator, C64-Maschine mit KERNAL-Emulation,
> Snapshots und die Agenten-Schnittstelle `m6510tool serve` (0.3) sowie der Monitor (0.4) sind fertig und geprüft:
> exhaustiver Roundtrip Dekoder/Assembler, Kreuzprüfung gegen ca65/da65, **alle 2,44 Millionen Einzelbefehls-Tests von Tom
> Harte ohne Abweichung** und Klaus Dormanns Funktionstest in der CI. Reverse Engineering (0.5) und Disk-Images (0.6) folgen
> ([Roadmap](docs/roadmap.md)). Die Dokumente unter `docs/` sagen jeweils oben, was davon schon umgesetzt ist.

## Umfang

Derselbe Umfang wie bei den Schwesterprojekten, übertragen auf den 6510 und den C64. **Fertig** sind Assembler,
Disassembler, Emulator, C64-Maschine und Agenten-Schnittstelle (Meilensteine 0.2 und 0.3); der Rest ist geplant:

**Assembler** (`m6510tool asm`, [docs/assembler-syntax.md](docs/assembler-syntax.md)) – *fertig*
- alle 151 dokumentierten Opcodes (56 Befehle, 13 Adressierungsarten), auf Wunsch auch die **undokumentierten** des NMOS-6510
  (`LAX`, `SAX`, `DCP`, `ISC`, `SLO`, `RLA`, `SRE`, `RRA`, `ANC`, `ALR`, `ARR`, `SBX`, …)
- Labels mit Vorwärtsreferenzen, lokale Labels, `EQU`/`=`, `ORG`/`*=` (mehrfach), `.BYTE`/`.WORD`/`.TEXT`/`.FILL`, PETSCII- und
  Bildschirmcode-Strings; Zero Page oder absolut wird **automatisch im Mehrpass-Verfahren** gewählt
- Ausdrücke mit `<` (Low-Byte), `>` (High-Byte), `*` (aktuelle Adresse); Zahlen als `123`, `$1F`, `0x1F`, `%1010`, `'A'`
- Ausgabe: **PRG** (mit Ladeadresse), Raw-Binary, Listing, Symboltabelle, VICE-Labeldatei; **BASIC-Starter** (`10 SYS2061`)
  per Pseudobefehl `.BASIC`

**Disassembler** (`m6510tool dis`, [docs/usage.md](docs/usage.md)) – *fertig bis auf D64/T64/CRT und I/O-Namen*
- Ablaufanalyse: folgt `JSR`, `JMP` und Verzweigungen ab den Einstiegspunkten (Standard: die `SYS`-Adresse des
  BASIC-Starters) und trennt Code von Daten; alternativ linearer Durchlauf. Geplant (0.5): `JMP (…)`-Vektoren,
  `RTS`-Sprungtabellen
- Labels, Querverweise, Kommentardatei (`Adresse,"Text"`), Symboldateien (auch VICE-Labels), KERNAL-Einsprünge als Namen
  (`JSR CHROUT`; I/O-Register wie `STA EXTCOL` geplant), **selbstmodifizierender Code** als `label+1`
- Ausgabe als lesbares Listing **oder** als wieder assemblierbarer Quelltext (`--asm`, byte-genau)
- Eingabe: PRG, P00, Raw (geplant: Datei aus einem **D64**/**T64**, Cartridge **CRT**); `m6510tool basic list` detokenisiert
  BASIC-Programme

**Emulator** (`m6510::Cpu`, [docs/emulator.md](docs/emulator.md)) – *fertig*
- alle 256 Opcodes inklusive der undokumentierten (JAM hält an), NMOS-**Dezimalmodus** mit seinen Flag-Eigenheiten,
  `JMP ($xxFF)`-Fehler, Dummy-Zugriffe von Read-Modify-Write-Befehlen; **Zyklen pro Befehl** (Seitenüberschreitung, genommene
  Verzweigungen)
- IRQ, NMI, RESET, `BRK`; Prozessorport `$00/$01` mit der Speicherumschaltung des C64
- Einzelschritt, Breakpoints, Watchpoints, `runUntil`/`runUntilReturn`/`stepOver`/`call`, Speicher- und Ausführungs-Hooks
- **geprüft** gegen Tom Hartes Einzelbefehls-Tests (2 440 000 Fälle: Register, Speicher, Zyklen; 0 Abweichungen) und Klaus
  Dormanns Funktionstest, beide in der CI ([docs/testing.md](docs/testing.md))

**Commodore 64** ([docs/c64.md](docs/c64.md)) – *fertig bis auf Bildschirmbild und Images*
- Speicherkarte mit allen Konfigurationen von `$01` (BASIC/KERNAL/Zeichensatz-ROM, I/O), Farb-RAM, VIC-II/SID/CIA-Register als
  Speicher mit **Schreibprotokoll** und Registernamen; Rasterzeile und CIA-1-Timer A aus dem Zyklenzähler (Raster- und
  Timer-IRQs)
- **KERNAL-Emulation** ohne ROM: echte Sprungtabelle über die RAM-Vektoren, `CHROUT` auf Konsole und Bildschirm, `GETIN`,
  `PLOT`, `LOAD`/`SAVE`, Jiffy-Uhr über den IRQ, …; alles andere wird protokolliert. Echte ROMs (eigene Kopie, nie im
  Repository) lassen sich laden
- geplant: **Bildschirm als Bild** (Textmodi, Bitmap, Multicolor, später Sprites) für ein Modell, das Bilder versteht (0.5);
  T64, D64, CRT (0.6). Fertig: PRG, P00, eigenes Snapshot-Format

**Agenten-Schnittstelle** (`m6510tool serve`, [docs/agent-protocol.md](docs/agent-protocol.md)) – *fertig bis auf Analyse und Screenshots*
- JSON-Zeilenprotokoll mit denselben Befehlsnamen wie `z80tool serve` und `m68ktool serve`: assemblieren, disassemblieren,
  Speicher, Register, Lauf/`call`, Breakpoints, Watchpoints, Checkpoints mit Rollback und Speicher-Diff, Snapshots,
  KERNAL-Aufrufe, I/O-Protokoll, Screenshots; Dateizugriff nur mit `--allow-files`

**Reverse Engineering** ([docs/reverse-engineering.md](docs/reverse-engineering.md))
- Codefluss-Analyse mit Routinen und Aufrufgraph, Sprungtabellen (auch der `RTS`-Trick und getrennte Low-/High-Byte-Tabellen),
  Datentypen (PETSCII, Bildschirmcodes, Sprites, Zeichensätze), Annotationen und ein **verifiziertes Listing**: aus den echten
  Bytes erzeugt, neu assembliert und Byte für Byte verglichen
- **Orchestrator** `agent/m6510_annotate.py` für ein lokales Modell über **Ollama** und `agent/m6510_drive.py` für die
  Bedienung von Hand ([docs/walkthrough.md](docs/walkthrough.md))
- selbstentpackende Programme (Exomizer & Co.) bis zum entpackten Code laufen lassen ([docs/unpacking.md](docs/unpacking.md))

**Monitor** (`m6510tool monitor`, [docs/monitor.md](docs/monitor.md))
- klassische Eingabeschleife wie bei den Schwesterprojekten, Zahlen hexadezimal: disassemblieren, Speicher ansehen/ändern/
  füllen/vergleichen/kopieren/suchen, **Zeilen assemblieren**, Register, Einzelschritt/Step over/bis Rückkehr/laufen,
  Haltepunkte und Watchpoints, Symbole, Dateien, Checkpoints, Snapshots, C64-Speicherkonfiguration

## Schnellstart

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build

./build/m6510tool asm examples/hello.asm -l - -o hello.prg      # assemblieren, Listing auf den Bildschirm
./build/m6510tool dis hello.prg -c examples/hello.comments.txt  # disassemblieren (Ladeadresse aus dem PRG)
./build/m6510tool dis hello.prg --asm                           # als wieder assemblierbarer Quelltext
./build/m6510tool basic list hello.prg                          # BASIC-Teil detokenisiert
./build/m6510tool serve < examples/agent_session.jsonl          # Emulator per JSON-Protokoll (C64 mit KERNAL-Emulation)
```

```sh
./build/m6510tool monitor --c64 hello.prg                       # interaktiver Monitor (C64 mit KERNAL-Emulation)
./build/m6510tool monitor < examples/monitor_session.txt        # Monitor-Skript
```

Im Monitor zum Beispiel `d` (disassemblieren), `run` (starten wie `RUN`), `console` (Ausgabe), `r` (Register), `?` (Hilfe).

Voraussetzungen: CMake ≥ 3.16, C++20-Compiler (GCC ≥ 11, Clang ≥ 14, MSVC 2022).

## Dokumentation

| Dokument                                                   | Inhalt                                                     |
|------------------------------------------------------------|------------------------------------------------------------|
| [docs/architecture.md](docs/architecture.md)               | Aufbau des Codes, Komponenten, Besonderheiten des 6510     |
| [docs/c64.md](docs/c64.md)                                 | Zielplattform: Speicherkarte, Banking, I/O, KERNAL, Video  |
| [docs/instruction-set.md](docs/instruction-set.md)         | Befehlssatz: Register, Adressierung, Opcodes, Eigenheiten  |
| [docs/assembler-syntax.md](docs/assembler-syntax.md)       | Quelltextformat, Ausdrücke, Pseudobefehle                  |
| [docs/formats.md](docs/formats.md)                         | PRG, P00, T64, D64, CRT, Kommentardatei, Snapshot, BASIC   |
| [docs/usage.md](docs/usage.md)                             | Kommandozeile                                              |
| [docs/emulator.md](docs/emulator.md)                       | Emulator: Umfang, Genauigkeit, Zustand speichern           |
| [docs/monitor.md](docs/monitor.md)                         | Monitor: Befehle und Beispiel                              |
| [docs/agent-protocol.md](docs/agent-protocol.md)           | JSON-Protokoll für LLM-Agenten                             |
| [docs/reverse-engineering.md](docs/reverse-engineering.md) | Programme mit einem lokalen Modell kommentieren            |
| [docs/walkthrough.md](docs/walkthrough.md)                 | Anleitung: vom Programm zum kommentierten Listing          |
| [docs/unpacking.md](docs/unpacking.md)                     | Selbstentpackende Programme und Disk-Images                |
| [docs/testing.md](docs/testing.md)                         | Teststrategie und Kreuzprüfung                             |
| [docs/roadmap.md](docs/roadmap.md)                         | Meilensteine, Entscheidungen, offene Fragen                |
| [CONTRIBUTING.md](CONTRIBUTING.md)                         | Bauen, Konventionen                                        |
| [CHANGELOG.md](CHANGELOG.md)                               | Änderungen                                                 |

## Beispiel

`examples/hello.asm` assemblieren (mit Symboltabelle) und wieder zerlegen:

```console
$ m6510tool asm examples/hello.asm -o hello.prg -s hello.sym
examples/hello.asm: 33 bytes, $0801 - $0821, 0 error(s)

$ m6510tool dis hello.prg -c examples/hello.comments.txt --symbols hello.sym --no-bytes
; 6510 disassembly  $0801 - $0821

CHROUT = $FFD2

0801     .BASIC 10,start                      ; 10 SYS2061
start:
080D     LDX #$00                             ; entry: print the message below
next:                               ; <- $0818
080F     LDA message,X                        ; fetch the next character
0812     BEQ done
0814     JSR CHROUT
0817     INX
0818     BNE next
done:                               ; <- $0812
081A     RTS
message:                            ; <- $080F
081B     .BYTE "HELLO",$0D,$00                ; |HELLO..|
```

Ohne Symboltabelle heißen die Labels `L_080D` usw.; KERNAL-Einsprünge (`CHROUT`) und der BASIC-Starter werden immer
erkannt. Mit `--asm` entsteht Quelltext, der sich wieder zum identischen Programm assemblieren lässt:

```console
$ m6510tool dis hello.prg --asm -o re.asm && m6510tool asm re.asm -o re.prg && cmp hello.prg re.prg
```

## Projektstruktur

```
include/m6510/ öffentliche Header der Bibliothek (m6510core)
src/           Implementierung
app/main.cpp   Kommandozeilenwerkzeug m6510tool
agent/         Orchestrator für Ollama (Python, nur Standardbibliothek) – geplant
tests/         Unit-Tests (CTest); tests/agent: Orchestrator gegen einen Mock-Ollama – geplant
tools/         Entwicklerwerkzeuge: Formentabelle, Kreuzprüfung gegen ca65/da65, Runner für Hartes und Dormanns Tests, CI-Annotationen
examples/      Beispielprogramm mit Kommentardatei, Agenten-Sitzung
docs/          Dokumentation (Deutsch)
```

## Lizenz

[MIT](LICENSE)
