# Reverse Engineering mit lokalem LLM

> **Stand 0.5:** umgesetzt – Analyse, Sprungtabellen, Datentypen, Annotationen, verifiziertes Listing und Screenshots im
> Server (`m6510tool serve`, [agent-protocol.md](agent-protocol.md#reverse-engineering-reverse-engineeringmd)), der
> Orchestrator `agent/m6510_annotate.py` und `agent/m6510_drive.py` (getestet gegen einen Mock-Ollama, noch nicht mit einem
> echten Modell erprobt). D64/T64 als Quelle seit 0.6 (`--prg disk.d64:NAME`). Vorbild sind `agent/z80_annotate.py` (mit Screenshots für Qwen3-VL) und
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

## Benutzung

```sh
python3 agent/m6510_annotate.py --prg spiel.prg --project spiel.m6510proj.json --out spiel_kommentiert.asm \
        --lang Deutsch --model qwen3-vl:8b-instruct --log spiel.log.jsonl --report luecken.md
python3 agent/m6510_annotate.py --prg spiel.prg --dry-run            # Analyse ohne Modell: Reihenfolge, Tabellen, Token-Schätzung
python3 agent/m6510_annotate.py --prg "disk.d64:SPIEL" …              # direkt aus einem Diskettenabbild (auch band.t64:NAME)
python3 agent/m6510_annotate.py --bin teil.bin --org 0xC000 --machine generic --entry 0xC000
```

| Option | Wirkung |
|---|---|
| `--prg` / `--bin` + `--org` | Programm (PRG/P00 oder `disk.d64:NAME`/`band.t64:NAME`; `--org` verschiebt) bzw. Raw-Datei an eine Adresse |
| `--machine c64\|generic` | C64 mit KERNAL-Emulation (Standard) oder 64 KiB RAM ohne I/O |
| `--entry` | Einstiegspunkte (mehrfach); Standard: `SYS`-Adresse des BASIC-Starters, sonst die Ladeadresse |
| `--project`, `--out`, `--report`, `--log` | Projektdatei (wird nach jeder Routine gespeichert, beim Start geladen), Listing, Lückenbericht, Protokoll |
| `--model`, `--refine-model` (mehrfach), `--refine-all`, `--refine-medium` | Modell des ersten Durchgangs, weitere Durchgänge für Routinen mit Lücken |
| `--think`, `--max-tokens`, `--num-ctx`, `--temperature`, `--max-turns` | Modellparameter wie bei den Schwesterprojekten |
| `--only` | nur diese Routine(n) |
| `--no-auto-tables`, `--no-auto-data` | vorgeschlagene Zeigertabellen nicht verfolgen bzw. erkannte Daten nicht markieren |
| `--lang` | Sprache der Kommentare |
| `--snapshot` + `--from`/`--to` | einen gesicherten Maschinenzustand als Ausgangspunkt (z. B. ein laufendes Spiel nach dem Entpacken); `--from`/`--to` geben den Bereich des Programms im Speicher an |
| `--hints datei.json` | ausgeführte Adressen (`{"cmd":"executed","addresses":true}` nach Spielläufen mit `track_execution`) als Hinweise für Code, den der Codefluss nicht erreicht |
| `--attach` | Disketten-/Bandabbild oder Datei für `LOAD` (Programme, die nachladen) |

Werkzeuge des Modells: `get_listing`, `disassemble`, `read_memory` (mit PETSCII- und Bildschirmcode-Spalte), `run_routine`
(Register, Speicher, `setup`-Routinen, `screenshot`, `as_interrupt`), `screenshot`, `reset_machine`, `name_label`,
`document_routine`, `name_variable`, `mark_data` (alle Datenarten inklusive `lohi` mit `partner`), `add_comment`,
`mark_constant`, `remove_annotation`, `finish_routine`. Ein Modell ohne Bildverständnis (Ollama meldet keine Fähigkeit
`vision`) bekommt statt der Bilder den Bildschirm als Text.

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
- **Zero Page:** benutzte Adressen je Routine, Zeigerpaare (`($FB),Y` → `$FB/$FC`); mit `ann_variable … size:2` heißen
  die Zugriffe `ptr` und `ptr+1`.
- **Zeiger als Immediate-Paare:** `LDA #<text / LDY #>text` (Registerpaar A/Y bzw. X/Y wie bei den KERNAL-Aufrufen) und
  `LDA #<x / STA z / LDA #>x / STA z+1` werden erkannt, wenn das Ziel im Programm liegt; das Listing schreibt `#<name` /
  `#>name`, solange der Befehl nicht mit `ann_constant` als reine Zahl markiert ist.
- **Interrupt-Handler:** ein so eingetragener Zeiger in `$0314` (IRQ), `$0316` (BRK), `$0318` (NMI), `$FFFE` oder `$FFFA`
  macht das Ziel zu einem Einstiegspunkt.
- **Selbstmodifizierender Code:** Befehle, deren Operand von anderem Code geschrieben wird (`STA patch+1`), werden markiert;
  das Listing schreibt `patch+1` und kommentiert die Stelle („self-modifying: written by $xxxx“). Das Ziel eines solchen
  Befehls ist das, was im Speicher steht – die Analyse kann den späteren Wert nicht kennen.
- **Banking:** Schreibt das Programm `$01`, wird das je Routine gemeldet (`port_writes` mit dem Wert, wenn er direkt davor
  geladen wird); Code unter ROM wird in der RAM-Sicht analysiert.

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
| `words`, `pointers` | Tabelle, auf die mit `LDA tab,X` und `LDA tab+1,X` zugegriffen wird; `pointers`, wenn jeder Eintrag auf einen Befehl zeigt |
| `lohi`, `rts_pointers` | aus den Sprungtabellen (oder von Hand annotiert); Listing als `.LO`/`.HI` bzw. `.WORD ziel-1` |
| `sprites` | 64-Byte-Block, auf den ein Sprite-Zeiger (`LDA #n / STA $07F8–$07FF`) zeigt; Listing mit Bitmuster-Kommentar |
| `charset` | Bereich, auf den `LDA #n / STA $D018` zeigt (außer dem Zeichensatz-ROM bei `$1000/$1800`); Listing 8 Byte je Zeichen |
| `basic` | BASIC-Programm bei `$0801`; ein reiner Starter wird `.BASIC zeile,ziel` |
| `zeros` | Nullbereiche ab 8 Byte (das Listing schreibt ab 16 Byte `.FILL`) |
| `bitmap`, `colors` | 8000 Byte Bitmap / 1000 Byte Farben im Bitmap-Modus – Idee |

## Ohne Modell: von Hand oder durch einen Agenten

`agent/m6510_drive.py` (wie `m68k_drive.py`): dieselben Werkzeuge, Berichte und Prüfungen, aber ein Schritt je Aufruf, Zustand
in einem Arbeitsverzeichnis (`init`, `status`, `show`, `tool`, `finish`, `listing`, `report`). Jeder Befehl sperrt das
Arbeitsverzeichnis: Mehrere Agenten können parallel an verschiedenen Routinen desselben Programms arbeiten und sehen dabei die
Namen der anderen. Für einen Agenten oder einen
Menschen, der die Rolle des Modells übernimmt. Anleitung: [walkthrough.md](walkthrough.md).

## Projektdateien

JSON mit allen Annotationen (Namen, Routinen-Dokumentation, Variablen, Datenbereiche, Kommentare, Konstanten, Titel), dazu
Prüfsumme und Ladeadresse des Programms; `ann_save`/`ann_load`. Das Format folgt dem MC68000-Projekt.

## Was noch fehlt (bekannt im Voraus)

- Erprobung mit einem echten Modell (bei allen drei Projekten offen)
- Musik-Player (SID) als eigene Datenart, Rasterzeilen-Logik (IRQ-Ketten) als eigene Sicht
- Schnelllader und Kopierschutz – nur, soweit sie im Speicher sichtbar werden ([unpacking.md](unpacking.md))
