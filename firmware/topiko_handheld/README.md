# TOPIKO Handheld-Firmware — Setup & Test

Board: **Waveshare ESP32-S3-Touch-AMOLED-1.75**
Funktion: Taste → 5 s Aufnahme → WAV per WLAN an das UNO-Q-Dock (`/classify`) → JSON-Ergebnis auf dem AMOLED.

## 1. Dateien in den Sketch-Ordner legen
Dieser Ordner braucht neben `topiko_handheld.ino` noch den ES8311-Treiber aus dem Waveshare-Beispiel:
- `es8311.c` und `es8311.h` aus dem Repo `waveshareteam/ESP32-S3-Touch-AMOLED-1.75`,
  Pfad `examples/Arduino-v3.3.5/examples/08_ES8311/` — beide Dateien hierher kopieren.

## 2. Arduino-IDE
- Boardpaket **arduino-esp32 Core ≥ 3.1.0** (Beispiele sind für 3.3.5).
- Board: **ESP32S3 Dev Module**
- **PSRAM: OPI PSRAM** (zwingend — der Audiopuffer liegt im PSRAM)
- Flash Size: **16MB**, USB CDC On Boot: **Enabled**
- Bibliotheken (Bibliotheksverwalter): **GFX Library for Arduino**, **ArduinoJson** (v7).
  `ESP_I2S` ist Teil des Cores.

## 3. Konfiguration im Sketch ausfüllen
```
#define WIFI_SSID  "DEIN_WLAN"
#define WIFI_PASS  "DEIN_PASSWORT"
#define DOCK_IP    "192.168.2.86"   // IP des UNO Q im selben WLAN
```

## 4. Dock prüfen (läuft bereits)
Der Endpunkt ist auf dem UNO Q aktiv. Schnelltest von einem Rechner im selben Netz:
```
curl -F "audio=@irgendeine_16k_mono.wav" http://192.168.2.86:5000/classify
```
Antwort ist JSON mit `transcript`, `result`, `whisper_time`, `llm_time`.

## 5. Auf dem Board zu verifizieren (kann ich blind nicht testen)
Diese Stellen sind im Code mit `VERIFY` markiert:
1. **Mikrofon-Datenpin (wichtigste Stelle):** Die DI/DO-Benennung ist im Waveshare-Repo widersprüchlich. Wenn die Aufnahme **stumm** ist, in `setup()` bei `i2s.setPins(...)` die Argumente **PIN_DI und PIN_DO tauschen**.
2. **Mikrofon-Typ:** `es8311_microphone_config(h, false)` = analoges Mikro. Falls kein Signal, auf `true` (digital) testen. Sollte das Mic-Array über den **ES7210** laufen, muss dieser zusätzlich initialisiert werden (dann Bescheid geben).
3. **Kanal:** Mono nimmt den linken Kanal (`s[i*2]`). Bei Stille/Rauschen ggf. `s[i*2+1]`.
4. **Mic-Gain:** `MIC_GAIN` (Default 30 dB) bei zu leiser/übersteuerter Aufnahme anpassen.
5. **Auslöser-Taste:** Default ist die **BOOT-Taste (GPIO0)**. Für einen eigenen Gehäuse-Taster `BTN_PIN` auf den verwendeten GPIO setzen.
6. **AMOLED-Stromversorgung:** Das Hello-World-Beispiel des Boards treibt das Display ohne PMU-Code, daher kein AXP2101-Init nötig. Falls das Display dunkel bleibt, AXP2101 per `XPowersLib` `power.begin(Wire, AXP2101_SLAVE_ADDRESS, 15, 14)` ergänzen.

## 6. Erwarteter Ablauf
Start → „Bereit · Taste = Notiz" → Taste drücken → „Aufnahme… sprich jetzt" (5 s) → „Verarbeite…" (~50 s) → Anzeige von `type`, `title` und Transkript.

## Status
Firmware ist vollständig geschrieben, aber **noch nicht auf dem Gerät getestet** (kein Board im Zugriff). Erwartbar treffsicher: WLAN, Multipart-Upload, JSON, Display. Erfahrungsgemäß braucht der **Audio-Aufnahmepfad** (Punkt 1–3) eine kurze Hardware-Iteration.
