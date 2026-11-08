# Roadmap

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang folgen dem Weg der
> Schwesterprojekte [Z80](https://github.com/PhotonPower/Z80) und [MC68000](https://github.com/PhotonPower/MC68000);
> Ziel ist **derselbe Funktionsumfang**, übertragen auf den 6510 und den Commodore 64.

## Wo wir stehen

**Meilensteine 0.2 bis 0.5 sind erledigt:** Assembler, Disassembler, PRG/P00, BASIC-Detokenisierer (0.2); Prozessorkern
(2,44 Mio. Einzelbefehls-Tests ohne Abweichung, Dormanns Funktionstest), Snapshots, `m6510tool serve`, C64-Maschine und
KERNAL-Emulation (0.3); Monitor (0.4); Bildschirm als Bild, Analyse, verifiziertes Listing und Orchestrator für ein lokales
Modell (0.5). Als Nächstes: Meilenstein 0.6 (Disk-, Tape- und Cartridge-Images, Entpacken).

## Meilenstein 0.2 – Opcode-Tabelle, Dekoder, Assembler, Disassembler (erledigt)

Umgesetzt wie unten beschrieben, mit diesen Abweichungen: Der Assembler wird gegen `ca65` geprüft (nicht `64tass`);
Intel HEX ist zurückgestellt (niedrige Priorität); `JMP (vektor)` und Sprungtabellen verfolgt erst die Analyse in 0.5;
I/O-Registernamen im Disassembler kommen mit der C64-Maschine (0.3), weil die Namensfrage noch offen ist.

- **eine** Opcode-Tabelle mit 256 Einträgen (Mnemonic, Adressierungsart, Zyklen, Klasse: dokumentiert / undokumentiert
  stabil / instabil / JAM), gemeinsam benutzt von Dekoder, Encoder und Emulator ([architecture.md](architecture.md))
- `Instruction`, `decode(memory, address, options)`; Ausdrucksauswerter; `encodeInstruction()`; Mehrpass-Assembler mit
  Wahl Zero Page / absolut ([assembler-syntax.md](assembler-syntax.md))
- Pseudobefehle inklusive `.BASIC` (BASIC-Starter), PETSCII-/Bildschirmcode-Strings
- Disassembler mit Ablaufanalyse (`JSR`, `JMP`, `JMP (…)`, Verzweigungen, `BRK`/`RTS`/`RTI`/JAM als Ende), Querverweisen,
  Kommentardatei, `--asm`, Labels mitten im Befehl (`label+1`, selbstmodifizierender Code)
- Ein-/Ausgabe: Raw, **PRG**, P00, Listing, Symboltabelle, VICE-Labeldatei; BASIC-Detokenisierer (`m6510tool basic list`)
- Tests: Einzelbefehle, exhaustiver Roundtrip (alle 256 Opcodes × alle Operandenwerte), Robustheit, Kreuzprüfung gegen
  `da65` (Dekoder) und `ca65` (Assembler) ([testing.md](testing.md))

## Meilenstein 0.3 – Emulator, Sitzung, C64 (erledigt)

Umgesetzt wie unten beschrieben, mit diesen Abweichungen: Rasterzeile und CIA-1-Timer A laufen aus dem Zyklenzähler
(statt beim Lesen weiterzurücken), damit Raster- und Timer-IRQs funktionieren. Die Lorenz-Testsuite ist zurückgestellt (keine
stabile Quelle, siehe [emulator.md](emulator.md#wie-der-kern-geprüft-ist)); ein Differenztest gegen eine weitere Referenz
ist wegen des vollständigen Einzelbefehls-Testsatzes nicht mehr nötig und bleibt eine Idee. `screen` (Bildschirm als Text)
ist schon da, das Bild folgt mit 0.5.

- `Cpu`: alle 256 Opcodes, NMOS-Dezimalmodus, Zyklen pro Befehl, IRQ/NMI/RESET/`BRK`, Prozessorport `$00/$01`;
  Einzelschritt, Breakpoints, Watchpoints, `runUntil`/`runUntilReturn`/`stepOver`/`call`, Hooks ([emulator.md](emulator.md))
- Prüfung: Tom Hartes Einzelbefehls-Tests, Klaus Dormanns Funktions- und Dezimaltest, Differenztest gegen eine unabhängige
  Referenz; CI-Jobs dafür
- Snapshots (Speicher und Datei), Speicher-Diff
- `Session` und `m6510tool serve` (JSON-Zeilenprotokoll mit den Befehlsnamen der Schwesterprojekte,
  [agent-protocol.md](agent-protocol.md))
- `m6510::C64`: Speicherkarte mit allen `$01`-Konfigurationen, Farb-RAM, VIC-II/SID/CIA-Register als Speicher mit
  Schreibprotokoll und Registernamen, Rasterzähler, ROMs laden ([c64.md](c64.md))
- **KERNAL-Emulation** (`m6510::Kernal`): Sprungtabelle `$FF81–$FFF3` abgefangen, `CHROUT`/`GETIN`/`PLOT`/`RDTIM`/
  `SETLFS`/`SETNAM`/`LOAD`/… nachgebildet, alles andere protokolliert; Programme wie mit `RUN` starten (BASIC-Starter
  erkennen)
- Testsuite von Wolfgang Lorenz auf der C64-Maschine (mit KERNAL-Emulation)

## Meilenstein 0.4 – Monitor (erledigt)

Umgesetzt wie unten beschrieben, mit diesen Abweichungen: `shot` (Bildschirm als PNG) kommt mit dem Bild in 0.5, Laden aus
D64/T64 mit den Images in 0.6. Maschine, Symbole, Checkpoints und Watchpoints liegen jetzt in `m6510::Machine`, die Monitor
und Sitzung gemeinsam benutzen. Zusätzlich: `run` (Start wie `RUN`/`SYS`), `console`, `keys`, `irq on|off`.

- `m6510::Monitor` und `m6510tool monitor` mit den Befehlen aus [monitor.md](monitor.md) (wie bei den
  Schwesterprojekten, ergänzt um C64-Befehle: `bank`, `rom`, `io`, `kernal`, `screen`)

## Meilenstein 0.5 – Reverse Engineering (erledigt)

Umgesetzt wie unten beschrieben, mit diesen Abweichungen: Sprites werden im Bild noch nicht gezeigt (Idee); `bitmap`/`colors`
als Datenarten sind eine Idee. Zusätzlich: Interrupt-Handler, die das Programm über `$0314`/`$0318`/`$FFFE`/… einträgt,
werden Einstiegspunkte und laufen in `run_routine` mit Interrupt-Rahmen (`call` mit `frame`); Zeiger in Immediate-Paaren
(`LDA #<text / LDY #>text`) erscheinen im Listing als `#<name`/`#>name`. Die Erprobung mit einem echten Modell steht noch aus
(wie bei den Schwesterprojekten).

- `Analysis` (Codefluss, Routinen, Aufrufgraph, KERNAL-Aufrufe, I/O-Zugriffe), `Annotations`, **verifiziertes Listing**
- Protokollbefehle `analyze`, `routines`, `references`, `listing`, `jump_tables`, `data_regions`, `ann_*`
- Sprungtabellen: `JMP (vektor)`, **`RTS`-Trick** (Adresse−1 auf den Stapel, dann `RTS`), getrennte Low-/High-Byte-Tabellen
- Datentypen: PETSCII- und Bildschirmcode-Text, Zeigertabellen (auch getrennt), Sprites, Zeichensätze, Nullbereiche
- **Bildschirm als PNG** (VIC-II-Textmodi und Bitmap aus dem Speicherzustand) für Modelle mit Bildverständnis
- Orchestrator `agent/m6510_annotate.py` (Ollama, Routinen von unten nach oben, Mehrdurchgang-Verfahren,
  Vollständigkeitsprüfung, Projektdatei, Bericht, Dry-Run) und `agent/m6510_drive.py`; Tests gegen einen Mock-Ollama
- Anleitung [walkthrough.md](walkthrough.md) mit einem Beispielprogramm, das alle Fälle enthält

## Meilenstein 0.6 – Disk-, Tape- und Cartridge-Images, Entpacken

- **D64** lesen (35/40 Spuren, mit/ohne Fehlerbytes; BAM, Verzeichnis, Dateiketten), `m6510tool d64 info|ls|extract|boot`;
  Kreuzprüfung gegen `c1541` (VICE)
- **T64** lesen, **CRT** lesen (zunächst 8K/16K/Ultimax; Bank-Umschaltung als Idee)
- `track_writes`, `written`, `run_until_unpacked`, `save_memory` (als PRG) für selbstentpackende Programme
  ([unpacking.md](unpacking.md))
- Ein selbstentpackendes Testprogramm (Packer in Python, Entpacker in 6510-Assembler) mit Integrationstest

## Danach (Ideen)

- Erprobung des Orchestrators mit einem echten Modell und Feinschliff von Prompt und Parametern (wie bei den
  Schwesterprojekten offen)
- Sprites, Rasterzeilen-Effekte (Registerstand pro Zeile aus dem Schreibprotokoll) im Bildschirmbild
- D64 **schreiben** (Assembler-Ausgabe direkt auf eine Diskette), weitere CRT-Typen (Ocean, Magic Desk, EasyFlash …)
- SID-Musik erkennen (Player-Routinen, `.sid`-Dateien lesen)
- Makros, `INCLUDE`, bedingte Assemblierung, anonyme Labels (`+`/`-`)
- 65C02/65816 – ausdrücklich **kein** Ziel, nur falls es sich später ergibt
- Zeileneditor mit Verlauf im Monitor, Tab-Vervollständigung, TUI

## Entscheidungen

Bestätigt am 2026-11-08 (Punkte 2, 5, 6 und 8 ausdrücklich, ebenso die offene Frage 1; die übrigen folgen daraus bzw. aus den Schwesterprojekten):

1. **Zielplattform:** Commodore 64, PAL ([c64.md](c64.md)). Der CPU-Kern bleibt maschinenunabhängig (auch für einen
   reinen 6502 nutzbar; der Prozessorport ist abschaltbar).
2. **Prozessor:** NMOS-6510 einschließlich **aller undokumentierten Opcodes** im Emulator und im Dekoder. Im Assembler
   und im Disassembler sind sie per Schalter wählbar (Standard: an, weil C64-Software sie häufig benutzt). Die instabilen
   Opcodes (`ANE`, `LXA`, `SHA`, `SHX`, `SHY`, `TAS`, `LAS`) bekommen ein festgelegtes, dokumentiertes Verhalten.
3. **Namen der undokumentierten Opcodes:** nach dem Dokument „No More Secrets“ (NMOS 6510 Unintended Opcodes);
   gängige Alternativen (`DCM`, `INS`, `ASO`, `LSE`, `AXS`, …) nimmt der Assembler als Synonyme an.
4. **Syntax:** übliche MOS-/Commodore-Schreibweise (`LDA ($FB),Y`, `$` für Hex, `*=`), Pseudobefehle mit Punkt
   (`.BYTE`, `.WORD`), dazu die Formen der Schwesterprojekte (`ORG`, `EQU`, `DB`, `DW`, `DS`) – ohne den Anspruch,
   einen bestimmten Fremd-Assembler vollständig nachzubilden.
5. **Genauigkeit:** Register, Speicher und **Zyklen pro Befehl** (inklusive Seitenüberschreitung und genommener
   Verzweigung) – das ist billig und durch die Testsätze prüfbar. **Kein** buszyklengenaues Timing, keine VIC-
   Badlines/Sprite-DMA, keine echten Geräte (wie beim MC68000).
6. **Geräte:** VIC-II, SID, CIAs nur als Register mit Schreibprotokoll und Namen; dazu ein Rasterzähler, der beim Lesen
   vorrückt (damit Warteschleifen nicht hängen), und das Rendern des Bildschirms **aus dem Speicherzustand** für
   Screenshots. Kein Ton, keine Diskettenlaufwerks-Emulation (Dateien kommen über die KERNAL-Emulation aus dem Image).
7. **Monitor:** Bedienung wie bei den Schwesterprojekten, keine Nachbildung historischer Monitore (Action Replay,
   SMON, VICE-Monitor); die VICE-Labeldatei wird aber unterstützt.
8. **Namen in der Bibliothek:** Namespace `m6510`, Bibliothek `m6510core`, Werkzeug `m6510tool`, CMake-Präfix `M6510_`,
   Python-Skripte `agent/m6510_*.py` (analog zu `m68k`).

## Offene Fragen

1. **ROMs in der CI** – *entschieden:* die CI läuft mit der KERNAL-Emulation; Tests mit echten ROMs laufen lokal
   (`C64_ROMS=<verzeichnis>`). Falls nötig, stellt der Projektinhaber eine **private** Quelle mit ROM-Abbildern eines
   eigenen C64 bereit; sie würde in der CI nur über ein GitHub-Secret angesprochen, nie im Repository oder im Log stehen.
2. **Registernamen** im I/O-Protokoll und im Disassembler – *umgesetzt wie vorgeschlagen:* die Namen aus „Mapping the
   Commodore 64“ (`EXTCOL`, `BGCOL0`, `SCROLY`, …). Lässt sich ändern, solange noch keine Projektdateien davon abhängen.
3. **Palette** für Screenshots – *umgesetzt wie vorgeschlagen:* Colodore (neuere Messung), Pepto per Option.
4. **Verhalten der instabilen Opcodes** – *entschieden:* `$EE` für `ANE` und `LXA`, wie in Tom Hartes Testsatz (aus den
   Testdaten ermittelt); die übrigen instabilen Opcodes ebenfalls nach dem Testsatz ([emulator.md](emulator.md)).
5. **Schreibzugriffe auf `$00/$01`** – *umgesetzt wie vorgeschlagen:* der geschriebene Wert landet auch im RAM
   (dokumentierte Vereinfachung).
