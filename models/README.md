# KI-Modelle

Das **Dock** benötigt zwei Modelle. Beide sind zu groß fürs git-Repo und werden
separat geladen. Das Skript [`download-models.sh`](download-models.sh) holt beide
an die richtigen Stellen.

## 1. Whisper — Transkription

`ggml-small-q4_0.bin`, ein Standardmodell von whisper.cpp. Wird direkt über
whisper.cpp geladen:

```bash
cd ~/whisper.cpp && ./models/download-ggml-model.sh small-q4_0
```

Zielpfad: `~/whisper.cpp/models/ggml-small-q4_0.bin`

## 2. TOPIKO-Sprachmodell — Klassifizierung

`LFM2.5-350M-TOPIKO-Q4_K_M.gguf` — ein auf **LFM2.5-350M** feinjustiertes,
quantisiertes Sprachmodell, das den transkribierten Text in die TOPIKO-Kategorien
(Termin, To-do, Erinnerung, Idee, Tagebuch) einsortiert. Gehostet auf Hugging Face:

> **➡ Download: https://huggingface.co/colin18054/TOPIKO-LFM2.5-350M-GGUF**

Nach dem Download ablegen unter:

```
~/models/LFM2.5-350M-TOPIKO-Q4_K_M.gguf
```

Der Pfad entspricht `GGUF_PATH` in [`../software/topiko_uno_q_app.py`](../software/topiko_uno_q_app.py).

## Beides auf einmal laden

```bash
bash download-models.sh
```

> Vorher im Skript den Platzhalter `HF_REPO` durch das echte Hugging-Face-Repo ersetzen.

---

## Für Maintainer: Modell auf Hugging Face veröffentlichen

Das feinjustierte Modell liegt auf dem Dock unter `~/models/`. Von dort einmalig
hochladen:

```bash
pip install -U "huggingface_hub[cli]"
huggingface-cli login                     # Token von huggingface.co/settings/tokens (Rolle: Write)
huggingface-cli upload colin18054/TOPIKO-LFM2.5-350M-GGUF \
  ~/models/LFM2.5-350M-TOPIKO-Q4_K_M.gguf \
  LFM2.5-350M-TOPIKO-Q4_K_M.gguf
```

Das Modell ist bereits veröffentlicht unter
`https://huggingface.co/colin18054/TOPIKO-LFM2.5-350M-GGUF`.
