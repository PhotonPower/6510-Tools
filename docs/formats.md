# Dateiformate

> **Stand 0.6:** Kommentardatei, Raw, PRG, P00 (lesen), BASIC (detokenisieren, Starter erzeugen/erkennen), alle
> Ausgabeformate des Assemblers, Snapshots, Annotationen, PNG-Screenshots und das Lesen von T64, D64 und CRT sind umgesetzt.
> D64 schreiben ist eine Idee, Intel HEX ist zurückgestellt. Alle Leser lehnen beschädigte Dateien mit einer Fehlermeldung ab; Robustheitstests mit mutierten Dateien gehören
> zu jedem Leser ([testing.md](testing.md)).

## Kommentardatei (Disassembler, `-c`)

Eine Zeile pro Kommentar: `adresse,"text"`. Adressen dezimal, `$hex` oder `0xhex`; Zeilen mit `;` am Anfang sind
Kommentare. **Kommentierte Adressen sind Einstiegspunkte** der Ablaufanalyse. Beispiel: `examples/hello.comments.txt`.

```
; address,"text"
$080D,"entry: print the message below"
```

## Raw-Binary

Nur Bytes, keine Adresse. Beim Laden muss die Adresse angegeben werden (`--org`, `addr`).

## PRG

Das Standardformat des C64 (Dateityp `PRG` auf Diskette): **zwei Byte Ladeadresse** (Little-Endian), dann die Daten.
`$0801` ist die Ladeadresse eines BASIC-Programms. Länge höchstens 65536 − Ladeadresse; längere Dateien werden abgelehnt
(der C64 würde den Speicher umlaufend überschreiben).

## P00 (PC64)

`C64File` + NUL (8 Byte), 17 Byte Dateiname (PETSCII, mit NUL aufgefüllt), 1 Byte Satzlänge (REL), dann die PRG-Daten.
`.P00`/`.S00`/`.U00`/`.R00` stehen für die Dateitypen; gelesen wird P00 (als PRG, erkannt an der Signatur, nicht an der
Endung). S00/U00 als Rohdaten: geplant.

## BASIC-Programme (tokenisiert)

Ein BASIC-Programm im Speicher ist eine Kette von Zeilen:

| Bytes | Inhalt |
|---|---|
| 2 | Adresse der nächsten Zeile (Link); `$0000` beendet das Programm |
| 2 | Zeilennummer |
| n | Text mit **Token** `$80–$CB` für Schlüsselwörter (`$9E` = `SYS`, `$99` = `PRINT`, …), `$FF` = π |
| 1 | `$00` Zeilenende |

`m6510tool basic list prog.prg` detokenisiert (Ausgabe in ASCII, Steuerzeichen als `{CLR}`, `{RED}` …, andere Zeichen als
`{$xx}`; in Strings und hinter `REM` werden Bytes nicht als Token gelesen). Der Disassembler zeigt einen `SYS`-Starter, der
genau dem von `.BASIC` erzeugten entspricht, als `.BASIC zeile,ziel`; andere BASIC-Zeilen bleiben `.BYTE` (die `SYS`-Adresse
der ersten Zeile wird trotzdem als Einstiegspunkt benutzt). Die Token-Tabelle ist die des BASIC V2.

## Intel HEX (zurückgestellt)

Wie im Z80-Projekt lesbar und schreibbar, für den Austausch mit EPROM-Brennern und anderen Werkzeugen. Niedrige Priorität.

## T64 (Tape-Archiv)

Kopf (64 Byte: Signatur `C64S tape …`, Version, Zahl der Einträge, Bandname), Verzeichnis (32 Byte je Eintrag: Typ,
Dateityp, Start- und Endadresse, Offset in der Datei, Name), dann die Daten. Bekannte Fehler in verbreiteten T64-Dateien
(falsche Endadresse, Zahl der benutzten Einträge = 0) werden toleriert und gemeldet.

## D64 (Diskettenabbild 1541)

- 35 Spuren (174 848 Byte) oder 40 Spuren (196 608 Byte), jeweils optional mit einem **Fehlerbyte je Sektor** angehängt.
- Sektoren je Spur: 21 (Spur 1–17), 19 (18–24), 18 (25–30), 17 (31–35, auch 36–40); 256 Byte je Sektor.
- Spur 18, Sektor 0: **BAM** (Belegung, Diskettenname, ID); ab Spur 18, Sektor 1: **Verzeichnis** (8 Einträge à 32 Byte je
  Sektor: Dateityp `DEL`/`SEQ`/`PRG`/`USR`/`REL`, „geschlossen“/„geschützt“, erster Sektor, Name in PETSCII mit `$A0`
  aufgefüllt, Größe in Blöcken).
- Dateien sind Sektorketten: die ersten beiden Bytes eines Sektors zeigen auf den nächsten (Spur, Sektor); Spur 0 = letzter
  Sektor, das zweite Byte ist dann die Position des letzten benutzten Bytes.
- Geprüft wird auf Schleifen in Ketten, Zeiger außerhalb der Diskette und falsche Größen.
- `m6510tool d64 info|ls|extract|boot|block`: Überblick, Verzeichnis, Datei (als PRG), erstes Programm wie
  `LOAD"*",8,1`, rohen Block ansehen. Kreuzprüfung gegen `c1541` aus VICE in der CI ([testing.md](testing.md)).
- Namen werden wie auf dem C64 gesucht: `*` passt auf den Rest, `?` auf ein Zeichen, `*` allein auf die erste Datei;
  Groß-/Kleinschreibung zählt beim Vergleich der Texte nicht. Bei 40 Spuren zählen die freien Blöcke nur Spur 1–35 (die BAM-
  Erweiterungen von SpeedDOS & Co. werden nicht ausgewertet).
- Schreiben (`D64 → Diskette`) gibt es nicht; die Tests bauen ihre Images selbst.
- Nicht unterstützt: G64 (GCR-Rohdaten, Kopierschutz), D71/D81 (Idee), Schnelllader und eigene Diskettenformate (die
  Blöcke lassen sich mit `block` trotzdem lesen).

## CRT (Cartridge)

Kopf `C64 CARTRIDGE   ` (Länge, Version, **Hardwaretyp**, `EXROM`/`GAME`, Name), dann `CHIP`-Pakete (Typ, Bank,
Ladeadresse, Größe, Daten). Typ 0 (normale 8K-/16K-/Ultimax-Cartridge) läuft im Emulator: `load_file`/Monitor `l` stecken
sie in die C64-Maschine ([c64.md](c64.md)), der PC steht auf ihrem Kaltstartvektor (Signatur `CBM80` bei `$8004`) bzw.
im Ultimax-Modus auf ihrem RESET-Vektor. Bank-umschaltende Typen (Ocean, Magic Desk, EasyFlash, Action Replay …) zeigt
`m6510tool crt info` mit Namen; ihre Bänke lassen sich mit `crt extract` einzeln disassemblieren, laufen aber nicht.

## Ausgabe des Assemblers

| Datei | Option | Format |
|---|---|---|
| Programm | `-o x.prg` | PRG (Standard) |
| Programm | `-o x.bin --raw` | Raw |
| Listing | `-l datei` (`-` = Bildschirm) | Adresse, Bytes, Quelltextzeile |
| Symboltabelle | `-s datei` | `name = $XXXX`, nach Namen sortiert (auch vom Monitor lesbar: `symload`) |
| VICE-Labels | `--vice-labels datei` | `al C:XXXX .name` (für den VICE-Monitor) |

## Snapshot (`M6510SN1`)

Eigenes Format für `save_state`/`load_state` (auch Monitor `savestate`/`loadstate`), alle Zahlen Little-Endian:

| Teil | Inhalt |
|---|---|
| Kopf | `M6510SN1`, Version (1) |
| CPU | `A`, `X`, `Y`, `S`, `P`, `PC` (2), Flags (angehalten, IRQ-Leitung, NMI anstehend, I-Verzögerung, Port aktiv), Port-Richtung, Port-Daten, Befehle (8), Zyklen (8), Interrupts (8), Aufruftiefe (4) |
| RAM | 65 536 Byte |
| C64 (optional, Kennbyte) | Farb-RAM (1024), VIC (64), SID (32), CIA 1 (16), CIA 2 (16), Konfiguration, `$D019`-Latch, CIA-1-ICR und -Maske, Timer-Latch (2), Timer-Start (8), nächster Unterlauf (8), letzte Rasterprüfung (8), CIA-2-ICR und -Maske, Zufallszustand (4), IRQ von außen |
| KERNAL-Emulation (optional, Kennbyte) | Zahl der Tasten im Puffer (2), Tasten |

Beschädigte Dateien werden abgelehnt (jede Abkürzung, falsche Kennbytes, Daten am Ende). ROMs werden **nicht** im Snapshot
gespeichert; ein Snapshot passt nur zur gleichen Maschinenart (`generic` bzw. `c64`).

VICE-Snapshots (`.vsf`) zu **lesen** wäre nützlich (Programme im laufenden Zustand aus dem Emulator holen); eine Idee für
nach 0.6.

## Annotationen

`ann_save`/`ann_load` (und `ann_list`) benutzen JSON im Format des MC68000-Projekts; Adressen als `"0xC000"`:

```json
{"version": 1, "title": "Spiel",
 "labels":   [{"addr": "0x082D", "name": "print"}],
 "routines": [{"addr": "0x082D", "name": "print", "summary": "…", "inputs": "A/Y = Adresse", "outputs": "",
               "clobbers": "A Y", "confidence": "high"}],
 "variables": [{"name": "ptr", "addr": "0x00FB", "size": 2, "comment": ""}],
 "data":     [{"addr": "0x08B3", "length": 13, "kind": "text", "name": "", "comment": ""},
              {"addr": "0x08A7", "length": 3, "kind": "lohi", "partner": "0x08AA", "minus_one": true, "name": "", "comment": ""}],
 "comments": [{"addr": "0x0860", "text": "…"}], "block_comments": [], "constants": [{"addr": "0x080D"}]}
```

Datenarten: `bytes`, `words`, `text`, `screen`, `pointers`, `rts_pointers`, `lohi`, `sprites`, `charset`. Beim Laden gelten
dieselben Regeln wie beim Annotieren (gültige, eindeutige Namen; Datenbereiche ohne Überlappung); eine verletzte Regel
lehnt die ganze Datei ab.
