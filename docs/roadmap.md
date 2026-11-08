# Roadmap

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang folgen dem Weg der
> Schwesterprojekte [Z80](https://github.com/PhotonPower/Z80) und [MC68000](https://github.com/PhotonPower/MC68000);
> Ziel ist **derselbe Funktionsumfang**, übertragen auf den 6510 und den Commodore 64.

## Wo wir stehen

**Version 0.1 – Projektgerüst:** Build, Tests, CI, Dokumentation, `types`/`format`/`memory`, `m6510tool help|version`.

## Meilenstein 0.2 – Opcode-Tabelle, Dekoder, Assembler, Disassembler

- **eine** Opcode-Tabelle mit 256 Einträgen (Mnemonic, Adressierungsart, Zyklen, Klasse: dokumentiert / undokumentiert
  stabil / instabil / JAM), gemeinsam benutzt von Dekoder, Encoder und Emulator ([architecture.md](architecture.md))
- `Instruction`, `decode(memory, address, options)`; Ausdrucksauswerter; `encodeInstruction()`; Mehrpass-Assembler mit
  Wahl Zero Page / absolut ([assembler-syntax.md](assembler-syntax.md))
- Pseudobefehle inklusive `.BASIC` (BASIC-Starter), PETSCII-/Bildschirmcode-Strings
- Disassembler mit Ablaufanalyse (`JSR`, `JMP`, `JMP (…)`, Verzweigungen, `BRK`/`RTS`/`RTI`/JAM als Ende), Querverweisen,
  Kommentardatei, `--asm`, Labels mitten im Befehl (`label+1`, selbstmodifizierender Code)
- Ein-/Ausgabe: Raw, **PRG**, P00, Listing, Symboltabelle, VICE-Labeldatei; BASIC-Detokenisierer (`m6510tool basic list`)
- Tests: Einzelbefehle, exhaustiver Roundtrip (alle 256 Opcodes × alle Operandenwerte), Robustheit, Kreuzprüfung gegen
  `da65` (Dekoder) und `64tass`/`ca65` (Assembler) ([testing.md](testing.md))

## Meilenstein 0.3 – Emulator, Sitzung, C64

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

## Meilenstein 0.4 – Monitor

- `m6510::Monitor` und `m6510tool monitor` mit den Befehlen aus [monitor.md](monitor.md) (wie bei den
  Schwesterprojekten, ergänzt um C64-Befehle: `bank`, `rom`, `io`, `kernal`, `screen`)

## Meilenstein 0.5 – Reverse Engineering

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

## Entscheidungen (Vorschlag – bitte bestätigen)

Bei den Schwesterprojekten wurden diese Punkte direkt nach dem Gerüst entschieden. Die folgenden Vorschläge sind so
gewählt, dass sie zum Ziel (Reverse Engineering von C64-Programmen mit einem lokalen Modell) passen:

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

1. **ROMs in der CI:** Für die Schwesterprojekte holte die CI ein ROM mit Erlaubnis des Rechteinhabers (CPC). Für den C64
   gibt es keine solche Quelle. Optionen: (a) nur KERNAL-Emulation in der CI, (b) ein freier Ersatz wie die
   MEGA65-OpenROMs (Lizenz und Kompatibilität vorher prüfen), (c) Tests mit echten ROMs nur lokal (`C64_ROMS=…`).
   Vorschlag: (a) + (c).
2. **Registernamen** im I/O-Protokoll und im Disassembler: die Namen aus „Mapping the Commodore 64“ (`EXTCOL`, `BGCOL0`,
   `SCROLY`, …) oder sprechende Namen (`VIC_BORDER`, …)? Vorschlag: „Mapping“-Namen, weil sie in vielen Quellen stehen;
   der Kommentar nennt die Bedeutung.
3. **Palette** für Screenshots: Pepto oder Colodore? Vorschlag: Colodore (neuere Messung), per Option umschaltbar.
4. **Verhalten der instabilen Opcodes:** welche „magische Konstante“ für `ANE`/`LXA` (häufig `$EE` bzw. `$EE`/`$FF`)?
   Vorschlag: so, wie es der gewählte Einzelbefehls-Testsatz erwartet, und in [emulator.md](emulator.md) dokumentiert.
5. **Schreibzugriffe auf `$00/$01`:** Was landet im RAM unter dem Prozessorport (auf echter Hardware ein Wert vom Bus)?
   Vorschlag: der geschriebene Wert landet auch im RAM; als bekannte Vereinfachung dokumentiert.
