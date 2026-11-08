# agent/

Reverse Engineering mit einem lokalen Modell ([docs/reverse-engineering.md](../docs/reverse-engineering.md),
Anleitung: [docs/walkthrough.md](../docs/walkthrough.md)):

- `m6510_annotate.py` – Orchestrator für ein lokales Modell über Ollama: Routinen von den Blättern zur Wurzel, Werkzeuge zum
  Ansehen, Ausführen (mit Screenshots für Modelle mit Bildverständnis) und Annotieren, Vollständigkeitsprüfung,
  Mehrdurchgang-Verfahren, Projektdatei, Bericht, `--dry-run` (Vorbild: `z80_annotate.py`, `m68k_annotate.py`)
- `m6510_drive.py` – dieselben Werkzeuge ein Schritt je Aufruf, ohne Modell, Zustand in einem Arbeitsverzeichnis
  (Vorbild: `m68k_drive.py`)

Regeln: nur Python-Standardbibliothek (Python ≥ 3.9). Tests unter `tests/agent/` gegen einen scripteten Mock-Ollama
(`mock_ollama.py`) und den echten `m6510tool serve`; sie laufen als CTest `agent`
(`M6510TOOL=build/m6510tool python3 -m unittest discover -s tests/agent`).
