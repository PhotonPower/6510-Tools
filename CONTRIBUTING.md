# Mitarbeit

## Bauen und testen

```sh
cmake -S . -B build -DM6510_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Voraussetzungen: CMake ≥ 3.16 und ein C++20-Compiler (GCC ≥ 11, Clang ≥ 14, MSVC 2022).

Weitere CMake-Optionen:

| Option                     | Standard | Bedeutung                                                |
|----------------------------|----------|----------------------------------------------------------|
| `M6510_BUILD_TESTS`        | `ON`     | Unit-Tests bauen                                          |
| `M6510_WARNINGS_AS_ERRORS` | `OFF`    | Compilerwarnungen als Fehler (in der CI auf Linux `ON`)   |
| `M6510_SANITIZE`           | `OFF`    | AddressSanitizer + UBSan (GCC/Clang)                      |
| `M6510_BUILD_TOOLS`        | `OFF`    | Entwicklerwerkzeuge für die Kreuzprüfung (`m6510_dump_forms`) |

## Regeln

- Neue Funktionalität bekommt Tests in `tests/`. Fehlerkorrekturen bekommen zuerst einen Test,
  der den Fehler zeigt. Neue Test-Suites werden in `tests/CMakeLists.txt` eingetragen, neue
  Quelldateien in der Liste `m6510core` in `CMakeLists.txt`.
- Änderungen an Opcode-Tabelle, Dekoder oder Assembler müssen den exhaustiven Roundtrip-Test (alle 256 Opcodes mit
  allen Operandenwerten) bestehen und, wo möglich, gegen unabhängige Werkzeuge geprüft werden
  (siehe [docs/testing.md](docs/testing.md)).
- Änderungen am Emulator müssen die Einzelbefehls-Testsätze und die Funktionstests bestehen; Details in
  [docs/testing.md](docs/testing.md).
- Die Kernbibliothek (`include/m6510`, `src`) hat keine Ein-/Ausgabe auf der Konsole und keine
  globalen Zustände; Benutzerinteraktion gehört nach `app/`.
- Quelltext, Bezeichner und Kommentare sind Englisch, die Dokumentation unter `docs/` Deutsch.
- ROM-Abbilder (BASIC, KERNAL, Zeichensatz, 1541-DOS) gehören **nie** ins Repository (Urheberrecht); `/roms/` und
  `*.rom` stehen in der `.gitignore`. Ebenso Disk-, Tape- und Cartridge-Images fremder Programme (`*.d64`, `*.t64`,
  `*.tap`, `*.crt`, …) und Listings davon; Testdaten werden in den Tests selbst erzeugt. Externe Testsätze und
  Referenz-Emulatoren werden zur Laufzeit geholt und nicht eingecheckt.
- Formatierung: siehe `.clang-format` / `.editorconfig`.
- Jede nennenswerte Änderung kommt in `CHANGELOG.md`; Dokumente, die etwas als „geplant“ beschreiben, werden
  angepasst, sobald es umgesetzt ist.
