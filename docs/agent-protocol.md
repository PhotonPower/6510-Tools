# Agenten-Protokoll (`m6510tool serve`)

> **Stand 0.5 (Teil 1):** Grundbefehle, Lauf, Checkpoints, Snapshots, die C64-Befehle und `screenshot` mit den Bildvergleichen
> sind umgesetzt (Abschnitt „Befehle“). Die Reverse-Engineering-Befehle (0.5) und die Entpack-Befehle (0.6) sind noch
> **geplant** und unten gekennzeichnet. Grundregeln und Befehlsnamen sind die von
> [`z80tool serve`](https://github.com/PhotonPower/Z80/blob/main/docs/agent-protocol.md) und
> [`m68ktool serve`](https://github.com/PhotonPower/MC68000/blob/main/docs/agent-protocol.md), damit ein Orchestrator
> alle drei Maschinen gleich bedienen kann.

Ein lokaler Agent (z. B. ein LLM mit Werkzeugaufruf) führt 6510-Code aus, indem er JSON-Objekte an `m6510tool serve`
schickt. **Eine Zeile Anfrage → genau eine Zeile Antwort**, über stdin/stdout. Leere Zeilen werden übergangen.

```sh
m6510tool serve                  # ohne Dateizugriff (Standard)
m6510tool serve --allow-files    # erlaubt load_file / load_rom / attach / save_state / load_state
```

```console
$ echo '{"cmd":"asm","source":"  LDA #5\n  ASL A\n  STA $0400\n  BRK","entry":"0xC000"}' | m6510tool serve
{"ok":true,"size":7,"start":"0xC000","end":"0xC006","segments":[{"start":"0xC000","size":7}],"symbols":{},"entry":"0xC000"}
```

Eine vollständige Beispielsitzung liegt in `examples/agent_session.jsonl` (C64: Programm assemblieren, wie mit `RUN`
starten, Konsole, Bildschirm, I/O-Protokoll, KERNAL-Aufrufe, Diff, Rollback, Haltepunkt, `call`):
`m6510tool serve < examples/agent_session.jsonl` (in CTest als `cli_serve`).

## Grundregeln

- Antwort: `{"ok":true, …}` oder `{"ok":false,"error":"…"}`. Der Server **stürzt bei Fehlern nicht ab**; ungültiges JSON
  ergibt eine Fehlerantwort. Ein fehlgeschlagenes `asm` ändert nichts.
- **Zahlen** dürfen überall als JSON-Zahl oder als String stehen: `"0xC000"`, `"$C000"`, `"49152"`. Strings sind
  **Ausdrücke** ([assembler-syntax.md](assembler-syntax.md#zahlen-und-ausdrücke)): `"start+4"`, `"X"`, `"<table"`;
  Marken aus `asm`, Registernamen (`A`, `X`, `Y`, `S`/`SP`, `PC`, `P`) und auf der C64-Maschine die KERNAL-Namen
  (`"CHROUT"`) sind verwendbar. Adressen werden auf 16 Bit beschränkt.
- **Ausgaben** sind Strings: 8-Bit-Register `"0x2A"`, Adressen und `PC` `"0xC000"`; `flags` als `NV-BDIZC` mit `.` für
  gelöschte Bits (Bit 5 immer `-`); Speicherinhalte als Hex-Paare `"A9 01 60"`.
- Läufe haben immer ein **Limit** (`max_instructions`, Standard 1 000 000, höchstens 100 000 000; ein größerer Wert – auch
  bei `step n` – ist ein Fehler). Anders als beim MC68000 gibt es **Zyklen**: jede Lauf-Antwort enthält `cycles`.
- Dateizugriff ist **aus**, solange der Betreiber `--allow-files` nicht angibt.
- Eine Sitzung ist eine Maschine. Parallele Maschinen = mehrere Prozesse.

## Maschinen

| `machine` | Speicher |
|---|---|
| `generic` (Standard) | 64 KiB RAM überall; `$00/$01` sind gewöhnliches RAM (Prozessorport abgeschaltet); Stapel `S = $FF` |
| `c64` | Commodore 64 ([c64.md](c64.md)): Speicherkarte mit `$01`-Banking, Farb-RAM, VIC-II/SID/CIA-Register mit Schreibprotokoll, Rasterzähler und CIA-1-Timer A aus dem Zyklenzähler; `kernal:"hle"` (Standard) = KERNAL-Emulation, `kernal:"rom"` = eigene ROMs (mit `load_rom` laden) |

`machine` setzt die Sitzung komplett zurück (Speicher, Marken, Checkpoints, Watchpoints, angehängte Dateien). Auf der
C64-Maschine mit KERNAL-Emulation ist danach der Zustand wie nach dem Einschalten hergestellt (Bildschirm gelöscht,
`$01 = $37`, Timer-IRQ eingerichtet), der PC steht auf der Rücksprungfalle `$FFF6`.

## Antwort der Lauf-Befehle

`step`, `step_over`, `run`, `run_until_return`, `call`, `run_program` liefern:

```json
{"ok":true,"stop":"breakpoint","instructions":150,"cycles":723,"pc":"0x081A","interrupts":0,
 "regs":{"A":"0x00","X":"0x15","Y":"0x00","S":"0xF8","PC":"0x081A","P":"0x22","flags":"..-...Z.",
         "halted":false,"instructions":150,"cycles":730}}
```

`stop` ist einer von: `stepped`, `breakpoint`, `address` (`until` erreicht, bzw. bei `call` die Rücksprungadresse auf
falscher Stapelhöhe), `returned`, `halted` (`JAM`), `instruction_limit`, `watchpoint` (zusätzlich
`"watch":{"addr":"0x…"}`), `exit` (die KERNAL-Emulation beendete den Lauf; Text in `message`: „program returned to BASIC“
oder „BRK at $xxxx“). `interrupts` zählt die angenommenen IRQ/NMI/`BRK` während des Aufrufs; die Zähler in `regs` sind
Gesamtzähler seit dem letzten `reset`.

## Befehle

| Befehl | Argumente | Wirkung / Antwort |
|---|---|---|
| `asm` | `source`, `origin?` (`0xC000`), `entry?` | assemblieren ([assembler-syntax.md](assembler-syntax.md)) und ins RAM schreiben; frühere Marken und auf `c64` die KERNAL-Namen sind bekannt. Antwort: `size`, `start`, `end`, `segments`, `symbols` (die neuen); `entry` setzt den PC. Bei Fehlern `errors:[{line,message,source}]`, nichts wird geändert |
| `dis` | `addr?` (PC), `count?` (8, höchstens 1000) | `lines:[{addr,label?,bytes,text,target?}]` – so, wie der Prozessor den Speicher sieht; Marken und KERNAL-Namen als Operanden |
| `read` | `addr`, `len?` (16, höchstens 4096), `view?` (`cpu` = mit ROM/I/O nach `$01`, `ram`) | `hex`, `ascii`, `screen` (die Bytes als Bildschirmcodes gelesen) |
| `write` | `addr`, `hex` oder `bytes[]` (höchstens 65 536) | über den Bus schreiben: unter ROM ins RAM, in sichtbares I/O an die Register (mit Protokolleintrag), `$00/$01` an den Port |
| `load_file` | `file`, `type?` (`auto`, `raw`, `prg`, `p00`), `addr?` | Datei laden (*`--allow-files`*); `raw` braucht `addr`, bei PRG/P00 verschiebt `addr`. Antwort: `start`, `end`, `entry` (`SYS`-Adresse eines BASIC-Starters bei `$0801`, sonst die Ladeadresse; der PC wird gesetzt), `basic` (erste BASIC-Zeile). D64/T64/CRT: Meilenstein 0.6 |
| `load_rom` | `which` (`basic`, `kernal`, `char`), `file` | ROM laden (*`--allow-files`*, nur `c64`). Ein KERNAL-ROM ersetzt die Emulation (RESET aus dem ROM) |
| `attach` | `file` (PRG/P00 oder Verzeichnis) | Quelle für `LOAD` der KERNAL-Emulation (*`--allow-files`*): eine Datei beantwortet jeden Namen, in einem Verzeichnis wird `NAME.prg` gesucht (`*` = die erste) |
| `regs` / `set` | `regs:{"A":5,"X":"0x10","PC":"start","P":"0x24"}` | Register lesen / setzen; `PC` weckt aus `JAM`; ein neues `I` in `P` gilt sofort (ohne die Verzögerung von `CLI`/`SEI`) |
| `step` | `n?` (1), `trace?` | `n` Befehle (Haltepunkte werden übergangen); mit `trace:true` zusätzlich `trace:[{addr,bytes,text,cycles}]` (höchstens 1000) |
| `step_over` | – | ein Befehl; bei `JSR` bis zur Rückkehr auf gleicher Stapelhöhe |
| `run` | `until?`, `max_instructions?` | bis Haltepunkt/Watchpoint/`JAM`/Ende/Limit; mit `until` bis der PC diese Adresse erreicht |
| `run_until_return` | `max_instructions?` | bis die aktuelle Routine zurückkehrt (`RTS`/`RTI`) |
| `call` | `addr`, `regs?`, `memory?` (`{"0xFB":"05 07"}`), `return_address?` (`0xFFF0`), `max_instructions?` | Routine wie eine Funktion aufrufen: `return_address − 1` stapeln (wie `JSR`), Register und Speicher setzen, laufen. `returned` ist **nur dann true**, wenn der PC die Rücksprungadresse erreicht **und** `S` wieder den Wert vor dem Aufruf hat; sonst `returned:false` (mit `note`, wenn nur die Stapelhöhe nicht stimmt) |
| `run_program` | `addr?`, `max_instructions?` | `c64`: Programm starten wie `SYS` aus BASIC (Einschaltzustand, `A`/`X`/`Y`/`P` aus `$030C–$030F`, Rücksprung zur Falle `$FFF6` = „program returned to BASIC“, IRQs an). Ohne `addr` die `SYS`-Adresse des BASIC-Starters bei `$0801`. Antwort wie `run`, dazu `entry` |
| `break` / `unbreak` | `addr` | Breakpoint setzen / entfernen |
| `watch` / `unwatch` | `addr`, `len?` (1), `kind?` (`write`, `read`, `access`) | Speicherüberwachung; der Lauf hält **nach** dem zugreifenden Befehl |
| `breaks` / `clear_breaks` | – | Breakpoints und Watchpoints auflisten / alles löschen |
| `checkpoint` / `rollback` / `drop` / `checkpoints` | `name` | kompletten Zustand (CPU, RAM, auf `c64` Farb-RAM, Bausteine, KERNAL-Tastenpuffer) merken / zurückspielen / löschen / auflisten |
| `diff` | `name` | geänderte Bereiche seit dem Checkpoint: `changed_bytes`, `ranges:[{addr,len,before,after,area?}]` (höchstens 50 Bereiche, je höchstens 64 Byte angezeigt, nahe Änderungen zusammengefasst; Farb-RAM mit `area:"color"` und Adressen ab `$D800`) |
| `state` / `set_state` | `state` | CPU-Zustand als JSON (Register, Interrupt-Leitungen, Aufruftiefe, Prozessorport) lesen / setzen |
| `save_state` / `load_state` | `file` | Snapshot-Datei `M6510SN1` ([formats.md](formats.md#snapshot-m6510sn1), *`--allow-files`*); die Maschine muss zur Datei passen |
| `interrupt` | `kind` (`irq`, `nmi`), `release?` | NMI auslösen bzw. die IRQ-Leitung setzen; sie bleibt aktiv bis `release:true` (auf `c64` zusätzlich zu den Bausteinen) |
| `io` | – | `c64`: Schreibprotokoll seit dem letzten Abruf: `output:[{addr,value,pc,chip,what?}]` (höchstens 4096, Spiegeladressen auf das Grundregister abgebildet, `what` = Registername wie `EXTCOL`); die Schreibzugriffe der Einrichtung beim Programmstart fehlen |
| `kernalcalls` | – | `c64`: `calls:[{addr,name,emulated,call_site,regs_in,regs_out,note?}]` seit dem letzten Abruf (entspricht `libcalls` beim MC68000); `emulated:false` = nur protokolliert, Rückkehr mit `C` = 0; `addr` ist bei Einsprüngen über RAM-Vektoren die Routine dahinter (`CHROUT` → `$F1CA`) |
| `console` | – | `c64`: `text`, was das Programm seit dem letzten Abruf über `CHROUT` ausgegeben hat (Steuerzeichen als `{CLR}` usw.) |
| `keys` | `text` | `c64`: Tasten für `GETIN`/`CHRIN` anhängen (`\n` = RETURN) |
| `screen` | – | `c64`: Bildschirminhalt als Text (`lines`, 25 Zeilen), `mode`, `screen` (Adresse), `bank`, `border`, `background`, `display_on`, `memory` (sichtbare ROM/I/O-Bereiche) |
| `screenshot` | `name?` (`last`), `border?`, `palette?` (`colodore`, `pepto`), `crop?` (`{"col0":0,"row0":0,"col1":9,"row1":2}`, Textzellen), `zoom?` | `c64`: Bildschirm als PNG ([c64.md](c64.md#video-der-bildschirm-als-bild)): `png_base64`, `width`, `height`, `mode`, `charset` (`rom` oder `replacement`), `zoom`. Ohne `crop` 320×200 (mit Rahmen 384×272), `zoom` 1–4; mit `crop` Standard-Zoom so, dass etwa 320 Pixel Breite entstehen (höchstens 8), `zoom` 1–16. Das ganze Bild wird unter `name` für `screen_diff` gemerkt |
| `screens` / `screen_drop` | `name` | gemerkte Bilder auflisten (`screens:[{name,width,height}]`) / löschen |
| `screen_diff` | `a`, `b?` | Bild `a` mit `b` oder dem aktuellen Bildschirm vergleichen: `same_size`, `changed_pixels`, `bbox` (Pixel), `text_cells` (`col0`…`row1`) |
| `reset` | `memory?` | Zähler auf 0, Register auf Einschaltwerte (mit `memory:true` auch das RAM löschen); auf `c64` mit KERNAL-Emulation zusätzlich der Einschaltzustand der Maschine |
| `symbols` | – | alle Marken aus `asm` |
| `machine` | `type`, `kernal?` (`hle`, `rom`) | Maschine wählen (siehe oben) |
| `help` | – | Befehlsliste |

Geplant (Meilensteine 0.5/0.6):

| Befehl | Zweck |
|---|---|
| `track_writes`, `written`, `run_until_unpacked`, `save_memory` | selbstentpackende Programme ([unpacking.md](unpacking.md)) |
| `analyze`, `data_regions`, `jump_tables`, `routines`, `references`, `listing`, `ann_*` | Reverse Engineering, siehe unten |

### Reverse Engineering ([reverse-engineering.md](reverse-engineering.md)) – geplant

Dieselben Befehle wie beim MC68000 (`analyze`, `data_regions`, `jump_tables`, `routines`, `references`, `listing`,
`ann_label`, `ann_routine`, `ann_variable`, `ann_data`, `ann_comment`, `ann_constant`, `ann_remove`, `ann_title`, `ann_list`,
`ann_save`, `ann_load`) mit diesen Unterschieden:

| Befehl | 6510-spezifisch |
|---|---|
| `analyze` | Standard-Einstieg: PC bzw. `SYS`-Adresse; `JMP (vektor)` und der `RTS`-Trick werden verfolgt; Analyse in der **RAM-Sicht**, KERNAL-/BASIC-Aufrufe als externe Aufrufe |
| `data_regions` | Arten `text` (PETSCII), `screen` (Bildschirmcodes), `pointers`, `lohi` (getrennte Tabellen), `bytes`, `words`, `sprites`, `charset`, `zeros` |
| `jump_tables` | Arten `vector` (`JMP (abs)`), `rts` (Stapel-Trick), `lohi`, `pointers` |
| `routines` | zusätzlich `kernal_calls`, `io_access` (gelesene/geschriebene Register), `zero_page` (benutzte Zero-Page-Adressen) |
| `references` | Arten zusätzlich `kernal`, `io`, `zeropage`, `selfmod` (Befehl wird von anderem Code verändert) |
| `listing` | Labels mitten im Befehl als `name+n` (selbstmodifizierender Code); KERNAL- und I/O-Namen als `=`-Zeilen am Anfang |
| `ann_variable` | Zero-Page-Variablen und -Zeiger (`size:2`) |

## Typischer Ablauf

```json
{"cmd":"machine","type":"c64"}
{"cmd":"asm","source":"        * = $C000\nstart:  LDA #$00\n        STA $D020\n        LDX #$05\nloop:   DEX\n        BNE loop\n        RTS","entry":"start"}
{"cmd":"checkpoint","name":"fresh"}
{"cmd":"run_program","addr":"start"}
{"cmd":"io"}
{"cmd":"diff","name":"fresh"}
{"cmd":"rollback","name":"fresh"}
{"cmd":"call","addr":"loop","regs":{"X":3}}
```

Hinweise zur Verwendung durch ein Modell:

- Nach `asm` bzw. `load_file` die Marken verwenden statt Adressen zu merken (`"until":"loop"`).
- `diff` zeigt, was ein Programm im Speicher verändert hat; auf `c64` zeigen `io` die Hardware-Zugriffe,
  `console`/`screen` die Ausgabe und `kernalcalls` die Systemaufrufe.
- `checkpoint` vor riskantem Code, `rollback` danach.
- `call` ist der einfachste Weg, eine einzelne Routine zu testen (Argumente in `A`/`X`/`Y` und in der Zero Page über
  `memory`).
- Ein Programm, das auf eine Taste wartet, bekommt sie mit `keys`; eines, das in einer Endlosschleife läuft, hält mit
  `instruction_limit` an – mit `run` geht es weiter.

## Sicherheit

Der emulierte Code kann nur den eigenen 64-KiB-Speicher und die Protokolle beeinflussen, nichts auf dem Host. Dateizugriff
gibt es nur mit `--allow-files`; dann kann der Agent alle Pfade lesen/schreiben, die der Serverprozess erreicht – den Server
deshalb mit eingeschränkten Rechten oder in einem Verzeichnis-Sandbox starten. Laufzeit über `max_instructions` begrenzt;
`read` höchstens 4096 Byte, `write` höchstens 65 536 Byte, JSON höchstens 64 Ebenen tief. Der Robustheitstest der Suite
`session` schickt 3000 zufällige Befehle mit zufälligen Argumenten.
