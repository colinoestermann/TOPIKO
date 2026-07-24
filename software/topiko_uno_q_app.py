#!/usr/bin/env python3
"""TOPIKO — Arduino UNO Q App (arecord + whisper-cli + llama-cpp-python)

Unterschiede zur Pi3-Version:
  - Aufnahme: arecord (ALSA, server-seitig) statt Browser-MediaRecorder
  - Transkription: whisper-cli (Binary) statt faster-whisper (Python)
  - LLM: identisch (llama-cpp-python)
  - UI: Record-Button triggert Server, kein client-seitiges Audio nötig
"""

import os, json, time, subprocess, tempfile, threading, uuid, datetime
from flask import Flask, jsonify, render_template_string, request

# ── Config ────────────────────────────────────────────────────────────────────
GGUF_PATH      = os.path.expanduser("~/models/LFM2.5-350M-TOPIKO-Q4_K_M.gguf")
WHISPER_BIN    = os.path.expanduser("~/whisper.cpp/build/bin/whisper-cli")
WHISPER_MODEL  = os.path.expanduser("~/whisper.cpp/models/ggml-small-q4_0.bin")
ALSA_DEVICE    = "plughw:CARD=ArduinoImolaHPH,DEV=2"
REC_DURATION   = 5          # Sekunden Aufnahme
SAMPLE_RATE    = 16000

# ── Notiz-Speicher (Queue + Ergebnisse, persistiert) ─────────────────────────
DATA_DIR   = os.path.expanduser("~/topiko_data")
AUDIO_DIR  = os.path.join(DATA_DIR, "audio")
NOTES_FILE = os.path.join(DATA_DIR, "notes.json")
os.makedirs(AUDIO_DIR, exist_ok=True)

_notes_lock  = threading.Lock()
_work_event  = threading.Event()
notes: list = []          # [{id, received, source, status, transcript, result, ...}]

def _load_notes():
    global notes
    try:
        with open(NOTES_FILE) as f:
            notes = json.load(f)
        # Abgebrochene Verarbeitung nach Neustart wieder einreihen
        for n in notes:
            if n.get("status") == "processing":
                n["status"] = "pending"
    except FileNotFoundError:
        notes = []
    except Exception as e:
        print(f"[Store] notes.json defekt ({e}) — starte leer")
        notes = []

def _save_notes():
    tmp = NOTES_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(notes, f, ensure_ascii=False, indent=1)
    os.replace(tmp, NOTES_FILE)

def _find_note(nid: str):
    for n in notes:
        if n["id"] == nid:
            return n
    return None

# ── STM32 Monitor-Port (arduino-router TCP-Proxy) ────────────────────────────
STM32_HOST     = "127.0.0.1"
STM32_PORT     = 7500             # arduino-router --monitor-port (raw serial proxy)
N_THREADS      = 2
N_CTX          = 1024

SYSTEM_PROMPT = """Du bist TOPIKO, ein lokaler Personal Assistant auf einem Arduino UNO Q.
Du empfängst transkribierten Spracheingang und wandelst ihn in strukturiertes JSON um.

Antworte IMMER nur mit einem einzigen validen JSON-Objekt. Kein erklärender Text.

Kategorien und Felder:
  termin:     {type, title, date_raw, time, person, location, notes}
  todo:       {type, title, date_raw, priority, notes}
  erinnerung: {type, title, date_raw, priority}
  idee:       {type, title, content, project}
  tagebuch:   {type, title, content}
  unbekannt:  {type, notes}

Regeln:
- Felder ohne erkennbaren Wert: null
- date_raw: Datum/Zeit GENAU wie gesprochen
- priority: "hoch", "normal" oder "niedrig"
- title: kurz, max 6 Woerter
- Passt nichts: type "unbekannt"

Beispiele:
Input: Ich habe naechsten Montag um 9 Uhr einen Termin beim Arzt.
Output: {"type": "termin", "title": "Arzttermin", "date_raw": "naechsten Montag", "time": "09:00", "person": null, "location": null, "notes": null}

Input: Erinnere mich morgen daran, die Apotheke anzurufen.
Output: {"type": "erinnerung", "title": "Apotheke anrufen", "date_raw": "morgen", "priority": "normal"}

Input: Wie heisst die Hauptstadt von Frankreich?
Output: {"type": "unbekannt", "notes": null}"""

# ── App ───────────────────────────────────────────────────────────────────────
app = Flask(__name__)
llm  = None
_recording = False   # Einfaches Lock: verhindert parallele Aufnahmen

# ── STM32 Monitor-Port Interface (arduino-router TCP-Proxy) ──────────────────
import socket as _socket_module
_stm32_sock = None
_stm32_lock = threading.Lock()

def display_send(msg: str):
    """Schickt eine Zeile an den STM32 über den Monitor-Port."""
    global _stm32_sock
    with _stm32_lock:
        if _stm32_sock:
            try:
                _stm32_sock.sendall((msg + "\n").encode())
            except Exception as e:
                print(f"[Display] Send Fehler: {e}")
                _stm32_sock = None

def display_result(result_json: str):
    """Schickt JSON-Ergebnis an den STM32 zur Anzeige."""
    display_send(result_json)

def _connect_stm32():
    """Verbindet zum arduino-router Monitor-Port. Gibt socket zurück oder None."""
    try:
        s = _socket_module.socket(_socket_module.AF_INET, _socket_module.SOCK_STREAM)
        s.settimeout(5)
        s.connect((STM32_HOST, STM32_PORT))
        s.settimeout(None)  # blockierend für readline
        print(f"[Display] STM32 Monitor-Port verbunden: {STM32_HOST}:{STM32_PORT}")
        return s
    except Exception as e:
        print(f"[Display] Monitor-Port nicht erreichbar ({e})")
        return None

def stm32_listener():
    """Background-Thread: wartet auf BTN vom STM32 über TCP Monitor-Port."""
    global _stm32_sock, _recording
    _stm32_sock = _connect_stm32()
    if not _stm32_sock:
        return

    buf = b""
    while True:
        try:
            chunk = _stm32_sock.recv(256)
            if not chunk:
                raise ConnectionError("Verbindung getrennt")
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="ignore").strip()
                if line == "BTN" and not _recording:
                    print("[Display] BTN empfangen → starte Aufnahme")
                    threading.Thread(target=run_pipeline_for_display, daemon=True).start()
        except Exception as e:
            print(f"[Display] Verbindungsfehler: {e} — versuche Reconnect in 5s")
            _stm32_sock = None
            time.sleep(5)
            _stm32_sock = _connect_stm32()
            if not _stm32_sock:
                return
            buf = b""

def run_pipeline_for_display():
    """Vollständige Aufnahme+Transkription+Klassifikation, Ergebnis ans Display."""
    global _recording
    if _recording:
        return
    _recording = True
    wav = None
    try:
        display_send("REC")
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            wav = f.name
        record_audio(wav)
        display_send("PROC")
        transcript, _ = transcribe_audio(wav)
        if not transcript:
            display_send('{"type":"unbekannt","title":"kein Audio"}')
            return
        result, _ = classify_text(transcript)
        display_result(result)
        print(f"[Display] Ergebnis gesendet: {result}")
    except Exception as e:
        print(f"[Display] Pipeline Fehler: {e}")
    finally:
        _recording = False
        if wav and os.path.exists(wav):
            try: os.unlink(wav)
            except: pass

def load_llm():
    global llm
    if not os.path.exists(GGUF_PATH):
        print(f"[TOPIKO] GGUF nicht gefunden: {GGUF_PATH}")
        return
    from llama_cpp import Llama
    print("[TOPIKO] Lade LFM2.5-350M ...")
    t = time.time()
    llm = Llama(model_path=GGUF_PATH, n_ctx=N_CTX, n_threads=N_THREADS,
                n_gpu_layers=0, verbose=False)
    print(f"[TOPIKO] LFM geladen ({time.time()-t:.1f}s)")

def record_audio(path: str, duration: int = REC_DURATION) -> float:
    """Nimmt Audio mit arecord auf. Gibt Dauer zurück."""
    t = time.time()
    cmd = [
        "arecord",
        "-D", ALSA_DEVICE,
        "-d", str(duration),
        "-r", str(SAMPLE_RATE),
        "-c", "1",
        "-f", "S16_LE",
        path
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=duration + 5)
    if result.returncode != 0:
        raise RuntimeError(f"arecord Fehler: {result.stderr.decode()[:300]}")
    return round(time.time() - t, 2)

def transcribe_audio(wav_path: str) -> tuple[str, float]:
    """Transkribiert WAV mit whisper-cli. Gibt (Text, Dauer) zurück."""
    if not os.path.exists(WHISPER_BIN):
        raise RuntimeError(f"whisper-cli nicht gefunden: {WHISPER_BIN}")
    if not os.path.exists(WHISPER_MODEL):
        raise RuntimeError(f"Whisper-Modell nicht gefunden: {WHISPER_MODEL}")

    txt_path = wav_path.replace(".wav", "")  # whisper-cli erzeugt .txt automatisch

    cmd = [
        WHISPER_BIN,
        "-m", WHISPER_MODEL,
        "-f", wav_path,
        "-l", "de",
        "--output-txt",
        "-of", txt_path,
        "--no-prints",
    ]
    t = time.time()
    result = subprocess.run(cmd, capture_output=True, timeout=120)
    dur = round(time.time() - t, 2)

    if result.returncode != 0:
        raise RuntimeError(f"whisper-cli Fehler: {result.stderr.decode()[:300]}")

    out_file = txt_path + ".txt"
    if os.path.exists(out_file):
        with open(out_file) as f:
            text = f.read().strip()
        os.unlink(out_file)
    else:
        # Fallback: stdout parsen
        text = result.stdout.decode().strip()

    return text, dur

def classify_text(text: str) -> tuple[str, float]:
    """Klassifiziert Text mit LFM2.5-350M. Gibt (JSON-String, Dauer) zurück."""
    if llm is None:
        return '{"error": "Modell nicht geladen"}', 0.0
    prompt = (
        f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
        f"<|im_start|>user\n{text}<|im_end|>\n"
        f"<|im_start|>assistant\n"
    )
    t = time.time()
    resp = llm(prompt, max_tokens=200, temperature=0.05, top_p=0.9,
               repeat_penalty=1.05, stop=["<|im_end|>", "<|im_start|>"])
    return resp["choices"][0]["text"].strip(), round(time.time() - t, 2)

# ── HTML ──────────────────────────────────────────────────────────────────────
HTML = """<!DOCTYPE html>
<html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TOPIKO</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
     background:#0f0f0f;color:#e0e0e0;min-height:100vh;
     display:flex;flex-direction:column;align-items:center;padding:40px 20px}
h1{font-size:2rem;font-weight:300;letter-spacing:.2em;color:#fff;margin-bottom:8px}
.sub{color:#555;font-size:.8rem;margin-bottom:40px;letter-spacing:.1em}
.status{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;
        padding:10px 16px;margin-bottom:32px;font-size:.8rem;width:100%;max-width:600px;
        display:flex;gap:24px}
.ok{color:#88ee88}.warn{color:#ff8888}.info{color:#88aaff}
.rec-btn{width:90px;height:90px;border-radius:50%;border:3px solid #444;
         background:#1a1a1a;cursor:pointer;font-size:1.8rem;transition:all .2s;
         display:flex;align-items:center;justify-content:center;margin-bottom:12px}
.rec-btn:hover:not(:disabled){border-color:#888}
.rec-btn:disabled{opacity:.4;cursor:not-allowed}
.rec-btn.recording{border-color:#ff4444;background:#200000;animation:pulse 1s infinite}
.rec-btn.processing{border-color:#4488ff;animation:pulse2 1.5s infinite}
@keyframes pulse{0%,100%{box-shadow:0 0 0 0 rgba(255,68,68,.4)}
                 50%{box-shadow:0 0 0 10px rgba(255,68,68,0)}}
@keyframes pulse2{0%,100%{box-shadow:0 0 0 0 rgba(68,136,255,.4)}
                  50%{box-shadow:0 0 0 10px rgba(68,136,255,0)}}
.hint{color:#444;font-size:.75rem;margin-bottom:40px;height:16px}
.card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:10px;
      padding:18px;width:100%;max-width:600px;margin-bottom:14px}
.lbl{font-size:.65rem;letter-spacing:.15em;color:#444;text-transform:uppercase;margin-bottom:8px}
.txt{font-size:.9rem;color:#ccc;min-height:24px;white-space:pre-wrap;word-break:break-word}
.mono{font-family:'SF Mono','Fira Code',monospace;font-size:.82rem;color:#88ccff}
.t{font-size:.72rem;color:#444;margin-top:6px}
.t b{color:#666}.dim{color:#444;font-style:italic}.err{color:#ff6666}
</style></head><body>
<h1>TOPIKO</h1>
<p class="sub">OFFLINE &middot; ARDUINO UNO Q &middot; TEST</p>
<div class="status">
  <span>LFM: <span class="{{ 'ok' if model_loaded else 'warn' }}">
    {{ '&#x2705; geladen' if model_loaded else '&#x26A0; fehlt' }}</span></span>
  <span>Whisper: <span class="{{ 'ok' if whisper_ok else 'warn' }}">
    {{ '&#x2705; bereit' if whisper_ok else '&#x26A0; fehlt' }}</span></span>
  <span>Mic: <span class="{{ 'ok' if mic_ok else 'warn' }}">
    {{ '&#x2705; ' + alsa_device if mic_ok else '&#x26A0; nicht gefunden' }}</span></span>
</div>
<button class="rec-btn" id="btn" onclick="record()" {{ 'disabled' if not (model_loaded and whisper_ok and mic_ok) }}>&#127897;</button>
<p class="hint" id="hint">Klicken zum Aufnehmen &middot; {{ rec_duration }}s Aufnahme</p>
<div class="card">
  <div class="lbl">Transkription (Whisper Small &middot; DE)</div>
  <div class="txt dim" id="tr">Noch keine Aufnahme</div>
  <div class="t">Whisper: <b id="wt">&mdash;</b></div>
</div>
<div class="card">
  <div class="lbl">Klassifikation (LFM2.5-350M)</div>
  <div class="txt mono dim" id="res">&mdash;</div>
  <div class="t">LFM: <b id="lt">&mdash;</b> &nbsp; Gesamt: <b id="tot">&mdash;</b></div>
</div>
<script>
const REC_DURATION = {{ rec_duration }};
let busy = false;

async function record() {
  if (busy) return;
  busy = true;
  const btn = document.getElementById('btn');
  const hint = document.getElementById('hint');

  // Countdown während Aufnahme
  btn.classList.add('recording');
  btn.innerHTML = '&#9209;';
  btn.disabled = true;
  let remaining = REC_DURATION;
  hint.textContent = `Aufnahme läuft ... ${remaining}s`;
  const timer = setInterval(() => {
    remaining--;
    hint.textContent = remaining > 0
      ? `Aufnahme läuft ... ${remaining}s`
      : 'Verarbeite ...';
    if (remaining <= 0) clearInterval(timer);
  }, 1000);

  set('tr', 'txt dim', 'Aufnahme ...');
  set('res', 'mono dim', '&mdash;');
  document.getElementById('wt').textContent = '...';
  document.getElementById('lt').textContent = '...';

  try {
    const r = await fetch('/record', {method: 'POST'});
    const d = await r.json();
    clearInterval(timer);

    if (d.error) {
      set('tr', 'err', 'Fehler: ' + d.error);
    } else {
      set('tr', 'txt', d.transcript || '(leer)');
      document.getElementById('wt').textContent = d.whisper_time + 's';
      try {
        set('res', 'mono', JSON.stringify(JSON.parse(d.result), null, 2));
      } catch {
        set('res', 'mono', d.result);
      }
      document.getElementById('lt').textContent = d.llm_time + 's';
      document.getElementById('tot').textContent =
        ((d.whisper_time || 0) + (d.llm_time || 0)).toFixed(2) + 's';
    }
  } catch(e) {
    set('tr', 'err', 'Fehler: ' + e.message);
  }

  btn.classList.remove('recording', 'processing');
  btn.innerHTML = '&#127897;';
  btn.disabled = false;
  hint.textContent = `Klicken zum Aufnehmen · ${REC_DURATION}s Aufnahme`;
  busy = false;
}

function set(id, cls, html) {
  const el = document.getElementById(id);
  el.className = 'txt ' + cls;
  el.innerHTML = html;
}
</script></body></html>"""

# ── Verarbeitungs-Worker (arbeitet die Upload-Queue sequenziell ab) ──────────
def _process_note(n: dict):
    global _recording
    wav = os.path.join(AUDIO_DIR, n["id"] + ".wav")
    _recording = True
    try:
        transcript, whisper_time = transcribe_audio(wav)
        if not transcript:
            n.update(status="done", transcript="",
                     result={"type": "unbekannt", "notes": "kein Audio erkannt"},
                     whisper_time=whisper_time, llm_time=0.0)
            return
        raw, llm_time = classify_text(transcript)
        try:
            parsed = json.loads(raw)
        except Exception:
            parsed = {"type": "unbekannt", "notes": "ungueltiges JSON", "raw": raw[:200]}
        n.update(status="done", transcript=transcript, result=parsed,
                 whisper_time=whisper_time, llm_time=llm_time)
        display_result(json.dumps(parsed, ensure_ascii=False))
    except Exception as e:
        n.update(status="error", error=str(e)[:300])
    finally:
        _recording = False
        n["done_at"] = datetime.datetime.now().isoformat(timespec="seconds")

def queue_worker():
    while True:
        _work_event.wait(timeout=10)
        _work_event.clear()
        while True:
            with _notes_lock:
                job = next((n for n in notes if n["status"] == "pending"), None)
                if job:
                    job["status"] = "processing"
                    _save_notes()
            if not job:
                break
            if _recording:            # /record oder /classify laeuft gerade
                time.sleep(2)
                with _notes_lock:
                    job["status"] = "pending"
                continue
            print(f"[Queue] Verarbeite {job['id']} ...")
            _process_note(job)
            with _notes_lock:
                _save_notes()
            print(f"[Queue] {job['id']} -> {job['status']}")

# ── Hilfsfunktionen ───────────────────────────────────────────────────────────
def check_mic() -> bool:
    """Prüft ob das ALSA-Device verfügbar ist."""
    result = subprocess.run(
        ["arecord", "-l"],
        capture_output=True, timeout=5
    )
    return "ArduinoImolaHPH" in result.stdout.decode()

# ── Routes ────────────────────────────────────────────────────────────────────
@app.route("/")
def index():
    return render_template_string(HTML,
        model_loaded=(llm is not None),
        whisper_ok=(os.path.exists(WHISPER_BIN) and os.path.exists(WHISPER_MODEL)),
        mic_ok=check_mic(),
        alsa_device=ALSA_DEVICE.split("=")[-1],
        rec_duration=REC_DURATION,
    )

@app.route("/record", methods=["POST"])
def record():
    global _recording
    if _recording:
        return jsonify({"error": "Aufnahme läuft bereits"}), 429

    _recording = True
    wav = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            wav = f.name

        # 1. Aufnahme
        record_audio(wav)

        # 2. Transkription
        transcript, whisper_time = transcribe_audio(wav)
        if not transcript:
            return jsonify({"transcript": "", "whisper_time": whisper_time,
                            "result": '{"type": "unbekannt", "notes": "kein Audio erkannt"}',
                            "llm_time": 0.0})

        # 3. Klassifikation
        result, llm_time = classify_text(transcript)

        return jsonify({
            "transcript": transcript,
            "whisper_time": whisper_time,
            "result": result,
            "llm_time": llm_time,
        })

    except Exception as e:
        return jsonify({"error": str(e)}), 500
    finally:
        _recording = False
        if wav and os.path.exists(wav):
            try: os.unlink(wav)
            except: pass

@app.route("/classify", methods=["POST"])
def classify():
    """Empfängt eine WAV-Datei (multipart: field 'audio'), transkribiert und klassifiziert.

    Erwartet: 16 kHz, mono, 16-bit PCM WAV.
    Gibt zurück: {"transcript":..., "result":"{...}", "whisper_time":.., "llm_time":..}
    """
    global _recording
    if _recording:
        return jsonify({"error": "Aufnahme/Verarbeitung läuft bereits"}), 429

    if "audio" not in request.files:
        return jsonify({"error": "Kein Audio-File im Feld 'audio'"}), 400

    upload = request.files["audio"]
    if not upload.filename:
        return jsonify({"error": "Leerer Dateiname"}), 400

    _recording = True
    wav = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            upload.save(f)
            wav = f.name

        # Transkription
        transcript, whisper_time = transcribe_audio(wav)
        if not transcript:
            return jsonify({
                "transcript": "",
                "whisper_time": whisper_time,
                "result": '{"type": "unbekannt", "notes": "kein Audio erkannt"}',
                "llm_time": 0.0,
            })

        # Klassifikation
        result, llm_time = classify_text(transcript)

        # Ergebnis ans STM32-Display senden (falls verbunden)
        display_result(result)

        return jsonify({
            "transcript": transcript,
            "whisper_time": whisper_time,
            "result": result,
            "llm_time": llm_time,
        })

    except Exception as e:
        return jsonify({"error": str(e)}), 500
    finally:
        _recording = False
        if wav and os.path.exists(wav):
            try:
                os.unlink(wav)
            except:
                pass


@app.route("/upload", methods=["POST"])
def upload():
    """Nimmt eine WAV vom Handheld entgegen und reiht sie in die Queue ein.

    Multipart: Feld 'audio' = WAV (16 kHz mono 16-bit).
    Form-Felder (optional): 'id' (Idempotenz), 'duration' (s), 'uptime_ms'.
    Antwort sofort (keine Verarbeitung inline): {"id":..., "status":...}
    """
    if "audio" not in request.files:
        return jsonify({"error": "Kein Audio-File im Feld 'audio'"}), 400
    nid = (request.form.get("id") or uuid.uuid4().hex[:12]).strip()
    if not nid.replace("-", "").replace("_", "").isalnum() or len(nid) > 40:
        return jsonify({"error": "Ungueltige id"}), 400

    with _notes_lock:
        existing = _find_note(nid)
        if existing:   # Idempotent: Re-Upload nach verpasster Antwort
            return jsonify({"id": nid, "status": existing["status"], "dup": True})

    path = os.path.join(AUDIO_DIR, nid + ".wav")
    request.files["audio"].save(path)
    if os.path.getsize(path) < 1000:
        os.unlink(path)
        return jsonify({"error": "Audio leer"}), 400

    note = {
        "id": nid,
        "received": datetime.datetime.now().isoformat(timespec="seconds"),
        "source": "handheld",
        "duration": float(request.form.get("duration", 0) or 0),
        "uptime_ms": int(request.form.get("uptime_ms", 0) or 0),
        "status": "pending",
        "transcript": None,
        "result": None,
        "synced": False,
    }
    with _notes_lock:
        notes.append(note)
        pending = sum(1 for n in notes if n["status"] in ("pending", "processing"))
        _save_notes()
    _work_event.set()
    print(f"[Upload] {nid} angenommen ({note['duration']}s, Queue: {pending})")
    return jsonify({"id": nid, "status": "pending", "queue": pending})

@app.route("/api/results")
def api_results():
    """Fertige, noch nicht ans Handheld gespielte Ergebnisse (aelteste zuerst)."""
    limit = int(request.args.get("limit", 5))
    with _notes_lock:
        out = [{
            "id": n["id"],
            "type": (n.get("result") or {}).get("type", "unbekannt"),
            "title": (n.get("result") or {}).get("title") or "",
            "transcript": (n.get("transcript") or "")[:120],
        } for n in notes
          if n["source"] == "handheld" and n["status"] == "done" and not n.get("synced")
        ][:limit]
        pending = sum(1 for n in notes if n["status"] in ("pending", "processing"))
    return jsonify({"results": out, "pending": pending})

@app.route("/api/ack", methods=["POST"])
def api_ack():
    """Handheld bestaetigt gespeicherte Ergebnisse: {"ids": [...]}"""
    ids = (request.get_json(silent=True) or {}).get("ids", [])
    with _notes_lock:
        for n in notes:
            if n["id"] in ids:
                n["synced"] = True
        _save_notes()
    return jsonify({"acked": len(ids)})

@app.route("/api/notes")
def api_notes():
    """Vollstaendige Notizliste fuer das Web-Frontend (neueste zuerst)."""
    with _notes_lock:
        return jsonify(sorted(notes, key=lambda n: n.get("received", ""), reverse=True))

@app.route("/health")
def health():
    with _notes_lock:
        pending = sum(1 for n in notes if n["status"] in ("pending", "processing"))
    return jsonify({
        "llm": llm is not None,
        "whisper_bin": os.path.exists(WHISPER_BIN),
        "whisper_model": os.path.exists(WHISPER_MODEL),
        "mic": check_mic(),
        "queue_pending": pending,
        "notes_total": len(notes),
    })

# ── Start ─────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print("[TOPIKO] Starte UNO Q App ...")
    _load_notes()
    print(f"[TOPIKO] {len(notes)} Notizen geladen, "
          f"{sum(1 for n in notes if n['status'] == 'pending')} in der Queue")
    load_llm()
    # Queue-Worker fuer Handheld-Uploads
    threading.Thread(target=queue_worker, daemon=True).start()
    _work_event.set()
    # STM32 Serial Listener starten (Display + Button)
    threading.Thread(target=stm32_listener, daemon=True).start()
    ip = subprocess.run(["hostname", "-I"], capture_output=True).stdout.decode().split()[0]
    print(f"[TOPIKO] http://{ip}:5000")
    app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)
