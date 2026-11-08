# Tests

```sh
ctest --test-dir build --output-on-failure
```

Die Tests brauchen kein externes Framework; `tests/test_util.hpp` stellt `CHECK`/`CHECK_EQ` bereit. Jede Suite ist ein
eigenes Programm (`tests/test_<name>.cpp`), eingetragen in `tests/CMakeLists.txt`.

## Suites

**Vorhanden (0.2–0.4):**

| Suite | Inhalt |
|---|---|
| `format` | Hexadezimalausgabe in allen Zahlenstilen, Parsen der Stilnamen |
| `memory` | Little-Endian-Zugriffe, Umlauf bei 64 KiB, `read16Wrapped` (`JMP ($xxFF)`, Zero Page bei `$FF`), `load`/`readBytes` mit Umlauf, Kopieren, `clear`, feste Adressen |
| `opcodes` | Tabelle: 151 dokumentierte Opcodes mit 56 Mnemonics, 105 undokumentierte (12 `JAM`, 8 instabile, 27 `NOP`-Varianten), Spalte `cc = 11` undokumentiert, über 40 Stichproben (Mnemonic, Adressierungsart, Zyklen, Seitenzuschlag), Vorzugsformen eindeutig, Synonyme, CPU-Stufen |
| `expression` | alle Zahlenschreibweisen, Operatoren und Vorrang, `<`/`>` über den ganzen Ausdruck, `LOW()`/`HIGH()`, `*`, lokale Namen, undefinierte Symbole, Fehlerfälle, Hex-Standard (Monitor), tiefe Verschachtelung wird abgelehnt |
| `decoder` | alle Adressierungsarten, Längen, `.W` bei Zero-Page-Werten (und kein `.W`, wo es keine Zero-Page-Form gibt), Ablaufart und Ziele, Umlauf bei 64 KiB (Verzweigungen und Operanden), CPU-Stufen, Vorzugsformen, Zahlenstile, Symbolizer |
| `assembler` | **über 75 Einzelbefehle mit bekannten Bytes** (alle Adressierungsarten, erzwungene Formen, Verzweigungsgrenzen, undokumentierte Befehle und Synonyme), CPU-Stufen und `.CPU`, Zero Page/absolut mit Vorwärtsreferenzen, Labels in Spalte 1, lokale Labels, alle Pseudobefehle inklusive `.BASIC` (3-, 4- und 5-stellige Ziele), `.PETSCII`, `.SCREEN`, `.LOHI`, mehrere `ORG`, Listing, Symboltabelle, VICE-Labels, über 30 Fehlerfälle, gesammelte Fehler mit Zeilennummern, 3000 zufällige Quelltexte ohne Absturz |
| `roundtrip` | **exhaustiv:** alle 256 Opcodes mit allen Operandenwerten (zweistellige Operanden: alle 65 536), Verzweigungen auch am Umlauf `$FFFF`/`$0000`, mit und ohne Symbolizer – rund 5,4 Millionen Prüfungen. Was der Dekoder als Befehl ausgibt, muss der Assembler zu denselben Bytes übersetzen (Nicht-Vorzugsformen zur Vorzugsform mit gleichen Operanden). Unter den Sanitizern jede 15. High-Byte-Stufe (`M6510_ROUNDTRIP_STEP`) |
| `disassembler` | Ablaufanalyse (Code/Daten, Ende an `JMP`/`RTS`), KERNAL-Namen mit `=`-Zeilen, Querverweise, Kommentare, selbstmodifizierender Code (`label+1`), `.W` erhalten, Nicht-Vorzugsformen als `.BYTE`, CPU-Stufen, BASIC-Starter als `.BASIC` mit Einsprung, Kommentar- und Symboldateien (auch VICE), linearer Modus, **byte-genauer Roundtrip `assemble(--asm) == Speicher`** für 300 zufällige Speicher (alle drei CPU-Stufen, linear und mit zufälligen Einstiegspunkten) |
| `petscii` | ASCII → PETSCII und Bildschirmcodes, PETSCII → Text mit Steuerzeichennamen, BASIC-Token, BASIC-Starter erzeugen/erkennen, Detokenisieren (Strings und `REM` behalten ihre Bytes), defekte Programme werden gemeldet |
| `image_io` | PRG (Ladeadresse, genau bis `$FFFF`, zu lang, zu kurz), P00 (Name, Signatur), Dateizugriff, Endungen |
| `cli_version` | `m6510tool version` |
| `cli_roundtrip` | Beispielprogramm assemblieren → disassemblieren (Ausgabe geprüft) → `--asm` mit Symboldatei → erneut assemblieren → Dateien identisch; `basic list` |
| `c64` | C64-Maschine (rund 100 Prüfungen): alle acht `$01`-Konfigurationen (Prozessorsicht und `peek`), Schreiben unter ROM, ROM-Größen, Schreibprotokoll (Spiegel, Namen, PC, doppeltes Schreiben von `INC`), Raster-Warteschleife, Raster-IRQ über einen eigenen Vektor, CIA-1-Timer-A-IRQ; KERNAL-Emulation: Programm mit `CHROUT`/`GETIN`/`PLOT`/`SCREEN` (Konsole, Bildschirmspeicher, Farbe, Aufrufprotokoll mit Aufrufstelle), nicht emulierter Aufruf, umgebogener `CHROUT`-Vektor, Jiffy-Uhr über den eigenen IRQ-Handler (60 Hz, nicht protokolliert), `LOAD` über die Dateiquelle, Datei nicht gefunden, `BRK`, unbekannter Einsprung, ausgeblendeter KERNAL, Zustände |
| `session` | Agenten-Protokoll (rund 5000 Prüfungen): Befehle der generischen Maschine (asm/dis/read/write/set/step/run/break/symbols), über 20 fehlerhafte Anfragen ohne Wirkung, `asm` mit gesammelten Fehlern, `call` (echte Rückkehr, Stapel daneben, Speicher vorgeben), **Checkpoint → Lauf → Rollback → Lauf liefert identische Antworten**, `diff`, Watchpoints, Trace, `step_over`, `run_until_return`; C64: `run_program`, `console`, `io`, `kernalcalls`, `screen`, Rollback stellt Bausteine und Farb-RAM wieder her, `keys`, CPU- und RAM-Sicht, IRQ von außen über die KERNAL-Vektoren; Dateien nur mit `--allow-files` (PRG mit BASIC-Starter, Snapshot speichern/laden, `attach` + `LOAD`, Maschine passt nicht), **jede Abkürzung einer Snapshot-Datei wird abgelehnt**; 3000 zufällige Befehle mit zufälligen Argumenten |
| `cli_serve` | `examples/agent_session.jsonl` durch `m6510tool serve`: jede Antwort `ok:true`, die Ausgabe des Programms ist da |
| `monitor` | Monitor (rund 8000 Prüfungen): Zahlenschreibweisen (hex, `100.`, Präfixe, Symbole vor Hexzahlen, `@`-Register), Speicherbefehle (Ausgabeformat, Fortsetzung, `f` einschließlich `bis`, `c`, überlappendes `t`, `h` mit Text), Assemble-Modus (Labels, Zuweisungen, Direktiven, undokumentierte Befehle, Fehler ändern nichts, keine Vorwärtsreferenzen), Disassembler mit Marken und PC-Markierung, Register (von links nach rechts, alles oder nichts), `n`/`o`/`u`/`g`/`ru`, Haltepunkte und Watchpoints, `JAM`, Limit, Checkpoints, `reset`, IRQ/NMI (auch die Verzögerung nach `CLI`), Dateibefehle (`asm`, `l`, `sv` PRG/Raw, `symsave`/`symload`, Snapshots, Maschine passt nicht, falsche ROM-Größe), `--no-files`; C64: `machine`, `bank`, KERNAL-Namen, `run`, `console`, `kernal`, `io`, `screen`, Schreiben unter ROM, `keys` + `GETIN`, BASIC-Starter, ohne KERNAL-Emulation; **4000 zufällige Befehlszeilen ohne Absturz**; das Beispielskript |
| `cli_monitor` | `examples/monitor_session.txt` durch `m6510tool monitor`: kein Fehler, die wichtigen Ausgabezeilen sind da |
| `cpu` | Emulator (rund 350 Prüfungen): Arithmetik und Flags (auch Dezimalmodus), Seitenfehler (`zp,X`, `(zp),Y` bei `$FF`, `JMP ($xxFF)`), Zyklen (Seitenzuschlag, Verzweigungen), Stapel, `JSR`/`RTS`, `BRK`/`RTI`, IRQ mit Verzögerung nach `CLI`, NMI, `JAM`, Breakpoints/Watchpoints/`runUntil`/`stepOver`/`runUntilReturn`/`call` (falsches „returned“), Hooks (Ausführungs-, Lese-, Schreib-Hook mit dem doppelten Schreiben von `INC`), Prozessorport, Zustand; Abgleich mit dem Dekoder für alle Opcodes |

**Geplant** (Vorbild: die Suites der Schwesterprojekte, jeweils mit dem Meilenstein):

| Suite | Inhalt | MS |
|---|---|---|
| `video` | Rendern aller Modi aus selbst gebauten Speicherzuständen (Pixel an bekannten Stellen), Rahmen, Bank-Umschaltung, Text-Auslesen, PNG-Ausgabe (Kopf, Prüfsummen) | 0.5 |
| `reverse` | Annotationen, Codefluss-Analyse, verifiziertes Listing; **zufällige Programme mit zufälligen Annotationen: das Listing ist jedes Mal verifiziert** | 0.5 |
| `jumptable` | `vector`, `rts`, `lohi`, `pointers` mit Zielen; Gegenbeispiele; Zufallsprogramme | 0.5 |
| `datatypes` | PETSCII-/Bildschirmcode-Text, Sprites, Zeichensätze, Nullbereiche; Gegenbeispiele; Zufallsbilder | 0.5 |
| `agent` (Python) | Orchestrator und `m6510_drive.py` gegen einen **scripteten Mock-Ollama** und den echten `m6510tool serve` (wie in den Schwesterprojekten) | 0.5 |
| `d64`, `t64`, `crt` | Leser: selbst gebaute Images (Dateiketten, Verzeichnis über mehrere Sektoren, 40 Spuren, Fehlerbytes), beschädigte Images ohne Absturz | 0.6 |
| `unpack` | selbstentpackendes Programm (Packer in Python, Entpacker in 6510-Assembler): `run_until_unpacked`, Speicherauszug = Original, Weg über ein D64 | 0.6 |
| `robustness` | Eingaben von außen: JSON-Parser, Server mit zufälligen Befehlen und Argumenten jedes JSON-Typs, PRG/P00/T64/D64/CRT/Snapshot/Projektdateien mit Mutationen: nur dokumentierte Fehler, nie ein Absturz | ab 0.3 |

Der Roundtrip-Test beweist die Konsistenz von Dekoder und Assembler **untereinander**. Gegen gemeinsame Denkfehler schützen
die Kreuzprüfungen mit unabhängigen Werkzeugen.

## Kreuzprüfung

```sh
sudo apt install cc65
cmake -S . -B build -DM6510_BUILD_TOOLS=ON && cmake --build build
python3 tools/crosscheck_ca65.py build/m6510tool build/m6510_dump_forms
python3 tools/crosscheck_da65.py build/m6510_dump_forms
```

| Prüfung | Werkzeug | Was wird verglichen | Skript | Stand |
|---|---|---|---|---|
| Assembler (und Opcode-Tabelle) | `ca65`+`ld65` (cc65, `--cpu 6502X`) | für jede Vorzugsform der Tabelle ein Befehl mit typischem Operanden, dazu die erzwungen absoluten Formen (`LDA.W $0012` = `lda a:$0012`): gleiche Bytes und der erwartete Opcode | `tools/crosscheck_ca65.py` | CI-Job `crosscheck`: 274 Fälle, 0 Abweichungen (`LXA`, `USBC` kennt ca65 nicht) |
| Dekoder | `da65` (cc65, `--cpu 6502x`) | alle 256 Opcodes einzeln: Mnemonic, Adressierungsart (aus der Operandenschreibweise), Länge; ein dokumentierter Opcode, den da65 nicht kennt, ist ein Fehler | `tools/crosscheck_da65.py` | CI-Job `crosscheck` |
| D64-Leser | `c1541` (VICE) | Zufallsdisketten: Verzeichnis und Dateien byte-genau | `tools/crosscheck_d64.py` | geplant (0.6) |

`tools/dump_forms.cpp` (`m6510_dump_forms`, mit `-DM6510_BUILD_TOOLS=ON`) gibt die Opcode-Tabelle für die Skripte aus.
Bewusste Unterschiede zählen die Skripte getrennt: Namen werden umgesetzt (cc65: `SBX` = `AXS`; da65 zusätzlich `ANE` = `XAA`, `SHA` = `AHX`, `LXA` = `LAX #`, `USBC` = `SBC` mit `$EB`); Befehle, die cc65
nicht kennt, erscheinen als „not supported“ bzw. „.byte in da65“ und lassen die Prüfung nur dann scheitern, wenn es
dokumentierte Opcodes sind. Die CI schreibt die Zusammenfassung beider Prüfungen als Hinweis in die Zusammenfassung des Laufs.

## Emulator gegen Testsätze und Referenz

```sh
cmake -S . -B build -DM6510_BUILD_TOOLS=ON && cmake --build build
./build/m6510_harte 65x02/6502/v1/*.json                         # Testsatz vorher holen (rund 1 GB)
./build/m6510_dormann 6502_functional_test.bin 0 0x400 0x3469
```

| Prüfung | Quelle | Umfang | Werkzeug | Stand |
|---|---|---|---|---|
| Einzelbefehle | Tom Harte, SingleStepTests/65x02 „6502“ (JSON, MIT) | 244 Opcodes × 10 000 Fälle (ohne `JAM`): Register, Speicher, Zyklenzahl | `tools/harte_runner.cpp` | CI-Job `singlestep`: 2 440 000 Fälle, 0 Abweichungen |
| Funktionstest | Klaus Dormann, `6502_functional_test.bin` (GPL-3.0) | ganzes Programm, Erfolg = Endlosschleife bei `$3469` | `tools/dormann_runner.cpp` | CI-Job `functional`: bestanden |
| Dezimalmodus | Klaus Dormann, `6502_decimal_test` | liegt nur als Quelltext vor; durch die Einzelbefehls-Tests abgedeckt | – | entfällt |
| C64-Verhalten | Wolfgang Lorenz, Testsuite 2.15 | C64-Programme je Befehl/Thema | – | zurückgestellt (keine stabile Quelle) |
| Differenztest | perfect6502 (Kandidat) | zufällige Zustände und Programme, Vergleich nach jedem Befehl | – | Idee (durch den vollständigen Einzelbefehls-Testsatz weniger nötig) |

Die Testsätze werden **zur Laufzeit geholt** (CI-Job bzw. Skript) und nicht ins Repository aufgenommen; vor dem Einbau wird
jeweils die Lizenz geprüft und hier vermerkt. Echte C64-ROMs werden in der CI nicht benutzt
([offene Fragen](roadmap.md#offene-fragen)).

## CI

`.github/workflows/ci.yml`: Build und Tests auf Linux (mit `-Werror`), macOS und Windows, dazu ein Lauf mit
AddressSanitizer/UBSan. Fehlgeschlagene Schritte werden mit `tools/ci_annotate.py` als Annotation sichtbar gemacht.
Dazu die Jobs `crosscheck` (ca65/da65), `singlestep` (Harte) und `functional` (Dormann). Geplant: D64 gegen `c1541` (0.6).
