# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt
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
