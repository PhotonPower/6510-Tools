# Tests

```sh
ctest --test-dir build --output-on-failure
```

Die Tests brauchen kein externes Framework; `tests/test_util.hpp` stellt `CHECK`/`CHECK_EQ` bereit. Jede Suite ist ein
eigenes Programm (`tests/test_<name>.cpp`), eingetragen in `tests/CMakeLists.txt`.

## Suites

**Vorhanden (0.1):**

| Suite | Inhalt |
|---|---|
| `format` | Hexadezimalausgabe in allen Zahlenstilen, Parsen der Stilnamen |
| `memory` | Little-Endian-Zugriffe, Umlauf bei 64 KiB, `read16Wrapped` (`JMP ($xxFF)`, Zero Page bei `$FF`), `load`/`readBytes` mit Umlauf, Kopieren, `clear`, feste Adressen |
| `cli_version` | `m6510tool version` |

**Geplant** (Vorbild: die Suites der Schwesterprojekte, jeweils mit dem Meilenstein):

| Suite | Inhalt | MS |
|---|---|---|
| `opcodes` | Tabelle: 256 Einträge, 151 dokumentierte, Längen aus der Adressierungsart, Vorzugsformen eindeutig, Stichproben gegen bekannte Werte | 0.2 |
| `expression` | Zahlenschreibweisen, Operatoren und Vorrang (`<`/`>`), `*`, Symbole, undefinierte Symbole, Fehlerfälle, Hex-Standard | 0.2 |
| `decoder` | Bytes → Text/Länge für alle Adressierungsarten, Sprungziele (Umlauf bei 64 KiB), Flussart, Optionen für undokumentierte Opcodes, Symbolizer, Zahlenstile | 0.2 |
| `assembler` | Einzelzeilen mit Bytes aus einem unabhängigen Assembler (`tests/verified.inc`), Zero Page/absolut-Wahl mit Vorwärtsreferenzen, `.B`/`.W`, lokale Labels, alle Pseudobefehle inklusive `.BASIC`, `.PETSCII`, `.SCREEN`, `.LOHI`, Listing, Symboltabelle, VICE-Labels, Fehlerfälle, zufällige Quelltexte ohne Absturz | 0.2 |
| `roundtrip` | **exhaustiv:** alle 256 Opcodes mit allen Operandenwerten (bis 65536 je Opcode); was der Dekoder als Befehl ausgibt, muss der Assembler zu denselben Bytes übersetzen | 0.2 |
| `disassembler` | Ablaufanalyse, Einstiegspunkte, überlappender Code, Labels mitten im Befehl (`label+1`), Kommentare, linearer Modus, **byte-genauer Roundtrip `assemble(--asm) == Speicher`** für zufällige Speicher | 0.2 |
| `image_io` | Raw, PRG (Ladeadresse, Länge über `$FFFF`), P00, Intel HEX; Fehler mit Meldung | 0.2 |
| `petscii` | PETSCII ↔ ASCII, Bildschirmcodes, BASIC-Detokenisierer, BASIC-Starter erzeugen und erkennen | 0.2 |
| `cpu` | Emulator: Arithmetik und Flags (auch Dezimalmodus), alle Adressierungsarten mit Seitenfehlern, Zyklen, Stapel, `BRK`/`RTI`, IRQ/NMI mit Verzögerung nach `CLI`/`SEI`/`PLP`, `JAM`, Prozessorport, Breakpoints/Watchpoints/`runUntil`/`stepOver`/`call`, Hooks; Abgleich mit dem Dekoder für alle 256 Opcodes | 0.3 |
| `session` | alle Befehle des Agenten-Protokolls; Fehler ohne Absturz; **Checkpoint → Lauf → Rollback → Lauf liefert identische Antworten**; Dateizugriff nur mit `--allow-files`; Snapshot-Serialisierung (jede Abkürzung wird abgelehnt) und Speicher-Diff | 0.3 |
| `c64` | Speicherkarte: alle acht `$01`-Konfigurationen, Schreiben unter ROM, Spiegelungen, Farb-RAM, Schreibprotokoll mit Namen, Rasterzähler, VIC-Sicht | 0.3 |
| `kernal` | KERNAL-Emulation: `CHROUT` (Konsole und Bildschirm, Steuerzeichen, Scrollen), `GETIN` mit Tastenpuffer, `PLOT`, `LOAD` aus einem selbst gebauten D64, nicht emulierte Aufrufe werden protokolliert, Rückkehr ins BASIC = `exit`, Checkpoints | 0.3 |
| `monitor` | Zahlenschreibweisen, alle Befehle, C64-Befehle, Dateibefehle, `--no-files`, **zufällige Befehlszeilen ohne Absturz** | 0.4 |
| `video` | Rendern aller Modi aus selbst gebauten Speicherzuständen (Pixel an bekannten Stellen), Rahmen, Bank-Umschaltung, Text-Auslesen, PNG-Ausgabe (Kopf, Prüfsummen) | 0.5 |
| `reverse` | Annotationen, Codefluss-Analyse, verifiziertes Listing; **zufällige Programme mit zufälligen Annotationen: das Listing ist jedes Mal verifiziert** | 0.5 |
| `jumptable` | `vector`, `rts`, `lohi`, `pointers` mit Zielen; Gegenbeispiele; Zufallsprogramme | 0.5 |
| `datatypes` | PETSCII-/Bildschirmcode-Text, Sprites, Zeichensätze, Nullbereiche; Gegenbeispiele; Zufallsbilder | 0.5 |
| `agent` (Python) | Orchestrator und `m6510_drive.py` gegen einen **scripteten Mock-Ollama** und den echten `m6510tool serve` (wie in den Schwesterprojekten) | 0.5 |
| `d64`, `t64`, `crt` | Leser: selbst gebaute Images (Dateiketten, Verzeichnis über mehrere Sektoren, 40 Spuren, Fehlerbytes), beschädigte Images ohne Absturz | 0.6 |
| `unpack` | selbstentpackendes Programm (Packer in Python, Entpacker in 6510-Assembler): `run_until_unpacked`, Speicherauszug = Original, Weg über ein D64 | 0.6 |
| `robustness` | Eingaben von außen: JSON-Parser, Server mit zufälligen Befehlen und Argumenten jedes JSON-Typs, PRG/P00/T64/D64/CRT/Snapshot/Projektdateien mit Mutationen: nur dokumentierte Fehler, nie ein Absturz | ab 0.3 |
| `cli_*` | Beispielprogramm assemblieren → disassemblieren (`--asm`) → erneut assemblieren → Dateien identisch | 0.2 |

Der Roundtrip-Test beweist die Konsistenz von Dekoder und Assembler **untereinander**. Gegen gemeinsame Denkfehler schützen
die Kreuzprüfungen mit unabhängigen Werkzeugen.

## Kreuzprüfung (geplant)

| Prüfung | Werkzeug | Was wird verglichen | Skript |
|---|---|---|---|
| Dekoder | `da65` (cc65, `--cpu 6502x`) | für alle 256 Opcodes Gültigkeit, Länge, Mnemonic, Operand (Schreibweisen normalisiert) | `tools/crosscheck_da65.py` |
| Assembler | `64tass` und/oder `ca65`+`ld65` | Befehle und kleine Programme mit Labels, Zero Page/absolut, Daten: gleiche Bytes | `tools/crosscheck_asm.py` |
| D64-Leser | `c1541` (VICE) | Zufallsdisketten: Verzeichnis und Dateien byte-genau | `tools/crosscheck_d64.py` |

Alle drei Werkzeuge gibt es als Pakete (`apt install cc65 64tass vice`); die CI installiert sie in eigenen Jobs. Bewusste
Unterschiede (z. B. andere Namen für undokumentierte Opcodes) zählen die Skripte getrennt und dokumentieren sie hier.

## Emulator gegen Testsätze und Referenz (geplant)

| Prüfung | Quelle | Umfang | Werkzeug |
|---|---|---|---|
| Einzelbefehle | Tom Harte, SingleStepTests „6502“ (JSON) | 256 Opcodes × 10 000 Fälle: Register, Speicher, Zyklen und Buszugriffe | `tools/harte_runner.cpp` |
| Funktionstest | Klaus Dormann, `6502_functional_test` | ganzes Programm, Erfolg = Endlosschleife an bekannter Adresse | `tools/dormann_runner.cpp` |
| Dezimalmodus | Klaus Dormann, `6502_decimal_test` | alle Operanden-/Flag-Kombinationen für `ADC`/`SBC` | ebenso |
| C64-Verhalten | Wolfgang Lorenz, Testsuite 2.15 | C64-Programme je Befehl/Thema, auf der C64-Maschine mit KERNAL-Emulation | `tests/agent/test_lorenz.py` o. Ä. |
| Differenztest | perfect6502 (Kandidat) | zufällige Zustände und Programme, Vergleich nach jedem Befehl | `tools/diff_reference.cpp`, `tools/run_diff_reference.sh` |

Die Testsätze werden **zur Laufzeit geholt** (CI-Job bzw. Skript) und nicht ins Repository aufgenommen; vor dem Einbau wird
jeweils die Lizenz geprüft und hier vermerkt. Echte C64-ROMs werden in der CI nicht benutzt
([offene Fragen](roadmap.md#offene-fragen)).

## CI

`.github/workflows/ci.yml`: Build und Tests auf Linux (mit `-Werror`), macOS und Windows, dazu ein Lauf mit
AddressSanitizer/UBSan. Fehlgeschlagene Schritte werden mit `tools/ci_annotate.py` als Annotation sichtbar gemacht.
Geplante zusätzliche Jobs: `crosscheck` (da65/64tass/c1541), `singlestep` (Harte), `functional` (Dormann), `lorenz`,
`emulator-diff`.
