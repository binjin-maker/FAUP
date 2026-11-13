/* Frutiger Servers - activity monitor + announcements for Frutiger Astro.
   Native Win32, no dependencies. Serves a dashboard at http://127.0.0.1:8787/.
   It only listens: it receives heartbeats and shows a dashboard, and it can
   push DISPLAY-ONLY announcements (text a user sees and dismisses). It cannot
   run anything on a copy, change its settings, or delete it. No file contents,
   typed text, or PC data are ever received - only app/version/heartbeat. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

static char *nexttok(char **s, char sep) {
    if (!*s) return NULL;
    char *start = *s, *p = strchr(start, sep);
    if (p) { *p = 0; *s = p + 1; } else *s = NULL;
    return start;
}

#define PORT 8787
#define MAXI 1024
#define MAXA 256
#define MAXD 8192
#define ONLINE 150

typedef struct { char id[40], app[20], ver[20], nick[64]; long long first, last; int used; } Inst;
typedef struct { int id; char target[40], text[300]; long long created; int used; } Ann;

static Inst g_inst[MAXI]; static int g_ni;
static Ann g_ann[MAXA]; static int g_na, g_seq;
static char g_dism[MAXD][96]; static int g_nd;        /* "installid|annid" */
static CRITICAL_SECTION g_lock;
static char g_dat[MAX_PATH];
static HWND g_win;

static long long now_s(void) { return (long long)time(NULL); }

/* ---------- tiny growable buffer ---------- */
typedef struct { char *p; size_t len, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
static void bs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bf(Buf *b, const char *fmt, ...) {
    char t[1200]; va_list a; va_start(a, fmt); int n = vsnprintf(t, sizeof t, fmt, a); va_end(a);
    if (n > 0) bput(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}
static void besc(Buf *b, const char *s) {           /* HTML escape */
    for (; *s; s++) {
        if (*s == '&') bs(b, "&amp;"); else if (*s == '<') bs(b, "&lt;");
        else if (*s == '>') bs(b, "&gt;"); else if (*s == '"') bs(b, "&quot;");
        else bput(b, s, 1);
    }
}
static void jesc(Buf *b, const char *s) {            /* JSON string escape */
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { bput(b, "\\", 1); bput(b, (char *)&c, 1); }
        else if (c == '\n') bs(b, "\\n"); else if (c == '\r') bs(b, "\\r");
        else if (c < 0x20) bf(b, "\\u%04x", c); else bput(b, (char *)&c, 1);
    }
}

/* ---------- url decode + query ---------- */
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }
static void urldec(const char *s, char *out, int n) {
    int i = 0;
    for (; *s && i < n - 1; s++) {
        if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { out[i++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 2; }
        else if (*s == '+') out[i++] = ' ';
        else out[i++] = *s;
    }
    out[i] = 0;
}
static int qget(const char *q, const char *key, char *out, int n) {
    size_t kl = strlen(key); const char *p = q;
    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '&');
            char raw[1024]; int len = e ? (int)(e - v) : (int)strlen(v);
            if (len > (int)sizeof raw - 1) len = sizeof raw - 1;
            memcpy(raw, v, len); raw[len] = 0; urldec(raw, out, n);
            return 1;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return 0;
}
static void strip_nl(char *s) { for (; *s; s++) if (*s == '\n' || *s == '\r') *s = ' '; }

/* ---------- persistence ---------- */
static void save_locked(void) {
    FILE *f = fopen(g_dat, "wb"); if (!f) return;
    for (int i = 0; i < g_ni; i++) if (g_inst[i].used)
        fprintf(f, "I|%s|%s|%s|%lld|%lld|%s\n", g_inst[i].id, g_inst[i].app, g_inst[i].ver,
                g_inst[i].first, g_inst[i].last, g_inst[i].nick);
    for (int i = 0; i < g_na; i++) if (g_ann[i].used)
        fprintf(f, "A|%d|%s|%lld|%s\n", g_ann[i].id, g_ann[i].target, g_ann[i].created, g_ann[i].text);
    for (int i = 0; i < g_nd; i++) fprintf(f, "D|%s\n", g_dism[i]);
    fclose(f);
}
static void load(void) {
    FILE *f = fopen(g_dat, "rb"); if (!f) return;
    char ln[600];
    while (fgets(ln, sizeof ln, f)) {
        char *nl = strpbrk(ln, "\r\n"); if (nl) *nl = 0;
        if (ln[0] == 'I' && g_ni < MAXI) {
            Inst *it = &g_inst[g_ni];
            char *t = ln + 2, *f1 = nexttok(&t, (char)0x7c), *f2 = nexttok(&t, (char)0x7c), *f3 = nexttok(&t, (char)0x7c),
                 *f4 = nexttok(&t, (char)0x7c), *f5 = nexttok(&t, (char)0x7c);
            if (f1 && f2 && f3 && f4 && f5) {
                memset(it, 0, sizeof *it); it->used = 1;
                strncpy(it->id, f1, 39); strncpy(it->app, f2, 19); strncpy(it->ver, f3, 19);
                it->first = atoll(f4); it->last = atoll(f5); strncpy(it->nick, t ? t : "", 63);
                g_ni++;
            }
        } else if (ln[0] == 'A' && g_na < MAXA) {
            Ann *a = &g_ann[g_na];
            char *t = ln + 2, *f1 = nexttok(&t, (char)0x7c), *f2 = nexttok(&t, (char)0x7c), *f3 = nexttok(&t, (char)0x7c);
            if (f1 && f2 && f3) {
                memset(a, 0, sizeof *a); a->used = 1; a->id = atoi(f1);
                strncpy(a->target, f2, 39); a->created = atoll(f3); strncpy(a->text, t ? t : "", 299);
                if (a->id > g_seq) g_seq = a->id; g_na++;
            }
        } else if (ln[0] == 'D' && g_nd < MAXD) {
            strncpy(g_dism[g_nd++], ln + 2, 95);
        }
    }
    fclose(f);
}

/* ---------- model ops (call under lock) ---------- */
static Inst *find_inst(const char *id) {
    for (int i = 0; i < g_ni; i++) if (g_inst[i].used && !strcmp(g_inst[i].id, id)) return &g_inst[i];
    return NULL;
}
static int dismissed(const char *id, int ann) {
    char key[96]; snprintf(key, sizeof key, "%s|%d", id, ann);
    for (int i = 0; i < g_nd; i++) if (!strcmp(g_dism[i], key)) return 1;
    return 0;
}
static int pending_ann(const char *id, char *textout, int n) {   /* newest undismissed for id */
    for (int i = g_na - 1; i >= 0; i--) {
        Ann *a = &g_ann[i];
        if (!a->used) continue;
        if (a->target[0] && strcmp(a->target, id)) continue;      /* targeted elsewhere */
        if (dismissed(id, a->id)) continue;
        strncpy(textout, a->text, n - 1); textout[n - 1] = 0;
        return a->id;
    }
    return 0;
}

/* ---------- dashboard ---------- */
static const char *CSS =
"*{box-sizing:border-box;font-family:'Segoe UI',system-ui,sans-serif}"
"body{margin:0;min-height:100vh;color:#0e345c;background:linear-gradient(#1688d6,#8ad4f7 45%,#eefbff 70%,#bfe89a 100%)}"
".wrap{max-width:780px;margin:0 auto;padding:26px 18px 60px}"
"h1{margin:0;color:#fff;text-shadow:0 1px 2px #1a5f94;font-size:30px;font-weight:600}"
".sub{color:#eaf7ff;text-shadow:0 1px 1px #2a7;margin:2px 0 18px;font-size:13px}"
".cards{display:flex;gap:12px;margin-bottom:18px;flex-wrap:wrap}"
".card{flex:1;min-width:140px;background:linear-gradient(#fff,#e6f7ff);border:1px solid #9fd0ef;border-radius:14px;padding:14px 16px;box-shadow:0 2px 6px rgba(10,42,74,.18)}"
".card .n{font-size:32px;font-weight:600;color:#1570b8}.card .l{font-size:12px;color:#5a7f9f;text-transform:uppercase;letter-spacing:.04em}"
".panel{background:rgba(255,255,255,.62);border:1px solid #9fd0ef;border-radius:14px;box-shadow:0 2px 6px rgba(10,42,74,.14);margin-bottom:16px;overflow:hidden}"
".panel h2{margin:0;padding:11px 16px;background:linear-gradient(#2b96e2,#1b74c0);color:#fff;font-size:15px}"
".row{display:flex;align-items:center;gap:10px;padding:10px 16px;border-top:1px solid #d6eefc}"
".dot{width:11px;height:11px;border-radius:50%;flex:none;box-shadow:0 0 0 2px rgba(255,255,255,.6)}"
".on{background:radial-gradient(circle at 35% 30%,#8fffa8,#34c23a)}.off{background:radial-gradient(circle at 35% 30%,#cfd6de,#9aa8b8)}"
".nm{font-weight:600}.meta{color:#5a7f9f;font-size:12.5px;margin-left:auto;text-align:right}"
".nick{border:1px solid #b9d9f0;border-radius:7px;padding:3px 7px;font-size:12.5px;width:110px}"
"form.cmp{padding:14px 16px;border-top:1px solid #d6eefc;display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
".cmp input[type=text]{flex:1;min-width:220px;border:1px solid #b9d9f0;border-radius:8px;padding:7px 10px;font-size:14px}"
".cmp select{border:1px solid #b9d9f0;border-radius:8px;padding:7px;font-size:13px}"
".btn{background:linear-gradient(#5cc8ff,#1f8fe0);border:1px solid #1a6fae;color:#fff;font-weight:600;border-radius:8px;padding:7px 16px;font-size:13px;cursor:pointer;text-shadow:0 1px 1px #15567f}"
".ann{padding:9px 16px;border-top:1px solid #d6eefc;font-size:13px;display:flex;gap:10px;align-items:center}"
".ann .to{color:#1570b8;font-weight:600}.ann .t{color:#20405c}.ann .x{margin-left:auto;color:#c0392b;text-decoration:none;font-weight:600}"
".empty{padding:16px;color:#5a7f9f}";

static void dashboard(Buf *b) {
    long long now = now_s();
    int online = 0, vers[64], nv = 0;
    for (int i = 0; i < g_ni; i++) if (g_inst[i].used) {
        if (now - g_inst[i].last < ONLINE) online++;
    }
    (void)vers; (void)nv;
    bs(b, "<!doctype html><html><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>Frutiger Servers</title><style>");
    bs(b, CSS);
    bf(b, "</style></head><body><div class=\"wrap\"><h1>Frutiger Servers</h1>"
          "<div class=\"sub\">activity monitor &middot; auto-refreshes &middot; <a style=\"color:#eaf7ff\" href=\"/\">refresh now</a></div>"
          "<div class=\"cards\"><div class=\"card\"><div class=\"n\">%d</div><div class=\"l\">Active now</div></div>"
          "<div class=\"card\"><div class=\"n\">%d</div><div class=\"l\">Total installs</div></div></div>", online, g_ni);

    /* compose announcement */
    bs(b, "<div class=\"panel\"><h2>Send an announcement</h2>"
          "<form class=\"cmp\" action=\"/announce\" method=\"get\">"
          "<input type=\"text\" name=\"text\" maxlength=\"280\" placeholder=\"Message shown on copies until the user dismisses it\" required>"
          "<select name=\"target\"><option value=\"\">Everyone</option>");
    for (int i = 0; i < g_ni; i++) if (g_inst[i].used && now - g_inst[i].last < ONLINE) {
        bs(b, "<option value=\""); besc(b, g_inst[i].id); bs(b, "\">");
        besc(b, g_inst[i].nick[0] ? g_inst[i].nick : g_inst[i].id); bs(b, " (online)</option>");
    }
    bs(b, "</select><button class=\"btn\">Send</button></form>");
    /* active announcements */
    int any = 0;
    for (int i = g_na - 1; i >= 0; i--) if (g_ann[i].used) {
        any = 1;
        bs(b, "<div class=\"ann\"><span class=\"to\">");
        if (g_ann[i].target[0]) { Inst *it = find_inst(g_ann[i].target); besc(b, it && it->nick[0] ? it->nick : g_ann[i].target); }
        else bs(b, "Everyone");
        bs(b, "</span><span class=\"t\">"); besc(b, g_ann[i].text);
        bf(b, "</span><a class=\"x\" href=\"/retract?ann=%d\">retract</a></div>", g_ann[i].id);
    }
    if (!any) bs(b, "<div class=\"empty\">No active announcements.</div>");
    bs(b, "</div>");

    /* emergency: recall / notice command builder (recalls go through the signed publish, not a live button) */
    bs(b, "<div class=\"panel\"><h2>Emergency &middot; recall &amp; notice</h2>"
          "<div style=\"padding:12px 16px;font-size:12.5px;color:#20405c\">These are issued by publishing to the update channel (so they're authenticated and can't be forged). "
          "Fill this in, then run the command it builds in your FAUP clone.</div>"
          "<form class=\"cmp\" onsubmit=\"return false\">"
          "<input id=\"rv\" type=\"text\" placeholder=\"recall copies on this version or lower, e.g. 1.14.9\" style=\"min-width:150px\">"
          "<input id=\"rm\" type=\"text\" placeholder=\"message to users\">"
          "<input id=\"re\" type=\"text\" placeholder=\"ETA, e.g. ~30 min\" style=\"min-width:110px\">"
          "<select id=\"rmode\"><option value=\"\">full lockdown</option><option value=\" --soft\">soft (update tab only)</option></select>"
          "<button class=\"btn\" onclick=\"bld()\">Build command</button></form>"
          "<div style=\"padding:0 16px 14px\"><input id=\"rout\" readonly style=\"width:100%;border:1px solid #b9d9f0;border-radius:8px;padding:8px;font-family:monospace;font-size:12px\" "
          "placeholder=\"python faup.py recall ...\"><div style=\"margin-top:6px;color:#5a7f9f;font-size:12px\">To lift: <code>python faup.py recall --lift --confirm</code> &nbsp;&middot;&nbsp; "
          "Changelog notice: <code>python faup.py notice \"your message\"</code> (clear with <code>--clear</code>)</div></div>"
          "<script>function bld(){var v=rv.value.trim(),m=rm.value.replace(/\"/g,''),e=re.value.trim();"
          "if(!v){rout.value='enter a version first';return;}var c='python faup.py recall '+v+' \"'+m+'\"';"
          "if(e)c+=' --eta=\"'+e+'\"';c+=document.getElementById('rmode').value+' --confirm';rout.value=c;}</script></div>");

    /* installs */
    bs(b, "<div class=\"panel\"><h2>Copies</h2>");
    int shown = 0;
    for (int pass = 1; pass >= 0; pass--)
        for (int i = 0; i < g_ni; i++) if (g_inst[i].used) {
            int on = now - g_inst[i].last < ONLINE;
            if (on != pass) continue;
            shown++;
            long long age = now - g_inst[i].last;
            char agebuf[32];
            if (age < 60) snprintf(agebuf, 32, "%llds ago", age);
            else if (age < 3600) snprintf(agebuf, 32, "%lldm ago", age / 60);
            else if (age < 86400) snprintf(agebuf, 32, "%lldh ago", age / 3600);
            else snprintf(agebuf, 32, "%lldd ago", age / 86400);
            const char *label = !strcmp(g_inst[i].app, "beta") ? "Astro Beta" : "Frutiger Astro";
            bf(b, "<div class=\"row\"><span class=\"dot %s\"></span><span class=\"nm\">", on ? "on" : "off");
            besc(b, label); bs(b, " v"); besc(b, g_inst[i].ver); bs(b, "</span>");
            bs(b, "<input class=\"nick\" value=\""); besc(b, g_inst[i].nick);
            bs(b, "\" placeholder=\"nickname\" data-id=\""); besc(b, g_inst[i].id); bs(b, "\">");
            bf(b, "<span class=\"meta\">%s &middot; seen %s</span></div>", on ? "Online" : "Offline", agebuf);
        }
    if (!shown) bs(b, "<div class=\"empty\">No copies have checked in yet.</div>");
    bs(b, "</div>");

    bs(b, "<script>setTimeout(()=>location.reload(),15000);"
          "document.querySelectorAll('.nick').forEach(i=>i.addEventListener('change',()=>"
          "fetch('/nick?id='+i.dataset.id+'&name='+encodeURIComponent(i.value),{method:'POST'})));"
          "</script></div></body></html>");
}

/* ---------- request handling ---------- */
static void http_send(SOCKET c, const char *status, const char *ctype, const char *body, size_t n) {
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, ctype, n);
    send(c, hdr, h, 0);
    size_t sent = 0; while (sent < n) { int w = send(c, body + sent, (int)(n - sent), 0); if (w <= 0) break; sent += w; }
}

static void handle(SOCKET c) {
    char req[8192]; int r = recv(c, req, sizeof req - 1, 0);
    if (r <= 0) return;
    req[r] = 0;
    char method[8] = "", rawpath[2048] = "";
    sscanf(req, "%7s %2047s", method, rawpath);
    char *q = strchr(rawpath, '?'); char path[512];
    if (q) { *q = 0; q++; } else q = "";
    strncpy(path, rawpath, sizeof path - 1); path[sizeof path - 1] = 0;

    EnterCriticalSection(&g_lock);
    if (!strcmp(path, "/beat")) {
        char id[40], app[20], ver[20];
        qget(q, "id", id, sizeof id); qget(q, "app", app, sizeof app); qget(q, "ver", ver, sizeof ver);
        if (id[0]) {
            Inst *it = find_inst(id);
            if (!it && g_ni < MAXI) { it = &g_inst[g_ni++]; memset(it, 0, sizeof *it); it->used = 1; strncpy(it->id, id, 39); it->first = now_s(); }
            if (it) { strncpy(it->app, app[0] ? app : "astro", 19); strncpy(it->ver, ver[0] ? ver : "?", 19); it->last = now_s(); save_locked(); }
        }
        char text[300]; int a = id[0] ? pending_ann(id, text, sizeof text) : 0;
        Buf b = {0};
        if (a) { bs(&b, "{\"ann\":\""); bf(&b, "%d", a); bs(&b, "\",\"text\":\""); jesc(&b, text); bs(&b, "\"}"); }
        else bs(&b, "{}");
        LeaveCriticalSection(&g_lock);
        http_send(c, "200 OK", "application/json", b.p ? b.p : "{}", b.len); free(b.p); return;
    }
    if (!strcmp(path, "/dismiss")) {
        char id[40], an[16]; qget(q, "id", id, sizeof id); qget(q, "ann", an, sizeof an);
        if (id[0] && an[0] && g_nd < MAXD && !dismissed(id, atoi(an))) { snprintf(g_dism[g_nd++], 96, "%s|%d", id, atoi(an)); save_locked(); }
        LeaveCriticalSection(&g_lock); http_send(c, "200 OK", "text/plain", "ok", 2); return;
    }
    if (!strcmp(path, "/nick")) {
        char id[40], nm[64]; qget(q, "id", id, sizeof id); qget(q, "name", nm, sizeof nm);
        strip_nl(nm); Inst *it = find_inst(id); if (it) { strncpy(it->nick, nm, 63); it->nick[63] = 0; save_locked(); }
        LeaveCriticalSection(&g_lock); http_send(c, "200 OK", "text/plain", "ok", 2); return;
    }
    if (!strcmp(path, "/announce")) {
        char text[300], target[40]; qget(q, "text", text, sizeof text); qget(q, "target", target, sizeof target);
        strip_nl(text);
        if (text[0] && g_na < MAXA) { Ann *a = &g_ann[g_na++]; memset(a, 0, sizeof *a); a->used = 1; a->id = ++g_seq; strncpy(a->target, target, 39); strncpy(a->text, text, 299); a->created = now_s(); save_locked(); }
        LeaveCriticalSection(&g_lock);
        http_send(c, "303 See Other\r\nLocation: /", "text/plain", "", 0); return;
    }
    if (!strcmp(path, "/retract")) {
        char an[16]; qget(q, "ann", an, sizeof an); int id = atoi(an);
        for (int i = 0; i < g_na; i++) if (g_ann[i].used && g_ann[i].id == id) g_ann[i].used = 0;
        save_locked(); LeaveCriticalSection(&g_lock);
        http_send(c, "303 See Other\r\nLocation: /", "text/plain", "", 0); return;
    }
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        Buf b = {0}; dashboard(&b); LeaveCriticalSection(&g_lock);
        http_send(c, "200 OK", "text/html; charset=utf-8", b.p, b.len); free(b.p); return;
    }
    LeaveCriticalSection(&g_lock);
    http_send(c, "404 Not Found", "text/plain", "not found", 9);
}

static DWORD WINAPI ServerThread(LPVOID arg) {
    SOCKET srv = (SOCKET)(INT_PTR)arg;
    for (;;) { SOCKET c = accept(srv, NULL, NULL); if (c == INVALID_SOCKET) continue; handle(c); shutdown(c, SD_BOTH); closesocket(c); }
}

/* ---------- tiny window ---------- */
static void openDash(void) { ShellExecuteW(NULL, L"open", L"http://127.0.0.1:8787/", NULL, NULL, SW_SHOWNORMAL); }

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        for (int y = 0; y < rc.bottom; y++) {
            double t = (double)y / rc.bottom;
            int cr = (int)(22 + (238 - 22) * t), cg = (int)(136 + (251 - 136) * t), cb = (int)(214 + (255 - 214) * t);
            HBRUSH br = CreateSolidBrush(RGB(cr, cg, cb)); RECT ln = { 0, y, rc.right, y + 1 }; FillRect(dc, &ln, br); DeleteObject(br);
        }
        SetBkMode(dc, TRANSPARENT);
        HFONT big = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT sml = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, big); SetTextColor(dc, RGB(255, 255, 255));
        RECT t1 = { 0, 26, rc.right, 70 }; DrawTextW(dc, L"Frutiger Servers", -1, &t1, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, sml); SetTextColor(dc, RGB(20, 70, 110));
        RECT t2 = { 0, 70, rc.right, 92 }; DrawTextW(dc, L"running at http://127.0.0.1:8787/", -1, &t2, DT_CENTER | DT_SINGLELINE);
        DeleteObject(big); DeleteObject(sml); EndPaint(h, &ps);
    } return 0;
    case WM_COMMAND: if (LOWORD(wp) == 1) openDash(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd;
    GetModuleFileNameA(NULL, g_dat, MAX_PATH);
    char *sl = strrchr(g_dat, '\\'); if (sl) sl[1] = 0; strcat(g_dat, "frutiger_servers.dat");
    InitializeCriticalSection(&g_lock);
    load();

    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(PORT); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) || listen(srv, 16)) {
        MessageBoxW(NULL, L"Port 8787 is already in use. Is Frutiger Servers already running?", L"Frutiger Servers", MB_ICONWARNING);
        return 1;
    }
    CreateThread(NULL, 0, ServerThread, (LPVOID)(INT_PTR)srv, 0, NULL);

    WNDCLASSW wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"FrutigerServers";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); RegisterClassW(&wc);
    g_win = CreateWindowExW(0, L"FrutigerServers", L"Frutiger Servers",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 440, 190, NULL, NULL, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Open dashboard", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        140, 110, 160, 34, g_win, (HMENU)1, hi, NULL);
    ShowWindow(g_win, show ? show : SW_SHOWNORMAL);
    openDash();

    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
