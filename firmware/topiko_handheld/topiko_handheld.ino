/**
 * TOPIKO — Handheld-Firmware v3 (Waveshare ESP32-S3-Touch-AMOLED-1.75)
 * ---------------------------------------------------------------------
 * Reines Form-und-Farbe-Interface (Display strahlt durch Resin, KEIN Text):
 *   - Bildschirmfuellende, weichkantige Herz-Abstraktion mit Lub-dub-Pochen
 *   - Mikrofon ist DAUERHAFT offen: das Herz reagiert immer in Echtzeit
 *     auf den Raumklang (Atmen der Form mit dem Pegel)
 *   - Zustaende nur ueber Farbe/Bewegung:
 *       mint, ruhig        = bereit
 *       orange, groesser   = Aufnahme (Taste gehalten), pulsiert mit Stimme
 *       violett, unruhig   = speichert / laedt hoch / Dock rechnet
 *       Kategorie-Farbpuls = Ergebnis zurueckgekommen (termin=blau, todo=orange,
 *                            erinnerung=lila, idee=gelb, tagebuch=gruen)
 *       rot, hektisch      = Fehler (zu kurz / Speicher voll / Mikro stumm)
 *
 * Pipeline wie v2: Push-to-talk -> FFat-Warteschlange -> Upload zum Dock,
 * sobald erreichbar -> JSON-Ergebnisse zurueck aufs Geraet (/done.jsonl,
 * spaeter NFC-Sync ans iPhone).
 *
 * Hardware-Erkenntnis: Das Mikrofon-Array haengt am ES7210-ADC (I2C 0x40),
 * der ES8311 bedient nur den Lautsprecher. I2S: dout=8 (ES8311), din=10 (ES7210).
 *
 * Board: ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash 16MB,
 *   Partition: 3MB APP / 9.9MB FATFS, USB CDC On Boot: Enabled.
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <FFat.h>
#include <SD_MMC.h>   // 16-GB-TF-Karte des Boards: CLK=2, CMD=1, D0=3 (1-bit)
#include <math.h>
#include "Arduino_GFX_Library.h"
#include "es8311.h"
#include "es7210.h"
#include "esp_sleep.h"    // ADC des Mikrofon-Arrays (eigener Chip, I2C 0x40)
#include "esp_heap_caps.h" // internes DMA-RAM fuer den Display-Push
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"   // AXP2101: Akku-Erkennung, Laden, Fuel Gauge
#include "blob_types.h"

// 5 min Inaktivitaet -> Deep Sleep; Aufwachen durch Beruehren des Touch-Buttons (GPIO 16)
void powerOff() {
  Serial.println("[PWR] 5 min inaktiv -> Deep Sleep");
  extern Arduino_CO5300 *gfx;
  extern XPowersAXP2101 power;
  extern bool pmuOk;
  digitalWrite(46, LOW);                    // PA (Lautsprecher) aus
  gfx->fillScreen(0x0000);
  gfx->displayOff();
  delay(50);
  // WICHTIG: Deep Sleep legt nur den ESP32-S3 schlafen. Die Audio-Rail ALDO1
  // (ES8311 + ES7210 + Mikro-Array-Bias + Lautsprecher-PA) laeuft am AXP2101
  // sonst weiter und leert den Akku im "Aus"-Zustand. Rail vor dem Schlafen kappen.
  if (pmuOk) power.disableALDO1();
  while (digitalRead(16) == HIGH) delay(20); // warten bis Finger weg, sonst sofortiger Wake
  delay(200);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 1);  // Touch = HIGH weckt auf
  esp_deep_sleep_start();
}

fs::FS *store = &FFat;   // aktives Dateisystem: SD bevorzugt, FFat-Fallback
bool sdOk = false;
static inline uint64_t storeFree() {
  return sdOk ? (SD_MMC.totalBytes() - SD_MMC.usedBytes()) : FFat.freeBytes();
}

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "SSID_HIER"
#define WIFI_PASS "PASSWORT_HIER"
#endif
#ifndef AP_SSID
#define AP_SSID "TOPIKO"
#define AP_PASS "topiko2026"
#endif

// ====== KONFIGURATION ======
#define DOCK_HOST_HOME "192.168.2.86"  // Dock im Heim-WLAN
#define DOCK_HOST_AP   "10.42.0.1"     // Dock als eigener Hotspot (Ausstellung)
#define DOCK_PORT   5000
#define SAMPLE_RATE 16000
#define MAX_REC_S   20
#define MIN_REC_S   1.0f   // kuerzer = Versehen, wird still verworfen
#define SYNC_EVERY_MS 12000
#define MIC_GAIN_ES7210 ES7210_MIC_GAIN_37_5DB   // Array-Mikro braucht hohe Verstaerkung

// ====== PINS ======
#define LCD_CS 12
#define LCD_SCLK 38
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_RESET 39
#define LCD_W 466
#define LCD_H 466
#define IIC_SDA 15
#define IIC_SCL 14
#define PIN_BCLK 9
#define PIN_WS 45
#define PIN_DI 8       // I2S DOUT des ESP -> ES8311 (Lautsprecher)
#define PIN_DO 10      // I2S DIN  des ESP <- ES7210 (Mikrofon-Array)
#define PIN_MCLK 42
#define PIN_PA 46
#define BTN_PIN   0    // BOOT-Taste (Fallback), aktiv LOW
#define TOUCH_PIN 16   // TTP223 am 8-Pin-Header "16", aktiv HIGH

// ====== Renderer: 232 px intern, 2x hochskaliert auf 464 px (fast vollflaechig) ======
#define CV 232
#define OUTW (CV * 2)                    // 464
#define OUT_X 0
#define OUT_Y 0
#define CX (CV / 2)
#define CY (CV / 2)
#define EDGE_SOFT 16                     // weiche Kante (Renderpixel; wirkt 2x)

// Heller Kern exakt in der Mitte (radialer Verlauf) — damit ist die Optik
// unabhaengig von der Einbaurotation des Displays.
#define LIGHT_DX  0
#define LIGHT_DY  0

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_W, LCD_H, 6, 0, 0, 0);
XPowersAXP2101 power;
uint16_t *fbSmall = nullptr;             // CV*CV Renderpuffer
#define ROWSPAN 16                       // Ausgabezeilen je QSPI-Push
uint16_t *rowBuf = nullptr;              // ROWSPAN*OUTW im internen DMA-RAM (schneller als PSRAM)

I2SClass i2s;

// ====== Zustaende ======
enum State { ST_IDLE, ST_RECORDING, ST_SAVING, ST_RESULT };
volatile State state = ST_IDLE;
volatile bool  syncBusy   = false;
volatile int   queueCount = 0;
volatile int   dockPending = -1;
volatile bool  wifiOk = false;
WiFiMulti wifiMulti;
String dockHost = DOCK_HOST_HOME;      // wird nach Verbindung je nach SSID gesetzt
volatile float micLevel = 0.0f;          // 0..1, laeuft IMMER (Dauer-Mikrofon)
volatile int batteryPct = -1;            // 0..100 (Ladestand-Warnfarbe), -1 = unbekannt

uint32_t resUntil = 0;                   // Ergebnis-Farbpuls aktiv bis
uint32_t errUntil = 0;                   // Fehler-Farbpuls aktiv bis

// ====== Audio (Task auf Core 0, Mikro dauerhaft offen) ======
int16_t *recBuf = nullptr;
volatile size_t recSamples = 0;
volatile bool   recActive = false;
volatile bool   recDone = true;
volatile int    micChannel = 0;
es8311_handle_t codecHandle = nullptr;
es7210_dev_handle_t es7210 = nullptr;

// Drone-Bestaetigungstoene (Puffer werden in setup() gebaut)
#define DRONE_S ((SAMPLE_RATE * 14) / 10)          // 1,4 s
int16_t *droneSave = nullptr;                      // Notiz gespeichert (tief, A2)
int16_t *droneResult = nullptr;                    // Ergebnis da (heller, D3)
volatile int16_t *dronePending = nullptr;

void audioTask(void *arg) {
  const int FRAMES = 256;                // 16 ms Latenz fuer die Visualisierung
  static int16_t chunk[FRAMES * 2];
  for (;;) {
    size_t got = i2s.readBytes((char *)chunk, sizeof(chunk));
    int frames = got / 4;
    if (frames <= 0) { vTaskDelay(1); continue; }
    uint32_t acc = 0;
    size_t n = recSamples;
    bool rec = recActive;
    for (int i = 0; i < frames; i++) {
      int16_t s = chunk[i * 2 + micChannel];
      acc += (uint32_t)abs(s);
      if (rec && n < (size_t)SAMPLE_RATE * MAX_REC_S) recBuf[n++] = s;
    }
    if (rec) {
      recSamples = n;
      recDone = false;
      if (n >= (size_t)SAMPLE_RATE * MAX_REC_S) recActive = false;
    } else {
      recDone = true;
    }
    // Pegel: schneller Anstieg, langsamer Abfall — wirkt organisch
    float inst = fminf(1.0f, (float)(acc / frames) / 3500.0f);
    micLevel = fmaxf(inst, micLevel * 0.93f);
  }
}

// ====== WAV / Warteschlange (FFat) ======
void writeWavHeader(uint8_t *h, uint32_t dataBytes) {
  uint32_t chunk = 36 + dataBytes, sub1 = 16, sr = SAMPLE_RATE, br = SAMPLE_RATE * 2;
  uint16_t fmt = 1, ch = 1, ba = 2, bps = 16;
  memcpy(h, "RIFF", 4); memcpy(h + 4, &chunk, 4); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); memcpy(h + 16, &sub1, 4); memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &ch, 2); memcpy(h + 24, &sr, 4); memcpy(h + 28, &br, 4);
  memcpy(h + 32, &ba, 2); memcpy(h + 34, &bps, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataBytes, 4);
}

int countQueue() {
  int n = 0;
  File dir = store->open("/q");
  if (!dir) return 0;
  File f;
  while ((f = dir.openNextFile())) { if (!f.isDirectory()) n++; f.close(); }
  dir.close();
  return n;
}

bool saveRecording() {
  uint32_t dataBytes = (uint32_t)recSamples * 2;
  if (storeFree() < dataBytes + 65536) return false;
  char id[24];
  snprintf(id, sizeof(id), "hh%08lx%04x", (unsigned long)esp_random(), (unsigned)(millis() & 0xFFFF));
  String path = String("/q/") + id + ".wav";
  File f = store->open(path, FILE_WRITE);
  if (!f) return false;
  uint8_t hdr[44];
  writeWavHeader(hdr, dataBytes);
  f.write(hdr, 44);
  size_t written = 0;
  const size_t CH = 16384;
  uint8_t *p = (uint8_t *)recBuf;
  while (written < dataBytes) {
    size_t k = min(CH, (size_t)(dataBytes - written));
    if (f.write(p + written, k) != k) { f.close(); store->remove(path); return false; }
    written += k;
  }
  f.close();
  queueCount = countQueue();
  Serial.printf("[REC] gespeichert %s (%.1fs, Queue %d)\n", path.c_str(),
                (float)recSamples / SAMPLE_RATE, queueCount);
  return true;
}

// ====== Ergebnis-Farbpuls (Kategorie -> Farbe, KEIN Text) ======
BlobStyle S_RESULT;   // wird je Kategorie gesetzt

void setResultStyle(const String &type) {
  struct { const char *t; uint8_t r1, g1, b1, r2, g2, b2; } M[] = {
    { "termin",     0x4F, 0xC3, 0xF7, 0x06, 0x1E, 0x30 },
    { "todo",       0xFF, 0xB7, 0x4D, 0x38, 0x1E, 0x04 },
    { "erinnerung", 0xBA, 0x68, 0xC8, 0x26, 0x0C, 0x2C },
    { "idee",       0xFF, 0xF1, 0x76, 0x33, 0x2E, 0x06 },
    { "tagebuch",   0x81, 0xC7, 0x84, 0x0C, 0x28, 0x10 },
  };
  // weisser Kern, Kategorie-Farbe als Koerper, abgedunkelte Variante als Glow
  S_RESULT = { 0xFF, 0xEE, 0xF2,  0xB0, 0xBE, 0xC5,  0x40, 0x48, 0x50,
               0.65f, 0.11f, 0.20f, 0.022f, 106 };
  for (auto &m : M) {
    if (type == m.t) {
      S_RESULT.r2 = m.r1; S_RESULT.g2 = m.g1; S_RESULT.b2 = m.b1;
      S_RESULT.r3 = m.r1 * 0.45f; S_RESULT.g3 = m.g1 * 0.45f; S_RESULT.b3 = m.b1 * 0.45f;
      break;
    }
  }
}

// ====== Sync (Core 0): Upload + Ergebnisse zurueckholen ======
bool uploadFile(const String &path) {
  File f = store->open(path, FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  uint8_t *data = (uint8_t *)ps_malloc(sz);
  if (!data) { f.close(); return false; }
  f.read(data, sz);
  f.close();

  String id = path.substring(3, path.length() - 4);
  float dur = (sz > 44) ? (float)(sz - 44) / (SAMPLE_RATE * 2) : 0;
  String boundary = "----topiko" + String(millis());
  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"id\"\r\n\r\n" + id +
                "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"duration\"\r\n\r\n" + String(dur, 1) +
                "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"" + id +
                ".wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  size_t bodyLen = head.length() + sz + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(bodyLen);
  if (!body) { free(data); return false; }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), data, sz);
  memcpy(body + head.length() + sz, tail.c_str(), tail.length());
  free(data);

  HTTPClient http;
  http.begin(String("http://") + dockHost + ":" + DOCK_PORT + "/upload");
  http.setTimeout(20000);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int code = http.POST(body, bodyLen);
  free(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  Serial.printf("[SYNC] Upload %s -> %d %s\n", id.c_str(), code, resp.c_str());
  if (code == 200) { store->remove(path); return true; }
  return false;
}

bool fetchResults() {
  HTTPClient http;
  http.begin(String("http://") + dockHost + ":" + DOCK_PORT + "/api/results?limit=3");
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String resp = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  dockPending = doc["pending"] | -1;
  JsonArray arr = doc["results"].as<JsonArray>();
  if (arr.size() == 0) return true;

  String ids = "", lastType = "unbekannt";
  File log = store->open("/done.jsonl", FILE_APPEND);   // Basis fuer NFC-Sync ans iPhone
  for (JsonObject r : arr) {
    String id = r["id"] | "";
    if (id.isEmpty()) continue;
    if (log) { serializeJson(r, log); log.print("\n"); }
    lastType = String((const char *)(r["type"] | "unbekannt"));
    if (!ids.isEmpty()) ids += ",";
    ids += "\"" + id + "\"";
  }
  if (log) log.close();
  if (ids.isEmpty()) return true;

  setResultStyle(lastType);
  resUntil = millis() + 4500;
  if (state == ST_IDLE) state = ST_RESULT;
  dronePending = droneResult;                      // hellerer Drone: Ergebnis ist da

  HTTPClient ack;
  ack.begin(String("http://") + dockHost + ":" + DOCK_PORT + "/api/ack");
  ack.setTimeout(8000);
  ack.addHeader("Content-Type", "application/json");
  int ac = ack.POST("{\"ids\":[" + ids + "]}");
  ack.end();
  Serial.printf("[SYNC] Ergebnis '%s' geholt, ACK -> %d\n", lastType.c_str(), ac);
  return true;
}

void syncTask(void *arg) {
  uint32_t lastTry = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    wifiOk = (WiFi.status() == WL_CONNECTED);
    if (wifiOk) dockHost = (WiFi.SSID() == AP_SSID) ? DOCK_HOST_AP : DOCK_HOST_HOME;
    if (!wifiOk) {
      static uint32_t lastRe = 0;
      if (millis() - lastRe > 20000) { wifiMulti.run(); lastRe = millis(); }
      continue;
    }
    if (state == ST_RECORDING || state == ST_SAVING) continue;
    if (millis() - lastTry < SYNC_EVERY_MS) continue;
    lastTry = millis();

    syncBusy = true;
    File dir = store->open("/q");
    if (dir) {
      File f = dir.openNextFile();
      String path = "";
      while (f && f.isDirectory()) { f.close(); f = dir.openNextFile(); }
      if (f) { path = String("/q/") + f.name(); f.close(); }
      dir.close();
      if (!path.isEmpty()) {
        uploadFile(path);
        queueCount = countQueue();
        if (queueCount > 0) lastTry = millis() - SYNC_EVERY_MS + 1500;
      }
    }
    fetchResults();
    syncBusy = false;
  }
}

// ====== Herz-Renderer ======
uint8_t *angLUT = nullptr;
uint8_t *radLUT = nullptr;
uint8_t *lightLUT = nullptr;
uint16_t paletteCur[256];
uint8_t contour[256];
uint32_t invContour[256];                // Kehrwert (205<<16)/contour -> spart Division je Pixel
float heartBase[256];                    // Herz-Abstraktion (statisch, einmal berechnet)

// Farbwelt nach Referenzbild: weiss-rosa Kern oben, Orange im Koerper, Pink als Glow.
// Idle: kaum Herzschlag — stattdessen zyklisches Verblassen/Aufloesen ins Korn.
// Aufnahme: kraeftiges Lub-dub, das mit der Stimme mitgeht.
//                     Kern (hell)        Mitte (Koerper)     Rand (Glow)       Periode  Amp    Audio  Unruhe Radius
BlobStyle S_IDLE  = { 0xFF, 0xEC, 0xF4,  0xEE, 0x9C, 0x20,  0xE2, 0x63, 0xBE, 1.55f, 0.020f, 0.10f, 0.008f, 112 };
// Aufnahme: Blob schrumpft deutlich (Radius 72 statt Idle 112) als Sprech-Feedback;
// pulsiert nur noch leicht mit der Stimme, damit er klein bleibt. Rueckweg = Idle-Groesse.
BlobStyle S_REC   = { 0xFF, 0xF2, 0xD8,  0xFF, 0x7A, 0x30,  0xE0, 0x30, 0x50, 0.75f, 0.060f, 0.12f, 0.014f, 72 };
BlobStyle S_PROC  = { 0xEA, 0xE0, 0xFF,  0x9A, 0x6C, 0xF0,  0x4A, 0x2C, 0xB0, 0.95f, 0.065f, 0.10f, 0.024f, 110 };
BlobStyle S_ERR   = { 0xFF, 0xD0, 0xD0,  0xF0, 0x50, 0x50,  0x80, 0x10, 0x20, 0.45f, 0.095f, 0.10f, 0.032f, 110 };
BlobStyle S_SAVED = { 0xFF, 0xFF, 0xF6,  0x9C, 0xE8, 0xC0,  0x2A, 0x70, 0x58, 0.90f, 0.070f, 0.16f, 0.010f, 116 };
// Akku-Warnfarben im Ruhezustand: gleiche ruhige Bewegung wie S_IDLE, nur Farbe rot.
BlobStyle S_LOW20 = { 0xFF, 0xE6, 0xE2,  0xE8, 0x6A, 0x52,  0x90, 0x28, 0x2E, 1.55f, 0.020f, 0.10f, 0.008f, 112 }; // <20% leichtes Rot
BlobStyle S_LOW10 = { 0xF0, 0x60, 0x50,  0xB0, 0x14, 0x10,  0x38, 0x04, 0x06, 1.55f, 0.022f, 0.10f, 0.008f, 112 }; // <10% starkes Dunkelrot
uint32_t savedUntil = 0;   // kurze "gespeichert"-Bestaetigung
float recPeak = 0.0f;      // lautester Pegel waehrend der Aufnahme (Sprach-Gate)
BlobStyle cur     = S_IDLE;

// ====== Idle-Aufloesungszyklus (Verblassen -> Korn -> scharf zurueck) ======
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothf(float t) { return t * t * (3.0f - 2.0f * t); }
float dissolve = 0;                      // 0 = scharf, 1 = komplett im Noise
static uint32_t rng = 0x2F6E2B1u;
static inline uint32_t xrnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

float dissolveTarget(float tSec) {
  const float T = 11.0f;                             // Zyklusdauer
  float s = fmodf(tSec, T) / T;
  int cycle = (int)(tSec / T);
  // Tiefe variiert je Zyklus: mal nur Verblassen, mal ganz im Noise verschwinden
  float ph = cycle * 2.399f;
  float depth = 0.45f + 0.55f * (0.5f + 0.5f * sinf(ph));
  // langsam aufloesen (0.32..0.62), kurz weg, schneller scharf zurueck (0.78..0.90)
  float up = smoothf(fminf(1.0f, fmaxf(0.0f, (s - 0.32f) / 0.30f)));
  float dn = smoothf(fminf(1.0f, fmaxf(0.0f, (s - 0.78f) / 0.12f)));
  return depth * up * (1.0f - dn);
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void buildLUTs() {
  angLUT   = (uint8_t *)ps_malloc(CV * CV);
  radLUT   = (uint8_t *)ps_malloc(CV * CV);
  lightLUT = (uint8_t *)ps_malloc(CV * CV);
  if (!angLUT || !radLUT || !lightLUT) return;
  // Orb exakt zentriert (garantiert rund); Licht in Gehaeuse-oben-Richtung
  const float lx = CX + 44.0f * LIGHT_DX, ly = CY + 44.0f * LIGHT_DY;
  for (int y = 0; y < CV; y++) {
    for (int x = 0; x < CV; x++) {
      int i = y * CV + x;
      float dx = x - CX, dy = y - CY;
      radLUT[i] = (uint8_t)fminf(255.0f, sqrtf(dx * dx + dy * dy));
      float a = atan2f(dy, dx);
      angLUT[i] = (uint8_t)((a + (float)M_PI) * (255.0f / (2.0f * (float)M_PI)));
      float ex = x - lx, ey = y - ly;
      lightLUT[i] = (uint8_t)fminf(255.0f, sqrtf(ex * ex + ey * ey) * 0.95f);
    }
  }
  // Herz-Abstraktion: Index 64 = oben, 192 = unten (Bildschirm-y waechst nach unten)
  for (int a = 0; a < 256; a++) {
    float u = ((float)(a - 64)) * (2.0f * (float)M_PI / 256.0f);   // 0 = oben
    if (u > (float)M_PI)  u -= 2.0f * (float)M_PI;
    if (u < -(float)M_PI) u += 2.0f * (float)M_PI;
    float au = fabsf(u);
    (void)u; (void)au;
    heartBase[a] = 1.0f;                             // exakt kreisrund
  }
}

float beatEnv(float tSec, float period) {
  float p = fmodf(tSec, period) / period;          // Lub-dub: zwei Schlaege, dann Pause
  float e1 = expf(-powf((p - 0.11f) / 0.050f, 2));
  float e2 = 0.55f * expf(-powf((p - 0.33f) / 0.075f, 2));
  return e1 + e2;
}

float envSmooth = 0, audioVis = 0;

void blendStyle(const BlobStyle &t, float k) {
  cur.r1 += (t.r1 - cur.r1) * k;  cur.g1 += (t.g1 - cur.g1) * k;  cur.b1 += (t.b1 - cur.b1) * k;
  cur.r2 += (t.r2 - cur.r2) * k;  cur.g2 += (t.g2 - cur.g2) * k;  cur.b2 += (t.b2 - cur.b2) * k;
  cur.r3 += (t.r3 - cur.r3) * k;  cur.g3 += (t.g3 - cur.g3) * k;  cur.b3 += (t.b3 - cur.b3) * k;
  cur.beatPeriod += (t.beatPeriod - cur.beatPeriod) * k;
  cur.beatAmp    += (t.beatAmp - cur.beatAmp) * k;
  cur.audioReact += (t.audioReact - cur.audioReact) * k;
  cur.wobble     += (t.wobble - cur.wobble) * k;
  cur.radius     += (t.radius - cur.radius) * k;
  // Dreistufiger Verlauf Kern -> Koerper -> Glow; Kern pulst mit Schlag und Klang.
  // dissolve blendet zusaetzlich ab: Helligkeit sinkt, Farben waschen aus (Defokus).
  float glow = 1.0f + 0.18f * envSmooth + 0.22f * audioVis;
  float dim  = 1.0f - 0.85f * dissolve;
  float wash = 0.50f * dissolve;
  for (int i = 0; i < 256; i++) {
    float tt = i / 255.0f;
    float r, g, b;
    if (tt < 0.42f) {
      float s = smoothf(tt / 0.42f);
      r = mixf(cur.r1 * glow, cur.r2, s);
      g = mixf(cur.g1 * glow, cur.g2, s);
      b = mixf(cur.b1 * glow, cur.b2, s);
    } else {
      float s = smoothf((tt - 0.42f) / 0.58f);
      r = mixf(cur.r2, cur.r3, s);
      g = mixf(cur.g2, cur.g3, s);
      b = mixf(cur.b2, cur.b3, s);
    }
    r = mixf(r, cur.r3 * 0.55f, wash) * dim;
    g = mixf(g, cur.g3 * 0.55f, wash) * dim;
    b = mixf(b, cur.b3 * 0.55f, wash) * dim;
    // aeusserer Glow laeuft weich auf Schwarz aus
    float fade = (i > 185) ? (255.0f - i) / 70.0f : 1.0f;
    // Big-Endian ablegen -> Display-Push kann die Bytes direkt per QIO-DMA schicken
    // (kein CPU-Byteswap pro Pixel mehr; siehe draw16bitBeRGBBitmap in flushUpscaled).
    paletteCur[i] = __builtin_bswap16(rgb565(
      (uint8_t)fminf(255.0f, fmaxf(0.0f, r * fade)),
      (uint8_t)fminf(255.0f, fmaxf(0.0f, g * fade)),
      (uint8_t)fminf(255.0f, fmaxf(0.0f, b * fade))));
  }
  paletteCur[255] = 0;
}

void renderHeart(float tSec) {
  float env = beatEnv(tSec, cur.beatPeriod);
  envSmooth = envSmooth * 0.7f + env * 0.3f;
  audioVis  = audioVis * 0.85f + micLevel * 0.15f;

  // Beim Verblassen waechst der Orb leicht und die Kante wird sehr weich
  float scale = (1.0f + cur.beatAmp * env + cur.audioReact * audioVis) * (1.0f + 0.05f * dissolve);
  int esoft = EDGE_SOFT + (int)(46.0f * dissolve);
  float p1 = tSec * 0.50f, p2 = -tSec * 0.34f;
  const float maxR = (float)CX;                          // Display ist rund: CX = randfuellend
  for (int a = 0; a < 256; a++) {
    float u = a * (2.0f * (float)M_PI / 256.0f);
    float w = 1.0f + cur.wobble * sinf(3 * u + p1) + 0.6f * cur.wobble * sinf(5 * u + p2);
    float r = cur.radius * scale * heartBase[a] * w;
    contour[a] = (uint8_t)fminf(maxR, fmaxf(14.0f, r));
    invContour[a] = (205u << 16) / contour[a];     // contour >= 14, kein Div-durch-0
  }

  const uint8_t *ra = radLUT, *an = angLUT, *li = lightLUT;
  uint16_t *px = fbSmall;
  const int ramp = 255 / (2 * esoft);
  for (int i = 0; i < CV * CV; i++) {
    uint8_t r = ra[i], c = contour[an[i]];
    int lim = (int)c + esoft;
    if (r < lim) {
      int idx = ((int)li[i] * (int)invContour[an[i]]) >> 16;   // statt Division
      int soft = ((int)r - ((int)c - esoft)) * ramp;
      if (soft > idx) idx = soft;
      if (idx > 255) idx = 255;
      px[i] = paletteCur[idx];
    } else {
      px[i] = 0;
    }
  }
}

void flushUpscaled() {
  // 232 -> 464 Pixelverdopplung blockweise in internem RAM, dann pushen.
  // Vermeidet den grossen PSRAM-Ausgabepuffer (dessen Lesen war der Flaschenhals).
  int outY = 0, filled = 0;
  for (int y = 0; y < CV; y++) {
    const uint16_t *src = fbSmall + y * CV;         // eine Zeile aus PSRAM lesen
    uint16_t *r0 = rowBuf + (size_t)filled * OUTW;
    uint32_t *d32 = (uint32_t *)r0;                 // zwei gleiche Pixel = ein 32-Bit-Wort
    for (int x = 0; x < CV; x++) { uint32_t c = src[x]; d32[x] = (c << 16) | c; }
    memcpy(r0 + OUTW, r0, OUTW * 2);                // verdoppelte zweite Zeile
    filled += 2;
    if (filled >= ROWSPAN) {
      gfx->draw16bitBeRGBBitmap(0, outY, rowBuf, OUTW, filled);
      outY += filled; filled = 0;
    }
  }
  if (filled > 0) gfx->draw16bitBeRGBBitmap(0, outY, rowBuf, OUTW, filled);
}

// ====== Codecs ======
esp_err_t es8311_codec_init(bool digitalMic) {
  if (!codecHandle) {
    codecHandle = es8311_create(0, ES8311_ADDRRES_0);
    if (!codecHandle) return ESP_FAIL;
    const es8311_clock_config_t clk = {
      .mclk_inverted = false,
      .sclk_inverted = false,
      .mclk_from_mclk_pin = true,
      .mclk_frequency = SAMPLE_RATE * 256,
      .sample_frequency = SAMPLE_RATE
    };
    if (es8311_init(codecHandle, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return ESP_FAIL;
    es8311_sample_frequency_config(codecHandle, SAMPLE_RATE * 256, SAMPLE_RATE);
  }
  es8311_microphone_config(codecHandle, digitalMic);
  return ESP_OK;
}

// ES7210 direkt via Wire mit der bewaehrten esp_codec_dev-Sequenz. Die frueher
// genutzte esp-bsp-Variante (es7210_config_codec) fuhr den Analog-Vorverstaerker
// nie voll hoch -> Mikro lieferte nur Rauschen. Diese Sequenz macht die Mikros
// lebendig (am Geraet verifiziert: Ruhe ~1000, Sprache 4000-9000).
#define ES7210_I2C_ADDR 0x40
#define ES7210_GAIN     0x0A   // 30 dB (0x0E = 37.5 dB uebersteuert deutlich)
static void es7210wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_I2C_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
static uint8_t es7210rd(uint8_t reg) {
  Wire.beginTransmission(ES7210_I2C_ADDR); Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom((int)ES7210_I2C_ADDR, 1); return Wire.available() ? Wire.read() : 0;
}
static void es7210rmw(uint8_t reg, uint8_t clr, uint8_t set) {
  es7210wr(reg, (uint8_t)((es7210rd(reg) & ~clr) | set));
}

bool es7210_mic_init() {
  // --- open ---
  es7210wr(0x00, 0xff); es7210wr(0x00, 0x41);
  es7210wr(0x01, 0x3f);
  es7210wr(0x09, 0x30); es7210wr(0x0a, 0x30);
  es7210wr(0x23, 0x2a); es7210wr(0x22, 0x0a); es7210wr(0x20, 0x0a); es7210wr(0x21, 0x2a);
  es7210rmw(0x08, 0x01, 0x00);            // Slave-Mode (ESP ist I2S-Master)
  es7210wr(0x40, 0x43);                   // ANALOG power
  es7210wr(0x41, 0x70); es7210wr(0x42, 0x70);   // Mic-Bias 2.87 V
  es7210wr(0x07, 0x20); es7210wr(0x02, 0xc1);   // OSR / MAINCLK
  // --- mic_select MIC1|MIC2 + Gain ---
  es7210wr(0x4b, 0xff); es7210wr(0x4c, 0xff);
  es7210rmw(0x01, 0x0b, 0x00); es7210wr(0x4b, 0x00);
  es7210rmw(0x43, 0x10, 0x10); es7210rmw(0x43, 0x0f, ES7210_GAIN);
  es7210rmw(0x01, 0x0b, 0x00); es7210wr(0x4b, 0x00);
  es7210rmw(0x44, 0x10, 0x10); es7210rmw(0x44, 0x0f, ES7210_GAIN);
  es7210wr(0x12, 0x00);                   // non-TDM
  // --- start ---
  es7210wr(0x01, 0x00); es7210wr(0x06, 0x00); es7210wr(0x40, 0x43);
  es7210wr(0x47, 0x08); es7210wr(0x48, 0x08); es7210wr(0x49, 0x08); es7210wr(0x4a, 0x08);
  es7210wr(0x4b, 0x00);
  es7210rmw(0x43, 0x10, 0x10); es7210rmw(0x43, 0x0f, ES7210_GAIN);
  es7210rmw(0x44, 0x10, 0x10); es7210rmw(0x44, 0x0f, ES7210_GAIN);
  es7210wr(0x40, 0x43);
  es7210wr(0x00, 0x71); es7210wr(0x00, 0x41);
  Serial.printf("[MIC] ES7210 init (esp_codec_dev): REG40=0x%02X REG06=0x%02X REG4B=0x%02X\n",
                es7210rd(0x40), es7210rd(0x06), es7210rd(0x4b));
  return true;
}

bool i2sStart(int dout, int din, int mclk) {
  i2s.end();
  delay(30);
  i2s.setPins(PIN_BCLK, PIN_WS, dout, din, mclk);
  return i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
}

bool micAutoProbe() {
  delay(80);
  int pl = 0, pr = 0;
  static int16_t buf[512 * 2];
  uint32_t t0 = millis();
  while ((int)(millis() - t0) < 400) {
    size_t got = i2s.readBytes((char *)buf, sizeof(buf));
    for (int i = 0; i < (int)(got / 4); i++) {
      if (abs(buf[i * 2])     > pl) pl = abs(buf[i * 2]);
      if (abs(buf[i * 2 + 1]) > pr) pr = abs(buf[i * 2 + 1]);
    }
  }
  Serial.printf("[MIC] ES7210-Probe: PeakL=%d PeakR=%d\n", pl, pr);
  // Das Mikrofon-Array dieses Boards liegt fest auf dem LINKEN Kanal (ADC1).
  // Die alte Auto-Erkennung lief in der Stille direkt nach dem Boot und waehlte
  // faelschlich rechts (nur Rauschen), wodurch jede Aufnahme stumm war.
  micChannel = 0;
  Serial.println("[MIC] Kanal fest: links (ADC1)");
  return true;
}

// ====== AXP2101 PMU: Akku-Erkennung + Fuel Gauge (XPowersLib) ======
bool pmuOk = false;

void pmuInit() {
  for (int i = 0; i < 5 && !pmuOk; i++) {          // Erkennung mehrfach versuchen (AXP braucht ggf. Zeit)
    pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (!pmuOk) delay(20);
  }
  if (!pmuOk) { Serial.println("[BAT] AXP2101 nicht gefunden"); return; }
  power.enableBattDetection();
  power.enableBattVoltageMeasure();
  power.enableVbusVoltageMeasure();
  power.enableSystemVoltageMeasure();

  // --- Laderegler wie im Waveshare-Referenzbeispiel (01_AXP2101) konfigurieren ---
  // Ohne diese Werte kommt der AXP2101 mit unguenstigen Defaults hoch.
  // disableTSPinMeasure(): Das Board hat KEINEN Akku-NTC am TS-Pin. Bleibt die
  // Temperaturschutz-Erkennung an, laed der Chip "abnormal" (Waveshare-Hinweis) —
  // das aeussert sich als sprunghafter Ladestand und unsauberes Verhalten.
  power.disableTSPinMeasure();
  power.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  power.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
  power.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
  power.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  // Sauberes Abschalten bei Unterspannung statt Brownout-Zappeln im Akkubetrieb.
  power.setSysPowerDownVoltage(2600);

  // Audio-Rail nach einem Deep Sleep sicher wieder aktivieren
  // (powerOff() schaltet ALDO1 ab; der AXP2101 behaelt den Zustand ueber den Sleep).
  power.enableALDO1();

  Serial.println("[BAT] AXP2101 initialisiert (Charger + Schutz konfiguriert)");
}

void logBattery() {
  if (!pmuOk) return;
  bool conn = power.isBatteryConnect();
  int pct = power.getBatteryPercent();
  int mv = power.getBattVoltage();
  batteryPct = conn ? (pct < 0 ? -1 : (pct > 100 ? 100 : pct)) : -1;   // fuer Warnfarbe
  Serial.printf("[BAT] %s, %d%%, %d mV, USB %s, %s\n",
                conn ? "Akku erkannt" : "KEIN Akku erkannt",
                pct, mv,
                power.isVbusIn() ? "an" : "aus",
                power.isCharging() ? "laedt" : (power.isDischarge() ? "entlaedt" : "standby"));
}

// ====== Drone-Bestaetigungston (weiche, geschichtete Sinus-Flaeche) ======
void buildDrone(int16_t *buf, float f0) {
  const float dur = (float)DRONE_S / SAMPLE_RATE;
  for (int n = 0; n < DRONE_S; n++) {
    float t = (float)n / SAMPLE_RATE;
    float atk = smoothf(fminf(1.0f, t / 0.14f));
    float relT = fminf(1.0f, (dur - t) / 0.55f);
    float env = atk * relT * relT;
    float shimmer = smoothf(fminf(1.0f, t / 0.7f));
    float am = 1.0f + 0.10f * sinf(2 * (float)M_PI * 3.1f * t);
    float v = 0.45f * sinf(2 * (float)M_PI * f0 * t)
            + 0.28f * sinf(2 * (float)M_PI * f0 * 1.4983f * t + 0.4f * sinf(2 * (float)M_PI * 0.7f * t))
            + 0.22f * sinf(2 * (float)M_PI * f0 * 2.0021f * t)
            + 0.12f * shimmer * sinf(2 * (float)M_PI * f0 * 2.9970f * t);
    int16_t s = (int16_t)(v * env * am * 8500.0f);
    buf[n * 2] = s; buf[n * 2 + 1] = s;
  }
}

void droneTask(void *arg) {
  for (;;) {
    if (dronePending) {
      const int16_t *b = (const int16_t *)dronePending;
      i2s.write((uint8_t *)b, (size_t)DRONE_S * 4);
      dronePending = nullptr;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

void i2cScan() {
  Serial.print("[I2C] gefunden:");
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();
}

void fatal(const char *msg) {                      // nur Entwicklung; unter Resin unsichtbar
  Serial.printf("[FATAL] %s\n", msg);
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_RED); gfx->setTextSize(3);
  gfx->setCursor(80, 210); gfx->println(msg);
  while (1) delay(100);
}

// ====== Setup / Loop ======
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[TOPIKO] Handheld v3 boot");

  pinMode(PIN_PA, OUTPUT); digitalWrite(PIN_PA, HIGH);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);  // liegt fest auf LOW, falls Kabel ab
  while (digitalRead(TOUCH_PIN) == HIGH) delay(20);  // Wake-Touch nicht als Aufnahme werten

  gfx->begin(80000000);                            // QSPI 80 MHz statt Default 40 -> ~2x Framerate
  gfx->setBrightness(255);                         // volle Helligkeit (Resin schluckt Licht)
  gfx->fillScreen(RGB565_BLACK);

  fbSmall = (uint16_t *)ps_malloc((size_t)CV * CV * 2);
  rowBuf  = (uint16_t *)heap_caps_malloc((size_t)OUTW * ROWSPAN * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!fbSmall || !rowBuf) fatal("FB alloc");
  buildLUTs();
  if (!angLUT || !radLUT || !lightLUT) fatal("PSRAM LUT");
  blendStyle(S_IDLE, 1.0f);

  recBuf = (int16_t *)ps_malloc((size_t)SAMPLE_RATE * MAX_REC_S * 2);
  if (!recBuf) fatal("PSRAM Audio");

  SD_MMC.setPins(2, 1, 3);                     // TF-Slot (SPI-Pins im SDMMC-Modus)
  if (SD_MMC.begin("/sd", true)) {             // true = 1-bit Modus
    sdOk = true; store = &SD_MMC;
    Serial.printf("[FS] SD-Karte gemountet: %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
  } else if (!FFat.begin(true)) fatal("FS");
  store->mkdir("/q");
  queueCount = countQueue();
  Serial.printf("[FS] %s frei: %u KB, Queue: %d\n", sdOk ? "SD" : "FFat", (unsigned)(storeFree() / 1024), queueCount);

  Wire.begin(IIC_SDA, IIC_SCL);
  i2cScan();
  pmuInit();
  logBattery();
  if (!i2sStart(PIN_DI, PIN_DO, PIN_MCLK)) fatal("I2S");
  if (es8311_codec_init(false) != ESP_OK) fatal("ES8311");
  es8311_voice_volume_set(codecHandle, 85, NULL);
  if (!es7210_mic_init()) fatal("ES7210");
  if (!micAutoProbe()) errUntil = millis() + 8000;

  droneSave   = (int16_t *)ps_malloc((size_t)DRONE_S * 4);
  droneResult = (int16_t *)ps_malloc((size_t)DRONE_S * 4);
  if (droneSave && droneResult) {
    buildDrone(droneSave, 110.0f);                 // A2 — Notiz gespeichert
    buildDrone(droneResult, 146.83f);              // D3 — Ergebnis da
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("TOPIKO-Handheld");
  // Modem-Sleep statt Dauervollbetrieb: senkt den Idle-Strom deutlich; bei
  // 12-s-Sync-Takt spielt die minimal hoehere Latenz keine Rolle.
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  // Sendeleistung begrenzen -> kleinere Stromspitzen beim TX. Genau diese
  // Spitzen lassen die Akkuspannung einbrechen und loesen im reinen Akkubetrieb
  // den Brownout-Reset aus (an USB liefert VBUS die Spitze mit, daher dort stabil).
  WiFi.setTxPower(WIFI_POWER_13dBm);
  wifiMulti.addAP(AP_SSID, AP_PASS);      // TOPIKO-Hotspot des Docks
  wifiMulti.addAP(WIFI_SSID, WIFI_PASS);  // Heim-WLAN
  wifiMulti.run();

  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(syncTask,  "sync", 12288, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(droneTask, "drone", 4096, nullptr, 3, nullptr, 0);
  delay(300);
  dronePending = droneResult;                      // Boot-Ton = Lautsprecher-Test
  Serial.println("[TOPIKO] bereit");
}

void loop() {
  static uint32_t t0 = millis();
  float tSec = (millis() - t0) / 1000.0f;

  // --- Push-to-talk ---
  // Touch-Entprellung: Pegel muss stabil anliegen, Stoerimpulse werden ignoriert
  static bool     touchStable = false;   // entprellter Zustand
  static bool     rawLast     = false;
  static uint32_t rawSince    = 0;
  bool rawNow = (digitalRead(TOUCH_PIN) == HIGH);
  if (rawNow != rawLast) { rawLast = rawNow; rawSince = millis(); }
  if (rawNow  && !touchStable && millis() - rawSince >= 100) touchStable = true;   // 100 ms stabil HIGH
  if (!rawNow &&  touchStable && millis() - rawSince >= 200) touchStable = false;  // 200 ms stabil LOW
  bool pressed = (digitalRead(BTN_PIN) == LOW) || touchStable;
  static bool wasPressed = false;

  if (pressed && !wasPressed) {
    if (state == ST_IDLE || state == ST_RESULT) {
      recSamples = 0;
      recPeak = 0.0f;
      recActive = true;
      state = ST_RECORDING;
      Serial.println("[REC] Start");
    }
  }
  if (!pressed && wasPressed && state == ST_RECORDING) {
    recActive = false;
    while (!recDone) delay(5);
    float dur = (float)recSamples / SAMPLE_RATE;
    Serial.printf("[REC] Stop (%.2fs)\n", dur);
    // Ausschalten passiert NICHT mehr ueber langes Halten (verwarf faelschlich
    // echte Aufnahmen), sondern nur noch ueber den 5-min-Inaktivitaetstimer.
    if (dur < MIN_REC_S || recPeak < 0.12f) {
      // Versehen oder keine Sprache: still verwerfen
      Serial.printf("[REC] verworfen (%.2fs, Peak %.2f)\n", dur, recPeak);
      state = ST_IDLE;
    } else {
      if (saveRecording()) { dronePending = droneSave; savedUntil = millis() + 1500; }
      else errUntil = millis() + 2800;
      state = ST_IDLE;   // sofort wieder bereit, Verarbeitung laeuft asynchron am Dock
    }
  }
  if (state == ST_RECORDING && micLevel > recPeak) recPeak = micLevel;
  wasPressed = pressed;

  // Auto-Sleep: 5 min Inaktivitaet auf Akku (kein USB) -> Deep Sleep, Touch weckt.
  // (Manuelles Ausschalten per langem Halten wurde entfernt.)
  static uint32_t lastActivity = millis();
  if (pressed || state != ST_IDLE || queueCount > 0 ||
      millis() < savedUntil || millis() < resUntil) lastActivity = millis();
  // Beim Abziehen des USB-Kabels die Leerlaufuhr neu starten, damit das Geraet
  // nicht sofort schlaeft, wenn es vorher schon laenger am Dock geruht hat.
  static bool prevVbus = true;
  bool vbusNow = power.isVbusIn();
  if (prevVbus && !vbusNow) lastActivity = millis();
  prevVbus = vbusNow;
  if (millis() - lastActivity > 300000 && !vbusNow) powerOff();

  // Zwischenstufe vor dem Sleep: auf Akku nach 60 s Ruhe dimmen. Der AMOLED-Strom
  // skaliert stark mit der Helligkeit; bei Beruehrung sofort wieder volle Helligkeit.
  static uint8_t curBright = 255;
  uint8_t wantBright = 255;
  if (pmuOk && state == ST_IDLE && !power.isVbusIn() &&
      millis() - lastActivity > 60000) wantBright = 90;
  if (wantBright != curBright) { gfx->setBrightness(wantBright); curBright = wantBright; }

  if (state == ST_RESULT && millis() > resUntil) state = ST_IDLE;

  // --- Zielstil bestimmen, weich ueberblenden, rendern ---
  BlobStyle *target = &S_IDLE;
  if (state == ST_RECORDING) target = &S_REC;
  else if (millis() < errUntil) target = &S_ERR;
  else if (millis() < savedUntil) target = &S_SAVED;  // kurze Bestaetigung
  else if (state == ST_RESULT) target = &S_RESULT;
  else if (batteryPct >= 0 && batteryPct < 10) target = &S_LOW10;  // <10% Dunkelrot
  else if (batteryPct >= 0 && batteryPct < 20) target = &S_LOW20;  // <20% leichtes Rot

  // Aufloesungszyklus nur im ruhigen Idle; bei Interaktion sofort scharf
  if (target == &S_IDLE || target == &S_LOW20 || target == &S_LOW10)
    dissolve += (dissolveTarget(tSec) - dissolve) * 0.06f;
  else dissolve *= 0.80f;

  blendStyle(*target, 0.10f);

  renderHeart(tSec);
  flushUpscaled();

  static uint32_t frames = 0, lastFps = millis(), lastBat = 0;
  frames++;
  if (millis() - lastFps > 5000) {
    Serial.printf("[UI] %.1f fps, Level %.2f, Heap %u, PSRAM %u\n", frames / 5.0f,
                  micLevel, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    frames = 0; lastFps = millis();
  }
  if (millis() - lastBat > 30000 && state == ST_IDLE) {
    lastBat = millis();
    if (!pmuOk) pmuInit();     // AXP2101 erneut versuchen, falls beim Boot nicht erkannt
    logBattery();
  }
}
