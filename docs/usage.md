# Benutzung von `m6510tool`

> **Stand 0.1:** nur `help` und `version`. Die übrigen Befehle sind die Spezifikation für die Meilensteine 0.2–0.6;
> Optionen orientieren sich an `z80tool` und `m68ktool`, damit man zwischen den Projekten nicht umlernen muss.

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
| `-D name=wert` | Symbol vorgeben |

Ausgabe auf der Konsole: `datei: N bytes, $XXXX - $YYYY, E error(s)`.

## `dis` – Disassembler

```sh
m6510tool dis eingabe [--org adresse] [--from a --to b] [-e einstieg …] [--linear] [-c kommentare.txt]
                      [--asm] [--no-bytes] [--style dollar|c|h] [--illegal none|stable|all] [--symbols datei]
                      [--no-kernal-names] [--no-io-names] [-o ausgabe]
```

| Option | Bedeutung |
|---|---|
| Eingabe | PRG/P00 (Ladeadresse aus der Datei), Raw (braucht `--org`), `disk.d64:NAME` (Datei aus einem D64), `band.t64:NAME`, CRT (`--bank n`) |
| `-e adresse` | Einstiegspunkt (mehrfach); Standard: die `SYS`-Adresse eines BASIC-Starters, sonst die Ladeadresse |
| `--linear` | ohne Ablaufanalyse alles der Reihe nach dekodieren |
| `-c datei` | Kommentardatei ([formats.md](formats.md#kommentardatei-disassembler--c)); kommentierte Adressen sind Einstiegspunkte |
| `--asm` | wieder assemblierbarer Quelltext (mit Labels, byte-genau) |
| `--no-bytes` | Bytes nicht anzeigen |
| `--illegal` | welche undokumentierten Opcodes als Befehle gelten (Standard `stable`) |
| `--symbols datei` | Symbole vorgeben (Format der Symboltabelle oder VICE-Labels) |
| `--no-kernal-names`, `--no-io-names` | KERNAL-Einsprünge bzw. I/O-Register nicht als Namen zeigen |

## `basic` – BASIC-Programme

```sh
m6510tool basic list prog.prg        # detokenisiertes Listing
```

## `d64`, `t64`, `crt` – Images

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
m6510tool monitor [datei] [--org adresse] [--c64] [--roms verzeichnis] [--no-files]
```

Siehe [monitor.md](monitor.md).

## `serve` – Emulator-Server für Agenten

```sh
m6510tool serve [--allow-files]
```

Siehe [agent-protocol.md](agent-protocol.md).
