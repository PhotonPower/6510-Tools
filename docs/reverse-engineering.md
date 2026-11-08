# Reverse Engineering mit lokalem LLM

> **Stand 0.1:** geplant (Meilenstein 0.5). Vorbild sind `agent/z80_annotate.py` (mit Screenshots für Qwen3-VL) und
> `agent/m68k_annotate.py` / `agent/m68k_drive.py` (Analyse, Sprungtabellen, Datentypen, Blindtest-Befunde) der
> Schwesterprojekte. Was sich dort bewährt hat, wird übernommen; dieses Dokument nennt die 6510-spezifischen Teile.

Ziel: aus einem C64-Programm (PRG, Datei aus einem D64, Snapshot) ein **möglichst vollständig kommentiertes Listing**
machen – mit sinnvollen Namen für Routinen, Variablen (besonders Zero-Page-Zeiger) und Daten, mit KERNAL-Aufrufen und
Hardware-Zugriffen als Symbole – mit Hilfe eines lokalen Modells über **Ollama**.

```
        +-------------------------+      Tool-Aufrufe       +---------------------------+
        |  Modell (Ollama)        | <---------------------> |  agent/m6510_annotate.py  |
        |  liest, testet, benennt |   Text + Screenshots    |  (Python, nur stdlib)     |
        +-------------------------+                         +-------------+-------------+
                                                                          | JSON-Zeilen
                                                            +-------------v-------------+
                                                            |  m6510tool serve          |
                                                            |  Emulator, C64, Analyse,  |
                                                            |  Annotationen, Listing    |
                                                            +---------------------------+
```

## Was das Modell kann – und was nicht

| Werkzeug | Zweck |
|---|---|
| `get_listing`, `disassemble`, `read_memory`, `references` | ansehen |
| `run_routine` | Routine im Emulator wie eine Funktion aufrufen (Register, Zero Page, Speicher vorgeben; `setup`-Routinen vorher); Ergebnis: Register, geänderter Speicher (mit Variablennamen und PETSCII-Spalte), KERNAL-Aufrufe, I/O-Schreibzugriffe mit Registernamen, Konsolentext, **Bildschirmänderung** (Text und optional Bild) |
| `screenshot` | aktueller Bildschirm als Bild, optional ausgeschnitten und vergrößert |
| `reset_machine` | Ausgangszustand (sonst automatisch nach jedem `run_routine`) |
| `name_label`, `document_routine`, `name_variable`, `mark_data`, `mark_constant`, `add_comment`, `remove_annotation` | **Namen und Kommentare** festhalten |
| `finish_routine` | Routine abschließen (mit Vollständigkeitsprüfung) |

Das Modell **kann den Code nicht verändern**. Es liefert nur Namen und Texte; das Listing wird aus den echten Bytes erzeugt,
**neu assembliert und Byte für Byte verglichen** (`VERIFIED`). Falsche Namen (Mnemonics, Registernamen `A`/`X`/`Y`,
KERNAL-Namen, doppelte Namen) werden mit Erklärung abgewiesen. Jedes Experiment läuft auf einem Checkpoint und wird
zurückgerollt (inklusive Farb-RAM, Bausteinregistern und KERNAL-Zustand).

## Benutzung (geplant)

```sh
python3 agent/m6510_annotate.py --prg spiel.prg --project spiel.m6510proj.json --out spiel_kommentiert.asm \
        --lang Deutsch --model qwen3-vl:8b-instruct --log spiel.log.jsonl
python3 agent/m6510_annotate.py --d64 disk.d64 --file "SPIEL" …     # Datei aus einem Diskettenabbild
python3 agent/m6510_annotate.py --prg spiel.prg --dry-run            # Analyse ohne Modell: Reihenfolge, Tabellen, Token-Schätzung
```

Wie bei den Schwesterprojekten: Routinen **von den Blättern zur Wurzel**, mehrere Durchgänge (schnelles Modell zuerst,
gründliches für die Lücken), Projektdatei zum Abbrechen/Fortsetzen, Bericht am Ende, Standardmodell
`qwen3-vl:8b-instruct` (mit Bildern); nur Python-Standardbibliothek.

## Das Prinzip: das Modell kann den Code nicht verändern

Übernommen aus den Schwesterprojekten. Der Roundtrip `assemble(listing) == bytes` ist auf dem 6510 besonders wichtig, weil
die Assemblersyntax Mehrdeutigkeiten hat (Zero Page vs. absolut, mehrere Opcodes für `NOP`/`SBC #`): das Listing erzwingt
die Form, die die Originalbytes ergibt (`LDA.W`, `USBC`, `.BYTE`), und der Vergleich beweist es.

## Was die Analyse liefert (6510-spezifisch)

- **Codefluss** ab den Einstiegspunkten (PC, `SYS`-Adresse, Vektoren, die das Programm setzt: `$0314/$0315` (IRQ),
  `$0318/$0319` (NMI), `$FFFE`, `$FFFA` bei ausgeblendetem KERNAL).
- **KERNAL-Aufrufe** (`JSR $FFD2` → `JSR CHROUT`) und Sprünge in BASIC-/KERNAL-ROM als **externe Aufrufe**; Aufrufe in
  ROM-Routinen hinter der Sprungtabelle (z. B. `$E544` Bildschirm löschen) werden ebenfalls benannt, soweit eine Tabelle
  bekannter Einsprünge vorhanden ist.
- **I/O-Zugriffe** je Routine: welche VIC-/SID-/CIA-Register gelesen und geschrieben werden (starker Hinweis auf den Zweck:
  `$D020` Rahmenfarbe, `$D418` Lautstärke, `$DC01` Tastatur …).
- **Zero Page:** benutzte Adressen, Zeigerpaare (`($FB),Y` → `$FB/$FC` als 16-Bit-Zeiger), Vorschläge für Namen.
- **Selbstmodifizierender Code:** Befehle, deren Operand von anderem Code geschrieben wird (`STA patch+1`), werden markiert;
  das Listing schreibt `patch+1` und kommentiert die Stelle. Die Analyse folgt `JMP`/`JSR` mit veränderbarem Ziel nicht blind.
- **Banking:** Schreibt das Programm `$01`, wird das gemeldet (z. B. „schaltet BASIC und KERNAL aus: `$35`“); Code unter ROM
  wird in der RAM-Sicht analysiert.

## Sprungtabellen

| Art | Muster | Behandlung |
|---|---|---|
| `vector` | `JMP (tab)` mit festem Vektor, `JMP (tab,X)` gibt es auf dem NMOS-6502 nicht | Ziel verfolgen, wenn der Vektor im Programm einen festen Wert hat |
| `rts` | `LDA hi,X` / `PHA` / `LDA lo,X` / `PHA` / `RTS` – Tabelle mit **Adresse − 1** | Ziele = Eintrag + 1; erkannt am Muster vor dem `RTS` |
| `lohi` | getrennte Low-/High-Byte-Tabellen (`LDA lo,X` / `STA ptr` / `LDA hi,X` / `STA ptr+1` / `JMP (ptr)`) | beide Tabellen gleich lang, Ziele zusammengesetzt; Listing als `.LO`/`.HI`-Paar |
| `pointers` | Tabelle von 16-Bit-Wörtern | Vorschlag (`followed:false`), nach `ann_data kind=pointers` verfolgt |

## Datentypen

| Art | Erkennung |
|---|---|
| `text` (PETSCII) | druckbare PETSCII-Zeichen, mit `0`, `$0D` oder gesetztem Bit 7 am Ende; über `CHROUT`-Schleifen referenziert |
| `screen` (Bildschirmcodes) | Bytes, die in den Bildschirmspeicher kopiert werden; Listing als `.SCREEN "…"` |
| `lohi`, `pointers`, `words`, `bytes` | Zugriffsmuster (`LDA tab,X` mit gleichem Index auf zwei Tabellen, …) |
| `sprites` | 63-Byte-Blöcke an 64-Byte-Grenzen, auf die Sprite-Zeiger (`$07F8–$07FF` bzw. Bildschirm + `$3F8`) zeigen; Listing mit Bitmuster-Kommentar |
| `charset` | 2-KiB-Bereich, auf den `$D018` zeigt |
| `bitmap`, `colors` | 8000 Byte Bitmap / 1000 Byte Farben im Bitmap-Modus |
| `basic` | BASIC-Programm am Anfang (detokenisiert als Kommentar, `.BASIC` für einen reinen Starter) |
| `zeros` | Nullbereiche → `.FILL` |

## Ohne Modell: von Hand oder durch einen Agenten

`agent/m6510_drive.py` (wie `m68k_drive.py`): dieselben Werkzeuge, Berichte und Prüfungen, aber ein Schritt je Aufruf, Zustand
in einem Arbeitsverzeichnis (`init`, `status`, `show`, `tool`, `finish`, `listing`, `report`). Für einen Agenten oder einen
Menschen, der die Rolle des Modells übernimmt. Anleitung: [walkthrough.md](walkthrough.md).

## Projektdateien

JSON mit allen Annotationen (Namen, Routinen-Dokumentation, Variablen, Datenbereiche, Kommentare, Konstanten, Titel), dazu
Prüfsumme und Ladeadresse des Programms; `ann_save`/`ann_load`. Das Format folgt dem MC68000-Projekt.

## Was noch fehlt (bekannt im Voraus)

- Erprobung mit einem echten Modell (bei allen drei Projekten offen)
- Musik-Player (SID) als eigene Datenart, Rasterzeilen-Logik (IRQ-Ketten) als eigene Sicht
- Schnelllader und Kopierschutz – nur, soweit sie im Speicher sichtbar werden ([unpacking.md](unpacking.md))
