# Benutzung von `m6510tool`

> **Stand 0.4:** `asm`, `dis`, `basic list`, `monitor` und `serve` sind umgesetzt. `d64`, `t64`, `crt` und die
> Disassembler-Eingabe aus Images sind die Spezifikation für die Meilensteine 0.5–0.6. Optionen orientieren
> sich an `z80tool` und `m68ktool`, damit man zwischen den Projekten nicht umlernen muss.

```sh
m6510tool help
m6510tool version
```

Rückgabewerte: `0` Erfolg, `1` Fehler im Quelltext bzw. in der Eingabedatei, `2` falsche Kommandozeile oder E/A-Problem.

## `asm` – Assembler

```sh
m6510tool asm quelle.asm [-o ausgabe.prg] [--raw] [-l listing|-] [-s symbole] [--vice-labels datei]
                         [--org adresse] [--no-illegal|--unstable] [-D name=wert]
```

| Option | Bedeutung |
|---|---|
| `-o datei` | Ausgabedatei (Standard: Quelltextname mit `.prg`) |
| `--raw` | ohne Ladeadresse |
| `-l datei` | Listing (`-` = Bildschirm) |
| `-s datei` | Symboltabelle |
| `--vice-labels datei` | Labeldatei für den VICE-Monitor |
| `--org adresse` | Startadresse, falls der Quelltext keine setzt (Standard `$0801`) |
| `--no-illegal`, `--unstable` | erlaubte Opcodes (siehe [assembler-syntax.md](assembler-syntax.md#wahl-der-befehlsform)) |
| `-D name[=wert]` | Symbol vorgeben (ohne Wert: 1) |

Ausgabe auf der Konsole: `datei: N bytes, $XXXX - $YYYY, 0 error(s)` (auf stderr, wenn Listing oder Symbole nach `-`
gehen). Fehler: `datei:zeile: error: text`, Rückgabewert 1, keine Ausgabedatei.

## `dis` – Disassembler

```sh
m6510tool dis eingabe [--org adresse] [--raw] [--from a] [--to b] [-e einstieg …] [--linear] [-c kommentare.txt]
                      [--asm] [--no-bytes] [--no-xrefs] [--style dollar|c|h] [--illegal none|stable|all]
                      [--symbols datei] [--no-kernal-names] [-o ausgabe]
```

| Option | Bedeutung |
|---|---|
| Eingabe | PRG/P00 (Ladeadresse aus der Datei; `--org` verschiebt), Raw (`--raw` oder Endung `.bin`/`.raw`; braucht `--org`); geplant: `disk.d64:NAME`, `band.t64:NAME`, CRT (`--bank n`) |
| `--from a`, `--to b` | nur diesen Bereich zerlegen (`--to` **einschließlich**); Standard: das ganze Programm |
| `-e adresse` | Einstiegspunkt (mehrfach); Standard: die `SYS`-Adresse eines BASIC-Starters, sonst die Ladeadresse |
| `--linear` | ohne Ablaufanalyse alles der Reihe nach dekodieren |
| `-c datei` | Kommentardatei ([formats.md](formats.md#kommentardatei-disassembler--c)); kommentierte Adressen sind Einstiegspunkte |
| `--asm` | wieder assemblierbarer Quelltext (mit Labels, byte-genau) |
| `--no-bytes` | Bytes nicht anzeigen |
| `--no-xrefs` | keine Aufruferliste an den Labels |
| `--style` | Zahlenschreibweise `$1F` (Standard), `0x1F` oder `1FH` |
| `-o datei` | Ausgabe in eine Datei (Standard: Bildschirm) |
| `--illegal` | welche undokumentierten Opcodes als Befehle gelten (Standard `stable`) |
| `--symbols datei` | Symbole vorgeben (Format der Symboltabelle `-s` oder VICE-Labels); Namen im Bereich werden zu Labels |
| `--no-kernal-names` | KERNAL-Einsprünge nicht als Namen zeigen (I/O-Registernamen und `--no-io-names` kommen mit 0.3) |

Ohne `-e` und Kommentardatei beginnt die Ablaufanalyse an der `SYS`-Adresse eines BASIC-Starters, sonst an der
Ladeadresse. Ein BASIC-Starter, wie ihn `.BASIC` erzeugt, erscheint als `.BASIC zeile,ziel`; andere BASIC-Zeilen bleiben
`.BYTE` (lesbar mit `basic list`).

## `basic` – BASIC-Programme

```sh
m6510tool basic list prog.prg        # detokenisiertes Listing
```

## `d64`, `t64`, `crt` – Images (geplant, 0.6)

```sh
m6510tool d64 info  disk.d64                 # Name, ID, freie Blöcke, Fehlerbytes
m6510tool d64 ls    disk.d64                 # Verzeichnis
m6510tool d64 extract disk.d64 "NAME" [-o datei.prg]
m6510tool d64 boot  disk.d64 [-o datei.prg]  # erste Datei wie LOAD"*",8,1
m6510tool d64 block disk.d64 spur sektor     # Hexdump eines Blocks
m6510tool t64 ls|extract band.t64 …
m6510tool crt info  modul.crt                # Typ, Leitungen, Chips/Bänke
```

## `monitor` – interaktiver Monitor

```sh
m6510tool monitor [datei] [--org adresse] [--c64] [--roms verzeichnis] [--no-files] [--quiet]
m6510tool monitor < examples/monitor_session.txt
```

| Option | Wirkung |
|---|---|
| `datei` | PRG/P00 (Ladeadresse aus der Datei) oder Raw (`.bin`/`.raw`, braucht `--org`) laden; PC = Ladeadresse bzw. `SYS`-Ziel |
| `--org adresse` | Ladeadresse (überschreibt die des PRG; wie bei `asm`/`dis` dezimal, `$` für hex) |
| `--c64` | C64-Speicherkarte mit KERNAL-Emulation statt 64 KiB RAM |
| `--roms verzeichnis` | C64 mit eigenen ROMs (`basic*`, `kernal*`, `char*`); ein KERNAL-ROM ersetzt die Emulation |
| `--no-files` | Dateibefehle im Monitor sperren |
| `--quiet`, `-q` | kein Banner und kein Prompt (bei umgeleiteter Eingabe ohnehin) |

Siehe [monitor.md](monitor.md).

## `serve` – Emulator-Server für Agenten

```sh
m6510tool serve [--allow-files]
m6510tool serve < examples/agent_session.jsonl
```

Eine JSON-Anfrage je Zeile auf stdin, eine Antwort je Zeile auf stdout; `--allow-files` erlaubt Dateizugriff. Siehe
[agent-protocol.md](agent-protocol.md).
