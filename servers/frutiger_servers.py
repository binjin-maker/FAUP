#!/usr/bin/env python3
"""Frutiger Servers - activity monitor for the Frutiger series.

Run:   python frutiger_servers.py
Then open http://127.0.0.1:8787/ in a browser.

Copies of Frutiger Astro send a tiny heartbeat (install id, app, version, a
launch/ping event) after they're activated. That's all this stores. It never
receives files, typed text, or anything else from a user's PC, and it has no
way to send anything back to a copy - it only listens.

Point copies at this by setting, in the FrutigerMacro.ini next to their exe:
    [Servers]
    Url=http://YOUR-HOST:8787/beat
For copies out on the internet, run this where they can reach it and use that
host. For just your own machines, the default localhost is fine.
"""
import http.server
import json
import os
import socketserver
import threading
import time
import urllib.parse

HOST = os.environ.get("FS_HOST", "127.0.0.1")
PORT = int(os.environ.get("FS_PORT", "8787"))
TOKEN = os.environ.get("FS_TOKEN", "")          # optional: require ?key=... to view
ONLINE_SECS = 150                                # no beat for this long -> Offline
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "frutiger_servers_data.json")
LOCK = threading.Lock()
APP_LABELS = {"astro": "Frutiger Astro", "beta": "Astro Beta"}


def load():
    try:
        return json.load(open(DATA, encoding="utf-8"))
    except Exception:
        return {"installs": {}}


def save(d):
    tmp = DATA + ".tmp"
    json.dump(d, open(tmp, "w", encoding="utf-8"), indent=2)
    os.replace(tmp, DATA)


def record_beat(q):
    iid = (q.get("id", [""])[0] or "").strip()[:32]
    if not iid:
        return
    app = (q.get("app", ["astro"])[0] or "astro").strip()[:16]
    ver = (q.get("ver", ["?"])[0] or "?").strip()[:16]
    now = time.time()
    with LOCK:
        d = load()
        it = d["installs"].get(iid, {})
        it.setdefault("first_seen", now)
        it.setdefault("nickname", "")
        it["app"] = app
        it["ver"] = ver
        it["last_seen"] = now
        d["installs"][iid] = it
        save(d)


def set_nick(iid, name):
    with LOCK:
        d = load()
        if iid in d["installs"]:
            d["installs"][iid]["nickname"] = name.strip()[:40]
            save(d)


def ago(secs):
    s = int(secs)
    if s < 60:
        return f"{s}s ago"
    if s < 3600:
        return f"{s // 60}m ago"
    if s < 86400:
        return f"{s // 3600}h ago"
    return f"{s // 86400}d ago"


def esc(s):
    return (s or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>frutiger-servers</title>
<style>
 :root{--g:#3bf06a;--dim:#1f7a3a;--amber:#ffb738;--off:#53705d;--bg:#060a07}
 *{box-sizing:border-box}
 body{margin:0;min-height:100vh;background:var(--bg);color:var(--g);
   font:14px/1.65 'Cascadia Code',ui-monospace,Menlo,Consolas,monospace;
   text-shadow:0 0 5px rgba(59,240,106,.28)}
 .wrap{max-width:900px;margin:0 auto;padding:26px 18px 70px}
 .bar{color:var(--dim)} .bar b{color:var(--g)}
 .stats{margin:14px 0 6px;white-space:pre-wrap}
 .stats .k{color:var(--dim)}
 .sec{margin:20px 0 6px;color:var(--amber);border-top:1px dashed #163020;padding-top:12px}
 .row{display:flex;align-items:center;gap:14px;padding:2px 0;white-space:nowrap;overflow:hidden}
 .st{flex:none;width:12ch} .on .st{color:var(--g)} .off{color:var(--off)}
 .nm{flex:none;width:26ch} .off .nm{color:var(--off)}
 .meta{color:var(--dim);margin-left:auto;flex:none}
 .off .meta{color:var(--off)}
 .nick{background:transparent;border:none;border-bottom:1px solid #163020;color:var(--amber);
   font:inherit;width:16ch;padding:0 3px;outline:none}
 .nick::placeholder{color:#2b4a35} .nick:focus{border-color:var(--amber)}
 .empty{color:var(--dim);padding:4px 0}
 a{color:var(--amber);text-decoration:none} a:hover{text-decoration:underline}
 .cur{animation:bl 1.1s steps(1) infinite}@keyframes bl{50%{opacity:0}}
</style></head><body><div class="wrap">
<div class="bar">frutiger-servers:~$ <b>monitor --live</b> <span class="cur">&#9608;</span></div>
<div class="stats"><span class="k">active   </span>%ONLINE%    <span class="k">installs </span>%TOTAL%    <span class="k">versions </span>%VERSIONS%</div>
%TABS%
<div class="bar" style="margin-top:24px">[ auto-refresh 15s &middot; <a href="">refresh now</a> ]</div>
<script>setTimeout(()=>location.reload(),15000);
document.querySelectorAll('.nick').forEach(i=>i.addEventListener('change',()=>{
 fetch('/nick?id='+i.dataset.id+'&name='+encodeURIComponent(i.value)+'%KEYQ%',{method:'POST'});}));
</script>
</div></body></html>"""


def render(key_q):
    d = load()
    installs = d["installs"]
    now = time.time()
    online = sum(1 for it in installs.values() if now - it["last_seen"] < ONLINE_SECS)
    vers = sorted({it["ver"] for it in installs.values()})
    by_app = {}
    for iid, it in installs.items():
        by_app.setdefault(it.get("app", "astro"), []).append((iid, it))
    tabs = []
    for app in sorted(by_app, key=lambda a: a != "astro"):
        label = APP_LABELS.get(app, app)
        rows = []
        for iid, it in sorted(by_app[app], key=lambda x: (-(now - x[1]["last_seen"] < ONLINE_SECS), x[1]["last_seen"])):
            on = now - it["last_seen"] < ONLINE_SECS
            st = ("[●] ONLINE " if on else "[○] OFFLINE")
            rows.append(
                f'<div class="row {"on" if on else "off"}">'
                f'<span class="st">{st}</span>'
                f'<span class="nm">{esc(label)} v{esc(it["ver"])}</span>'
                f'<input class="nick" data-id="{esc(iid)}" value="{esc(it.get("nickname",""))}" placeholder="+ nickname">'
                f'<span class="meta">seen {ago(now - it["last_seen"])}</span></div>')
        body = "".join(rows) if rows else '<div class="empty">no copies have checked in yet</div>'
        tabs.append(f'<div class="sec">== {esc(label)} ({len(by_app[app])}) ==</div>{body}')
    html = PAGE.replace("%ONLINE%", str(online)).replace("%TOTAL%", str(len(installs)))
    html = html.replace("%VERSIONS%", str(len(vers))).replace("%TABS%", "".join(tabs))
    html = html.replace("%KEYQ%", ("&key=" + key_q) if key_q else "")
    return html


class H(http.server.BaseHTTPRequestHandler):
    def _auth(self, q):
        return not TOKEN or (q.get("key", [""])[0] == TOKEN)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/beat":
            record_beat(q)
            return self._send(200, "ok", "text/plain")
        if u.path in ("/", "/index.html"):
            if not self._auth(q):
                return self._send(403, "frutiger-servers: add ?key=YOUR_TOKEN", "text/plain")
            return self._send(200, render(q.get("key", [""])[0]), "text/html")
        self._send(404, "not found", "text/plain")

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/nick" and self._auth(q):
            set_nick(q.get("id", [""])[0], q.get("name", [""])[0])
            return self._send(200, "ok", "text/plain")
        self._send(404, "not found", "text/plain")

    def _send(self, code, body, ctype):
        b = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype + "; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def log_message(self, *a):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    print(f"frutiger-servers on http://{HOST}:{PORT}/  (data: {os.path.basename(DATA)})")
    if TOKEN:
        print(f"  dashboard requires ?key={TOKEN}")
    Server((HOST, PORT), H).serve_forever()
