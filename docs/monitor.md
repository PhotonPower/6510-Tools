# Monitor

> **Stand 0.1:** geplant (Meilenstein 0.4). Befehle und Bedienung übernehmen den Monitor der Schwesterprojekte
> ([Z80](https://github.com/PhotonPower/Z80/blob/main/docs/monitor.md),
> [MC68000](https://github.com/PhotonPower/MC68000/blob/main/docs/monitor.md)), ergänzt um C64-Befehle.

`m6510tool monitor` wird ein klassischer Maschinensprache-Monitor mit Eingabeschleife: Speicher ansehen und ändern,
disassemblieren, Zeilen assemblieren, Programme im Emulator einzeln oder bis zu einem Haltepunkt ausführen. Alles basiert auf
den Bausteinen der Bibliothek (`m6510::Cpu`, Dekoder, Assembler, `C64`). Die Klasse `m6510::Monitor` hat keine eigene
Ein-/Ausgabe (`execute(zeile) → Text`), sodass sie sich direkt testen oder in andere Oberflächen einbauen lässt.

```sh
m6510tool monitor                          # leerer Speicher (generische Maschine, 64 KiB RAM)
m6510tool monitor programm.prg             # PRG laden (Ladeadresse aus der Datei), PC = SYS-Adresse
m6510tool monitor --c64                    # C64-Speicherkarte mit KERNAL-Emulation
m6510tool monitor --c64 --roms ~/c64roms   # mit eigenen ROMs (basic, kernal, chargen)
m6510tool monitor --no-files               # Dateibefehle sperren
m6510tool monitor < befehle.txt            # Skript: ohne Banner und Prompt
```

Ohne Zeileneditor; mit `rlwrap m6510tool monitor` bekommt man Pfeiltasten und Verlauf.

## Zahlen

**Argumente sind hexadezimal**, wie bei klassischen Monitoren: `d c000`, `e 0400 08 05 0c 0c 0f`.

| Schreibweise | Bedeutung |
|---|---|
| `C000`, `FF`, `D020` | hexadezimal (auch wenn mit einem Buchstaben begonnen wird) |
| `100.` | **dezimal** (Punkt dahinter) |
| `$FF`, `0xFF`, `%1010` | mit Präfix wie im Assembler |
| `start`, `loop+3`, `<text`, `>text` | Symbole und Ausdrücke |
| `@A`, `@X`, `@Y`, `@S`, `@PC`, `@P` | Registerinhalt |

Ein bekanntes Symbol hat Vorrang vor einer gleichlautenden Hexzahl (`sym -name` entfernt es). Adressen werden auf 16 Bit
beschränkt.

**Im Assemble-Modus** (Befehl `a`) gilt die Syntax des Assemblers: `LDA #10` ist dezimal, hex schreibt man `#$0A`
([assembler-syntax.md](assembler-syntax.md)).

## Befehle

| Befehl | Wirkung |
|---|---|
| `d [adr [n]]` | disassemblieren (`n` Befehle, Standard 16; ohne Adresse: Fortsetzung). `>` markiert den PC, Marken stehen als `NAME:` davor |
| `m [adr [n]]` | Speicher als Hex + ASCII/PETSCII (`n` Bytes, Standard 128); `m.s` zeigt die Bytes als Bildschirmcodes |
| `e adr b…` | Bytes schreiben; auch Text: `e 2000 "HALLO" 0`; `e.w` schreibt Wörter (Little-Endian) |
| `f von bis b…` | füllen (Muster wird wiederholt); `bis` ist **einschließlich** |
| `c von bis ziel` | vergleichen (zeigt bis zu 32 Unterschiede) |
| `t von bis ziel` | kopieren (überlappend korrekt) |
| `h von bis b…` | suchen: Bytes oder `"Text"` (bis zu 64 Treffer) |
| `a [adr]` | **Zeilen assemblieren**; leere Zeile beendet. Labels (`loop:`) und frühere Symbole sind verwendbar |
| `r [name=wert …]` | Register anzeigen / setzen: `A`, `X`, `Y`, `S`, `PC`, `P` (`r pc=c000 a=@x p=24`); Anzeige mit Flags `NV-BDIZC` und Zyklenzähler |
| `n [anzahl]` | Einzelschritt(e); bei 2–20 Schritten wird jeder Befehl gezeigt |
| `o` | Step over: ein `JSR` läuft bis zur Rückkehr |
| `u` | läuft, bis die aktuelle Routine zurückkehrt (`RTS`/`RTI`) |
| `g [adr]` | ab `adr` (oder PC) laufen lassen bis Haltepunkt / Watchpoint / `JAM` / Limit |
| `ru adr` | laufen, bis `PC == adr` |
| `b [adr…]`, `bc [adr…]` | Haltepunkte setzen / löschen; `b` ohne Argument listet Haltepunkte und Watchpoints |
| `w adr [länge] [r\|w\|rw]` | Watchpoint (Standard: 1 Byte, Schreiben) |
| `limit n` | höchstens `n` Befehle pro Lauf (Standard 10 000 000) |
| `sym [name wert \| -name]` | Symbole anzeigen, setzen, löschen |
| `symload datei` / `symsave datei` | Symboldateien (Format der Assembler-Ausgabe `-s`; VICE-Labeldateien werden ebenfalls gelesen) |
| `asm datei [org]` | Quelltext-Datei assemblieren und in den Speicher laden; Symbole werden übernommen |
| `l datei [adr]` | Datei laden: **PRG**/P00 (Ladeadresse aus der Datei, `adr` überschreibt), **Raw** (braucht `adr`), `disk.d64:NAME`, `band.t64:NAME` |
| `sv datei von bis [raw]` | Speicherbereich als PRG (Standard) oder Raw speichern (einschließlich `bis`) |
| `ck name` / `rb name` | Checkpoint (CPU + Speicher + Maschine) anlegen / zurückspielen |
| `savestate datei` / `loadstate datei` | Snapshot-Datei (`M6510SN1`) |
| `irq` / `nmi` | Interrupt anfordern; IRQ wird angenommen, wenn `I` = 0 |
| `reset [mem]` | CPU zurücksetzen (mit `mem` auch Speicher löschen) |
| `?`, `q` | Hilfe, Beenden |

C64-Befehle (nur mit `machine c64`):

| Befehl | Wirkung |
|---|---|
| `machine [generic\|c64 [kernal\|rom]]` | Maschine anzeigen / wählen (löscht Speicher und Symbole); `kernal` = KERNAL-Emulation (Standard), `rom` = eigene ROMs |
| `bank [wert]` | Speicherkonfiguration (`$01`) anzeigen / setzen, mit Tabelle der sichtbaren Bereiche |
| `rom basic\|kernal\|char datei` | ROM-Abbild laden |
| `io` | Schreibprotokoll der Bausteinregister mit Namen (wird beim Lesen geleert) |
| `kernal` | KERNAL-Aufrufe seit dem letzten Lesen (nachgebildet / nur protokolliert) |
| `console` | Text, den das Programm über `CHROUT` ausgegeben hat |
| `screen` | Bildschirminhalt als Text (40×25, Bildschirmcodes → Zeichen) |
| `shot datei.png [x y b h] [zoom]` | Bildschirm als PNG speichern |
| `keys "text"` | Tastenpuffer für `GETIN`/`CHRIN` füllen |
| `run` | Programm wie mit `RUN` starten (BASIC-Starter → `SYS`-Adresse) |

Fehler erscheinen als `? Meldung` und ändern den Zustand nicht. Nach jedem Lauf (`n`, `o`, `u`, `g`, `ru`) zeigt der
Monitor den Grund (`stopped: breakpoint at C014  [19 instructions, 58 cycles]`), die Register und den nächsten Befehl.

## Beispiel (Ziel)

Eingabe (als Skript, künftig `examples/monitor_session.txt`):

```
a c000
start: LDX #$05
       LDA #$00
loop:  CLC
       ADC #$03
       DEX
       BNE loop
       STA $0400
       RTS

d start 8
b loop
g start
r
bc
u
m 0400 1
q
```

Erwartete Ausgabe (Auszug):

```
  C000  A2 05     LDX #$05
  C002  A9 00     LDA #$00
  C004  18        CLC
  C005  69 03     ADC #$03
  C007  CA        DEX
  C008  D0 FA     BNE loop
  C00A  8D 00 04  STA $0400
  C00D  60        RTS
...
stopped: breakpoint at C004  [2 instructions, 4 cycles]
...
0400  0F                                                .
```

## Grenzen

- Zeilenweises Assemblieren kennt **keine Vorwärtsreferenzen** (mit `sym name adr` vorher festlegen oder `asm datei`).
- Keine Befehlsverlauf-/Zeilenbearbeitung im Programm selbst (siehe `rlwrap`).
- Die Analyse- und Annotationsbefehle des Agenten-Protokolls (`analyze`, `listing`, `ann_*`) gibt es im Monitor nicht;
  sie gehören zu `m6510tool serve` ([reverse-engineering.md](reverse-engineering.md)).
