/* Frutiger Servers - activity monitor + announcements for Frutiger Astro.
   Native Win32, no dependencies. Serves a dashboard at http://127.0.0.1:8787/.
   It only listens: it receives heartbeats and shows a dashboard, and it can
   push DISPLAY-ONLY announcements (text a user sees and dismisses). It cannot
   run anything on a copy, change its settings, or delete it. No file contents,
   typed text, or PC data are ever received - only app/version/heartbeat. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
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
#define DISCO_PORT 8788
static char g_addr[32] = "127.0.0.1";

/* best-effort primary LAN IP (no packets sent) */
static void find_local_ip(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        struct sockaddr_in me; int ml = sizeof me;
        if (getsockname(s, (struct sockaddr *)&me, &ml) == 0) {
            unsigned char *p = (unsigned char *)&me.sin_addr;
            snprintf(g_addr, sizeof g_addr, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
        }
    }
    closesocket(s);
}

/* answer LAN discovery pings: a copy broadcasts, we reply so it learns our address+port */
static DWORD WINAPI DiscoThread(LPVOID a) {
    (void)a;
    SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u == INVALID_SOCKET) return 0;
    int yes = 1; setsockopt(u, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(DISCO_PORT); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(u, (struct sockaddr *)&sa, sizeof sa)) { closesocket(u); return 0; }
    for (;;) {
        char buf[64]; struct sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(u, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (!strncmp(buf, "FRUTIGER-DISCOVER", 17)) {
            char reply[32]; int rn = snprintf(reply, sizeof reply, "FRUTIGER-SERVER %d", PORT);
            sendto(u, reply, rn, 0, (struct sockaddr *)&from, fl);
        }
    }
}
#define MAXI 1024
#define MAXA 256
#define MAXD 8192
#define ONLINE 150

typedef struct { char id[40], app[20], ver[20], nick[64]; long long first, last; int used, gone, working; } Inst;
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
        char id[40], app[20], ver[20], ev[24];
        qget(q, "id", id, sizeof id); qget(q, "app", app, sizeof app); qget(q, "ver", ver, sizeof ver); qget(q, "event", ev, sizeof ev);
        if (id[0]) {
            Inst *it = find_inst(id);
            if (!it && g_ni < MAXI) { it = &g_inst[g_ni++]; memset(it, 0, sizeof *it); it->used = 1; strncpy(it->id, id, 39); it->first = now_s(); }
            if (it) {
                strncpy(it->app, app[0] ? app : "astro", 19); strncpy(it->ver, ver[0] ? ver : "?", 19);
                it->last = now_s();
                it->gone = !strcmp(ev, "offline");
                it->working = !strcmp(ev, "working");
                save_locked();
            }
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

/* ---------- native panel ---------- */
#define IDC_LIST 100
#define IDC_NICK 101
#define IDC_SAVENK 102
#define IDC_ANN 103
#define IDC_TARGET 104
#define IDC_SEND 105
#define IDC_RCVER 106
#define IDC_RCMSG 107
#define IDC_RCETA 108
#define IDC_RCCOPY 109

static HFONT g_ui, g_hdr, g_small;
static HWND g_list, g_nick, g_annedit, g_target, g_rcver, g_rcmsg, g_rceta;
static int g_sel = -1;   /* instance index of the selected copy, kept even when the list loses focus */

static HWND mkctl(HWND h, HINSTANCE hi, const wchar_t *cls, const wchar_t *txt, DWORD st, DWORD ex, int x, int y, int w, int hh, int id) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, hh, h, (HMENU)(INT_PTR)id, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_ui, TRUE);
    return c;
}
static void addcol(HWND lv, int i, const wchar_t *t, int w) {
    LVCOLUMNW c = {0}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; c.pszText = (wchar_t *)t; c.cx = w; c.iSubItem = i;
    SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
}
static void subtext(HWND lv, int row, int col, const wchar_t *t) {
    LVITEMW s = {0}; s.iSubItem = col; s.pszText = (wchar_t *)t; SendMessageW(lv, LVM_SETITEMTEXTW, row, (LPARAM)&s);
}
static void seentext(long long age, wchar_t *o) {
    if (age < 60) swprintf(o, 24, L"%llds ago", age);
    else if (age < 3600) swprintf(o, 24, L"%lldm ago", age / 60);
    else if (age < 86400) swprintf(o, 24, L"%lldh ago", age / 3600);
    else swprintf(o, 24, L"%lldd ago", age / 86400);
}
static void refreshUI(HWND h) {
    long long now = now_s();
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    int keep = (int)SendMessageW(g_target, CB_GETCURSEL, 0, 0);
    SendMessageW(g_target, CB_RESETCONTENT, 0, 0);
    int ev = (int)SendMessageW(g_target, CB_ADDSTRING, 0, (LPARAM)L"Everyone"); SendMessageW(g_target, CB_SETITEMDATA, ev, (LPARAM)-1);
    EnterCriticalSection(&g_lock);
    int row = 0;
    for (int i = 0; i < g_ni; i++) if (g_inst[i].used) {
        int on = !g_inst[i].gone && now - g_inst[i].last < ONLINE;
        int wk = on && g_inst[i].working;
        LVITEMW it = {0}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row; it.lParam = i;
        it.pszText = wk ? L"\x25CF Working" : on ? L"\x25CF Online" : L"\x25CF Offline";
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        const char *lbl = !strcmp(g_inst[i].app, "beta") ? "Astro Beta" : "Frutiger Astro";
        wchar_t lw[40], vw[24], col[80], nk[64], se[24];
        MultiByteToWideChar(CP_UTF8, 0, lbl, -1, lw, 40);
        MultiByteToWideChar(CP_UTF8, 0, g_inst[i].ver, -1, vw, 24);
        MultiByteToWideChar(CP_UTF8, 0, g_inst[i].nick, -1, nk, 64);
        swprintf(col, 80, L"%s v%s", lw, vw);
        subtext(g_list, row, 1, col); subtext(g_list, row, 2, nk);
        seentext(now - g_inst[i].last, se); subtext(g_list, row, 3, se);
        if (on) { int ix = (int)SendMessageW(g_target, CB_ADDSTRING, 0, (LPARAM)(nk[0] ? nk : col)); SendMessageW(g_target, CB_SETITEMDATA, ix, (LPARAM)i); }
        row++;
    }
    LeaveCriticalSection(&g_lock);
    SendMessageW(g_target, CB_SETCURSEL, keep < 0 ? 0 : keep, 0);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    RECT hr = {0, 0, 744, 66}; InvalidateRect(h, &hr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((LPCREATESTRUCTW)lp)->hInstance;
        g_ui = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_hdr = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_small = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                                 14, 76, 704, 210, h, (HMENU)IDC_LIST, hi, NULL);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_ui, TRUE);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        addcol(g_list, 0, L"Status", 96); addcol(g_list, 1, L"App / version", 180); addcol(g_list, 2, L"Nickname", 190); addcol(g_list, 3, L"Last seen", 110);
        mkctl(h, hi, L"STATIC", L"Nickname for selected copy:", SS_LEFT, 0, 14, 298, 200, 18, 0);
        g_nick = mkctl(h, hi, L"EDIT", L"", 0, WS_EX_CLIENTEDGE, 14, 318, 220, 24, IDC_NICK);
        mkctl(h, hi, L"BUTTON", L"Save name", BS_PUSHBUTTON | WS_TABSTOP, 0, 244, 317, 110, 26, IDC_SAVENK);
        mkctl(h, hi, L"STATIC", L"Announcement (shown till dismissed on the chosen copy):", SS_LEFT, 0, 14, 356, 440, 18, 0);
        g_annedit = mkctl(h, hi, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 14, 376, 420, 24, IDC_ANN);
        g_target = mkctl(h, hi, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 442, 376, 160, 240, IDC_TARGET);
        mkctl(h, hi, L"BUTTON", L"Send", BS_PUSHBUTTON | WS_TABSTOP, 0, 610, 375, 108, 26, IDC_SEND);
        mkctl(h, hi, L"STATIC", L"Emergency recall - builds the authenticated faup.py command to run in your FAUP clone:", SS_LEFT, 0, 14, 414, 700, 18, 0);
        mkctl(h, hi, L"STATIC", L"Version:", SS_LEFT, 0, 14, 440, 54, 18, 0);
        g_rcver = mkctl(h, hi, L"EDIT", L"", 0, WS_EX_CLIENTEDGE, 70, 438, 80, 24, IDC_RCVER);
        mkctl(h, hi, L"STATIC", L"Message:", SS_LEFT, 0, 160, 440, 60, 18, 0);
        g_rcmsg = mkctl(h, hi, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 222, 438, 250, 24, IDC_RCMSG);
        mkctl(h, hi, L"STATIC", L"ETA:", SS_LEFT, 0, 482, 440, 30, 18, 0);
        g_rceta = mkctl(h, hi, L"EDIT", L"", 0, WS_EX_CLIENTEDGE, 514, 438, 90, 24, IDC_RCETA);
        mkctl(h, hi, L"BUTTON", L"Copy command", BS_PUSHBUTTON | WS_TABSTOP, 0, 14, 470, 150, 26, IDC_RCCOPY);
        SetTimer(h, 1, 2000, NULL);
        refreshUI(h);
    } return 0;
    case WM_TIMER: refreshUI(h); return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_LIST && nh->code == LVN_ITEMCHANGED) {
            int sel = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (sel >= 0) {
                LVITEMW it = {0}; it.mask = LVIF_PARAM; it.iItem = sel; SendMessageW(g_list, LVM_GETITEMW, 0, (LPARAM)&it);
                int idx = (int)it.lParam; g_sel = idx; wchar_t nk[64] = L"";
                EnterCriticalSection(&g_lock); if (idx >= 0 && idx < g_ni && g_inst[idx].used) MultiByteToWideChar(CP_UTF8, 0, g_inst[idx].nick, -1, nk, 64);
                LeaveCriticalSection(&g_lock);
                SetWindowTextW(g_nick, nk);
            }
            break;
        }
        if (nh->idFrom == IDC_LIST && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                if (cd->iSubItem == 0) {
                    int i = (int)cd->nmcd.lItemlParam; long long now = now_s(); int on = 0, wk = 0;
                    EnterCriticalSection(&g_lock);
                    if (i >= 0 && i < g_ni && g_inst[i].used) { on = !g_inst[i].gone && now - g_inst[i].last < ONLINE; wk = on && g_inst[i].working; }
                    LeaveCriticalSection(&g_lock);
                    cd->clrText = wk ? RGB(201, 142, 0) : on ? RGB(0, 150, 0) : RGB(20, 20, 20);
                } else cd->clrText = RGB(30, 48, 68);
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SAVENK) {
            int idx = g_sel;   /* the copy picked in the list, remembered even after you click into the box */
            if (idx < 0) { MessageBoxW(h, L"Pick a copy in the list first, then type a nickname.", L"Frutiger Servers", MB_ICONINFORMATION); return 0; }
            wchar_t nk[64]; GetWindowTextW(g_nick, nk, 64);
            char na[96]; WideCharToMultiByte(CP_UTF8, 0, nk, -1, na, 96, 0, 0);
            EnterCriticalSection(&g_lock); if (idx >= 0 && idx < g_ni && g_inst[idx].used) { strncpy(g_inst[idx].nick, na, 63); g_inst[idx].nick[63] = 0; save_locked(); }
            LeaveCriticalSection(&g_lock); refreshUI(h);
            return 0;
        }
        if (LOWORD(wp) == IDC_SEND) {
            wchar_t tw[300]; GetWindowTextW(g_annedit, tw, 300);
            if (tw[0]) {
                char ta[600]; WideCharToMultiByte(CP_UTF8, 0, tw, -1, ta, 600, 0, 0); strip_nl(ta);
                int ci = (int)SendMessageW(g_target, CB_GETCURSEL, 0, 0), idx = (int)SendMessageW(g_target, CB_GETITEMDATA, ci, 0);
                char target[40] = "";
                EnterCriticalSection(&g_lock);
                if (idx >= 0 && idx < g_ni) strncpy(target, g_inst[idx].id, 39);
                if (g_na < MAXA) { Ann *a = &g_ann[g_na++]; memset(a, 0, sizeof *a); a->used = 1; a->id = ++g_seq; strncpy(a->target, target, 39); strncpy(a->text, ta, 299); a->created = now_s(); save_locked(); }
                LeaveCriticalSection(&g_lock);
                SetWindowTextW(g_annedit, L"");
                MessageBoxW(h, L"Sent. Copies that report to this server show it until the user dismisses it.", L"Frutiger Servers", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        if (LOWORD(wp) == IDC_RCCOPY) {
            wchar_t v[32], msg[300], eta[64]; GetWindowTextW(g_rcver, v, 32); GetWindowTextW(g_rcmsg, msg, 300); GetWindowTextW(g_rceta, eta, 64);
            if (!v[0]) { MessageBoxW(h, L"Enter the version to recall first (copies on that version or lower).", L"Frutiger Servers", MB_ICONWARNING); return 0; }
            for (wchar_t *p = msg; *p; p++) if (*p == L'"') *p = L'\'';
            wchar_t cmd[700];
            if (eta[0]) swprintf(cmd, 700, L"python faup.py recall %s \"%s\" --eta=\"%s\" --confirm", v, msg, eta);
            else swprintf(cmd, 700, L"python faup.py recall %s \"%s\" --confirm", v, msg);
            if (OpenClipboard(h)) { EmptyClipboard(); size_t n = (wcslen(cmd) + 1) * sizeof(wchar_t); HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n); memcpy(GlobalLock(g), cmd, n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); CloseClipboard(); }
            MessageBoxW(h, cmd, L"Copied - run this in your FAUP clone (lift later with: faup.py recall --lift --confirm)", MB_OK);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: SetBkColor((HDC)wp, RGB(238, 246, 251)); SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetStockObject(NULL_BRUSH);
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc); rc.top = 66; HBRUSH b = CreateSolidBrush(RGB(238, 246, 251)); FillRect((HDC)wp, &rc, b); DeleteObject(b); } return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        for (int y = 0; y < 66; y++) { double t = (double)y / 66; int cr = (int)(22 + 150 * t), cg = (int)(136 + 90 * t), cb = (int)(214 + 40 * t);
            HBRUSH br = CreateSolidBrush(RGB(cr, cg, cb)); RECT ln = {0, y, rc.right, y + 1}; FillRect(dc, &ln, br); DeleteObject(br); }
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_hdr); SetTextColor(dc, RGB(255, 255, 255));
        RECT t1 = {18, 10, rc.right, 48}; DrawTextW(dc, L"Frutiger Servers", -1, &t1, DT_LEFT | DT_SINGLELINE);
        long long now = now_s(); int on = 0, tot = 0;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_ni; i++) if (g_inst[i].used) { tot++; if (!g_inst[i].gone && now - g_inst[i].last < ONLINE) on++; }
        LeaveCriticalSection(&g_lock);
        wchar_t st[64]; swprintf(st, 64, L"Active %d  ·  Installs %d", on, tot);
        SelectObject(dc, g_ui); SetTextColor(dc, RGB(230, 245, 255));
        RECT t2 = {0, 24, rc.right - 18, 44}; DrawTextW(dc, st, -1, &t2, DT_RIGHT | DT_SINGLELINE);
        SelectObject(dc, g_small); SetTextColor(dc, RGB(216, 239, 252));
        wchar_t waddr[32], note[160]; MultiByteToWideChar(CP_UTF8, 0, g_addr, -1, waddr, 32);
        swprintf(note, 160, L"Serving at http://%s:%d · copies on this network connect automatically · check-in every 10s", waddr, PORT);
        RECT t3 = {20, 46, rc.right - 150, 62}; DrawTextW(dc, note, -1, &t3, DT_LEFT | DT_SINGLELINE);
        EndPaint(h, &ps);
    } return 0;
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
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&ic);

    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(PORT); sa.sin_addr.s_addr = htonl(INADDR_ANY);   /* reachable from other devices on the network */
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) || listen(srv, 16)) {
        MessageBoxW(NULL, L"Port 8787 is already in use. Is Frutiger Servers already running?", L"Frutiger Servers", MB_ICONWARNING);
        return 1;
    }
    CreateThread(NULL, 0, ServerThread, (LPVOID)(INT_PTR)srv, 0, NULL);
    find_local_ip();
    CreateThread(NULL, 0, DiscoThread, NULL, 0, NULL);

    WNDCLASSW wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"FrutigerServers";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.hbrBackground = NULL;
    RegisterClassW(&wc);
    RECT wr = {0, 0, 732, 512}; AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_win = CreateWindowExW(0, L"FrutigerServers", L"Frutiger Servers",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, hi, NULL);
    ShowWindow(g_win, show ? show : SW_SHOWNORMAL);

    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { if (!IsDialogMessageW(g_win, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0;
}
