# Anleitung: vom Programm zum kommentierten Listing

> **Stand 0.1:** Gerüst. Diese Anleitung wird mit Meilenstein 0.5 ausgefüllt und dann Schritt für Schritt gegen das
> echte Werkzeug geprüft – so wie [walkthrough.md im MC68000-Projekt](https://github.com/PhotonPower/MC68000/blob/main/docs/walkthrough.md).
> Die Schritte und Befehle unten sind der Plan.

## 1. Bauen und prüfen

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## 2. Das Beispiel übersetzen und starten

Ein Beispielprogramm `examples/c64/hello_c64.asm` (geplant) enthält alle Fälle, die die Analyse kennen muss: BASIC-Starter,
`CHROUT`-Ausgabe, einen PETSCII-String, eine `RTS`-Sprungtabelle, getrennte Low-/High-Byte-Tabellen, eine Zero-Page-
Zeigerschleife, selbstmodifizierenden Code, eine Ausgabe in den Bildschirmspeicher und ein Sprite.

```sh
./build/m6510tool asm examples/c64/hello_c64.asm -o hello_c64.prg
printf '%s\n' '{"cmd":"machine","type":"c64"}' '{"cmd":"load_file","file":"hello_c64.prg"}' \
              '{"cmd":"run_program"}' '{"cmd":"console"}' '{"cmd":"screen"}' | ./build/m6510tool serve --allow-files
```

## 3. Die Analyse ansehen

```sh
python3 agent/m6510_annotate.py --prg hello_c64.prg --dry-run
```

Erwartet: Routinen in Bearbeitungsreihenfolge, erkannte Sprungtabellen und Datenbereiche, KERNAL-Aufrufe, I/O-Zugriffe,
Token-Schätzung.

## 4. Das Listing ohne Modell

`analyze` + `listing` über das Protokoll: alle Routinen als `sub_XXXX`, Daten erkannt, **VERIFIED**.

## 5. Ollama vorbereiten

```sh
ollama pull qwen3-vl:8b-instruct
```

## 6. Der erste Lauf

```sh
python3 agent/m6510_annotate.py --prg hello_c64.prg --project hello.m6510proj.json --out hello_kommentiert.asm \
        --lang Deutsch --log hello.log.jsonl
```

## 6b. Ohne Ollama: ein Agent oder ein Mensch übernimmt die Rolle des Modells

```sh
python3 agent/m6510_drive.py init hello_c64.prg --dir work
python3 agent/m6510_drive.py show --dir work
python3 agent/m6510_drive.py tool run_routine '{"addr":"sub_0830","regs":{"A":65}}' --dir work
python3 agent/m6510_drive.py finish --dir work
python3 agent/m6510_drive.py listing --dir work
```

## 7. Woran man ein Problem erkennt

Wird mit den Erfahrungen aus der Erprobung gefüllt (bei den Schwesterprojekten: falsches `returned`, Lesen aus leerem
Speicher, Tabellen als Code, Strings als Routinen …).
