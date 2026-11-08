# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt
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
