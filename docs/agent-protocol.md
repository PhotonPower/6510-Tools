# Agenten-Protokoll (`m6510tool serve`)

> **Stand 0.1:** geplant (Meilensteine 0.3, 0.5, 0.6). Die Grundregeln und Befehlsnamen sind die von
> [`z80tool serve`](https://github.com/PhotonPower/Z80/blob/main/docs/agent-protocol.md) und
> [`m68ktool serve`](https://github.com/PhotonPower/MC68000/blob/main/docs/agent-protocol.md), damit ein Orchestrator
> alle drei Maschinen gleich bedienen kann. Abweichungen stehen ausdrücklich da.

Ein lokaler Agent (z. B. ein LLM mit Werkzeugaufruf) führt 6510-Code aus, indem er JSON-Objekte an `m6510tool serve`
schickt. **Eine Zeile Anfrage → genau eine Zeile Antwort**, über stdin/stdout.

```sh
m6510tool serve                  # ohne Dateizugriff (Standard)
m6510tool serve --allow-files    # erlaubt load_file / load_rom / attach / save_state / load_state / save_memory / ann_save / ann_load
```

```console
$ echo '{"cmd":"asm","source":"  LDA #5\n  ASL A\n  STA $0400\n  BRK","entry":"0xC000"}' | m6510tool serve
{"ok":true,"size":7,"start":"0xC000","end":"0xC006","segments":[{"start":"0xC000","size":7}],"symbols":{},"entry":"0xC000"}
```

Eine vollständige Beispielsitzung kommt nach `examples/agent_session.jsonl`.

## Grundregeln

- Antwort: `{"ok":true, …}` oder `{"ok":false,"error":"…"}`. Der Server **stürzt bei Fehlern nicht ab**; ungültiges JSON
  ergibt eine Fehlerantwort. Ein fehlgeschlagenes `asm` ändert nichts.
- **Zahlen** dürfen überall als JSON-Zahl oder als String stehen: `"0xC000"`, `"$C000"`, `"49152"`. Strings sind
  **Ausdrücke**: `"start+4"`, `"X"`, `"<table"`; Marken aus `asm`/`load_file`/Annotationen, KERNAL-Namen (`"CHROUT"`) und
  Registernamen (`A`, `X`, `Y`, `S`, `PC`, `P`) sind verwendbar. Adressen werden auf 16 Bit beschränkt.
- **Ausgaben** sind Strings: Register `"0x2A"` (8 Bit), Adressen und `PC` `"0xC000"` (16 Bit); `flags` als `NV-BDIZC` mit
  `.` für gelöschte Bits.
- Läufe haben immer ein **Limit** (`max_instructions`, Standard 1 000 000, höchstens 100 000 000; ein größerer Wert ist ein
  Fehler). Anders als beim MC68000 gibt es **Zyklen**: jede Lauf-Antwort enthält `cycles`.
- Dateizugriff ist **aus**, solange der Betreiber `--allow-files` nicht angibt.
- Eine Sitzung ist eine Maschine. Parallele Maschinen = mehrere Prozesse.

## Maschinen

| `machine` | Speicher |
|---|---|
| `generic` (Standard) | 64 KiB RAM überall, kein Banking; Stapel `S = $FF` |
| `c64` | Commodore 64 ([c64.md](c64.md)): Speicherkarte mit `$01`-Banking, Farb-RAM, VIC-II/SID/CIA-Register (Speicher mit Schreibprotokoll, Rasterzähler), KERNAL-Emulation (`kernal:"hle"`, Standard) oder eigene ROMs (`kernal:"rom"` nach `load_rom`) |

`machine` setzt die Sitzung komplett zurück.

## Antwort der Lauf-Befehle

`step`, `step_over`, `run`, `run_until_return`, `call` liefern:

```json
{"ok":true,"stop":"breakpoint","instructions":14,"cycles":38,"pc":"0xC00E",
 "regs":{"A":"0x09","X":"0x00","Y":"0x10","S":"0xFB","PC":"0xC00E","P":"0x22","flags":"..-...Z.",
         "halted":false,"instructions":123,"cycles":411}}
```

`stop` ist einer von: `stepped`, `breakpoint`, `address`, `returned`, `halted` (`JAM`), `instruction_limit`,
`exit` (die KERNAL-Emulation beendete den Lauf: Rückkehr ins BASIC, Text in `message`), `watchpoint` (dann zusätzlich
`"watch":{"addr":"0x…"}`), `unpacked` (nur `run_until_unpacked`). `interrupts` zählt angenommene IRQ/NMI/`BRK` während des
Aufrufs.

## Befehle

| Befehl | Argumente | Wirkung / Antwort |
|---|---|---|
| `asm` | `source`, `origin?` (`0xC000`), `entry?` | assemblieren und in den Speicher schreiben ([assembler-syntax.md](assembler-syntax.md)); Marken werden gemerkt. Antwort: `size`, `start`, `end`, `segments`, `symbols`; bei Fehlern `errors:[{line,message,source}]` |
| `dis` | `addr?` (PC), `count?` (8) | `lines:[{addr,label?,bytes,text,target?}]` |
| `read` | `addr`, `len?` (16, höchstens 4096), `view?` (`cpu`, `ram`, `vic`) | `hex`, `ascii`, `petscii` – Standard: so, wie der Prozessor den Speicher sieht (ROM/I/O je nach `$01`) |
| `write` | `addr`, `hex` oder `bytes[]` | Speicher schreiben (über den Bus: unter ROM ins RAM, in I/O an die Register) |
| `load_file` | `file`, `type?`, `addr?`, `name?` | Datei laden (*`--allow-files`*); `type`: `auto` (Standard), `raw` (braucht `addr`), `prg`, `p00`, `t64`, `d64` (mit `name`, Standard: erste Datei), `crt`. Antwort: `start`, `end`, `entry` (`SYS`-Adresse eines BASIC-Starters, sonst Ladeadresse), `basic?` (detokenisierter Starter) |
| `load_rom` | `which` (`basic`, `kernal`, `char`), `file` | ROM laden (*`--allow-files`*, nur `c64`) |
| `attach` | `file` | D64/T64 für die KERNAL-Emulation anhängen (`LOAD`/`OPEN` lesen daraus; *`--allow-files`*) |
| `regs` / `set` | `regs:{"A":5,"X":"0x10","PC":"start","P":"0x24"}` | Register lesen / setzen; `PC` weckt aus `JAM` |
| `step` | `n?` (1), `trace?` | `n` Befehle; mit `trace:true` zusätzlich `trace:[{addr,bytes,text,cycles}]` (höchstens 1000) |
| `step_over` | – | ein Befehl; bei `JSR` bis zur Rückkehr |
| `run` | `until?`, `max_instructions?` | bis Breakpoint/Watchpoint/`JAM`/Limit; mit `until` bis der PC diese Adresse erreicht |
| `run_until_return` | `max_instructions?` | bis die aktuelle Routine zurückkehrt (`RTS`/`RTI`) |
| `call` | `addr`, `regs?`, `memory?`, `return_address?`, `max_instructions?` | Routine wie eine Funktion aufrufen: `return_address − 1` stapeln (wie `JSR`), Register setzen, laufen. `returned` ist **nur dann true**, wenn der PC die Rücksprungadresse erreicht **und** `S` wieder den Wert vor dem Aufruf hat; sonst `returned:false` mit `note` |
| `run_program` | `max_instructions?` | `c64`: Programm wie mit `RUN` starten (BASIC-Starter → `SYS`-Adresse, Zustand wie nach dem Einschalten) |
| `track_writes` / `written` | wie MC68000 | geschriebene Bytes verfolgen / größte zusammenhängende Bereiche |
| `run_until_unpacked` | `min_written?` (1024), `continue?`, `ignore?:[[von,bis],…]`, `max_instructions?` | läuft, bis Code aus **geschriebenem Speicher** geholt wird, dessen Bereich groß genug ist ([unpacking.md](unpacking.md)) |
| `save_memory` | `addr`, `len`, `file`, `prg?` (true) | Speicher als PRG (mit Ladeadresse) oder Raw schreiben (*`--allow-files`*) |
| `break` / `unbreak` | `addr` | Breakpoint setzen / entfernen |
| `watch` / `unwatch` | `addr`, `len?` (1), `kind?` (`write`, `read`, `access`) | Speicherüberwachung; hält **nach** dem zugreifenden Befehl |
| `breaks` / `clear_breaks` | – | auflisten / alles löschen |
| `checkpoint` / `rollback` / `drop` / `checkpoints` | `name` | kompletten Zustand (CPU, Speicher, Maschine, KERNAL-Emulation) merken / zurückspielen / löschen / auflisten |
| `diff` | `name` | geänderte Speicherbereiche seit dem Checkpoint: `changed_bytes`, `ranges:[{addr,len,before,after}]` (wie MC68000; Farb-RAM als eigener Bereich) |
| `state` / `set_state` | `state` | CPU-Zustand als JSON |
| `save_state` / `load_state` | `file` | Snapshot-Datei `M6510SN1` (*`--allow-files`*) |
| `interrupt` | `kind` (`irq`, `nmi`), `release?` | Interrupt-Leitung setzen; IRQ bleibt aktiv bis `release:true` oder bis das Programm die Quelle quittiert |
| `io` | `clear?` (true) | `c64`: Schreibprotokoll `output:[{addr,value,pc,what?,chip}]` (höchstens 4096), `what` = Registername (z. B. `EXTCOL`) |
| `kernalcalls` | `clear?` (true) | `calls:[{addr,name,emulated,caller,call_site,regs_in,regs_out,note?}]` – entspricht `libcalls` beim MC68000 |
| `console` | `clear?` (true) | `text`: was das Programm über `CHROUT` ausgegeben hat |
| `keys` | `text` | Tastenpuffer für `GETIN`/`CHRIN` |
| `screen` | – | `c64`: Bildschirminhalt als Text (`lines[25]`), Modus, Farben, Bank, Adressen von Bildschirm/Zeichensatz/Bitmap |
| `screenshot` | `crop?:{x,y,w,h}`, `scale?` (2), `border?` (true) | `c64`: `png` (Base64), `width`, `height`, `mode` – für Modelle mit Bildverständnis (wie `screenshot` im Z80-Projekt) |
| `reset` | `memory?` | Register zurücksetzen (mit `memory:true` auch den Speicher löschen); Zähler auf 0 |
| `symbols` | – | alle Marken (mit KERNAL- und I/O-Namen auf `c64`) |
| `machine` | `type`, `kernal?` (`hle`, `rom`), `irq_every?` (Zyklen, 0 = aus) | Maschine wählen (siehe oben) |
| `help` | – | Befehlsliste |

### Reverse Engineering ([reverse-engineering.md](reverse-engineering.md))

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
{"cmd":"run"}
{"cmd":"io"}
{"cmd":"diff","name":"fresh"}
{"cmd":"rollback","name":"fresh"}
{"cmd":"call","addr":"loop","regs":{"X":3}}
```

Hinweise zur Verwendung durch ein Modell:

- Nach `asm` bzw. `load_file` die Marken verwenden statt Adressen zu merken (`"until":"loop"`).
- `diff` zeigt, was ein Programm im Speicher verändert hat; auf `c64` zeigt `io` zusätzlich die Hardware-Zugriffe,
  `console`/`screen`/`screenshot` die Ausgabe.
- `checkpoint` vor riskantem Code, `rollback` danach.
- `call` ist der einfachste Weg, eine einzelne Routine zu testen (Argumente in `A`/`X`/`Y` und in der Zero Page über
  `memory`).

## Sicherheit

Der emulierte Code kann nur den eigenen 64-KiB-Speicher und die Protokolle beeinflussen, nichts auf dem Host. Dateizugriff
gibt es nur mit `--allow-files`; dann kann der Agent alle Pfade lesen/schreiben, die der Serverprozess erreicht – den Server
deshalb mit eingeschränkten Rechten oder in einem Verzeichnis-Sandbox starten. Laufzeit über `max_instructions` begrenzt;
`read` höchstens 4096 Byte, `write` höchstens 65 536 Byte, JSON höchstens 64 Ebenen tief.
