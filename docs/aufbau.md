# TOPIKO — Hardware-Aufbau

Diese Anleitung beschreibt den Bau des **Handhelds**. Die Einrichtung des Docks (Arduino UNO Q) ist in [`software-installation.md`](software-installation.md) beschrieben. Eine bebilderte Kurzfassung liegt als [`TOPIKO_Anleitung_IKEA.pdf`](TOPIKO_Anleitung_IKEA.pdf) bei.

## Stückliste

| # | Bauteil | Details |
|---|---------|---------|
| 1 | **Waveshare ESP32-S3-Touch-AMOLED-1.75** | 1,75″ rundes AMOLED (466×466), Dual-Mikrofon-Array (ES7210), Lautsprecher-Codec (ES8311), Touch, AXP2101-Laderegler, microSD |
| 2 | **LiPo-Akku 3,7 V** | flach, ~320–500 mAh, MX1.25-Stecker (passend zum Board-Akkuanschluss) |
| 3 | **Lautsprecher** | kleiner 8-Ω-Lautsprecher mit MX1.25-Stecker |
| 4 | **TTP223 Touch-Sensor** | kapazitiver Taster, an den 8-Pin-Header (Pin „16") |
| 5 | **Gewindeeinsätze M2.5** + **Schrauben M2.5×7** | zum Verschrauben der Gehäusehälften |
| 6 | **Transluzentes PLA** | 1–2 mm Wandstärke über dem Display (Display scheint durch) |
| 7 | **Opakes PLA** | Gehäusekörper |
| 8 | **microSD-Karte** (optional) | größere Aufnahme-Warteschlange; sonst interner Flash (FFat) |

Richtwert Materialkosten: ~80–100 €.

## 1. Gehäuse drucken

Die druckbaren Teile liegen in [`../hardware/3d/`](../hardware/3d/) als STL, die editierbaren Quelldateien in [`../hardware/cad/`](../hardware/cad/) als STEP.

- **Oberteil** (`TOPIKO_4.0_Oberteil.stl`): über dem Display **transluzentes PLA**, 1–2 mm Wand über dem Displaybereich, damit das Licht diffus durchscheint.
- **Unterteil** (`TOPIKO_4.0_Unterteil.stl`): **opakes PLA**.
- Empfehlung: 0,12–0,16 mm Schichthöhe, keine Stützen nötig, Displayfläche nach oben drucken.

Gewindeeinsätze M2.5 nach dem Druck mit dem Lötkolben einschmelzen.

Explosionsansicht und Teile: siehe [`img/Explosionsansicht.png`](img/Explosionsansicht.png) und [`img/topiko-aufbauanleitung-teile-v1.png`](img/topiko-aufbauanleitung-teile-v1.png).

## 2. Bauteile montieren

1. **Lautsprecher** in den Halter im Unterteil setzen, MX1.25-Stecker in den Lautsprecher-Anschluss des Boards.
2. **Akku** einlegen (doppelseitiges Klebepad), MX1.25-Stecker an den Akkuanschluss. **Polung prüfen** — falsche Polung zerstört das Board.
3. **Touch-Sensor (TTP223)** ins Oberteil setzen, sodass der gedruckte Nippel bzw. die Gehäuseflanke den Sensor auslöst. Verdrahtung an den 8-Pin-Header:

   | TTP223 | Board 8-Pin-Header |
   |--------|--------------------|
   | VCC | 3V3 |
   | GND | GND |
   | I/O (Signal) | Pin **16** |

   Der Sensor ist **aktiv HIGH**. Ohne angeschlossenen Sensor liegt Pin 16 über einen internen Pulldown fest auf LOW (kein Fehlauslösen).
4. **Board** einsetzen, Display nach oben zur transluzenten Fläche ausrichten.
5. Gehäusehälften verschrauben (M2.5×7).

## 3. Firmware flashen

Die Firmware liegt in [`../firmware/topiko_handheld/`](../firmware/topiko_handheld/). Die ES7210-/ES8311-Treiber sind bereits enthalten.

### Arduino-IDE einrichten

- **Boardpaket:** `arduino-esp32` Core **≥ 3.1.0** (getestet mit 3.3.10).
- **Board:** *ESP32S3 Dev Module*
- **PSRAM:** *OPI PSRAM* (zwingend — der Bildpuffer liegt teils im PSRAM)
- **Flash Size:** *16MB*
- **Partition Scheme:** *16M Flash (3MB APP/9.9MB FATFS)*
- **USB CDC On Boot:** *Enabled*
- **Bibliotheken** (Bibliotheksverwalter): *GFX Library for Arduino*, *ArduinoJson* (v7), *XPowersLib*. `ESP_I2S` ist Teil des Cores.

### WLAN / Dock konfigurieren

Im Sketch bzw. in einer optionalen `secrets.h` (gleicher Ordner) eintragen:

```c
#define WIFI_SSID  "DEIN_WLAN"
#define WIFI_PASS  "DEIN_PASSWORT"
```

Das Handheld verbindet sich mit zwei WLANs (in dieser Reihenfolge):

- **`TOPIKO` / `topiko2026`** — der Hotspot des Docks (fest im Code, für den mobilen/Ausstellungsbetrieb), Dock unter `10.42.0.1`.
- **Dein Heim-WLAN** aus `WIFI_SSID`/`WIFI_PASS`, Dock unter `192.168.2.86` (Adresse ggf. im Sketch anpassen: `DOCK_HOST_HOME`).

### Kompilieren & Flashen (Kommandozeile, optional)

```bash
arduino-cli compile --upload -p /dev/cu.usbmodemXXXX \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" \
  firmware/topiko_handheld
```

## 4. Bedienung

- **Aufnahme:** Touch halten → sprechen → loslassen. Der Lichtkörper schrumpft während des Sprechens als Feedback.
- **Ergebnis:** Kommt vom Dock ein Ergebnis zurück, pulst die Kategorie-Farbe (Termin = blau, To-do = orange, Erinnerung = lila, Idee = gelb, Tagebuch = grün).
- **Akkuwarnung:** Unter 20 % färbt sich der Ruhezustand leicht rot, unter 10 % kräftig dunkelrot.
- **Deep-Sleep:** Nach 5 Minuten Inaktivität im Akkubetrieb; Aufwecken durch Berühren des Touch-Sensors.

## Hinweise & Troubleshooting

- **Mikrofon nimmt nichts auf:** Das Mikro-Array hängt am linken I2S-Kanal (ADC1). Der ES7210 muss die volle „start"-Sequenz durchlaufen (in der Firmware enthalten) — sonst bleibt der Analogteil stumm. Im seriellen Log erscheint beim Boot `[MIC] ES7210 init ...`.
- **Kein Laden / falscher Ladestand:** Der AXP2101 wird per XPowersLib initialisiert (`disableTSPinMeasure()` ist gesetzt, da kein Akku-NTC verbaut ist). Serielles Log: `[BAT] Akku erkannt, …%`.
- **Display bleibt dunkel:** PSRAM auf *OPI* gestellt? Ohne PSRAM scheitert die Pufferzuweisung.
