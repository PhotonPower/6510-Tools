# 6510 Tools

[![CI](https://github.com/PhotonPower/6510-Tools/actions/workflows/ci.yml/badge.svg)](https://github.com/PhotonPower/6510-Tools/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Assembler, Disassembler, **Emulator** und **Maschinensprache-Monitor** für den MOS 6510 in modernem C++20 – das dritte
Projekt der Familie nach [PhotonPower/Z80](https://github.com/PhotonPower/Z80) und
[PhotonPower/MC68000](https://github.com/PhotonPower/MC68000), mit derselben Architektur: eine Bibliothek ohne
Ein-/Ausgabe (`m6510core`), ein Kommandozeilenwerkzeug (`m6510tool`) und ein Emulator als Ausführungsumgebung für einen
lokalen LLM-Agenten, der Programme analysiert und kommentiert.

> **Zielplattform:** Commodore **64** (MOS 6510, PAL) – siehe [docs/c64.md](docs/c64.md).

> **Status: Version 0.1 – Projektgerüst.** Build, Tests, CI, Kernbausteine (`types`, `format`, `memory`) und die
> vollständige Dokumentation des geplanten Umfangs sind da. Alles Weitere ist **geplant**; die Reihenfolge steht in der
> [Roadmap](docs/roadmap.md). Die Dokumente unter `docs/` beschreiben den Zielzustand und sagen jeweils oben, was davon
> schon umgesetzt ist.

## Geplanter Umfang

Derselbe Umfang wie bei den Schwesterprojekten, übertragen auf den 6510 und den C64:

**Assembler** (`m6510tool asm`, [docs/assembler-syntax.md](docs/assembler-syntax.md))
- alle 151 dokumentierten Opcodes (56 Befehle, 13 Adressierungsarten), auf Wunsch auch die **undokumentierten** des NMOS-6510
  (`LAX`, `SAX`, `DCP`, `ISC`, `SLO`, `RLA`, `SRE`, `RRA`, `ANC`, `ALR`, `ARR`, `SBX`, …)
- Labels mit Vorwärtsreferenzen, lokale Labels, `EQU`/`=`, `ORG`/`*=` (mehrfach), `.BYTE`/`.WORD`/`.TEXT`/`.FILL`, PETSCII- und
  Bildschirmcode-Strings; Zero Page oder absolut wird **automatisch im Mehrpass-Verfahren** gewählt
- Ausdrücke mit `<` (Low-Byte), `>` (High-Byte), `*` (aktuelle Adresse); Zahlen als `123`, `$1F`, `0x1F`, `%1010`, `'A'`
- Ausgabe: **PRG** (mit Ladeadresse), Raw-Binary, Listing, Symboltabelle, VICE-Labeldatei; **BASIC-Starter** (`10 SYS 2061`)
  per Pseudobefehl

**Disassembler** (`m6510tool dis`, [docs/usage.md](docs/usage.md))
- Ablaufanalyse: folgt `JSR`, `JMP`, Verzweigungen und `JMP (…)`-Vektoren ab den Einstiegspunkten und trennt Code von Daten;
  alternativ linearer Durchlauf
- Labels, Querverweise, Kommentardatei (`Adresse,"Text"`), KERNAL-Einsprünge und I/O-Register als Namen (`JSR CHROUT`,
  `STA EXTCOL`), **selbstmodifizierender Code** als `label+1`
- Ausgabe als lesbares Listing **oder** als wieder assemblierbarer Quelltext (`--asm`, byte-genau)
- Eingabe: PRG, P00, Raw, Datei aus einem **D64**/**T64**, Cartridge (**CRT**); BASIC-Programme werden detokenisiert angezeigt

**Emulator** (`m6510::Cpu`, [docs/emulator.md](docs/emulator.md))
- alle 256 Opcodes inklusive der undokumentierten (JAM hält an), NMOS-**Dezimalmodus** mit seinen Flag-Eigenheiten,
  `JMP ($xxFF)`-Fehler, Dummy-Zugriffe von Read-Modify-Write-Befehlen; **Zyklen pro Befehl** (Seitenüberschreitung, genommene
  Verzweigungen)
- IRQ, NMI, RESET, `BRK`; Prozessorport `$00/$01` mit der Speicherumschaltung des C64
- Einzelschritt, Breakpoints, Watchpoints, `runUntil`/`runUntilReturn`/`stepOver`/`call`, Speicher- und Ausführungs-Hooks
- **geprüft** gegen öffentliche Einzelbefehls-Testsätze (Tom Harte), Klaus Dormanns Funktionstest, die Testsuite von
  Wolfgang Lorenz und einen Differenztest gegen eine unabhängige Referenz ([docs/testing.md](docs/testing.md))

**Commodore 64** ([docs/c64.md](docs/c64.md))
- Speicherkarte mit allen Konfigurationen von `$01` (BASIC/KERNAL/Zeichensatz-ROM, I/O), Farb-RAM, VIC-II/SID/CIA-Register als
  Speicher mit **Schreibprotokoll** und Registernamen, Rasterzähler
- **KERNAL-Emulation** ohne ROM (`CHROUT`, `GETIN`, `PLOT`, `SETLFS`/`SETNAM`/`LOAD` aus einem Disk-Image, …); alles andere wird
  protokolliert. Echte ROMs (eigene Kopie, nie im Repository) lassen sich laden
- **Bildschirm als Bild** (Textmodi, Bitmap, Multicolor, später Sprites) für ein Modell, das Bilder versteht
- Dateiformate: PRG, P00, T64, D64, CRT; eigenes Snapshot-Format

**Agenten-Schnittstelle** (`m6510tool serve`, [docs/agent-protocol.md](docs/agent-protocol.md))
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

./build/m6510tool version
./build/m6510tool help
```

Geplant (ab Meilenstein 0.2–0.4):

```sh
./build/m6510tool asm examples/hello.asm -l - -o hello.prg      # assemblieren, Listing auf den Bildschirm
./build/m6510tool dis hello.prg -c examples/hello.comments.txt  # disassemblieren (Ladeadresse aus dem PRG)
./build/m6510tool dis hello.prg --asm                           # als wieder assemblierbarer Quelltext
./build/m6510tool monitor hello.prg                             # interaktiver Monitor
./build/m6510tool serve < examples/agent_session.jsonl          # Emulator per JSON-Protokoll
```

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

## Beispiel (Ziel für Meilenstein 0.2)

```console
$ m6510tool asm examples/hello.asm -o hello.prg
examples/hello.asm: 33 bytes, $0801 - $0821, 0 error(s)

$ m6510tool dis hello.prg -c examples/hello.comments.txt --no-bytes
; 6510 disassembly  $0801 - $0821
0801     .BASIC 10,start            ; 10 SYS2061
start:
080D     LDX #$00                   ; entry: print the message below
next:                               ; <- $0818
080F     LDA message,X              ; fetch the next character
0812     BEQ done
0814     JSR CHROUT
0817     INX
0818     BNE next
done:                               ; <- $0812
081A     RTS
message:                            ; <- $080F
081B     .BYTE "HELLO",$0D,$00
```

(Adressen und Größe stehen erst fest, wenn der Assembler existiert; das Beispielprogramm liegt schon in `examples/`.)

## Projektstruktur

```
include/m6510/ öffentliche Header der Bibliothek (m6510core)
src/           Implementierung
app/main.cpp   Kommandozeilenwerkzeug m6510tool
agent/         Orchestrator für Ollama (Python, nur Standardbibliothek) – geplant
tests/         Unit-Tests (CTest); tests/agent: Orchestrator gegen einen Mock-Ollama – geplant
tools/         Entwicklerwerkzeuge: CI-Annotationen; geplant: Kreuzprüfung gegen da65/64tass, Testsatz-Runner, Differenztest
examples/      Beispielprogramm mit Kommentardatei
docs/          Dokumentation (Deutsch)
```

## Lizenz

[MIT](LICENSE)
