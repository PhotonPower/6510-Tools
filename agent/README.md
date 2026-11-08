# agent/

Geplant für Meilenstein 0.5 ([docs/reverse-engineering.md](../docs/reverse-engineering.md)):

- `m6510_annotate.py` – Orchestrator für ein lokales Modell über Ollama (Vorbild: `z80_annotate.py`, `m68k_annotate.py`)
- `m6510_drive.py` – dieselben Werkzeuge ein Schritt je Aufruf, ohne Modell (Vorbild: `m68k_drive.py`)

Regeln: nur Python-Standardbibliothek (Python ≥ 3.9), Tests unter `tests/agent/` gegen einen scripteten Mock-Ollama und den
echten `m6510tool serve`.
