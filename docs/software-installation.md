# TOPIKO — Dock-Software installieren

Das **Dock** (Arduino UNO Q, Debian Linux) übernimmt die KI-Verarbeitung: Es empfängt die WAV-Aufnahmen des Handhelds, transkribiert sie mit **whisper.cpp** und klassifiziert den Text mit einem kleinen, lokalen Sprachmodell über **llama-cpp-python**. Alles läuft offline.

Die Software liegt in [`../software/`](../software/):

- `topiko_uno_q_app.py` — der Flask-Server (Endpunkte `/upload`, `/classify`, `/api/results`, Weboberfläche).
- `topiko_mobile.py` — mobiles Web-Frontend / Captive-Portal-Modul (wird vom Hauptserver eingebunden).

## Voraussetzungen

- Arduino UNO Q mit Debian Linux (oder ein anderer Linux-Rechner, z. B. Raspberry Pi 5).
- Python 3.10+.
- Ein WLAN, in dem Handheld und Dock sich sehen — oder das Dock als Hotspot (siehe unten).

## 1. Systempakete

```bash
sudo apt update
sudo apt install -y git build-essential cmake python3-pip python3-venv alsa-utils
```

## 2. whisper.cpp bauen + Modell laden

Der Server erwartet die Binary unter `~/whisper.cpp/build/bin/whisper-cli` und das Modell unter `~/whisper.cpp/models/ggml-small-q4_0.bin`.

```bash
cd ~
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp
cmake -B build && cmake --build build --config Release -j
# Modell (klein, quantisiert) herunterladen:
./models/download-ggml-model.sh small-q4_0
```

## 3. Sprachmodell (LLM) bereitstellen

Der Server klassifiziert mit einem für TOPIKO feinjustierten, quantisierten Modell:

```
~/models/LFM2.5-350M-TOPIKO-Q4_K_M.gguf
```

Das Modell ist auf **Hugging Face** gehostet — Download-Link und Details stehen in [`../models/README.md`](../models/README.md). Der Zielpfad entspricht `GGUF_PATH` in `topiko_uno_q_app.py`.

> **Tipp:** Beide Modelle (Whisper + Sprachmodell) auf einmal laden mit
> [`../models/download-models.sh`](../models/download-models.sh).

## 4. Python-Abhängigkeiten

```bash
cd ~/topiko            # Ordner mit topiko_uno_q_app.py + topiko_mobile.py
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

> `llama-cpp-python` wird beim Installieren aus dem Quellcode gebaut — das kann auf dem UNO Q einige Minuten dauern.

## 5. Audiogerät (nur für lokale Server-Aufnahme)

Für den optionalen serverseitigen Aufnahme-Button nutzt die App `arecord`. Das ALSA-Gerät ist in der Datei gesetzt (`ALSA_DEVICE`). Verfügbare Geräte prüfen:

```bash
arecord -L
```

Für den normalen Handheld-Betrieb (Upload per WLAN) wird kein Mikrofon am Dock benötigt.

## 6. Server starten

```bash
source .venv/bin/activate
python3 topiko_uno_q_app.py
```

Der Server läuft auf **Port 5000**. Beim Start gibt er die IP-Adresse aus. Schnelltest von einem anderen Rechner im selben Netz:

```bash
curl -F "audio=@test_16k_mono.wav" http://<DOCK-IP>:5000/classify
```

Antwort ist JSON mit `transcript`, `result`, `whisper_time`, `llm_time`. Der Status ist auch im Browser unter `http://<DOCK-IP>:5000/health` einsehbar.

## 7. Automatisch starten (optional, systemd)

```ini
# /etc/systemd/system/topiko.service
[Unit]
Description=TOPIKO Dock
After=network-online.target

[Service]
User=<dein-user>
WorkingDirectory=/home/<dein-user>/topiko
ExecStart=/home/<dein-user>/topiko/.venv/bin/python3 topiko_uno_q_app.py
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now topiko
```

## 8. Dock als Hotspot (optional, mobiler Betrieb)

Damit das Handheld ohne vorhandenes WLAN funktioniert, kann das Dock einen Hotspot aufspannen — das Handheld sucht fest nach `TOPIKO` / `topiko2026` (Dock dann unter `10.42.0.1`):

```bash
sudo nmcli device wifi hotspot ssid TOPIKO password topiko2026 ifname wlan0
```

## Datenablage

Aufnahmen und Ergebnisse liegen unter `~/topiko_data/` (`audio/` + `notes.json`). Nach einem Neustart werden unterbrochene Verarbeitungen automatisch wieder eingereiht.
