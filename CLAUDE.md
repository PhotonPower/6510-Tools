# CLAUDE.md

Hinweise für KI-Assistenten, die in diesem Repository arbeiten.

## Projekt

MOS-6510-/Commodore-64-Werkzeuge in C++20: Assembler, Disassembler, Emulator, Monitor, JSON-Agentenschnittstelle,
Reverse Engineering mit lokalem LLM. Schwesterprojekte mit derselben Architektur und demselben Umfang:
[PhotonPower/Z80](https://github.com/PhotonPower/Z80) und [PhotonPower/MC68000](https://github.com/PhotonPower/MC68000).
Bei Entwurfsfragen zuerst nachsehen, wie das MC68000-Projekt es gelöst hat (es ist das neuere und vollständigere), und die
Lösung übernehmen, soweit der 6510 nicht etwas anderes verlangt. Die Schwesterprojekte nur lesen, nie dorthin schreiben.

## Wo was steht

- Plan und Reihenfolge: `docs/roadmap.md` (Meilensteine, Entscheidungen, offene Fragen)
- Aufbau und Komponenten: `docs/architecture.md`
- Spezifikationen: `docs/assembler-syntax.md`, `docs/instruction-set.md`, `docs/c64.md`, `docs/formats.md`,
  `docs/agent-protocol.md`, `docs/emulator.md`, `docs/monitor.md`, `docs/usage.md`
- Tests und Kreuzprüfung: `docs/testing.md`

## Bauen und testen

```sh
cmake -S . -B build -DM6510_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Mit Sanitizern: `-DM6510_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug`.

## Regeln (Kurzfassung von CONTRIBUTING.md)

- Namen: Namespace `m6510`, Bibliothek `m6510core`, Werkzeug `m6510tool`, Header unter `include/m6510/`, CMake-Optionen
  `M6510_*`, Python `agent/m6510_*.py`.
- Kernbibliothek ohne Konsolenausgabe und ohne globalen Zustand; Ein-/Ausgabe nur in `app/`.
- Code, Bezeichner, Kommentare: Englisch. Dokumentation unter `docs/`, README, CHANGELOG: Deutsch.
- Keine externen Abhängigkeiten in Bibliothek und Tests (eigenes Test-Makro `CHECK`/`CHECK_EQ`, eigener JSON- und
  PNG-Baustein).
- Jede neue Funktion bekommt Tests; neue Suites in `tests/CMakeLists.txt`, neue Quellen in `m6510core` in `CMakeLists.txt`.
- Opcode-Tabelle, Dekoder und Assembler müssen den exhaustiven Roundtrip bestehen; der Emulator die Testsätze.
- Nie ROMs, Disk-/Tape-/Cartridge-Images oder Listings fremder Programme einchecken; Testdaten in den Tests erzeugen,
  externe Testsätze zur Laufzeit holen.
- Nach jeder Änderung: `CHANGELOG.md` ergänzen und Dokumente, die etwas als „geplant“ beschreiben, auf den echten Stand
  bringen (Stand-Hinweis oben in jedem Dokument, Spalte „Stand“ in `docs/architecture.md`).
