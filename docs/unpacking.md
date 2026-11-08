# Selbstentpackende Programme untersuchen

> **Stand 0.6:** umgesetzt (`track_writes`, `written`, `run_until_unpacked`, `save_memory`, D64/T64 als Quelle) und geprüft
> mit einem selbstentpackenden Testprogramm, ein- und zweistufig und über ein D64 (`tests/agent/test_unpack.py`: Packer in
> Python, Entpacker in 6510-Assembler). Mit echten Packern (Exomizer & Co.) noch nicht erprobt. Vorbild ist
> [unpacking.md im MC68000-Projekt](https://github.com/PhotonPower/MC68000/blob/main/docs/unpacking.md).

Sehr viele C64-Programme sind gepackt („gecruncht“: Exomizer, ByteBoozer, PuCrunch, Time Cruncher, …). Typischer Ablauf
eines solchen Programms:

1. BASIC-Starter `SYS 2061` springt in einen kurzen Kopierer.
2. Der Kopierer schiebt den Entpacker an eine sichere Stelle (oft Stapelseite `$0100` oder Zero Page) und die gepackten
   Daten ans Speicherende.
3. Der Entpacker schaltet meist BASIC und KERNAL aus (`$01 = $34` oder `$35`), entpackt nach unten oder oben und
   springt mit `JMP $xxxx` ins entpackte Programm, oft nachdem er `$01` wiederhergestellt und `CLI` ausgeführt hat.

Ein Disassembler sieht davon nur den Entpacker. Das eigentliche Programm entsteht erst zur Laufzeit – deshalb lässt man den
Emulator laufen, bis Code aus **frisch geschriebenem Speicher** geholt wird.

## 1. Das Programm aus dem Image holen

```sh
m6510tool d64 ls disk.d64
m6510tool d64 extract disk.d64 "SPIEL" -o spiel.prg
```

## 2. Bis zum entpackten Programm laufen lassen

```json
{"cmd":"machine","type":"c64"}
{"cmd":"load_file","file":"spiel.prg"}
{"cmd":"track_writes"}
{"cmd":"run_until_unpacked","min_written":4096}
```

Antwort: `stop:"unpacked"`, `pc` (Einsprung des entpackten Programms), `region:{start,end,size}` (der zusammenhängend
geschriebene Bereich, in dem der PC liegt), `written_bytes`, `ranges`. Mehrstufige Packer: mit `continue:true` zum nächsten
Fund. Bereiche, die kein Ergebnis sind (z. B. der Bildschirmspeicher `$0400–$07FF`, den viele Entpacker als Fortschritts-
anzeige beschreiben), lassen sich mit `ignore` ausschließen; die Stapelseite und `$0000–$00FF` zählen nie. `min_written`
(Standard 4096) trennt einen kurzen Kopierer (z. B. Entpacker, der sich selbst verschiebt) vom eigentlichen Ergebnis.

Direkt aus einem Image: `{"cmd":"load_file","file":"disk.d64","name":"SPIEL"}`. Wenn ein Programm Teile nachlädt, liefert
`{"cmd":"attach","file":"disk.d64"}` die Dateien für `LOAD`.

## 3. Den entpackten Speicher sichern und analysieren

```json
{"cmd":"save_memory","addr":"0x0801","len":"0xC7FF","file":"spiel_entpackt.prg"}
{"cmd":"analyze","entries":["pc"],"from":"0x0801","to":"0xD000"}
{"cmd":"listing"}
```

Oder mit dem Orchestrator: `python3 agent/m6510_annotate.py --prg spiel_entpackt.prg --entry 0x…` (der Einsprung aus
`pc`).

`save_memory` schreibt standardmäßig ein PRG mit Ladeadresse, sodass das Ergebnis auch in anderen Werkzeugen oder auf
einem echten C64 läuft (sofern das Programm nicht vom Zustand der Hardware abhängt).

## Grenzen

- Was die KERNAL-Emulation mit `LOAD` in den Speicher schreibt, zählt nicht als geschrieben (nur Schreibzugriffe des
  Prozessors werden markiert); ein Nachlader, der so eine zweite Stufe holt, wird mit `run`/`until` und `written` verfolgt.
- Entpacker, die auf eine Rasterzeile warten oder Timer benutzen, laufen dank Rasterzähler und einfachem CIA-Modell; solche,
  die genaues Timing, Interrupts oder das Diskettenlaufwerk brauchen (Schnelllader, Nachlader), nicht.
- Entpacker, die über `$01` den RAM unter dem I/O-Bereich (`$D000–$DFFF`) beschreiben: funktioniert (Schreiben geht ins RAM),
  aber `save_memory` liest in der **CPU-Sicht**; für den RAM darunter `view:"ram"` bei `read` bzw. die Option in
  `save_memory` verwenden.
- Kopierschutz, der die Hardware prüft (Prozessorport-Fading, Laufwerkscode, instabile Opcodes mit bestimmtem Ergebnis),
  verhält sich anders als auf einem echten C64.
