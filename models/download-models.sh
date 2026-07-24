#!/usr/bin/env bash
# Lädt beide TOPIKO-Modelle an die vom Dock erwarteten Pfade.
# Siehe models/README.md für Details.
set -euo pipefail

# ── 1. Whisper-Modell (Transkription) ─────────────────────────────────────────
if [ -d "$HOME/whisper.cpp" ]; then
  echo "==> Whisper-Modell (ggml-small-q4_0) laden ..."
  ( cd "$HOME/whisper.cpp" && ./models/download-ggml-model.sh small-q4_0 )
else
  echo "!! ~/whisper.cpp nicht gefunden — bitte zuerst whisper.cpp bauen (siehe docs/software-installation.md)"
fi

# ── 2. TOPIKO-Sprachmodell (Klassifizierung, Hugging Face) ────────────────────
# HF_REPO durch das echte Hugging-Face-Repo ersetzen, z. B. "colinoestermann/TOPIKO-LFM2.5-350M-GGUF"
HF_REPO="<HF-USER>/TOPIKO-LFM2.5-350M-GGUF"
FILE="LFM2.5-350M-TOPIKO-Q4_K_M.gguf"

mkdir -p "$HOME/models"
if [[ "$HF_REPO" == *"<HF-USER>"* ]]; then
  echo "!! HF_REPO im Skript ist noch ein Platzhalter — bitte auf das echte Hugging-Face-Repo setzen."
  exit 1
fi

echo "==> TOPIKO-Sprachmodell von Hugging Face laden ..."
curl -L --fail -o "$HOME/models/$FILE" \
  "https://huggingface.co/$HF_REPO/resolve/main/$FILE"

echo "Fertig. Modelle liegen unter ~/whisper.cpp/models/ und ~/models/."
