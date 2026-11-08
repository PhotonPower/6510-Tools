# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt
- **Meilenstein 0.6 – Images und Entpacken:**
  - **D64** lesen (35/40 Spuren, mit/ohne Fehlerbytes; BAM, Verzeichnis, Dateiketten mit Prüfung auf Schleifen, Zeiger
    außerhalb und falsche Größen), **T64** lesen (verbreitete Fehler toleriert und gemeldet), **CRT** lesen (Hardwaretypen
    mit Namen); `m6510tool d64 info|ls|extract|boot|block`, `t64 ls|extract`, `crt info|extract`.
  - `disk.d64:NAME`/`band.t64:NAME` überall, wo ein Programm gelesen wird (`dis`, `basic list`, Monitor `l`, `load_file` mit
    `name`, Orchestrator `--prg`); `attach` eines D64/T64 für `LOAD` über die KERNAL-Emulation.
  - **Cartridges** (CRT-Typ 0) in der C64-Maschine: 8K, 16K, Ultimax (`C64::insertCartridge`), Start über den
    Kaltstartvektor (`CBM80`) bzw. den RESET-Vektor des Moduls.
  - **Selbstentpackende Programme:** `Cpu::trackWrites`/`watchWrittenCode`, Protokollbefehle `track_writes`, `written`,
    `run_until_unpacked` (mit `continue` für mehrstufige Packer, `ignore`), `save_memory` (PRG oder roh, CPU- oder RAM-Sicht).
  - Tests `d64`, `t64`, `crt` (selbst gebaute Images, Hunderte Mutationen), `test_unpack.py` (Packer in Python, Entpacker in
    6510-Assembler: ein-/zweistufig, über ein D64); Kreuzprüfung gegen `c1541` (VICE) als CI-Job.
- **Meilenstein 0.5, erster Teil – der Bildschirm als Bild:** `renderScreen` (VIC-II aus dem Speicherzustand: Standard-,
  Multicolor- und Extended-Background-Text, Hires- und Multicolor-Bitmap, Rahmen, Bild aus, 38 Spalten/24 Zeilen,
  Feinscrolling, VIC-Bank, Zeichensatz-ROM in Bank 0/2), selbst gezeichneter **Ersatzzeichensatz** (`tools/make_font.py`),
  Paletten **Colodore** (Standard) und Pepto, PNG-Encoder und Base64 aus dem Z80-Projekt. Protokollbefehle `screenshot`
  (Ausschnitt in Textzellen, Zoom), `screens`, `screen_drop`, `screen_diff`; Monitor `shot`. Test `video`.
  Offene Frage 3 (Palette) entschieden.
- **Meilenstein 0.5, zweiter Teil – Analyse und verifiziertes Listing:** `Analysis` (Codefluss, Routinen mit Aufrufgraph und
  Tiefe, KERNAL-Aufrufe, I/O-Zugriffe, Zero Page, `$01`-Schreibzugriffe, Sprungtabellen `vector`/`rts`/`lohi`/`pointers`,
  Zeiger in Immediate-Paaren, Interrupt-Handler aus `$0314`/`$0318`/…, selbstmodifizierender Code, Datentypen Text/
  Bildschirmcodes/Nullen/Wort- und Zeigertabellen/Sprites/Zeichensatz/BASIC), `Annotations` (Namensregeln, JSON),
  `renderListing` (neu assembliert und Byte für Byte verglichen). Protokollbefehle `analyze`, `routines`, `references`,
  `jump_tables`, `data_regions`, `listing`, `ann_*`. Beispielprogramm `examples/c64/hello_c64.asm`. Tests `reverse`,
  `jumptable`, `datatypes` (Zufallsprogramme mit Zufallsannotationen: jedes Listing verifiziert).
  - `m6510::Machine` merkt sich den Bereich des zuletzt geladenen oder assemblierten Programms.
- **Meilenstein 0.5, dritter Teil – Orchestrator:** `agent/m6510_annotate.py` (Ollama, Routinen von den Blättern zur Wurzel,
  Werkzeuge mit `run_routine` samt KERNAL-Aufrufen, Bausteinzugriffen, `CHROUT`-Text, Bildschirmänderung und Screenshots für
  Modelle mit Bildverständnis, Vollständigkeitsprüfung, Verfeinerungsdurchgänge, Projektdatei, Bericht, `--dry-run`) und
  `agent/m6510_drive.py` (dieselben Werkzeuge, ein Schritt je Aufruf). Tests gegen einen Mock-Ollama (`tests/agent`, CTest
  `agent`). Anleitung `docs/walkthrough.md` mit dem Beispielprogramm.
  - `call` mit `frame` (`irq`, `rti`): Interrupt-Handler laufen mit dem passenden Stapelrahmen und kehren mit `RTI` zurück
    (`Cpu::callInterrupt`).
  - Listing-Option `equates:false` (kein Kopf, für Ausschnitte); `max_entries:0` bei `jump_tables`.
- **Meilenstein 0.4 – Monitor** (`m6510::Monitor`, `m6510tool monitor`): klassische Eingabeschleife wie bei den
  Schwesterprojekten, Zahlen hexadezimal (`100.` dezimal, Präfixe, Symbole, `@`-Register): `d`, `m`/`m.s`, `e`/`e.w`, `f`,
  `c`, `t`, `h`, Assemble-Modus `a` (Labels, Zuweisungen, Direktiven), `r`, `n`, `o`, `u`, `g`, `ru`, `b`/`bc`/`w`, `limit`,
  `sym`/`symload`/`symsave`, `asm`, `l`, `sv`, `ck`/`rb`, `savestate`/`loadstate`, `irq [on|off]`/`nmi`, `reset`, `machine`;
  C64: `bank`, `rom`, `io`, `kernal`, `console`, `screen`, `keys`, `run`. Kommandozeile mit `--c64`, `--roms verzeichnis`
  (VICE-Dateinamen), `--org`, `--no-files`, `--quiet`; Banner und Prompt nur am Terminal.
  - **`m6510::Machine`:** Maschine, Symbole, Checkpoints, Watchpoints, ROMs, `attach` und Dateiladen aus der Sitzung
    herausgelöst; Sitzung und Monitor benutzen dieselbe Klasse.
  - Tests `monitor` (rund 8000 Prüfungen, 4000 zufällige Befehlszeilen) und `cli_monitor` mit dem Beispielskript
    `examples/monitor_session.txt`.

- **Meilenstein 0.3, zweiter und dritter Teil – Snapshots, Sitzung, C64:**
  - **`m6510::C64`:** Speicherkarte mit allen `$01`-Konfigurationen, ROMs laden, Farb-RAM, VIC-II/SID/CIA-Register mit
    Schreibprotokoll und Registernamen („Mapping the Commodore 64“), Rasterzeile und CIA-1-Timer A aus dem Zyklenzähler mit
    Raster- und Timer-IRQ, IRQ von außen.
  - **`m6510::Kernal`:** KERNAL-Emulation mit erzeugtem 8-KiB-Abbild (echte Sprungtabelle über die RAM-Vektoren, IRQ-/NMI-
    Einsprung als 6502-Code), `CHROUT` auf Konsole und Bildschirm, `GETIN`/`CHRIN` mit Tastenpuffer, `PLOT`, Uhr, `LOAD`/
    `SAVE`, `CINT`/`IOINIT`/`RAMTAS`/`RESTOR`, Aufrufprotokoll; Programmstart wie `SYS`, Ende bei Rückkehr ins BASIC oder `BRK`.
  - **Snapshots** (`M6510SN1`) mit Speicher-Diff; **`m6510::Session`** und **`m6510tool serve`** (JSON-Zeilenprotokoll mit den
    Befehlen der Schwesterprojekte und den C64-Befehlen `run_program`, `io`, `kernalcalls`, `console`, `keys`, `screen`,
    `attach`, `load_rom`); Beispielsitzung `examples/agent_session.jsonl`.
  - Tests `c64`, `session` (inklusive Rollback-Determinismus, abgelehnter Snapshot-Abkürzungen, 3000 Zufallsbefehle),
    `cli_serve`.
  - `Memory` hält die 64 KiB auf dem Heap (Stapelüberlauf unter Windows im Test `cpu`).
  - Offene Frage 2 entschieden: Registernamen nach „Mapping the Commodore 64“. Lorenz-Testsuite zurückgestellt (keine
    stabile Quelle).
- **Meilenstein 0.3, erster Teil – Prozessorkern** (`m6510::Cpu`): alle 256 Opcodes (undokumentierte und instabile nach
  Tom Hartes Testsatz, `JAM` hält an), NMOS-Dezimalmodus, Zyklen pro Befehl, IRQ/NMI/`BRK`/RESET mit der Verzögerung nach
  `CLI`/`SEI`/`PLP`, Prozessorport `$00/$01` (abschaltbar), doppeltes Schreiben der Read-Modify-Write-Befehle;
  Einzelschritt, Breakpoints, Watchpoints, `runUntil`/`runUntilReturn`/`stepOver`/`call`, Lese-/Schreib-/Ausführungs-Hooks,
  Zustand. JSON-Baustein aus dem MC68000-Projekt übernommen.
  - **Tom Hartes Einzelbefehls-Tests:** 2 440 000 Fälle, 0 Abweichungen (Register, Speicher, Zyklenzahl;
    `tools/harte_runner.cpp`, CI-Job `singlestep`). Gefunden und behoben: `JSR` holt sein Ziel-High-Byte nach dem Stapeln.
  - **Klaus Dormanns Funktionstest** bestanden (`tools/dormann_runner.cpp`, CI-Job `functional`).
  - Unit-Tests `cpu` (rund 350 Prüfungen).
  - Offene Frage 4 entschieden: `ANE`/`LXA` mit der Konstante `$EE` (aus den Testdaten ermittelt).
- **Meilenstein 0.2 – Assembler und Disassembler:**
  - **Opcode-Tabelle** (`m6510/opcodes.hpp`): alle 256 Opcodes mit Mnemonic (undokumentierte nach „No More Secrets“),
    Adressierungsart, Zyklen, Seitenzuschlag, Klasse (dokumentiert / stabil / instabil / JAM) und berechneter Vorzugsform;
    CPU-Stufen `Documented`/`Stable`/`All`; Synonyme (`DCM`, `ISB`, `ASO`, …).
  - **Dekoder** (`decode()`), **Ausdrucksauswerter** (`<`/`>`, `LOW()`/`HIGH()`, `*`, `1Fh`, `0b…`, Hex-Standard für den
    Monitor), **Encoder** und **Mehrpass-Assembler** (Zero Page/absolut automatisch, `.B`/`.W`, lokale Labels, `.BYTE`,
    `.WORD`, `.DBYTE`, `.TEXT`, `.NULL`, `.PETSCII`, `.SCREEN`, `.LO`/`.HI`/`.LOHI`, `.FILL`, `.ALIGN`, `.BASIC`, `.CPU`,
    `ORG`/`*=`, `EQU`/`=`, `END`); Listing, Symboltabelle, VICE-Labeldatei.
  - **Disassembler** mit Ablaufanalyse und linearem Modus, Querverweisen, Kommentar- und Symboldateien, KERNAL-Namen,
    selbstmodifizierendem Code als `label+n`, BASIC-Starter als `.BASIC` (Einsprung = `SYS`-Adresse) und
    wieder assemblierbarer Ausgabe (`--asm`, byte-genau, Nicht-Vorzugsformen als `.BYTE`).
  - **PETSCII/BASIC** (`m6510/petscii.hpp`): PETSCII und Bildschirmcodes, BASIC-V2-Detokenisierer, BASIC-Starter.
  - **PRG/P00** (`m6510/image_io.hpp`).
  - `m6510tool asm`, `m6510tool dis`, `m6510tool basic list`.
  - Tests: `opcodes`, `expression`, `decoder`, `assembler`, `roundtrip` (**exhaustiv**, rund 5,4 Millionen Prüfungen),
    `disassembler` (300 zufällige Speicher byte-genau zurück), `petscii`, `image_io`, `cli_roundtrip`; alle auch unter
    ASan/UBSan.
  - Kreuzprüfung gegen cc65: `tools/crosscheck_ca65.py` (Assembler) und `tools/crosscheck_da65.py` (Dekoder) mit
    `tools/dump_forms.cpp`; CI-Job `crosscheck`.
- Entscheidungen der Roadmap bestätigt (undokumentierte Opcodes, Genauigkeit/Geräte, CI ohne ROMs, Namen).
- Projektgerüst nach dem Vorbild von [PhotonPower/Z80](https://github.com/PhotonPower/Z80) und
  [PhotonPower/MC68000](https://github.com/PhotonPower/MC68000): CMake-Build (`m6510core`, `m6510tool`), CTest,
  GitHub-Actions-CI (Linux/macOS/Windows, ASan/UBSan), `.clang-format`, `.editorconfig`, `.gitattributes`,
  `.gitignore` (ROMs und Disk-/Tape-/Cartridge-Images werden nie eingecheckt), Mitarbeitsregeln, Pull-Request-Vorlage,
  `CLAUDE.md` für KI-Assistenten.
- Kernbausteine: `m6510/types.hpp` (16-Bit-Adressraum, `Byte`/`Word`, Vektoren, Prozessorport, Flags),
  `m6510/format.hpp` (Hexadezimalausgabe), `m6510/memory.hpp` (flacher 64-KiB-Speicher, Little-Endian, Umlauf bei 64 KiB,
  `read16Wrapped` für den `JMP ($xxFF)`-Fehler und Zero-Page-Zeiger) mit Tests.
- `m6510tool help|version`; `asm`, `dis`, `monitor`, `serve`, `d64`, `t64`, `crt`, `basic` sind angekündigt, aber noch nicht
  umgesetzt.
- Beispielprogramm `examples/hello.asm` (BASIC-Starter, `CHROUT`) mit Kommentardatei.
- Dokumentation unter `docs/` für den geplanten Umfang (Architektur, C64-Zielplattform, Befehlssatz, Assembler-Syntax,
  Formate, Benutzung, Emulator, Monitor, Agenten-Protokoll, Reverse Engineering, Anleitung, Entpacken, Tests, Roadmap mit
  Entscheidungsvorschlägen und offenen Fragen).

### Behoben
- **Befunde aus dem Blindtest** (ein Claude-Agent übernahm mit `m6510_drive.py` die Rolle des Modells, nur mit dem PRG;
  Ergebnis: alle 10 Routinen dokumentiert, Listing verifiziert, rund 115 Werkzeugaufrufe):
  - Listing-Ausschnitte benennen Adressen im analysierten Programm jetzt genauso wie das ganze Listing (vorher `#$B3` statt
    `#<data_08B3` und rohe Adressen, sobald das Ziel außerhalb des Ausschnitts lag; ein Modell hätte echte Zeiger als
    Konstanten markiert).
  - Kommentare an Datenzeilen und die Kommentare von `ann_variable` erscheinen im Listing (vorher stillschweigend verloren);
    eine benannte Variable und ein einzeln benutztes Byte bekommen eine eigene Zeile statt in `.FILL` aufzugehen;
    `ann_comment` mitten in einem Datenbereich oder einer Sprungtabelle wird mit Erklärung abgelehnt.
  - Mit `addresses:true` steht die Adresse immer vorn im Kommentar (`; $0860: Text`); „operand changed by the code at …“
    statt des missverständlichen „written by … / $0860“.
  - Annotierte Sprungtabellen bleiben mit ihrem `RTS`/`JMP` verknüpft (vorher verloren die Verteiler ihre Aufgerufenen).
  - `references` beschreibt Sprungtabellen, KERNAL-ROM-Adressen (`$EA31`), RAM-Vektoren (`CINV` & Co., jetzt auch als Namen
    im Listing) und Sprite-Zeiger; `ann_constant` meldet, wenn die Markierung nichts bewirkt.
  - Orchestrator: `get_listing` mit inklusivem Ende; der Auftrag nennt die benutzten Sprungtabellen und bei jedem Fall
    Tabelle und Index; `run_routine` meldet Sprites und erklärt das doppelte Schreiben von `INC $D019`; die
    Vollständigkeitsprüfung meldet unbenannte `data_`-Operanden; `m6510_drive.py` speichert Screenshots als PNG.
- `set` mit `P` (Sitzung) und `r p=…` (Monitor): Ein neues `I`-Flag galt für die Interrupt-Abfrage erst nach einem weiteren
  Befehl, als wäre es mit `CLI`/`SEI` gesetzt worden.
