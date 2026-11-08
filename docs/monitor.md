# Monitor

> **Stand 0.5:** umgesetzt (`m6510::Monitor`, `m6510tool monitor`; `shot` seit 0.5). Befehle und Bedienung übernehmen den Monitor der
> Schwesterprojekte ([Z80](https://github.com/PhotonPower/Z80/blob/main/docs/monitor.md),
> [MC68000](https://github.com/PhotonPower/MC68000/blob/main/docs/monitor.md)), ergänzt um C64-Befehle. Noch offen:
> Laden aus D64/T64 (Meilenstein 0.6).

`m6510tool monitor` ist ein klassischer Maschinensprache-Monitor mit Eingabeschleife: Speicher ansehen und ändern,
disassemblieren, Zeilen assemblieren, Programme im Emulator einzeln oder bis zu einem Haltepunkt ausführen. Alles basiert auf
den Bausteinen der Bibliothek (`m6510::Cpu`, Dekoder, Assembler, `C64`, `Kernal`). Die Klasse `m6510::Monitor` hat keine
eigene Konsolen-Ein-/Ausgabe (`execute(zeile) → Text`), sodass sie sich direkt testen oder in andere Oberflächen einbauen
lässt. Maschine, Symbole, Checkpoints und Watchpoints verwaltet `m6510::Machine`, dieselbe Klasse, die auch
`m6510tool serve` benutzt.

```sh
m6510tool monitor                          # leerer Speicher (generische Maschine, 64 KiB RAM)
m6510tool monitor programm.prg             # PRG/P00 laden (Ladeadresse aus der Datei), PC = SYS-Adresse
m6510tool monitor --org '$2000' daten.bin  # Raw-Datei an eine Adresse laden (Option dezimal, $ für hex)
m6510tool monitor --c64 programm.prg       # C64-Speicherkarte mit KERNAL-Emulation
m6510tool monitor --roms ~/c64roms         # C64 mit eigenen ROMs (basic*, kernal*, char*/chargen*)
m6510tool monitor --no-files               # Dateibefehle sperren
m6510tool monitor < befehle.txt            # Skript: ohne Banner und Prompt
```

Banner, Registeranzeige beim Start und Prompt (`-`, im Assemble-Modus die Adresse wie `C004:`) erscheinen nur, wenn die
Eingabe ein Terminal ist (oder nie, mit `--quiet`). Ohne Zeileneditor; mit `rlwrap m6510tool monitor` bekommt man
Pfeiltasten und Verlauf.

`--roms verzeichnis` sucht Dateien, deren Name mit `basic` (8 KiB), `kernal` (8 KiB) oder `char` (4 KiB, auch `chargen`)
beginnt, also auch die Dateinamen von VICE. Ein gefundenes KERNAL-ROM ersetzt die Emulation (`machine c64 rom`). ROMs auf der
Kommandozeile werden auch mit `--no-files` geladen; gesperrt sind nur die Dateibefehle im Monitor.

## Zahlen

**Argumente sind hexadezimal**, wie bei klassischen Monitoren: `d c000`, `e 0400 08 05 0c 0c 0f`.

| Schreibweise | Bedeutung |
|---|---|
| `C000`, `FF`, `D020` | hexadezimal (auch wenn mit einem Buchstaben begonnen wird) |
| `100.` | **dezimal** (Punkt dahinter) |
| `$FF`, `0xFF`, `%1010` | mit Präfix wie im Assembler |
| `start`, `loop+3`, `<text`, `>text`, `CHROUT` | Symbole und Ausdrücke; auf dem C64 auch die Namen der KERNAL-Sprungtabelle |
| `@A`, `@X`, `@Y`, `@S`, `@P`, `@PC` | Registerinhalt |

Ein bekanntes Symbol hat Vorrang vor einer gleichlautenden Hexzahl (`sym -name` entfernt es). Adressen werden auf 16 Bit
beschränkt, Anzahlen (`d c000 20`) sind ebenfalls hexadezimal.

**Text in Anführungszeichen** (`e 2000 "HALLO" 0`, `h 0800 9fff "READY"`) wird Byte für Byte als ASCII übernommen.
Großbuchstaben, Ziffern und Satzzeichen sind in PETSCII dieselben Codes.

**Im Assemble-Modus** (Befehl `a`) gilt die Syntax des Assemblers: `LDA #10` ist dezimal, hex schreibt man `#$0A`
([assembler-syntax.md](assembler-syntax.md)). Alle Opcodes einschließlich der instabilen undokumentierten sind erlaubt.

## Befehle

| Befehl | Wirkung |
|---|---|
| `d [adr [n]]` | disassemblieren (`n` Befehle, Standard 16 = `10`; ohne Adresse: Fortsetzung). `>` markiert den PC, Marken stehen als `NAME:` davor |
| `m [adr [n]]` | Speicher als Hex + ASCII (`n` Bytes, Standard 128 = `80`; ohne Adresse: Fortsetzung); `m.s` zeigt die Bytes als Bildschirmcodes |
| `e adr b…` | Bytes schreiben, auch Text: `e 2000 "HALLO" 0`; `e.w` schreibt Wörter (Little-Endian) |
| `f von bis b…` | füllen (Muster wird wiederholt); `bis` ist **einschließlich** |
| `c von bis ziel` | vergleichen (zeigt bis zu 32 Unterschiede) |
| `t von bis ziel` | kopieren (überlappend korrekt) |
| `h von bis b…` | suchen: Bytes oder `"Text"` (bis zu 64 Treffer) |
| `a [adr]` | **Zeilen assemblieren**; leere Zeile beendet. Labels (`loop:`), Zuweisungen (`wert = $FB`), Direktiven (`.BYTE`) und frühere Symbole sind verwendbar |
| `r [name=wert …]` | Register anzeigen / setzen: `A`, `X`, `Y`, `S`, `PC`, `P` (`r pc=c000 a=5 x=@a p=24`, von links nach rechts, bei einem Fehler wird nichts geändert); Anzeige mit Flags `NV-BDIZC`, auf dem C64 mit `$01` und Rasterzeile, und Zyklenzähler |
| `n [anzahl]` | Einzelschritt(e); bei 2–32 (`2`–`20`) Schritten wird jeder Befehl gezeigt |
| `o` | Step over: ein `JSR` läuft bis zur Rückkehr |
| `u` | läuft, bis die aktuelle Routine zurückkehrt (`RTS`/`RTI`) |
| `g [adr]` | ab `adr` (oder PC) laufen lassen bis Haltepunkt / Watchpoint / `JAM` / Limit; ein Haltepunkt am Startpunkt wird übergangen |
| `ru adr` | laufen, bis `PC == adr` |
| `b [adr…]`, `bc [adr…]` | Haltepunkte setzen / löschen (`bc` löscht auch Watchpoints an der Adresse, ohne Argument alles); `b` ohne Argument listet Haltepunkte und Watchpoints |
| `w adr [länge] [r\|w\|rw]` | Watchpoint (Standard: 1 Byte, Schreiben) |
| `limit [n]` | höchstens `n` Befehle pro Lauf (Standard 10 000 000) |
| `sym [name wert \| -name]` | Symbole anzeigen, setzen, löschen |
| `symload datei` / `symsave datei` | Symboldateien (Format der Assembler-Ausgabe `-s`; VICE-Labeldateien werden ebenfalls gelesen) |
| `asm datei [org]` | Quelltext-Datei assemblieren und ins RAM laden; Symbole werden übernommen |
| `l datei [adr]` | Datei laden: **PRG**/P00 (Ladeadresse aus der Datei, `adr` überschreibt; PC = Ladeadresse bzw. `SYS`-Ziel eines BASIC-Starters), **Raw** (`.bin`/`.raw`, braucht `adr`) |
| `sv datei von bis [raw]` | Speicherbereich als PRG (Standard) oder Raw speichern (einschließlich `bis`) |
| `ck name` / `rb name` | Checkpoint (CPU + Speicher + Maschine) anlegen / zurückspielen |
| `savestate datei` / `loadstate datei` | Snapshot-Datei (`M6510SN1`, [formats.md](formats.md)) |
| `irq [on\|off]` / `nmi` | Interrupt jetzt annehmen und den Handler zeigen. `irq` braucht `I` = 0 (direkt nach `CLI` läuft wie auf dem echten Prozessor erst noch ein Befehl); `irq on` hält die Leitung aktiv, bis `irq off` |
| `reset [mem]` | zurücksetzen (mit `mem` auch Speicher löschen); Symbole, Haltepunkte und Checkpoints bleiben |
| `machine [generic\|c64 [kernal\|rom]]` | Maschine anzeigen / wählen (löscht Speicher, Symbole, Checkpoints); `kernal` = KERNAL-Emulation (Standard), `rom` = ohne KERNAL (eigenes ROM mit `rom kernal datei`) |
| `?`, `q` | Hilfe, Beenden |

C64-Befehle (nur mit `machine c64`):

| Befehl | Wirkung |
|---|---|
| `bank [wert]` | Speicherkonfiguration (`$01`, Bits 0–2) anzeigen / setzen, mit den sichtbaren Bereichen |
| `rom basic\|kernal\|char datei` | ROM-Abbild laden; ein KERNAL-ROM schaltet die Emulation ab und löst einen Reset aus |
| `io` | Schreibprotokoll der Bausteinregister mit Namen (wird beim Lesen geleert) |
| `kernal` | KERNAL-Aufrufe seit dem letzten Lesen, mit Registern davor/danach und Aufrufstelle (nur protokollierte als `NOT emulated` markiert) |
| `console` | Text, den das Programm über `CHROUT` ausgegeben hat |
| `screen` | Bildschirminhalt als Text (40×25, Bildschirmcodes → Zeichen) |
| `keys "text"` | Tastenpuffer für `GETIN`/`CHRIN` füllen (`\n` = RETURN; Kleinbuchstaben ergeben die normalen Buchstabencodes) |
| `run [adr]` | Programm wie mit `RUN` (BASIC-Starter → `SYS`-Adresse) bzw. wie `SYS adr` starten und laufen lassen |
| `shot datei.png [spalte zeile breite höhe] [zoom] [border] [pepto]` | Bildschirm als PNG ([c64.md](c64.md#video-der-bildschirm-als-bild)): ganz (Zoom 1–4) oder ein Ausschnitt in Textzellen (hex: `28` = 40 Spalten; Standard-Zoom so, dass der Ausschnitt etwa 320 Pixel breit wird); `border` mit Rahmen, `pepto` andere Palette |

Fehler erscheinen als `? Meldung` und ändern den Zustand nicht. Nach jedem Lauf (`n`, `o`, `u`, `g`, `ru`, `run`) zeigt der
Monitor den Grund (`stopped: breakpoint at C004  [2 instructions, 4 cycles]`), die Anzahl angenommener Interrupts, die
Register und den nächsten Befehl.

## Beispiel

[`examples/monitor_session.txt`](../examples/monitor_session.txt) (läuft als Test `cli_monitor`):

```
a c000
start: LDX #$05
       LDA #$00
loop:  CLC
       ADC #$03
       DEX
       BNE loop
       STA $0400
done:  RTS

d start 8
b loop
g start
r
bc
ru done
m 0400 1
machine c64
a c000
       LDX #0
print: LDA $C100,X
       BEQ $C00D
       JSR CHROUT
       INX
       BNE print
       INC $D020
       RTS

e c100 "HELLO, C64" 0d 0
run c000
console
kernal
io
q
```

Ausgabe (Auszug):

```
  C000  A2 05     LDX #$05
  ...
loop:
  C004  18        CLC
  C005  69 03     ADC #$03
  C007  CA        DEX
  C008  D0 FA     BNE loop
  C00A  8D 00 04  STA $0400
done:
  C00D  60        RTS
stopped: breakpoint at C004  [2 instructions, 4 cycles]
 PC=C004  A=00 X=05 Y=00  S=FF  P=26 ..-..IZ.  cycles 11
loop:
> C004  18        CLC
...
stopped: reached C00D  [21 instructions, 48 cycles]
...
0400  0F                                                .
c64, KERNAL emulated, $A000:RAM $D000:I/O $E000:KERNAL
...
stopped: program returned to BASIC  [83 instructions, 395 cycles]
 PC=FFF6  A=00 X=0B Y=00  S=FA  P=A0 N.-.....  $01=37 line 6  cycles 402
> FFF6  60        RTS
HELLO, C64
  F1CA CHROUT  A=48 X=00 Y=00 -> A=48 X=00 Y=00 C=0  call at C007
  ...
  D020 <- FE  VIC EXTCOL      by C00D
  D020 <- FF  VIC EXTCOL      by C00D
```

`INC $D020` erscheint zweimal im Protokoll: Lese-Änderungs-Schreib-Befehle schreiben auf dem NMOS-6510 erst den alten Wert
zurück, dann den neuen ([emulator.md](emulator.md)). Die Zyklenzahl in der Registeranzeige zählt die 7 Zyklen des Resets mit.

## Grenzen

- Zeilenweises Assemblieren kennt **keine Vorwärtsreferenzen** (mit `sym name adr` vorher festlegen oder `asm datei`).
- Keine Befehlsverlauf-/Zeilenbearbeitung im Programm selbst (siehe `rlwrap`).
- Die Analyse- und Annotationsbefehle des Agenten-Protokolls (`analyze`, `listing`, `ann_*`) gibt es im Monitor nicht;
  sie gehören zu `m6510tool serve` ([reverse-engineering.md](reverse-engineering.md)).
