# -*- coding: utf-8 -*-
"""
TOPIKO — Mobile Web-Frontend + Captive Portal
Wird von topiko_uno_q_app.py per register_mobile(...) eingebunden.
Routes:
  /m                      Mobile UI (Notizen, Audio, Edit, Todo, .ics)
  /audio/<id>.wav         Original-Aufnahme
  /api/note/<id>/update   POST {title, content, type, date_raw, time}
  /api/note/<id>/toggle   POST — done-Flag umschalten (todo/erinnerung)
  /api/note/<id>/delete   POST — Notiz + WAV löschen
  /ics/<id>.ics           Einzeltermin als iCalendar
  /calendar.ics           Alle Termine (auch als webcal:// abonnierbar)
  Captive-Portal-Probes (Apple/Android/Windows) → Redirect auf /m
"""
import os, re, datetime as dt
from flask import jsonify, request, redirect, send_from_directory, Response, render_template_string

WEEKDAYS = {"montag":0,"dienstag":1,"mittwoch":2,"donnerstag":3,"freitag":4,"samstag":5,"sonntag":6}

def _parse_when(date_raw, time_str, received_iso):
    """Deutsches date_raw ('morgen','Samstag','12.07.') relativ zum Empfangsdatum -> datetime."""
    try:
        base = dt.datetime.fromisoformat(received_iso).date()
    except Exception:
        base = dt.date.today()
    d = base + dt.timedelta(days=1)  # Fallback: naechster Tag
    if date_raw:
        s = date_raw.strip().lower()
        if "heute" in s: d = base
        elif "übermorgen" in s or "uebermorgen" in s: d = base + dt.timedelta(days=2)
        elif "morgen" in s: d = base + dt.timedelta(days=1)
        else:
            m = re.search(r"(\d{1,2})\.(\d{1,2})\.?(\d{2,4})?", s)
            if m:
                day, mon = int(m.group(1)), int(m.group(2))
                yr = int(m.group(3)) if m.group(3) else base.year
                if yr < 100: yr += 2000
                try:
                    d = dt.date(yr, mon, day)
                    if not m.group(3) and d < base: d = dt.date(yr + 1, mon, day)
                except ValueError: pass
            else:
                for name, wd in WEEKDAYS.items():
                    if name in s:
                        delta = (wd - base.weekday()) % 7 or 7
                        d = base + dt.timedelta(days=delta)
                        if "nächste woche" in s or "naechste woche" in s:
                            # "naechste Woche X": X in der Folgewoche (Mo-So nach dieser Woche)
                            week_start_next = base + dt.timedelta(days=7 - base.weekday())
                            cand = week_start_next + dt.timedelta(days=wd)
                            d = cand
                        break
    hh, mm = 9, 0
    if time_str:
        m = re.search(r"(\d{1,2})[:.h]?(\d{2})?", str(time_str))
        if m:
            hh = int(m.group(1)); mm = int(m.group(2) or 0)
    return dt.datetime(d.year, d.month, d.day, hh, mm)

def _ics_escape(s):
    return str(s or "").replace("\\", "\\\\").replace(";", "\\;").replace(",", "\\,").replace("\n", "\\n")

def _event_ics(n):
    r = n.get("result") or {}
    start = _parse_when(r.get("date_raw"), r.get("time"), n.get("received", ""))
    end = start + dt.timedelta(hours=1)
    desc = n.get("transcript") or ""
    extra = [f"{k}: {r[k]}" for k in ("person", "notes") if r.get(k)]
    if extra: desc += "\n" + "\n".join(extra)
    fmt = "%Y%m%dT%H%M%S"
    return (
        "BEGIN:VEVENT\r\n"
        f"UID:{n['id']}@topiko.local\r\n"
        f"DTSTAMP:{dt.datetime.now().strftime(fmt)}\r\n"
        f"DTSTART:{start.strftime(fmt)}\r\n"
        f"DTEND:{end.strftime(fmt)}\r\n"
        f"SUMMARY:{_ics_escape(r.get('title') or 'TOPIKO Termin')}\r\n"
        f"DESCRIPTION:{_ics_escape(desc)}\r\n"
        + (f"LOCATION:{_ics_escape(r.get('location'))}\r\n" if r.get("location") else "")
        + "END:VEVENT\r\n"
    )

def _ics_wrap(events):
    return ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
            "PRODID:-//TOPIKO//Offline Room//DE\r\nCALSCALE:GREGORIAN\r\n"
            "X-WR-CALNAME:TOPIKO\r\n"
            + "".join(events) + "END:VCALENDAR\r\n")


MOBILE_HTML = r"""<!doctype html>
<html lang="de"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>TOPIKO</title>
<style>
:root{
  --bg:#0a0a0c; --card:#16161a; --line:#26262c; --txt:#f2eee8; --dim:#8a8690;
  --termin:#5b8cff; --todo:#ff9440; --erinnerung:#b07cff; --idee:#ffd84d;
  --tagebuch:#5fd68a; --unbekannt:#777; --core:#fff3ea; --mid:#ff9440; --edge:#ff5c8a;
}
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:var(--bg);color:var(--txt);font:16px/1.45 -apple-system,'Helvetica Neue',sans-serif;
  padding-bottom:60px}
header{padding:26px 20px 10px;display:flex;align-items:center;gap:14px}
.orb{width:44px;height:44px;border-radius:50%;flex:none;
  background:radial-gradient(circle at 50% 42%,var(--core) 0%,var(--mid) 42%,var(--edge) 78%,transparent 100%);
  animation:breathe 5.5s ease-in-out infinite}
@keyframes breathe{0%,100%{transform:scale(1);opacity:1}50%{transform:scale(.86);opacity:.55;filter:blur(2px)}}
h1{font-size:22px;letter-spacing:.14em;font-weight:600}
.sub{color:var(--dim);font-size:12px;letter-spacing:.06em}
.hint{margin:0 14px 4px;background:#1c1720;border:1px solid #3a2f45;border-radius:12px;
  padding:11px 13px;font-size:13px;color:#cbb8e8;display:flex;gap:10px;align-items:flex-start}
.hint b{color:#e6d8ff}
.hint .x{margin-left:auto;color:var(--dim);font-size:17px;padding:0 4px;cursor:pointer}
nav{display:flex;gap:8px;overflow-x:auto;padding:14px 20px;scrollbar-width:none}
nav::-webkit-scrollbar{display:none}
.chip{border:1px solid var(--line);background:var(--card);color:var(--dim);border-radius:999px;
  padding:7px 15px;font-size:13px;white-space:nowrap;cursor:pointer}
.chip.on{color:#0a0a0c;font-weight:600}
main{padding:0 14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:15px 16px;margin-bottom:12px;
  border-left:4px solid var(--cat,#555)}
.card.done-task{opacity:.45}
.row{display:flex;align-items:flex-start;gap:11px}
.check{width:26px;height:26px;border-radius:8px;border:2px solid var(--cat,#555);flex:none;margin-top:1px;
  display:flex;align-items:center;justify-content:center;font-size:16px;color:#0a0a0c;cursor:pointer}
.check.c{background:var(--cat)}
.tt{font-size:17px;font-weight:600}
.meta{color:var(--dim);font-size:12px;margin-top:2px}
.badge{display:inline-block;font-size:11px;letter-spacing:.08em;text-transform:uppercase;
  color:var(--cat);margin-bottom:4px}
.tr{color:var(--dim);font-size:14px;margin-top:8px;font-style:italic}
.detail{margin-top:8px;font-size:14px;color:var(--txt)}
.acts{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
.btn{border:1px solid var(--line);background:transparent;color:var(--txt);border-radius:10px;
  padding:8px 13px;font-size:13px;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
.btn.primary{background:var(--cat);border-color:var(--cat);color:#0a0a0c;font-weight:600}
.btn.danger{color:#ff6b6b;border-color:#3a2020}
.subrow{display:flex;justify-content:center;margin:6px 0 2px}
audio{width:100%;height:38px;margin-top:10px;filter:invert(.92) hue-rotate(180deg)}
.empty{color:var(--dim);text-align:center;padding:60px 20px}
textarea,input[type=text],select{width:100%;background:#0f0f12;border:1px solid var(--line);border-radius:10px;
  color:var(--txt);padding:10px;font:inherit;font-size:15px;margin-top:8px;-webkit-appearance:none;appearance:none}
.half{display:flex;gap:8px}
dialog{background:var(--card);color:var(--txt);border:1px solid var(--line);border-radius:18px;
  padding:20px;width:min(92vw,420px)}
dialog::backdrop{background:rgba(0,0,0,.7)}
.toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);background:var(--txt);color:#0a0a0c;
  border-radius:12px;padding:10px 18px;font-size:14px;font-weight:600;opacity:0;transition:.3s;pointer-events:none;z-index:9}
.toast.show{opacity:1}
</style></head><body>
<header><div class="orb"></div><div><h1>TOPIKO</h1><div class="sub">OFFLINE NOTIZEN · <span id="cnt"></span></div></div></header>
<div class="hint" id="hint" style="display:none">
  <span>💡</span>
  <span><b>Kalender-Import geht nicht im Anmelde-Fenster.</b> Öffne dafür Safari und tippe
  <b id="hinturl"></b> ein — oder abonniere unten alle Termine per webcal.</span>
  <span class="x" onclick="hideHint()">×</span>
</div>
<nav id="nav"></nav>
<main id="list"></main>
<div class="subrow"><a class="btn" id="webcal" style="--cat:var(--termin)">📅 Alle Termine im Kalender abonnieren</a></div>
<div class="toast" id="toast"></div>
<dialog id="dlg">
  <div style="font-weight:600;margin-bottom:4px">Notiz bearbeiten</div>
  <select id="e_type">
    <option value="termin">Termin</option><option value="todo">Todo</option>
    <option value="erinnerung">Erinnerung</option><option value="idee">Idee</option>
    <option value="tagebuch">Tagebuch</option><option value="unbekannt">Unbekannt</option>
  </select>
  <input type="text" id="e_title" placeholder="Titel">
  <div class="half" id="e_when" style="display:none">
    <input type="text" id="e_date" placeholder="Datum (z. B. 12.07. / morgen)">
    <input type="text" id="e_time" placeholder="Uhrzeit (z. B. 15:00)">
  </div>
  <textarea id="e_content" rows="4" placeholder="Inhalt"></textarea>
  <div class="acts" style="justify-content:flex-end">
    <button class="btn" onclick="dlg.close()">Abbrechen</button>
    <button class="btn primary" style="--cat:var(--todo)" onclick="saveEdit()">Speichern</button>
  </div>
</dialog>
<script>
const CATS={termin:"Termine",todo:"Todos",erinnerung:"Erinnerungen",idee:"Ideen",tagebuch:"Tagebuch"};
let notes=[],filter="alle",editId=null;
const $=s=>document.querySelector(s), dlg=document.getElementById("dlg");
document.getElementById("webcal").href="webcal://"+location.hostname+"/calendar.ics";
if(!localStorage.getItem("hideHint")){
  document.getElementById("hint").style.display="flex";
  document.getElementById("hinturl").textContent=location.hostname+":5000/m";
}
function hideHint(){localStorage.setItem("hideHint","1");document.getElementById("hint").style.display="none"}
function toast(m){const t=document.getElementById("toast");t.textContent=m;t.classList.add("show");
  setTimeout(()=>t.classList.remove("show"),1800)}
async function load(){
  notes=await (await fetch("/api/notes")).json();
  notes=notes.filter(n=>n.status==="done");
  render();
}
function nav(){
  const counts={alle:notes.length};
  notes.forEach(n=>{const t=(n.result||{}).type||"unbekannt";counts[t]=(counts[t]||0)+1});
  let h=`<div class="chip ${filter==='alle'?'on':''}" style="${filter==='alle'?'background:var(--txt)':''}" onclick="setF('alle')">Alle ${counts.alle}</div>`;
  for(const c in CATS){ if(!counts[c]) continue;
    h+=`<div class="chip ${filter===c?'on':''}" style="${filter===c?'background:var(--'+c+')':''}" onclick="setF('${c}')">${CATS[c]} ${counts[c]}</div>`;}
  document.getElementById("nav").innerHTML=h;
  document.getElementById("cnt").textContent=counts.alle+" NOTIZEN";
}
function setF(f){filter=f;render()}
function render(){
  nav();
  const rows=notes.filter(n=>filter==="alle"||((n.result||{}).type||"unbekannt")===filter);
  if(!rows.length){$("#list").innerHTML='<div class="empty">Keine Notizen in dieser Kategorie.</div>';return}
  $("#list").innerHTML=rows.map(card).join("");
}
function esc(s){return (s??"").toString().replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
function card(n){
  const r=n.result||{},t=r.type||"unbekannt",done=!!r.done;
  const when=(n.received||"").replace("T"," ").slice(0,16);
  const task=(t==="todo"||t==="erinnerung");
  let detail="";
  if(t==="termin"){
    detail=[r.date_raw&&"📅 "+esc(r.date_raw), r.time&&"🕐 "+esc(r.time), r.person&&"👤 "+esc(r.person),
            r.location&&"📍 "+esc(r.location)].filter(Boolean).join(" &nbsp; ");
  } else if(r.content){ detail=esc(r.content); }
  return `<div class="card ${done?'done-task':''}" style="--cat:var(--${CATS[t]?t:'unbekannt'})">
    <div class="row">
      ${task?`<div class="check ${done?'c':''}" onclick="toggle('${n.id}')">${done?"✓":""}</div>`:""}
      <div style="flex:1">
        <div class="badge">${esc(t)}</div>
        <div class="tt">${esc(r.title||n.transcript||"(ohne Titel)")}</div>
        <div class="meta">${when} · ${n.duration||"?"} s</div>
        ${detail?`<div class="detail">${detail}</div>`:""}
        ${n.transcript?`<div class="tr">„${esc(n.transcript)}"</div>`:""}
        <audio controls preload="none" src="/audio/${n.id}.wav"></audio>
        <div class="acts">
          ${t==="termin"?`<a class="btn primary" href="/ics/${n.id}.ics">+ Kalender</a>`:""}
          <button class="btn" onclick="openEdit('${n.id}')">Bearbeiten</button>
          <button class="btn danger" onclick="del('${n.id}')">Löschen</button>
        </div>
      </div></div></div>`;
}
function syncWhen(){$("#e_when").style.display = $("#e_type").value==="termin" ? "flex":"none"}
document.getElementById("e_type").addEventListener("change",syncWhen);
function openEdit(id){
  editId=id;const n=notes.find(x=>x.id===id),r=n.result||{};
  $("#e_type").value=CATS[r.type]?r.type:"unbekannt";
  $("#e_title").value=r.title||"";$("#e_content").value=r.content||r.notes||"";
  $("#e_date").value=r.date_raw||"";$("#e_time").value=r.time||"";
  syncWhen();dlg.showModal();
}
async function saveEdit(){
  const body={type:$("#e_type").value,title:$("#e_title").value,content:$("#e_content").value};
  if(body.type==="termin"){body.date_raw=$("#e_date").value;body.time=$("#e_time").value;}
  await fetch(`/api/note/${editId}/update`,{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify(body)});
  dlg.close();toast("Gespeichert");load();
}
async function toggle(id){await fetch(`/api/note/${id}/toggle`,{method:"POST"});load()}
async function del(id){
  if(!confirm("Notiz endgültig löschen?"))return;
  await fetch(`/api/note/${id}/delete`,{method:"POST"});toast("Gelöscht");load();
}
load();setInterval(load,15000);
</script></body></html>"""


def register_mobile(app, notes, notes_lock, save_notes, audio_dir):
    PROBES = ["/hotspot-detect.html", "/library/test/success.html", "/generate_204",
              "/gen_204", "/ncsi.txt", "/connecttest.txt", "/canonical.html",
              "/success.txt", "/check_network_status.txt", "/kindle-wifi/wifistub.html"]
    for p in PROBES:
        app.add_url_rule(p, "probe_" + p.strip("/").replace("/", "_"),
                         lambda: redirect("http://10.42.0.1:5000/m", code=302))

    @app.route("/m")
    def mobile():
        return render_template_string(MOBILE_HTML)

    @app.route("/audio/<nid>.wav")
    def audio(nid):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", nid):
            return "bad id", 400
        return send_from_directory(audio_dir, nid + ".wav")

    def _with_note(nid, fn):
        with notes_lock:
            for n in notes:
                if n["id"] == nid:
                    out = fn(n)
                    save_notes()
                    return out
        return jsonify({"error": "not found"}), 404

    @app.route("/api/note/<nid>/update", methods=["POST"])
    def note_update(nid):
        data = request.get_json(silent=True) or {}
        def fn(n):
            r = n.setdefault("result", {})
            for k in ("title", "content", "type", "date_raw", "time"):
                if k in data:
                    r[k] = data[k]
            return jsonify({"ok": True})
        return _with_note(nid, fn)

    @app.route("/api/note/<nid>/toggle", methods=["POST"])
    def note_toggle(nid):
        def fn(n):
            r = n.setdefault("result", {})
            r["done"] = not r.get("done")
            return jsonify({"ok": True, "done": r["done"]})
        return _with_note(nid, fn)

    @app.route("/api/note/<nid>/delete", methods=["POST"])
    def note_delete(nid):
        with notes_lock:
            for i, n in enumerate(notes):
                if n["id"] == nid:
                    notes.pop(i)
                    save_notes()
                    wav = os.path.join(audio_dir, nid + ".wav")
                    if os.path.exists(wav): os.remove(wav)
                    return jsonify({"ok": True})
        return jsonify({"error": "not found"}), 404

    @app.route("/ics/<nid>.ics")
    def ics_one(nid):
        with notes_lock:
            for n in notes:
                if n["id"] == nid:
                    return Response(_ics_wrap([_event_ics(n)]),
                                    mimetype="text/calendar",
                                    headers={"Content-Type": "text/calendar; charset=utf-8",
                                             "Content-Disposition": f"inline; filename={nid}.ics"})
        return "not found", 404

    @app.route("/calendar.ics")
    def ics_all():
        with notes_lock:
            evs = [_event_ics(n) for n in notes
                   if (n.get("result") or {}).get("type") == "termin" and n.get("status") == "done"]
        return Response(_ics_wrap(evs), mimetype="text/calendar",
                        headers={"Content-Type": "text/calendar; charset=utf-8",
                                 "Content-Disposition": "inline; filename=topiko.ics"})
