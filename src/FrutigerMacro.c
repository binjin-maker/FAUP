#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <richedit.h>
#include <uxtheme.h>
#include <wininet.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define WM_STATUS (WM_APP + 1)
#define WM_DONE   (WM_APP + 2)
#define WM_HL     (WM_APP + 3)
#define HL(i) PostMessageW(hMain, WM_HL, (WPARAM)(INT_PTR)(i), 0)
#define HLJ(j, i) do { if (!(j)->clip) HL(i); } while (0)

enum { EV_KD, EV_KU, EV_MV, EV_LD, EV_LU, EV_RD, EV_RU, EV_MD, EV_MU,
       EV_X1D, EV_X1U, EV_X2D, EV_X2U, EV_WH, EV_HW, EV_TYPE, EV_COUNT };
static const char *EVN[] = { "KD","KU","MV","LD","LU","RD","RU","MD","MU",
                             "X1D","X1U","X2D","X2U","WH","HW","TYPE" };

typedef struct { DWORD delay; int type, a, b, c; wchar_t *text; } Ev;

enum { ID_REC = 101, ID_STOP, ID_PLAY, ID_SAVE, ID_LOAD, ID_CLEAR, ID_REPEAT, ID_SPEED, ID_MOVES,
       ID_TEXT = 201, ID_FIXWPM, ID_FIXMS, ID_HUMAN, ID_WPM, ID_MS, ID_DELAY, ID_TYPOS, ID_TYPE, ID_TSTOP,
       ID_TYPOP, ID_BURSTP, ID_PAUSEP,
       ID_LIST = 601, ID_EDELAY, ID_EEVENT, ID_EARGS, ID_EUPD, ID_EINS, ID_EDEL, ID_EUP, ID_EDOWN,
       ID_ESCALE, ID_ESCALEGO, ID_ESTRIP, ID_ESAVE, ID_PRESET, ID_PSAVE, ID_PDEL,
       ID_UCHECK = 701, ID_UINSTALL, ID_UPAGE, ID_KBLOCK, ID_MAGIC, ID_UNOTES,
       ID_TEST = 501, ID_TRESTART, ID_TCLEAR, ID_TNEW, ID_TFIN, ID_TAPPLY, ID_TCLOSE, ID_TEDIT,
       ID_HK0 = 301, ID_HK1, ID_HK2, ID_HK3, ID_HKAPPLY, ID_STATUS = 400 };

#define DEF_TYPO  2.5
#define DEF_BURST 50.0
#define DEF_PAUSE 12.0

enum { HK_REC, HK_PLAY, HK_TYPE, HK_STOP, HK_CLIP, HK_N };
static const wchar_t *HKKEY[HK_N] = { L"Record", L"Play", L"AutoType", L"StopAll", L"TypeClipboard" };
static WORD hkVal[HK_N] = { VK_F9, VK_F10, VK_F11, VK_F8, (WORD)('V' | ((HOTKEYF_CONTROL | HOTKEYF_ALT) << 8)) };

static HWND hMain, hFrame, hBtn[HK_N], hHk[HK_N];
static HHOOK hkKb, hkMs;
static Ev *ev; static int nev, cap;
static volatile LONG recording, playing, typing, stopFlag;
static DWORD lastT, lastMoveT;
static int recMoves = 1, playReps = 1;
static double playSpeed = 1.0;
static wchar_t iniPath[MAX_PATH];
static double qpf;
static BYTE keyDown[256];
static int btnDown[5];
static float dpi = 1.0f, uiScale = 1.0f;

static int S(int v) { return (int)(v * dpi * uiScale + 0.5f); }

static double nowMs(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / qpf; }

static int waitUntil(double target) {
    for (;;) {
        if (stopFlag) return 0;
        double r = target - nowMs();
        if (r <= 0) return 1;
        Sleep(r > 16 ? 10 : (r > 2 ? 1 : 0));
    }
}
static int waitMs(double ms) { return waitUntil(nowMs() + ms); }

static void status(const wchar_t *fmt, ...) {
    wchar_t *b = malloc(512 * sizeof(wchar_t));
    va_list ap; va_start(ap, fmt); _vsnwprintf(b, 511, fmt, ap); va_end(ap); b[511] = 0;
    PostMessageW(hMain, WM_STATUS, 0, (LPARAM)b);
}

static void push(Ev e) {
    if (nev == cap) { cap = cap ? cap * 2 : 4096; ev = realloc(ev, cap * sizeof(Ev)); }
    ev[nev++] = e;
}
static void clearEv(void) { for (int i = 0; i < nev; i++) free(ev[i].text); nev = 0; }

static void rec(DWORD t, int type, int a, int b, int c) {
    Ev e = {0};
    e.delay = nev ? t - lastT : 0; lastT = t;
    e.type = type; e.a = a; e.b = b; e.c = c;
    push(e);
}

static int isHotkeyVk(DWORD vk) {
    for (int i = 0; i < HK_N; i++) if (hkVal[i] && LOBYTE(hkVal[i]) == vk) return 1;
    return 0;
}

static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && recording) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (!(k->flags & LLKHF_INJECTED) && !isHotkeyVk(k->vkCode)) {
            int up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
            rec(k->time, up ? EV_KU : EV_KD, k->vkCode, k->scanCode, (k->flags & LLKHF_EXTENDED) ? 1 : 0);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static int overOwnWindow(POINT p) {
    HWND w = WindowFromPoint(p);
    return w && GetAncestor(w, GA_ROOT) == hFrame;
}

static LRESULT CALLBACK MsProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && recording) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        if (!(m->flags & LLMHF_INJECTED)) {
            int t = -1, d = 0, x1 = HIWORD(m->mouseData) == XBUTTON1;
            switch (wp) {
            case WM_MOUSEMOVE:
                if (recMoves && m->time - lastMoveT >= 8) { t = EV_MV; lastMoveT = m->time; }
                break;
            case WM_LBUTTONDOWN: t = EV_LD; break;
            case WM_LBUTTONUP:   t = EV_LU; break;
            case WM_RBUTTONDOWN: t = EV_RD; break;
            case WM_RBUTTONUP:   t = EV_RU; break;
            case WM_MBUTTONDOWN: t = EV_MD; break;
            case WM_MBUTTONUP:   t = EV_MU; break;
            case WM_XBUTTONDOWN: t = x1 ? EV_X1D : EV_X2D; break;
            case WM_XBUTTONUP:   t = x1 ? EV_X1U : EV_X2U; break;
            case WM_MOUSEWHEEL:  t = EV_WH; d = (short)HIWORD(m->mouseData); break;
            case WM_MOUSEHWHEEL: t = EV_HW; d = (short)HIWORD(m->mouseData); break;
            }
            if (t >= 0 && (t == EV_MV || !overOwnWindow(m->pt)))
                rec(m->time, t, m->pt.x, m->pt.y, d);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

/* ---------- input injection ---------- */

static void sendKey(int vk, int scan, int ext, int up) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = scan ? 0 : (WORD)vk;
    in.ki.wScan = (WORD)scan;
    in.ki.dwFlags = (scan ? KEYEVENTF_SCANCODE : 0) | (up ? KEYEVENTF_KEYUP : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof in);
}

static void sendVk(WORD vk, int up) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

static void sendChar(wchar_t ch) {
    if (ch == L'\n') { sendVk(VK_RETURN, 0); sendVk(VK_RETURN, 1); return; }
    if (ch == L'\t') { sendVk(VK_TAB, 0); sendVk(VK_TAB, 1); return; }
    if (ch == 8)     { sendVk(VK_BACK, 0); sendVk(VK_BACK, 1); return; }
    INPUT in[2] = {0};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = ch;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void sendMouse(int x, int y, DWORD flags, int data) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dx = MulDiv(x - vx, 65535, vw - 1);
    in.mi.dy = MulDiv(y - vy, 65535, vh - 1);
    in.mi.mouseData = data;
    in.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof in);
}

static const DWORD MFLAG[] = { 0, 0, 0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTDOWN,
    MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP,
    MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP };

static void doEv(Ev *e) {
    switch (e->type) {
    case EV_KD: case EV_KU: {
        int up = e->type == EV_KU;
        sendKey(e->a, e->b, e->c, up);
        keyDown[e->a & 255] = !up;
    } break;
    case EV_MV: sendMouse(e->a, e->b, 0, 0); break;
    case EV_WH: sendMouse(e->a, e->b, MOUSEEVENTF_WHEEL, e->c); break;
    case EV_HW: sendMouse(e->a, e->b, MOUSEEVENTF_HWHEEL, e->c); break;
    case EV_TYPE:
        for (wchar_t *p = e->text; p && *p && !stopFlag; p++) { sendChar(*p); Sleep(4); }
        break;
    default: {
        int data = (e->type >= EV_X1D && e->type <= EV_X1U) ? XBUTTON1 : (e->type >= EV_X2D ? XBUTTON2 : 0);
        sendMouse(e->a, e->b, MFLAG[e->type], data);
        int btn = (e->type - EV_LD) / 2, down = !((e->type - EV_LD) & 1);
        if (btn >= 0 && btn < 5) btnDown[btn] = down;
    }
    }
}

static void releaseAll(void) {
    for (int i = 0; i < 256; i++) if (keyDown[i]) { sendVk((WORD)i, 1); keyDown[i] = 0; }
    static const DWORD up[5] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XUP, MOUSEEVENTF_XUP };
    POINT p; GetCursorPos(&p);
    for (int i = 0; i < 5; i++) if (btnDown[i]) { sendMouse(p.x, p.y, up[i], i == 3 ? XBUTTON1 : (i == 4 ? XBUTTON2 : 0)); btnDown[i] = 0; }
}

static int modsDown(void) {
    static const int K[] = { VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN };
    for (int i = 0; i < 5; i++) if (GetAsyncKeyState(K[i]) & 0x8000) return 1;
    return 0;
}
static void waitModsUp(void) {
    double t0 = nowMs();
    while (modsDown() && !stopFlag && nowMs() - t0 < 3000) Sleep(10);
}

static DWORD WINAPI PlayThread(LPVOID unused) {
    (void)unused;
    waitModsUp();
    for (int r = 0; (playReps == 0 || r < playReps) && !stopFlag; r++) {
        if (playReps == 0) status(L"Playing — loop %d (infinite)", r + 1);
        else status(L"Playing — loop %d of %d", r + 1, playReps);
        double t0 = nowMs(), tgt = 0;
        for (int i = 0; i < nev; i++) {
            tgt += ev[i].delay / playSpeed;
            if (!waitUntil(t0 + tgt)) break;
            doEv(&ev[i]);
        }
    }
    releaseAll();
    InterlockedExchange(&playing, 0);
    PostMessageW(hMain, WM_DONE, 0, 0);
    return 0;
}

/* ---------- Instruct / Humanizer ---------- */

typedef struct {
    wchar_t *text; int mode, wpm, ms, delay, typos;
    double typoP, burstP, pauseP;
    int docsSafe, start, pending, stopAt, stopExtra, clip;
} TypeJob;

static volatile int resumeIdx, resumePending;
static wchar_t *resumeText;

static unsigned long long rs = 88172645463325252ULL;
static double rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (rs >> 11) * (1.0 / 9007199254740992.0); }
static double gauss(void) { double u = rnd() + 1e-12, v = rnd(); return sqrt(-2 * log(u)) * cos(6.283185307 * v); }
static double rrange(double a, double b) { return a + (b - a) * rnd(); }

static const char *ROWS[] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static wchar_t neighbor(wchar_t ch) {
    int upper = iswupper(ch); char c = (char)towlower(ch);
    for (int r = 0; r < 3; r++) {
        const char *p = strchr(ROWS[r], c);
        if (!p) continue;
        int i = (int)(p - ROWS[r]), n = (int)strlen(ROWS[r]);
        int j = (i == 0) ? 1 : (i == n - 1) ? i - 1 : (rnd() < 0.5 ? i - 1 : i + 1);
        wchar_t o = ROWS[r][j];
        return upper ? towupper(o) : o;
    }
    return 0;
}

static int needsShift(wchar_t c) { return iswupper(c) || (c && wcschr(L"~!@#$%^&*()_+{}|:\"<>?", c)); }

typedef struct { double base, mul, burstP, pauseP; int burst; } HState;

static double hDelay(HState *s, wchar_t c, wchar_t prev) {
    if (s->burst <= 0) {
        s->burst = 3 + (int)(rnd() * 9);
        s->mul = rnd() < s->burstP ? rrange(0.45, 0.75) : rrange(0.85, 1.15);
    }
    s->burst--;
    double d = s->base * s->mul * (1 + 0.3 * gauss());
    if (c == prev) d *= 0.7;
    if (needsShift(c)) d += s->base * 0.4;
    return d < 18 ? 18 : d;
}

static double hPause(HState *s, wchar_t c) {
    double p = 0;
    if (c == L' ') { if (rnd() < s->pauseP) p = rrange(250, 700); if (rnd() < 0.3) s->burst = 0; }
    else if (c && wcschr(L".!?", c)) { p = rrange(350, 1100); s->burst = 0; }
    else if (c && wcschr(L",;:", c)) p = rrange(150, 450);
    else if (c == L'\n') { p = rrange(400, 1400); s->burst = 0; }
    if (rnd() < s->pauseP * 0.05) p += rrange(900, 2600);
    return p;
}

static int lagLength(void) {
    double r = rnd();
    if (r < 0.25) return 0;
    if (r < 0.60) return 1 + (int)(rnd() * 3);
    if (r < 0.90) return 4 + (int)(rnd() * 5);
    return 9 + (int)(rnd() * 8);
}

static void humanType(TypeJob *j) {
    HState s = { 60000.0 / (j->wpm * 5.0), 1.0, j->burstP / 100.0, j->pauseP / 100.0, 0 };
    double typoP = j->typos ? j->typoP / 100.0 : 0;
    int len = (int)wcslen(j->text), safeUntil = -1, extra = 0;
    rs ^= GetTickCount64() * 2654435761ULL;
    j->stopAt = j->start; j->stopExtra = 0;

    for (int i = j->start; i < len && !stopFlag; i++) {
        wchar_t c = j->text[i], prev = i ? j->text[i - 1] : 0;
        if (c == L'\r') continue;
        if (!waitMs(hDelay(&s, c, prev))) break;

        wchar_t wrong = (i > safeUntil && iswalpha(c) && rnd() < typoP) ? neighbor(c) : 0;
        if (wrong) {
            HLJ(j, i);
            sendChar(wrong); extra = 1; j->stopExtra = extra;
            int want = lagLength(), sent = 0;
            wchar_t pc = wrong;
            for (int k = i + 1; k < len && sent < want && !stopFlag; k++) {
                wchar_t nc = j->text[k];
                if (nc == L'\r') continue;
                if (nc == L'\n') break;
                if (j->docsSafe && !(iswalnum(nc) || nc == L'\'' || nc == 0x2019)) break;
                if (!waitMs(hDelay(&s, nc, pc))) break;
                HLJ(j, k); sendChar(nc); sent++; pc = nc; extra++; j->stopExtra = extra;
            }
            if (stopFlag) break;
            if (!waitMs(rrange(220, 650) + sent * 25)) break;
            for (int k = 0; k <= sent && !stopFlag; k++) {
                HLJ(j, i + sent - k);
                sendChar(8); extra--; j->stopExtra = extra;
                double bd = rrange(45, 95);
                if (rnd() < 0.08) bd += rrange(120, 300);
                if (!waitMs(bd)) break;
            }
            if (stopFlag || !waitMs(rrange(100, 260))) break;
            safeUntil = i + sent + 2;
        }
        HLJ(j, i);
        sendChar(c);
        j->stopAt = i + 1;
        double p = hPause(&s, c);
        if (p > 0 && !waitMs(p)) break;
    }
}

static DWORD WINAPI TypeThread(LPVOID arg) {
    TypeJob *j = arg;
    int len = (int)wcslen(j->text);
    j->stopAt = j->start; j->stopExtra = j->pending;
    for (int s = j->delay; s > 0 && !stopFlag; s--) {
        status(j->start ? L"Resuming in %d… click into the target window" : L"Auto-type starts in %d… click into the target window", s);
        waitMs(1000);
    }
    if (!stopFlag) {
        waitModsUp();
        if (j->clip) status(L"Typing clipboard (%d characters)…", len);
        else status(j->start ? L"Resuming…" : L"Typing…");
        while (j->stopExtra > 0 && !stopFlag) { sendChar(8); j->stopExtra--; waitMs(rrange(60, 110)); }
        if (!stopFlag) {
            if (j->mode == ID_HUMAN) humanType(j);
            else {
                double per = j->mode == ID_FIXMS ? j->ms : 60000.0 / (j->wpm * 5.0);
                double t0 = nowMs(); int n = 0;
                for (int i = j->start; i < len && !stopFlag; i++) {
                    if (n && !waitUntil(t0 + per * n)) break;
                    HLJ(j, i);
                    sendChar(j->text[i]); n++;
                    j->stopAt = i + 1;
                }
            }
        }
    }
    if (!j->clip) {
        if (j->stopAt >= len && j->stopExtra == 0) { resumeIdx = 0; resumePending = 0; HL(-1); }
        else { resumeIdx = j->stopAt; resumePending = j->stopExtra; HL(j->stopAt); }
    }
    free(j->text); free(j);
    InterlockedExchange(&typing, 0);
    PostMessageW(hMain, WM_DONE, 0, 0);
    return 0;
}

/* ---------- file I/O ---------- */

static void putUtf8Escaped(FILE *f, const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n); WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    for (char *p = s; *p; p++) {
        if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p == '\\') fputs("\\\\", f);
        else if (*p != '\r') fputc(*p, f);
    }
    free(s);
}

static int saveMacro(const wchar_t *path) {
    FILE *f = _wfopen(path, L"wb");
    if (!f) return 0;
    fputs("FRUTIGERMACRO 1\n# <delay ms since previous> <event> <args>\n"
          "# KD/KU vk scan ext | MV/LD/LU/RD/RU/MD/MU/X1D/X1U/X2D/X2U x y | WH/HW x y delta | TYPE text\n", f);
    for (int i = 0; i < nev; i++) {
        Ev *e = &ev[i];
        fprintf(f, "%lu %s ", (unsigned long)e->delay, EVN[e->type]);
        if (e->type == EV_KD || e->type == EV_KU) {
            wchar_t nm[64] = L""; char u8[128] = "";
            GetKeyNameTextW((e->b << 16) | (e->c << 24), nm, 64);
            WideCharToMultiByte(CP_UTF8, 0, nm, -1, u8, sizeof u8, NULL, NULL);
            fprintf(f, "%d %d %d  # %s\n", e->a, e->b, e->c, u8);
        } else if (e->type == EV_WH || e->type == EV_HW) fprintf(f, "%d %d %d\n", e->a, e->b, e->c);
        else if (e->type == EV_TYPE) { putUtf8Escaped(f, e->text ? e->text : L""); fputc('\n', f); }
        else fprintf(f, "%d %d\n", e->a, e->b);
    }
    fclose(f);
    return 1;
}

static int loadMacro(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    clearEv();
    static char line[65536];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || !strncmp(p, "DOOGMACRO", 9) || !strncmp(p, "FRUTIGERMACRO", 13)) continue;
        unsigned long d; char name[16]; int off = 0;
        if (sscanf(p, "%lu %15s%n", &d, name, &off) < 2) continue;
        int t = -1;
        for (int i = 0; i < EV_COUNT; i++) if (!_stricmp(name, EVN[i])) t = i;
        if (t < 0) continue;
        Ev e = {0}; e.delay = d; e.type = t;
        char *rest = p + off;
        if (t == EV_TYPE) {
            if (*rest == ' ') rest++;
            char *o = rest;
            for (char *q = rest; *q; q++) {
                if (*q == '\\' && q[1]) { q++; *o++ = *q == 'n' ? '\n' : *q == 't' ? '\t' : *q; }
                else *o++ = *q;
            }
            *o = 0;
            int n = MultiByteToWideChar(CP_UTF8, 0, rest, -1, NULL, 0);
            e.text = malloc(n * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, rest, -1, e.text, n);
        } else {
            char *h = strchr(rest, '#'); if (h) *h = 0;
            sscanf(rest, "%d %d %d", &e.a, &e.b, &e.c);
        }
        push(e);
    }
    fclose(f);
    return 1;
}

static int fileDlg(wchar_t *buf, int save) {
    OPENFILENAMEW o = {0};
    buf[0] = 0;
    o.lStructSize = sizeof o; o.hwndOwner = hMain;
    o.lpstrFilter = L"FrutigerMacro macros (*.fmac;*.dmac)\0*.fmac;*.dmac\0All files\0*.*\0";
    o.lpstrFile = buf; o.nMaxFile = MAX_PATH; o.lpstrDefExt = L"fmac";
    o.Flags = save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST;
    return save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
}

/* ---------- hotkeys ---------- */

static void hkName(WORD hk, wchar_t *out, int n) {
    out[0] = 0;
    if (!hk) { wcscpy(out, L"none"); return; }
    BYTE vk = LOBYTE(hk), m = HIBYTE(hk);
    if (m & HOTKEYF_CONTROL) wcscat(out, L"Ctrl+");
    if (m & HOTKEYF_SHIFT) wcscat(out, L"Shift+");
    if (m & HOTKEYF_ALT) wcscat(out, L"Alt+");
    wchar_t k[32] = L"?";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;
    if ((vk >= VK_PRIOR && vk <= VK_HELP) || vk == VK_DIVIDE || vk == VK_NUMLOCK) lp |= 1 << 24;
    if (vk >= VK_F1 && vk <= VK_F24) swprintf(k, 32, L"F%d", vk - VK_F1 + 1);
    else GetKeyNameTextW(lp, k, 32);
    wcsncat(out, k, n - wcslen(out) - 1);
}

static const wchar_t *BTNBASE[HK_N] = { L"Record", L"Play", L"Type", L"Stop", L"Clip" };
static void setTypeLabel(void);

static UINT hkMods(WORD v) {
    BYTE m = HIBYTE(v);
    return MOD_NOREPEAT | ((m & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) |
           ((m & HOTKEYF_SHIFT) ? MOD_SHIFT : 0) | ((m & HOTKEYF_ALT) ? MOD_ALT : 0);
}
static void unregisterAll(void) { for (int i = 0; i < HK_N; i++) UnregisterHotKey(hMain, i + 1); }
static void registerAll(void) {
    for (int i = 0; i < HK_N; i++) if (hkVal[i]) RegisterHotKey(hMain, i + 1, hkMods(hkVal[i]), LOBYTE(hkVal[i]));
}

static void applyHotkeys(int fromControls) {
    wchar_t nm[64], lbl[96], bad[256] = L"";
    for (int i = 0; i < HK_N; i++) {
        UnregisterHotKey(hMain, i + 1);
        if (fromControls) hkVal[i] = (WORD)SendMessageW(hHk[i], HKM_GETHOTKEY, 0, 0);
        BYTE m = HIBYTE(hkVal[i]);
        UINT mods = MOD_NOREPEAT | ((m & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) |
                    ((m & HOTKEYF_SHIFT) ? MOD_SHIFT : 0) | ((m & HOTKEYF_ALT) ? MOD_ALT : 0);
        hkName(hkVal[i], nm, 64);
        if (hkVal[i] && !RegisterHotKey(hMain, i + 1, mods, LOBYTE(hkVal[i]))) {
            wcscat(bad, nm); wcscat(bad, L" ");
        }
        swprintf(lbl, 96, L"%s (%s)", (i == HK_TYPE && resumeIdx > 0) ? L"Resume" : BTNBASE[i], nm);
        if (hBtn[i]) SetWindowTextW(hBtn[i], lbl);
        if (!fromControls) SendMessageW(hHk[i], HKM_SETHOTKEY, hkVal[i], 0);
        wchar_t v[16]; swprintf(v, 16, L"%u", hkVal[i]);
        WritePrivateProfileStringW(L"Hotkeys", HKKEY[i], v, iniPath);
    }
    if (bad[0]) status(L"Couldn't register %s— another app is using it. Pick a different combo.", bad);
    else if (fromControls) status(L"Hotkeys applied and saved.");
}

/* ---------- actions ---------- */

/* ---------- Frutiger Aero renderer ---------- */

typedef struct { int w, h; DWORD *px; HDC dc; HBITMAP bm, old; } Canvas;

static Canvas mkCanvas(int w, int h) {
    Canvas c = { w, h, NULL, NULL, NULL, NULL };
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    c.dc = CreateCompatibleDC(NULL);
    c.bm = CreateDIBSection(c.dc, &bi, DIB_RGB_COLORS, (void **)&c.px, NULL, 0);
    c.old = SelectObject(c.dc, c.bm);
    return c;
}
static void freeCanvas(Canvas *c) { SelectObject(c->dc, c->old); DeleteObject(c->bm); DeleteDC(c->dc); }

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int lerpCol(int a, int b, double t) {
    int r = ((a >> 16) & 255) + (int)((((b >> 16) & 255) - ((a >> 16) & 255)) * t);
    int g = ((a >> 8) & 255) + (int)((((b >> 8) & 255) - ((a >> 8) & 255)) * t);
    int l = (a & 255) + (int)(((b & 255) - (a & 255)) * t);
    return (r << 16) | (g << 8) | l;
}
static void blend(Canvas *c, int x, int y, int col, double a) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || a <= 0) return;
    if (a > 1) a = 1;
    DWORD *p = c->px + y * c->w + x;
    *p = (DWORD)lerpCol((int)(*p & 0xffffff), col, a);
}

static double sdRR(double px, double py, double x, double y, double w, double h, double r) {
    double qx = fabs(px - (x + w / 2)) - (w / 2 - r), qy = fabs(py - (y + h / 2)) - (h / 2 - r);
    return hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0) - r;
}

#define BBOX(BX, BY, BW, BH, BP) \
    int x0 = (int)floor((BX) - (BP)), y0 = (int)floor((BY) - (BP)), x1 = (int)ceil((BX) + (BW) + (BP)), y1 = (int)ceil((BY) + (BH) + (BP)); \
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;

static void rrFill(Canvas *c, double x, double y, double w, double h, double r, int c1, int c2, double a1, double a2) {
    BBOX(x, y, w, h, 1)
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double cov = clamp01(0.5 - sdRR(px + .5, py + .5, x, y, w, h, r));
        if (cov <= 0) continue;
        double t = clamp01((py + .5 - y) / h);
        blend(c, px, py, lerpCol(c1, c2, t), (a1 + (a2 - a1) * t) * cov);
    }
}

static void rrStroke(Canvas *c, double x, double y, double w, double h, double r, int col, double a, double width, int outside) {
    BBOX(x, y, w, h, width + 1)
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double d = sdRR(px + .5, py + .5, x, y, w, h, r);
        double cov = outside ? clamp01(d + .5) * clamp01(width + .5 - d) : clamp01(.5 - d) * clamp01(d + width + .5);
        if (cov > 0) blend(c, px, py, col, a * cov);
    }
}

static void rrGloss(Canvas *c, double x, double y, double w, double h, double r, double frac, double aTop, double aBot) {
    double gh = h * frac;
    BBOX(x, y, w, gh, 1)
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double cov = clamp01(0.5 - sdRR(px + .5, py + .5, x + 1, y + 1, w - 2, h - 2, r - 1));
        cov *= clamp01(y + gh - (py + .5) + .5);
        if (cov <= 0) continue;
        double t = clamp01((py + .5 - y) / gh);
        blend(c, px, py, 0xffffff, (aTop + (aBot - aTop) * t) * cov);
    }
}

static void rrGlow(Canvas *c, double x, double y, double w, double h, double r, double gx, double gy, double rad, int col, double a) {
    BBOX(x, y, w, h, 1)
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double cov = clamp01(0.5 - sdRR(px + .5, py + .5, x, y, w, h, r));
        if (cov <= 0) continue;
        double f = clamp01(1 - hypot((px + .5 - gx) / rad, (py + .5 - gy) / (rad * 0.55)));
        if (f > 0) blend(c, px, py, col, a * cov * f * f);
    }
}

static void rrShadow(Canvas *c, double x, double y, double w, double h, double r, double blur, double a) {
    BBOX(x, y, w, h, blur)
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double d = sdRR(px + .5, py + .5, x, y, w, h, r);
        if (d <= 0) continue;
        double f = clamp01(1 - d / blur);
        blend(c, px, py, 0x0a2a4a, a * f * f);
    }
}

static void disc(Canvas *c, double cx, double cy, double rad, int col, double a, double soft) {
    BBOX(cx - rad, cy - rad, rad * 2, rad * 2, soft + 1)
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double d = hypot(px + .5 - cx, py + .5 - cy) - rad;
        double cov = clamp01(.5 - d / soft);
        if (cov > 0) blend(c, px, py, col, a * cov);
    }
}

static void ring(Canvas *c, double cx, double cy, double rad, int col, double a, double width) {
    BBOX(cx - rad, cy - rad, rad * 2, rad * 2, 2)
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double d = hypot(px + .5 - cx, py + .5 - cy) - rad;
        double cov = clamp01(.5 - d) * clamp01(d + width + .5);
        if (cov > 0) blend(c, px, py, col, a * cov);
    }
}

static void segment(Canvas *c, double ax, double ay, double bx, double by, double th, int col, double a) {
    double mnx = fmin(ax, bx), mny = fmin(ay, by);
    BBOX(mnx, mny, fabs(bx - ax), fabs(by - ay), th + 1)
    double dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy;
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        double qx = px + .5 - ax, qy = py + .5 - ay;
        double t = clamp01((qx * dx + qy * dy) / L2);
        double d = hypot(qx - dx * t, qy - dy * t);
        double cov = clamp01(th / 2 + .5 - d);
        if (cov > 0) blend(c, px, py, col, a * cov);
    }
}

static void bubble(Canvas *c, double cx, double cy, double r) {
    disc(c, cx, cy, r, 0xffffff, 0.10, 1);
    disc(c, cx + r * 0.25, cy + r * 0.3, r * 0.65, 0xbff4ff, 0.10, r * 0.5);
    ring(c, cx, cy, r, 0xffffff, 0.55, fmax(1.2, r * 0.05));
    disc(c, cx - r * 0.38, cy - r * 0.42, r * 0.26, 0xffffff, 0.75, r * 0.18);
    disc(c, cx + r * 0.42, cy + r * 0.48, r * 0.1, 0xffffff, 0.45, r * 0.08);
}



/* ---------- updater ---------- */

#define APP_VERSION   "1.14.6"
#define APP_VERSION_W L"1.14.6"
#define WM_UPDCHECKED (WM_APP + 5)
#define WM_UPDFAIL    (WM_APP + 6)
#define WM_UPDRESTART (WM_APP + 7)

enum { CONN_UNKNOWN, CONN_CHECKING, CONN_ONLINE, CONN_OFFLINE, CONN_ERROR };
static int updConn = CONN_UNKNOWN, updAvail, justUpdated;
static volatile LONG updBusy;
static wchar_t updRepo[128] = L"binjin-maker/FAUP";
static wchar_t updLatest[32], updUrl[1024], updPage[512], updDigest[80], updConnMsg[160];
static wchar_t *updNotes;
static int updErr;
static SYSTEMTIME updWhen; static int updChecked;
static wchar_t exePath[MAX_PATH];
static void saveSettings(void);
static int busy(void);
static volatile LONG kbBlocked, magicOn;

static int httpGet(const wchar_t *url, const wchar_t *hdrs, char **out, size_t *outLen, DWORD *httpSt, int progress) {
    *out = NULL; *outLen = 0; *httpSt = 0;
    HINTERNET in = InternetOpenW(L"FrutigerMacro-Updater/" APP_VERSION_W, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!in) return (int)GetLastError();
    DWORD to = 15000;
    InternetSetOptionW(in, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
    InternetSetOptionW(in, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
    HINTERNET h = InternetOpenUrlW(in, url, hdrs, hdrs ? (DWORD)-1 : 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI, 0);
    if (!h) { int e = (int)GetLastError(); InternetCloseHandle(in); return e ? e : -1; }
    DWORD st = 0, sl = sizeof st, total = 0, tl = sizeof total;
    HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &st, &sl, NULL);
    HttpQueryInfoW(h, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &tl, NULL);
    *httpSt = st;
    size_t cap = 1 << 16, len = 0; char *b = malloc(cap + 1); DWORD got = 0; int err = 0, lastPct = -1;
    for (;;) {
        if (len + 65536 > cap) { cap *= 2; b = realloc(b, cap + 1); }
        if (!InternetReadFile(h, b + len, 65536, &got)) { err = (int)GetLastError(); if (!err) err = -1; break; }
        if (!got) break;
        len += got;
        if (progress && total) {
            int pct = (int)(len * 100 / total);
            if (pct != lastPct) { lastPct = pct; status(L"Downloading update… %d%%", pct); }
        }
        if (len > (64u << 20)) { err = -2; break; }
    }
    InternetCloseHandle(h); InternetCloseHandle(in);
    if (err) { free(b); return err; }
    b[len] = 0; *out = b; *outLen = len;
    return 0;
}

static char *jsonStr(const char *json, const char *key, const char *from, const char *until) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(from ? from : json, pat);
    if (!p || (until && p >= until)) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 256, n = 0; char *o = malloc(cap);
    while (*p && *p != '"') {
        if (n + 8 > cap) { cap *= 2; o = realloc(o, cap); }
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': o[n++] = '\n'; break; case 'r': break; case 't': o[n++] = '\t'; break;
            case 'u': {
                unsigned u = 0; sscanf(p + 1, "%4x", &u); p += 4;
                if (u < 0x80) o[n++] = (char)u;
                else if (u < 0x800) { o[n++] = (char)(0xC0 | (u >> 6)); o[n++] = (char)(0x80 | (u & 63)); }
                else { o[n++] = (char)(0xE0 | (u >> 12)); o[n++] = (char)(0x80 | ((u >> 6) & 63)); o[n++] = (char)(0x80 | (u & 63)); }
            } break;
            default: o[n++] = *p;
            }
            p++;
        } else o[n++] = *p++;
    }
    o[n] = 0;
    return o;
}

static void u8w(const char *s, wchar_t *out, int n) { MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, n); out[n - 1] = 0; }

static void verParse(const wchar_t *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    while (*s && !iswdigit(*s)) s++;
    swscanf(s, L"%d.%d.%d", &v[0], &v[1], &v[2]);
}
static int verNewer(const wchar_t *a, const wchar_t *b) {
    int x[3], y[3]; verParse(a, x); verParse(b, y);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    return 0;
}

static wchar_t *cleanNotes(const char *md) {
    int n = MultiByteToWideChar(CP_UTF8, 0, md, -1, NULL, 0);
    wchar_t *w = malloc(n * sizeof(wchar_t)), *o = malloc((n + 16) * sizeof(wchar_t)), *q = o;
    MultiByteToWideChar(CP_UTF8, 0, md, -1, w, n);
    int bol = 1;
    for (wchar_t *p = w; *p; p++) {
        if (bol) {
            while (*p == L'#' || *p == L'>') p++;
            while (*p == L' ') p++;
            if ((*p == L'-' || *p == L'*') && p[1] == L' ') { *q++ = L'•'; *q++ = L' '; p += 2; }
            if (!*p) break;
        }
        if (*p == L'*' || *p == L'`' || (*p == L'_' && p[1] == L'_')) { bol = 0; continue; }
        *q++ = *p;
        bol = *p == L'\n';
    }
    *q = 0; free(w);
    return o;
}

static DWORD WINAPI CheckThread(LPVOID unused) {
    (void)unused;
    wchar_t url[300];
    swprintf(url, 300, L"https://raw.githubusercontent.com/%s/main/update/latest.json?t=%llu", updRepo, (unsigned long long)GetTickCount64());
    char *b; size_t len; DWORD st;
    int e = httpGet(url, L"Cache-Control: no-cache\r\n", &b, &len, &st, 0);
    int conn = CONN_ONLINE, avail = 0;
    wchar_t latest[32] = L"", dl[1024] = L"", page[512] = L"", dig[80] = L"", msg[160] = L"";
    wchar_t *notes = NULL;
    swprintf(page, 512, L"https://github.com/%s", updRepo);
    if (e) {
        conn = (e == ERROR_INTERNET_NAME_NOT_RESOLVED || e == ERROR_INTERNET_CANNOT_CONNECT ||
                e == ERROR_INTERNET_TIMEOUT || e == ERROR_INTERNET_DISCONNECTED) ? CONN_OFFLINE : CONN_ERROR;
        if (conn == CONN_OFFLINE) wcscpy(msg, L"Offline — no internet connection (everything else still works)");
        else swprintf(msg, 160, L"Couldn't reach GitHub (network error %d) — maybe a school/work firewall", e);
    } else if (st == 404) {
        wcscpy(msg, L"Online — but no update has been published to the repo yet");
    } else if (st == 403 || st == 429) {
        conn = CONN_ERROR;
        wcscpy(msg, L"Online — GitHub rate-limited the check, try again in a bit");
    } else if (st != 200) {
        conn = CONN_ERROR;
        swprintf(msg, 160, L"Online — update server answered HTTP %lu", (unsigned long)st);
    } else {
        wcscpy(msg, L"Online — connected to GitHub");
        char *ver = jsonStr(b, "version", NULL, NULL), *u8 = jsonStr(b, "url", NULL, NULL);
        char *sha = jsonStr(b, "sha256", NULL, NULL), *nt = jsonStr(b, "notes", NULL, NULL);
        if (ver) { latest[0] = L'v'; u8w(ver, latest + 1, 31); }
        u8w(u8, dl, 1024); u8w(sha, dig, 80);
        notes = cleanNotes(nt ? nt : "");
        if (latest[0] && verNewer(latest, APP_VERSION_W)) avail = dl[0] ? 1 : 2;
        free(ver); free(u8); free(sha); free(nt);
    }
    free(b);
    wcscpy(updLatest, latest); wcscpy(updUrl, dl); wcscpy(updPage, page); wcscpy(updDigest, dig); wcscpy(updConnMsg, msg);
    free(updNotes); updNotes = notes;
    updConn = conn; updAvail = avail;
    GetLocalTime(&updWhen); updChecked = 1;
    InterlockedExchange(&updBusy, 0);
    PostMessageW(hMain, WM_UPDCHECKED, 0, 0);
    return 0;
}

static void fillNotes(void) {
    const wchar_t *src = (updNotes && updNotes[0]) ? updNotes :
        updConn == CONN_OFFLINE ? L"Changelog unavailable offline." :
        updConn == CONN_CHECKING ? L"Checking for updates…" :
        updLatest[0] ? L"No release notes for this version." : L"No changelog yet — press Check now.";
    size_t n = wcslen(src);
    wchar_t *o = malloc((n * 2 + 1) * sizeof(wchar_t)), *q = o;
    for (const wchar_t *p = src; *p; p++) { if (*p == L'\n') *q++ = L'\r'; *q++ = *p; }
    *q = 0;
    SetDlgItemTextW(hMain, ID_UNOTES, o);
    free(o);
}

static void startCheck(void) {
    if (InterlockedCompareExchange(&updBusy, 1, 0)) return;
    updConn = CONN_CHECKING; updErr = 0;
    wcscpy(updConnMsg, L"Checking…");
    fillNotes();
    InvalidateRect(hMain, NULL, FALSE);
    CloseHandle(CreateThread(NULL, 0, CheckThread, NULL, 0, NULL));
}

static int sha256Hex(const char *data, size_t len, wchar_t *out) {
    BCRYPT_ALG_HANDLE alg; UCHAR h[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    BCRYPT_HASH_HANDLE hh;
    int ok = !BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) &&
             !BCryptHashData(hh, (PUCHAR)data, (ULONG)len, 0) && !BCryptFinishHash(hh, h, 32, 0);
    BCryptDestroyHash(hh); BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; ok && i < 32; i++) swprintf(out + i * 2, 3, L"%02x", h[i]);
    return ok;
}

static void updFail(const wchar_t *m) {
    wchar_t *c = _wcsdup(m);
    InterlockedExchange(&updBusy, 0);
    PostMessageW(hMain, WM_UPDFAIL, 0, (LPARAM)c);
}

static DWORD WINAPI InstallThread(LPVOID unused) {
    (void)unused;
    status(L"Downloading update %s…", updLatest);
    char *b; size_t len; DWORD st;
    int e = httpGet(updUrl, NULL, &b, &len, &st, 1);
    if (e || st != 200) { free(b); updFail(L"Update download failed — check your connection and try again."); return 0; }
    if (len < 20000 || b[0] != 'M' || b[1] != 'Z') { free(b); updFail(L"The downloaded file isn't a valid EXE, so nothing was changed."); return 0; }
    if (updDigest[0]) {
        wchar_t hex[80] = L"";
        if (!sha256Hex(b, len, hex) || _wcsicmp(hex, updDigest)) { free(b); updFail(L"Update failed its checksum check, so nothing was changed."); return 0; }
    }
    wchar_t newPath[MAX_PATH + 16], oldPath[MAX_PATH + 16];
    swprintf(newPath, MAX_PATH + 16, L"%s.new", exePath);
    HANDLE f = CreateFileW(newPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, b, (DWORD)len, &wr, NULL) && wr == len;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(b);
    if (!ok) { DeleteFileW(newPath); updFail(L"Couldn't write the update next to the EXE — the folder may be write-protected."); return 0; }
    /* pick a backup name that is free (or a stale one we can delete) */
    int chosen = 0;
    for (int slot = 0; slot < 64 && !chosen; slot++) {
        if (slot == 0) swprintf(oldPath, MAX_PATH + 16, L"%s.old", exePath);
        else swprintf(oldPath, MAX_PATH + 16, L"%s.old%d", exePath, slot);
        DWORD at = GetFileAttributesW(oldPath);
        if (at == INVALID_FILE_ATTRIBUTES) chosen = 1;
        else if (DeleteFileW(oldPath)) chosen = 1;
    }
    if (!chosen) { DeleteFileW(newPath); updFail(L"Couldn't clear an old backup next to the EXE."); return 0; }
    int moved = 0;
    for (int i = 0; i < 12 && !moved; i++) { if (MoveFileExW(exePath, oldPath, MOVEFILE_REPLACE_EXISTING)) moved = 1; else Sleep(120); }
    if (!moved) { DeleteFileW(newPath); updFail(L"Couldn't move the running EXE aside — close other copies and retry, or the folder is write-protected."); return 0; }
    int placed = 0;
    for (int i = 0; i < 12 && !placed; i++) { if (MoveFileExW(newPath, exePath, MOVEFILE_REPLACE_EXISTING)) placed = 1; else Sleep(120); }
    if (!placed) { MoveFileExW(oldPath, exePath, MOVEFILE_REPLACE_EXISTING); DeleteFileW(newPath); updFail(L"Couldn't put the new EXE in place; kept the current version."); return 0; }
    MoveFileExW(oldPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    DeleteFileW(oldPath);
    PostMessageW(hMain, WM_UPDRESTART, 0, 0);
    return 0;
}

static void startInstall(void) {
    if (updAvail != 1) { status(updAvail == 2 ? L"The new release has no .exe attached, so it can't auto-install." : L"No update to install."); return; }
    if (busy()) { status(L"Stop recording/playing/typing before updating."); return; }
    if (InterlockedCompareExchange(&updBusy, 1, 0)) return;
    CloseHandle(CreateThread(NULL, 0, InstallThread, NULL, 0, NULL));
}

static void restartUpdated(void) {
    saveSettings();
    wchar_t cmd[MAX_PATH * 2];
    swprintf(cmd, MAX_PATH * 2, L"\"%s\" --updated", exePath);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    unregisterAll();
    if (CreateProcessW(exePath, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DestroyWindow(hFrame);
    } else {
        registerAll();
        status(L"Update installed — close and reopen FrutigerMacro to finish.");
    }
}

static void updInfo(wchar_t *out, int n) {
    wchar_t when[64] = L"never";
    if (updChecked) {
        wchar_t t[32];
        GetTimeFormatEx(NULL, TIME_NOSECONDS, &updWhen, NULL, t, 32);
        swprintf(when, 64, L"%s", t);
    }
    const wchar_t *lat = updLatest[0] ? updLatest : (updConn == CONN_CHECKING ? L"checking…" : L"unknown");
    const wchar_t *note = updAvail == 1 ? L"  — update available!" : updAvail == 2 ? L"  — newer, but no .exe attached" :
                          (updLatest[0] ? L"  — you're up to date" : L"");
    swprintf(out, n,
        L"Connection:  %s\n"
        L"Installed:  v%s        Latest:  %s%s\n"
        L"Source:  github.com/%s   ·   last checked %s",
        updConnMsg[0] ? updConnMsg : L"Not checked yet", APP_VERSION_W, lat, note, updRepo, when);
}

static int connColor(void) {
    if (updErr || updConn == CONN_OFFLINE || updConn == CONN_ERROR) return 0xe0483a;
    if (updConn == CONN_ONLINE) return updAvail ? 0xf5c21a : 0x34c23a;
    return 0x9aa8b8;
}


/* ---------- pages, layout, painting ---------- */

enum { PG_MACRO, PG_INSTR, PG_EDIT, PG_TEST, PG_UPDATE, PG_N, PG_GLOBAL = -1 };
static const wchar_t *TABNAME[PG_N] = { L"Recorder", L"Instruct", L"Editor", L"Test", L"Update" };
static int curPage = PG_INSTR, tabHover = -1;

static Canvas pageC[PG_N], backC;
static HFONT hFont, hBold, hTitle, hSmall, hPrompt;
static HWND hoverBtn;
static wchar_t statusText[512] = L"";
static wchar_t curFile[MAX_PATH] = L"";

#define NAVY RGB(14, 52, 92)
#define HINT RGB(70, 110, 150)

static RECT RS(int x, int y, int w, int h) { RECT r = { S(x), S(y), S(x + w), S(y + h) }; return r; }

static void glassPanel(Canvas *c, int lx, int ly, int lw, int lh) {
    double x = S(lx), y = S(ly), w = S(lw), h = S(lh), r = S(12);
    rrShadow(c, x, y + S(3), w, h, r, S(10), 0.22);
    rrFill(c, x, y, w, h, r, 0xffffff, 0xe6f7ff, 0.66, 0.42);
    rrGloss(c, x, y, w, h, r, 0.32, 0.55, 0.08);
    rrStroke(c, x, y, w, h, r, 0x1f5f94, 0.38, 1, 1);
    rrStroke(c, x, y, w, h, r, 0xffffff, 0.95, 1.3, 0);
}

static void inset(Canvas *c, int lx, int ly, int lw, int lh) {
    rrFill(c, S(lx), S(ly), S(lw), S(lh), S(8), 0xffffff, 0xf2faff, 0.7, 0.5);
    rrStroke(c, S(lx), S(ly), S(lw), S(lh), S(8), 0x2b7fc4, 0.35, 1, 0);
}

static void textAt(HDC dc, HFONT f, const wchar_t *t, int lx, int ly, int lw, int lh, COLORREF col, UINT fmt) {
    RECT r = RS(lx, ly, lw, lh);
    SelectObject(dc, f);
    SetTextColor(dc, col);
    DrawTextW(dc, t, -1, &r, fmt);
}

enum { TG_MOVES, TG_FIXWPM, TG_FIXMS, TG_HUMAN, TG_TYPOS, TG_DOCS, TG_UCHECK, TG_UAUTO, TG_N };
typedef struct { const wchar_t *t; int page, x, y, w, radio, on; } Tog;
static Tog tog[TG_N] = {
    { L"Record mouse moves",    PG_MACRO, 272, 112, 148, 0, 1 },
    { L"Fixed WPM",             PG_INSTR,  20, 218, 100, 1, 0 },
    { L"Fixed ms/char",         PG_INSTR, 122, 218, 112, 1, 0 },
    { L"Humanizer",             PG_INSTR, 236, 218, 102, 1, 1 },
    { L"Typos",                 PG_INSTR, 342, 218,  78, 0, 1 },
    { L"Docs-safe",             PG_INSTR, 332, 280,  88, 0, 1 },
    { L"Check on launch",       PG_UPDATE,  20, 296, 180, 0, 1 },
    { L"Auto-install updates",  PG_UPDATE, 220, 296, 200, 0, 0 },
};

typedef struct { const wchar_t *t; int page, x, y, w; } Lbl;
static const Lbl LBLS[] = {
    { L"Repeat (0 = ∞):", PG_MACRO, 20, 115, 92 }, { L"Speed ×", PG_MACRO, 166, 115, 46 },
    { L"WPM:", PG_INSTR, 20, 251, 40 }, { L"ms/char:", PG_INSTR, 112, 251, 60 }, { L"Start delay (s):", PG_INSTR, 226, 251, 102 },
    { L"Typo %:", PG_INSTR, 20, 283, 52 }, { L"Burst %:", PG_INSTR, 116, 283, 58 }, { L"Pause %:", PG_INSTR, 220, 283, 60 },
    { L"Record:", PG_GLOBAL, 20, 403, 62 }, { L"Play:", PG_GLOBAL, 222, 403, 62 },
    { L"Auto-type:", PG_GLOBAL, 20, 431, 62 }, { L"Stop all:", PG_GLOBAL, 222, 431, 62 },
    { L"Clipboard:", PG_GLOBAL, 20, 459, 62 },
    { L"Preset:", PG_INSTR, 20, 191, 44 },
    { L"Delay:", PG_EDIT, 20, 251, 40 }, { L"Event:", PG_EDIT, 124, 251, 40 }, { L"Args:", PG_EDIT, 266, 251, 32 },
    { L"Scale delays ×", PG_EDIT, 20, 319, 88 },
};

static int onPage(int tag, int p) { return tag == p || (tag == PG_GLOBAL && p != PG_TEST); }

static void renderSky(Canvas *c) {
    int W = c->w, H = c->h;
    double sx = W * 0.88, sy = -H * 0.04;
    for (int y = 0; y < H; y++) {
        double t = (double)y / H;
        int col = t < 0.45 ? lerpCol(0x1688d6, 0x8ad4f7, t / 0.45) : lerpCol(0x8ad4f7, 0xeefbff, (t - 0.45) / 0.55);
        for (int x = 0; x < W; x++) {
            double dx = x - sx, dy = y - sy, d = hypot(dx, dy) / (W * 0.9);
            double glow = clamp01(1 - d); glow *= glow;
            double ray = sin(atan2(dy, dx) * 16) * 0.5 + 0.5;
            ray = ray > 0.72 ? (ray - 0.72) / 0.28 : 0;
            c->px[y * W + x] = (DWORD)lerpCol(col, 0xffffff, clamp01(glow * 0.6 + ray * 0.10 * clamp01(1.2 - d)));
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        double base = pass ? H * 0.90 : H * 0.83, amp = pass ? S(14) : S(22);
        int top = pass ? 0x7fdc46 : 0xa8e86a, bot = pass ? 0x2f8f23 : 0x4fae34;
        for (int x = 0; x < W; x++) {
            double cy = base + amp * sin(x / (double)W * (pass ? 4.2 : 2.6) + (pass ? 1.9 : 0.4));
            for (int y = (int)cy - 1; y < H; y++) {
                if (y < 0) continue;
                double cov = clamp01(y + .5 - cy + .5);
                blend(c, x, y, lerpCol(top, bot, clamp01((y - cy) / (H - cy + 1))), cov);
                double crest = (y - cy) / S(4);
                if (crest >= 0 && crest < 1) blend(c, x, y, 0xffffff, 0.45 * (1 - crest));
            }
        }
    }
    static const float BUB[][3] = {
        { 0.06f, 0.10f, 26 }, { 0.93f, 0.17f, 18 }, { 0.80f, 0.06f, 10 }, { 0.04f, 0.42f, 14 }, { 0.97f, 0.45f, 32 },
        { 0.12f, 0.70f, 22 }, { 0.88f, 0.72f, 12 }, { 0.50f, 0.03f, 8 },  { 0.30f, 0.20f, 6 },  { 0.70f, 0.38f, 9 },
        { 0.02f, 0.92f, 30 }, { 0.62f, 0.95f, 16 }, { 0.38f, 0.60f, 7 },  { 0.97f, 0.97f, 20 }, { 0.22f, 0.98f, 10 },
    };
    for (unsigned i = 0; i < sizeof BUB / sizeof BUB[0]; i++) bubble(c, BUB[i][0] * W, BUB[i][1] * H, S((int)BUB[i][2]));
}

static void renderPages(void) {
    renderSky(&pageC[0]);
    for (int p = 1; p < PG_N; p++) memcpy(pageC[p].px, pageC[0].px, (size_t)pageC[0].w * pageC[0].h * 4);
    for (int p = 0; p < PG_N; p++) {
        Canvas *c = &pageC[p];
        HDC dc = c->dc;
        glassPanel(c, 10, 8, 420, 32);
        if (p == PG_MACRO) { glassPanel(c, 10, 48, 420, 320); inset(c, 20, 146, 400, 212); }
        if (p == PG_INSTR || p == PG_EDIT) glassPanel(c, 10, 48, 420, 320);
        if (p == PG_UPDATE) { glassPanel(c, 10, 48, 420, 320); inset(c, 20, 72, 400, 84); inset(c, 20, 164, 400, 126); }
        if (p == PG_TEST) { glassPanel(c, 10, 48, 420, 490); inset(c, 20, 90, 400, 104); inset(c, 20, 320, 400, 160); }
        if (p != PG_TEST) { glassPanel(c, 10, 378, 420, 112); glassPanel(c, 10, 498, 420, 40); }
        SetBkMode(dc, TRANSPARENT);
        if (p == PG_MACRO) textAt(dc, hTitle, L"Macro Recorder", 20, 50, 300, 18, NAVY, DT_LEFT | DT_SINGLELINE);
        if (p == PG_INSTR) {
            textAt(dc, hTitle, L"Instruct (auto-type)", 20, 50, 300, 18, NAVY, DT_LEFT | DT_SINGLELINE);
            textAt(dc, hSmall, L"% fields: blank = default", 200, 53, 220, 16, HINT, DT_RIGHT | DT_SINGLELINE);
        }
        if (p == PG_UPDATE) {
            textAt(dc, hTitle, L"Updates", 20, 50, 300, 18, NAVY, DT_LEFT | DT_SINGLELINE);
            textAt(dc, hSmall, L"pulled from GitHub · works offline", 200, 53, 220, 16, HINT, DT_RIGHT | DT_SINGLELINE);
        }
        if (p == PG_EDIT) {
            textAt(dc, hTitle, L"Macro Editor", 20, 50, 300, 18, NAVY, DT_LEFT | DT_SINGLELINE);
            textAt(dc, hSmall, L"select a row to load it into the fields", 200, 53, 220, 16, HINT, DT_RIGHT | DT_SINGLELINE);
            textAt(dc, hSmall, L"Args → keys: Enter, A, F5 or vk scan ext · mouse: x y (blank = cursor) · wheel: x y delta · TYPE: text, \\n = newline",
                   20, 348, 404, 16, HINT, DT_LEFT | DT_SINGLELINE);
        }
        if (p == PG_TEST) {
            textAt(dc, hTitle, L"Typing Test", 20, 50, 300, 18, NAVY, DT_LEFT | DT_SINGLELINE);
            textAt(dc, hSmall, L"Type the prompt exactly like you normally would — mistakes and fixes are part of the test.",
                   20, 70, 400, 16, HINT, DT_LEFT | DT_SINGLELINE);
            textAt(dc, hSmall, L"Apply fills WPM, Typo %, Burst % and Pause % in Instruct and switches to Humanizer. Paste is off here.",
                   20, 520, 400, 16, HINT, DT_LEFT | DT_SINGLELINE);
        }
        if (p != PG_TEST) {
            textAt(dc, hTitle, L"Hotkeys (global)", 20, 380, 300, 18, NAVY, DT_LEFT | DT_SINGLELINE);
            textAt(dc, hSmall, L"click a box, press the combo, then Apply", 200, 383, 220, 16, HINT, DT_RIGHT | DT_SINGLELINE);
        }
        for (unsigned i = 0; i < sizeof LBLS / sizeof LBLS[0]; i++)
            if (onPage(LBLS[i].page, p)) textAt(dc, hFont, LBLS[i].t, LBLS[i].x, LBLS[i].y, LBLS[i].w, 18, NAVY, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        for (int i = 0; i < TG_N; i++)
            if (tog[i].page == p) textAt(dc, hFont, tog[i].t, tog[i].x + 22, tog[i].y + 3, tog[i].w - 22, 18, NAVY, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

typedef struct { int top, bot, glow, border; } BtnPal;
static const BtnPal PAL_BLUE  = { 0x9ad8fb, 0x2b96e2, 0x86e8ff, 0x16578f };
static const BtnPal PAL_GREEN = { 0xa6ec7c, 0x2f9f26, 0xb4ff7a, 0x1a6514 };
static const BtnPal PAL_RED   = { 0xffa48e, 0xcf2d22, 0xffb08a, 0x821812 };
static const BtnPal PAL_PURPLE = { 0xdcb8ff, 0x7a3ed6, 0xe6b8ff, 0x46198a };

static void gel(Canvas *c, double x, double y, double bw, double bh, double r, const BtnPal *p, int hov, int pressed) {
    int top = p->top, bot = p->bot;
    if (hov && !pressed) { top = lerpCol(top, 0xffffff, 0.18); bot = lerpCol(bot, 0xffffff, 0.12); }
    if (pressed) { top = lerpCol(top, 0x000000, 0.18); bot = lerpCol(bot, 0x000000, 0.12); }
    rrShadow(c, x, y + 1.5, bw, bh, r, S(3), 0.30);
    rrFill(c, x, y, bw, bh, r, top, bot, 1, 1);
    rrGlow(c, x, y, bw, bh, r, x + bw / 2.0, y + bh * 1.05, bw * 0.55, p->glow, pressed ? 0.35 : 0.75);
    rrGloss(c, x, y, bw, bh, r, 0.5, pressed ? 0.35 : 0.75, pressed ? 0.12 : 0.28);
    rrStroke(c, x + 1, y + 1, bw - 2, bh - 2, r - 1, 0xffffff, 0.35, 1, 0);
    rrStroke(c, x, y, bw, bh, r, p->border, 0.9, 1.1, 0);
}

static HFONT hBtnSmall;
static void gelText(HDC dc, const wchar_t *t, RECT tr, const BtnPal *p, int pressed) {
    SetBkMode(dc, TRANSPARENT);
    SIZE sz; int room = tr.right - tr.left - S(10);
    SelectObject(dc, hBold);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    if (sz.cx > room) SelectObject(dc, hBtnSmall);
    OffsetRect(&tr, 0, pressed ? 2 : 1);
    SetTextColor(dc, RGB((p->border >> 16) & 255, (p->border >> 8) & 255, p->border & 255));
    InflateRect(&tr, -S(4), 0);
    DrawTextW(dc, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    OffsetRect(&tr, 0, -1);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static RECT tabRect(int i) { return RS(13 + i * 83, 12, 81, 24); }

static void drawTabs(Canvas *c) {
    for (int i = 0; i < PG_N; i++) {
        RECT r = tabRect(i);
        double x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top, rad = S(8);
        if (i == curPage) {
            gel(c, x, y, w, h, rad, &PAL_BLUE, 0, 0);
            gelText(c->dc, TABNAME[i], r, &PAL_BLUE, 0);
        } else {
            rrFill(c, x, y, w, h, rad, 0xffffff, 0xd6eefc, i == tabHover ? 0.85 : 0.5, i == tabHover ? 0.7 : 0.35);
            if (i == tabHover) rrGlow(c, x, y, w, h, rad, x + w / 2, y + h * 1.1, w * 0.5, 0x86e8ff, 0.6);
            rrGloss(c, x, y, w, h, rad, 0.5, 0.5, 0.1);
            rrStroke(c, x, y, w, h, rad, 0x2b7fc4, 0.45, 1, 0);
            SetBkMode(c->dc, TRANSPARENT);
            SelectObject(c->dc, hBold);
            SetTextColor(c->dc, NAVY);
            DrawTextW(c->dc, TABNAME[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (i == PG_UPDATE && updAvail) {
            disc(c, r.right - S(7), r.top + S(6), S(4), 0xff8a1e, 1, 1);
            disc(c, r.right - S(8), r.top + S(5), S(3) / 2.0, 0xffffff, 0.8, 1);
        }
    }
}

static void drawToggles(Canvas *c) {
    for (int i = 0; i < TG_N; i++) {
        if (tog[i].page != curPage) continue;
        double x = S(tog[i].x), y = S(tog[i].y + 3), s = S(16);
        if (tog[i].radio) {
            double cx = x + s / 2, cy = y + s / 2, r = s / 2;
            disc(c, cx, cy, r, 0xffffff, 0.95, 1);
            disc(c, cx, cy + r * 0.3, r * 0.8, 0xcfeaff, 0.6, r * 0.5);
            ring(c, cx, cy, r, 0x2b7fc4, 0.95, 1.2);
            if (tog[i].on) {
                disc(c, cx, cy, r * 0.55, 0x1f8fe6, 1, 1);
                disc(c, cx, cy + r * 0.15, r * 0.4, 0x6fe0ff, 0.8, r * 0.3);
                disc(c, cx - r * 0.15, cy - r * 0.22, r * 0.2, 0xffffff, 0.85, r * 0.12);
            }
        } else {
            double r = S(4);
            if (tog[i].on) {
                rrFill(c, x, y, s, s, r, 0x7fd0fb, 0x1f8fe6, 1, 1);
                rrGloss(c, x, y, s, s, r, 0.5, 0.7, 0.2);
                rrStroke(c, x, y, s, s, r, 0x1b5f9e, 0.95, 1.1, 0);
                segment(c, x + s * 0.24, y + s * 0.52, x + s * 0.43, y + s * 0.72, S(2), 0xffffff, 1);
                segment(c, x + s * 0.43, y + s * 0.72, x + s * 0.78, y + s * 0.28, S(2), 0xffffff, 1);
            } else {
                rrFill(c, x, y, s, s, r, 0xffffff, 0xdcefff, 0.95, 0.95);
                rrStroke(c, x, y, s, s, r, 0x2b7fc4, 0.95, 1.1, 0);
            }
        }
    }
}

static void macroInfo(wchar_t *out, int n) {
    int keys = 0, clicks = 0, moves = 0, wheels = 0, types = 0;
    double total = 0;
    for (int i = 0; i < nev; i++) {
        total += ev[i].delay;
        int t = ev[i].type;
        if (t == EV_KD) keys++;
        else if (t == EV_MV) moves++;
        else if (t == EV_LD || t == EV_RD || t == EV_MD || t == EV_X1D || t == EV_X2D) clicks++;
        else if (t == EV_WH || t == EV_HW) wheels++;
        else if (t == EV_TYPE) types++;
    }
    const wchar_t *fn = curFile[0] ? (wcsrchr(curFile, L'\\') ? wcsrchr(curFile, L'\\') + 1 : curFile) : L"(not saved)";
    swprintf(out, n,
        L"Loaded macro\n"
        L"%d events  ·  %.1f s long  ·  file: %s\n"
        L"%d key presses  ·  %d clicks  ·  %d mouse moves  ·  %d scrolls  ·  %d TYPE lines\n\n"
        L"Tips\n"
        L"• Clicks on this window and the hotkey keys are never recorded.\n"
        L"• Speed 2.0 plays twice as fast; Repeat 0 loops until you hit Stop.\n"
        L"• .fmac macro files are plain text: open one in Notepad to tweak delays or add lines like  500 TYPE hello world",
        nev, total / 1000.0, fn, keys, clicks, moves, wheels, types);
}

/* typing test state, used by paint */
static int promptIdx, testDone, haveRes;
static wchar_t testResult[1024];
static const wchar_t *PROMPTS[] = {
    L"The old lighthouse keeper climbed the spiral stairs every evening, counting each step out loud. "
    L"Nobody knew why he did it, but the townspeople said the light never once failed in forty years.",
    L"Building a go-kart from scratch takes patience. You measure the tubing, cut it, grind the edges, "
    L"and weld everything square. Then you realize the steering is backwards, and you start again.",
    L"Honestly, the best pizza in town comes from a tiny shop near the harbor. It's cash only, the "
    L"line is always long, and the owner yells at everyone, but somehow that's part of the charm.",
    L"Scientists finally announced their discovery on Tuesday: after years of careful research, they "
    L"confirmed that coffee tastes better when someone else makes it. The results were not surprising.",
};
#define NPROMPTS (int)(sizeof PROMPTS / sizeof PROMPTS[0])

static void paintMain(HDC hdc) {
    Canvas *src = &pageC[curPage];
    memcpy(backC.px, src->px, (size_t)src->w * src->h * 4);
    drawTabs(&backC);
    drawToggles(&backC);
    HDC dc = backC.dc;
    SetBkMode(dc, TRANSPARENT);
    if (curPage == PG_MACRO) {
        wchar_t info[1024]; macroInfo(info, 1024);
        textAt(dc, hFont, info, 32, 154, 378, 200, NAVY, DT_LEFT | DT_WORDBREAK);
    }
    if (curPage == PG_UPDATE) {
        wchar_t info[600]; updInfo(info, 600);
        disc(&backC, S(33), S(85), S(5), connColor(), 1, 1);
        disc(&backC, S(32), S(83), S(2), 0xffffff, 0.7, 1);
        textAt(dc, hFont, info, 46, 78, 368, 74, NAVY, DT_LEFT | DT_WORDBREAK);
        wchar_t hd[96];
        if (updLatest[0]) swprintf(hd, 96, L"What's new in %s", updLatest); else wcscpy(hd, L"What's new");
        textAt(dc, hBold, hd, 30, 170, 380, 18, NAVY, DT_LEFT | DT_SINGLELINE);
    }
    if (curPage == PG_TEST) {
        textAt(dc, hPrompt, PROMPTS[promptIdx], 30, 97, 380, 92, NAVY, DT_LEFT | DT_WORDBREAK);
        textAt(dc, hFont, testResult, 32, 330, 376, 144, NAVY, DT_LEFT | DT_WORDBREAK);
    } else {
        textAt(dc, hFont, statusText, 22, 502, 290, 34, NAVY, DT_LEFT | DT_WORDBREAK | DT_VCENTER | DT_END_ELLIPSIS);
    }
    BitBlt(hdc, 0, 0, src->w, src->h, dc, 0, 0, SRCCOPY);
}

static void drawButton(DRAWITEMSTRUCT *d) {
    Canvas *src = &pageC[curPage];
    RECT wr; GetWindowRect(d->hwndItem, &wr);
    MapWindowPoints(NULL, hMain, (POINT *)&wr, 2);
    int w = d->rcItem.right, h = d->rcItem.bottom;
    Canvas cv = mkCanvas(w, h), *c = &cv;
    for (int y = 0; y < h; y++) {
        int sy = wr.top + y;
        if (sy < 0 || sy >= src->h) continue;
        for (int x = 0; x < w; x++) {
            int sx = wr.left + x;
            c->px[y * w + x] = (sx >= 0 && sx < src->w) ? src->px[sy * src->w + sx] : 0;
        }
    }
    UINT id = d->CtlID;
    const BtnPal *p = (id == ID_REC || id == ID_EDEL || id == ID_PDEL || (id == ID_KBLOCK && kbBlocked) || (id == ID_MAGIC && magicOn)) ? &PAL_RED :
                      id == ID_MAGIC ? &PAL_PURPLE :
                      (id == ID_PLAY || id == ID_TYPE || id == ID_TAPPLY || id == ID_EINS || id == ID_PSAVE || id == ID_UINSTALL) ? &PAL_GREEN : &PAL_BLUE;
    int pressed = (d->itemState & ODS_SELECTED) != 0;
    gel(c, 1, 1, w - 2, h - 3, fmin((h - 3) / 2.0, S(9)), p, d->hwndItem == hoverBtn, pressed);
    wchar_t t[96]; GetWindowTextW(d->hwndItem, t, 96);
    RECT tr = { 0, 0, w, h - 2 };
    gelText(c->dc, t, tr, p, pressed);
    BitBlt(d->hDC, 0, 0, w, h, c->dc, 0, 0, SRCCOPY);
    freeCanvas(c);
}

static LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR rd) {
    (void)id; (void)rd;
    if (m == WM_MOUSEMOVE && hoverBtn != h) {
        HWND old = hoverBtn; hoverBtn = h;
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        if (old) InvalidateRect(old, NULL, FALSE);
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_MOUSELEAVE && hoverBtn == h) {
        hoverBtn = NULL; InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_ERASEBKGND) return 1;
    else if (m == WM_LBUTTONDBLCLK) m = WM_LBUTTONDOWN;
    return DefSubclassProc(h, m, wp, lp);
}

/* ---------- actions ---------- */

static int busy(void) { return recording || playing || typing || magicOn; }

static void idleStatus(void) {
    wchar_t a[64], b[64], c[64], d[64];
    hkName(hkVal[HK_REC], a, 64); hkName(hkVal[HK_PLAY], b, 64);
    hkName(hkVal[HK_TYPE], c, 64); hkName(hkVal[HK_STOP], d, 64);
    wchar_t e[64]; hkName(hkVal[HK_CLIP], e, 64);
    status(L"Idle — %d events.  %s rec · %s play · %s type · %s clipboard · %s stop", nev, a, b, c, e, d);
}

static int isMod(int vk) {
    return vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_SHIFT || vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN;
}

static void trimModifiers(void) {
    while (nev && (ev[nev - 1].type == EV_KD) && isMod(ev[nev - 1].a)) nev--;
    int lead = 0;
    while (lead < nev && ev[lead].type == EV_KU && isMod(ev[lead].a)) lead++;
    if (lead) { memmove(ev, ev + lead, (nev - lead) * sizeof(Ev)); nev -= lead; if (nev) ev[0].delay = 0; }
}

static void startRec(void) {
    if (busy()) return;
    clearEv();
    recMoves = tog[TG_MOVES].on;
    lastMoveT = 0;
    hkKb = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(NULL), 0);
    hkMs = SetWindowsHookExW(WH_MOUSE_LL, MsProc, GetModuleHandleW(NULL), 0);
    recording = 1;
    status(L"● Recording… (clicks on this window are ignored)");
}

static void stopRec(void) {
    if (!recording) return;
    recording = 0;
    if (hkKb) UnhookWindowsHookEx(hkKb);
    if (hkMs) UnhookWindowsHookEx(hkMs);
    hkKb = hkMs = NULL;
    trimModifiers();
    idleStatus();
}

static double getDbl(int id, double def) {
    wchar_t b[32]; GetDlgItemTextW(hMain, id, b, 32);
    wchar_t *e; double v = wcstod(b, &e);
    return e == b ? def : v;
}

static double getPct(int id, double def) {
    double v = getDbl(id, def);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void startPlay(void) {
    if (busy()) return;
    if (!nev) { status(L"Nothing to play — record or load a macro first."); return; }
    playReps = (int)getDbl(ID_REPEAT, 1); if (playReps < 0) playReps = 0;
    playSpeed = getDbl(ID_SPEED, 1.0); if (playSpeed < 0.05) playSpeed = 0.05;
    stopFlag = 0; playing = 1;
    CloseHandle(CreateThread(NULL, 0, PlayThread, NULL, 0, NULL));
}

static void resetTextFormat(HWND re) {
    CHARRANGE keep; SendMessageW(re, EM_EXGETSEL, 0, (LPARAM)&keep);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BACKCOLOR | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
    cf.dwEffects = CFE_AUTOBACKCOLOR; cf.crTextColor = RGB(14, 52, 92);
    SendMessageW(re, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(re, EM_EXSETSEL, 0, (LPARAM)&keep);
}

static int hlPrev = -1;
static void setBack(HWND re, int pos, int on) {
    CHARRANGE cr = { pos, pos + 1 };
    SendMessageW(re, EM_EXSETSEL, 0, (LPARAM)&cr);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BACKCOLOR;
    if (on) cf.crBackColor = RGB(255, 228, 40); else cf.dwEffects = CFE_AUTOBACKCOLOR;
    SendMessageW(re, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}
static void highlight(int pos) {
    HWND re = GetDlgItem(hMain, ID_TEXT);
    if (hlPrev >= 0) setBack(re, hlPrev, 0);
    if (pos >= 0) {
        setBack(re, pos, 1);
        CHARRANGE cr = { pos, pos };
        SendMessageW(re, EM_EXSETSEL, 0, (LPARAM)&cr);
        SendMessageW(re, EM_SCROLLCARET, 0, 0);
    }
    hlPrev = pos;
}

static LRESULT CALLBACK RichSub(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR rd) {
    (void)id; (void)rd;
    int paste = m == WM_PASTE ||
        (m == WM_KEYDOWN && ((wp == 'V' && GetKeyState(VK_CONTROL) < 0) || (wp == VK_INSERT && GetKeyState(VK_SHIFT) < 0)));
    if (m == WM_CHAR && wp == 0x16) return 0;
    if (paste) {
        if (SendMessageW(h, EM_GETOPTIONS, 0, 0) & ECO_READONLY) return 0;
        if (OpenClipboard(h)) {
            HANDLE d = GetClipboardData(CF_UNICODETEXT);
            wchar_t *t = d ? GlobalLock(d) : NULL;
            if (t) { SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)t); GlobalUnlock(d); }
            CloseClipboard();
        }
        return 0;
    }
    return DefSubclassProc(h, m, wp, lp);
}

static wchar_t *getRichText(HWND te) {
    GETTEXTLENGTHEX gl = { GTL_DEFAULT | GTL_NUMCHARS, 1200 };
    int n = (int)SendMessageW(te, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    if (n < 0) n = 0;
    wchar_t *t = calloc(n + 2, sizeof(wchar_t));
    GETTEXTEX gt = { (DWORD)((n + 1) * sizeof(wchar_t)), GT_DEFAULT, 1200, NULL, NULL };
    if (n) SendMessageW(te, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)t);
    for (wchar_t *q = t; *q; q++) if (*q == L'\r') *q = L'\n';
    return t;
}

static void setTypeLabel(void) {
    wchar_t nm[64], lbl[96];
    hkName(hkVal[HK_TYPE], nm, 64);
    swprintf(lbl, 96, L"%s (%s)", resumeIdx > 0 ? L"Resume" : L"Type", nm);
    SetWindowTextW(hBtn[HK_TYPE], lbl);
}

static void resetResume(void) {
    resumeIdx = 0; resumePending = 0;
    if (hlPrev >= 0) highlight(-1);
    setTypeLabel();
}

static int resumeValid(const wchar_t *text) {
    return resumeIdx > 0 && resumeText && (int)wcslen(text) > resumeIdx - 1 &&
           (int)wcslen(resumeText) >= resumeIdx && !wcsncmp(text, resumeText, resumeIdx);
}

static void startType(void) {
    if (busy()) return;
    HWND te = GetDlgItem(hMain, ID_TEXT);
    wchar_t *text = getRichText(te);
    int n = (int)wcslen(text);
    if (!n) { free(text); status(L"Auto-type: the text box is empty."); return; }
    if (!resumeValid(text) || resumeIdx >= n) { resumeIdx = 0; resumePending = 0; }
    TypeJob *j = calloc(1, sizeof *j);
    j->text = text;
    j->start = resumeIdx; j->pending = resumePending;
    free(resumeText); resumeText = _wcsdup(text);
    j->mode = tog[TG_HUMAN].on ? ID_HUMAN : tog[TG_FIXMS].on ? ID_FIXMS : ID_FIXWPM;
    j->wpm = (int)getDbl(ID_WPM, 60); if (j->wpm < 1) j->wpm = 1;
    j->ms = (int)getDbl(ID_MS, 50); if (j->ms < 0) j->ms = 0;
    j->delay = (int)getDbl(ID_DELAY, 3); if (j->delay < 0) j->delay = 0;
    j->typos = tog[TG_TYPOS].on;
    j->docsSafe = tog[TG_DOCS].on;
    j->typoP = getPct(ID_TYPOP, DEF_TYPO);
    j->burstP = getPct(ID_BURSTP, DEF_BURST);
    j->pauseP = getPct(ID_PAUSEP, DEF_PAUSE);
    resetTextFormat(te);
    hlPrev = -1;
    if (j->start) highlight(j->start);
    SendMessageW(te, EM_SETREADONLY, TRUE, 0);
    stopFlag = 0; typing = 1;
    CloseHandle(CreateThread(NULL, 0, TypeThread, j, 0, NULL));
}

static void magicStop(int finished);
static void stopAll(void) {
    stopFlag = 1;
    magicStop(0);
    if (recording) stopRec();
}

/* ---------- Vista glass frame ---------- */

#define FR 7
#define TT 30
static Canvas frameC, frameBack, snapC;
static int sizing, rescaling;
static void rescaleAll(void);
static int capHover = -1, capDown = -1;

static RECT capRect(int i) {
    int right = frameC.w - S(7), cw = S(43), mw = S(26);
    RECT r = { i ? right - cw : right - cw - mw, 1, i ? right : right - cw, 1 + S(19) };
    return r;
}

static void renderFrame(void) {
    Canvas *c = &frameC;
    int W = c->w, H = c->h, th = S(TT);
    double sx = W * 0.22, sy = -S(14);
    for (int y = 0; y < H; y++) {
        int base = y < th ? lerpCol(0xa9cdea, 0x76a5cf, (double)y / th) : lerpCol(0x76a5cf, 0x5b8cbc, (double)(y - th) / (H - th));
        for (int x = 0; x < W; x++) {
            double s1 = sin((x + y * 1.7) / (double)S(38)) * 0.5 + 0.5;
            double s2 = sin((x * 0.8 + y * 1.7) / (double)S(11) + 1.3) * 0.5 + 0.5;
            double d = hypot((x - sx) / (W * 0.7), (y - sy) / (H * 0.25));
            double sheen = clamp01(1 - d); sheen *= sheen;
            double a = 0.13 * s1 * s1 * s1 + 0.05 * s2 + 0.30 * sheen;
            c->px[y * W + x] = (DWORD)lerpCol(base, 0xffffff, clamp01(a));
        }
    }
    for (int y = 0; y < th / 2; y++) for (int x = 0; x < W; x++)
        blend(c, x, y, 0xffffff, 0.22 * (1 - (double)y / (th / 2)));
    int cx = S(FR), cy = th, cw = W - 2 * S(FR), ch = H - th - S(FR);
    rrStroke(c, cx - 2, cy - 2, cw + 4, ch + 4, 2, 0xffffff, 0.45, 1, 0);
    rrStroke(c, cx - 1, cy - 1, cw + 2, ch + 2, 1, 0x163a5e, 0.75, 1, 0);
}

static void capButton(Canvas *c, int i) {
    RECT r = capRect(i);
    int close = i == 1, hov = capHover == i, prs = capDown == i && hov;
    double rad = S(4), x = r.left, w = r.right - r.left, h = r.bottom - r.top;
    double y = r.top - rad, hh = h + rad;
    if (close) {
        int top = hov ? 0xffb9a3 : 0xe6a08c, bot = hov ? 0xe8452a : 0xc23a1f;
        if (prs) { top = lerpCol(top, 0, 0.2); bot = lerpCol(bot, 0, 0.2); }
        rrFill(c, x, y, w, hh, rad, top, bot, 1, 1);
        rrGlow(c, x, y, w, hh, rad, x + w / 2, y + hh * 1.08, w * 0.6, hov ? 0xffd27a : 0xff8a5a, hov ? 0.95 : 0.55);
    } else if (hov) {
        int top = prs ? 0x5fb4ea : 0xa6e2ff, bot = prs ? 0x0d5fae : 0x1f86dc;
        rrFill(c, x, y, w, hh, rad, top, bot, 1, 1);
        rrGlow(c, x, y, w, hh, rad, x + w / 2, y + hh * 1.08, w * 0.75, 0x7ff6ff, prs ? 0.6 : 1.0);
    } else {
        rrFill(c, x, y, w, hh, rad, 0xd3e6f5, 0x8db1d4, 0.6, 0.6);
    }
    rrGloss(c, x, r.top, w, h, rad, 0.5, prs ? 0.3 : 0.6, 0.12);
    rrStroke(c, x, y, w, hh, rad, 0x0b2440, 0.8, 1, 0);
    rrStroke(c, x + 1, y + 1, w - 2, hh - 2, rad - 1, 0xffffff, 0.45, 1, 0);
    double gx = x + w / 2, gy = r.top + h / 2;
    if (close) {
        double s = S(4);
        for (int pass = 0; pass < 2; pass++) {
            double t = pass ? S(2) : S(2) + 2.2; int col = pass ? 0xffffff : 0x5a1208; double a = pass ? 1 : 0.7;
            segment(c, gx - s, gy - s * 0.8, gx + s, gy + s * 0.8, t, col, a);
            segment(c, gx - s, gy + s * 0.8, gx + s, gy - s * 0.8, t, col, a);
        }
    } else {
        double s = S(4), yy = gy + S(3);
        segment(c, gx - s, yy, gx + s, yy, S(2) + 2.2, 0x0b2440, 0.7);
        segment(c, gx - s, yy, gx + s, yy, S(2), 0xffffff, 1);
    }
}

static void paintFrame(HDC hdc) {
    if (sizing && snapC.dc) {
        RECT r; GetClientRect(hFrame, &r);
        SetStretchBltMode(hdc, HALFTONE); SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, 0, 0, r.right, r.bottom, snapC.dc, 0, 0, snapC.w, snapC.h, SRCCOPY);
        return;
    }
    Canvas *c = &frameBack;
    memcpy(c->px, frameC.px, (size_t)c->w * c->h * 4);
    capButton(c, 0); capButton(c, 1);
    rrStroke(c, 0, 0, c->w, c->h, S(8), 0x0b2440, 0.85, 1, 0);
    rrStroke(c, 1, 1, c->w - 2, c->h - 2, S(7), 0xffffff, 0.45, 1, 0);
    for (int i = 0; i < c->w * S(2); i++) c->px[i] |= 0xFF000000;
    BitBlt(hdc, 0, 0, c->w, c->h, c->dc, 0, 0, SRCCOPY);
}

static int capAt(int x, int y) {
    POINT p = { x, y };
    for (int i = 0; i < 2; i++) { RECT r = capRect(i); if (PtInRect(&r, p)) return i; }
    return -1;
}

static void capInvalidate(void) {
    RECT r = { capRect(0).left - 2, 0, frameC.w, capRect(0).bottom + 2 };
    InvalidateRect(hFrame, &r, FALSE);
}

static LRESULT CALLBACK FrameProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        if (capAt(p.x, p.y) >= 0) return HTCLIENT;
        RECT r; GetClientRect(h, &r);
        int b = S(6), L = p.x < b, Rt = p.x >= r.right - b, T = p.y < S(3), B = p.y >= r.bottom - b;
        if (T && L) return HTTOPLEFT;
        if (T && Rt) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && Rt) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (Rt) return HTRIGHT;
        if (B) return HTBOTTOM;
        if (T) return HTTOP;
        return HTCAPTION;
    }
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION && fabs(uiScale - 1.0f) > 0.01f) { uiScale = 1.0f; rescaleAll(); }
        return 0;
    case WM_SIZING: {
        RECT *r = (RECT *)lp;
        if (!sizing) {
            sizing = 1;
            snapC = mkCanvas(frameC.w, frameC.h);
            PrintWindow(h, snapC.dc, 2);
            ShowWindow(hMain, SW_HIDE);
        }
        double bw = (440 + 2 * FR) * dpi, bh = (546 + TT + FR) * dpi;
        int w = r->right - r->left, hg = r->bottom - r->top;
        double sc = (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) ? hg / bh : w / bw;
        if (sc < 0.7) sc = 0.7;
        if (sc > 2.2) sc = 2.2;
        int nw = (int)(bw * sc + .5), nh = (int)(bh * sc + .5);
        if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - nw; else r->right = r->left + nw;
        if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) r->top = r->bottom - nh; else r->bottom = r->top + nh;
    } return TRUE;
    case WM_SIZE: if (sizing) InvalidateRect(h, NULL, FALSE); break;
    case WM_EXITSIZEMOVE:
        if (sizing) {
            RECT wr; GetWindowRect(h, &wr);
            uiScale = (float)((wr.right - wr.left) / ((440 + 2 * FR) * dpi));
            sizing = 0;
            freeCanvas(&snapC); memset(&snapC, 0, sizeof snapC);
            rescaleAll();
        }
        break;
    case WM_NCCALCSIZE: if (wp) return 0; break;
    case WM_NCACTIVATE: return DefWindowProcW(h, m, wp, -1);
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MAXIMIZE) return 0;
        break;
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS *wpos = (WINDOWPOS *)lp;
        if (!IsIconic(h) && !sizing && !rescaling && !(wpos->flags & SWP_NOSIZE)) { wpos->cx = frameC.w; wpos->cy = frameC.h; }
    } break;
    case WM_MOUSEMOVE: {
        int i = capAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i != capHover) {
            capHover = i; capInvalidate();
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
    } return 0;
    case WM_MOUSELEAVE: if (capDown < 0) { capHover = -1; capInvalidate(); } return 0;
    case WM_LBUTTONDOWN: capDown = capAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); SetCapture(h); capInvalidate(); return 0;
    case WM_LBUTTONUP: {
        int i = capAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), was = capDown;
        capDown = -1; ReleaseCapture();
        capHover = i; capInvalidate();
        if (i >= 0 && i == was) {
            if (i == 1) PostMessageW(h, WM_CLOSE, 0, 0);
            else { capHover = -1; ShowWindow(h, SW_MINIMIZE); }
        }
    } return 0;
    case WM_SETFOCUS: SetFocus(GetDlgItem(hMain, curPage == PG_TEST ? ID_TEDIT : curPage == PG_INSTR ? ID_TEXT : ID_REC)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        paintFrame(dc);
        EndPaint(h, &ps);
    } return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}


/* ---------- typing test ---------- */

typedef struct { double t; wchar_t c; } KeyRec;
static KeyRec *kr; static int nkr, kcap;
static double resWpm, resTypo, resBurst, resPause;

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int isBreak(wchar_t c) { return c == L' ' || c == L'\n' || (c && wcschr(L".,!?;:", c)); }

static void analyzeTest(void) {
    HWND ed = GetDlgItem(hMain, ID_TEDIT);
    int n = GetWindowTextLengthW(ed);
    wchar_t *fin = calloc(n + 1, sizeof(wchar_t));
    GetWindowTextW(ed, fin, n + 1);
    haveRes = 0;
    if (nkr < 30) {
        swprintf(testResult, 1024, L"Not enough typing to analyze yet — type more of the prompt, then hit Finish.");
        free(fin); return;
    }
    double T = kr[nkr - 1].t - kr[0].t;
    int net = 0; for (wchar_t *q = fin; *q; q++) if (*q != L'\r') net++;
    double wpm = T > 0 ? net / 5.0 / (T / 60000.0) : 0;

    const wchar_t *pr = PROMPTS[promptIdx];
    int pl = (int)wcslen(pr), errs = 0, k = 0;
    for (wchar_t *q = fin; *q; q++) { if (*q == L'\r') continue; if (k >= pl || *q != pr[k]) errs++; k++; }
    if (k < pl) errs += pl - k;
    double acc = 100.0 * (1 - (double)errs / (pl > 0 ? pl : 1)); if (acc < 0) acc = 0;

    int episodes = 0, letters = 0, spaces = 0, pauses = 0, nf = 0;
    double *flow = malloc(nkr * sizeof(double)), *sorted = malloc(nkr * sizeof(double));
    for (int i = 0; i < nkr; i++) {
        wchar_t c = kr[i].c, pc = i ? kr[i - 1].c : 0;
        if (c == 8) { if (pc != 8) episodes++; continue; }
        if (iswalpha(c)) letters++;
        if (i == 0) continue;
        double d = kr[i].t - kr[i - 1].t;
        if (pc != 8 && !isBreak(pc) && d < 1500) flow[nf++] = d;
    }
    memcpy(sorted, flow, nf * sizeof(double));
    qsort(sorted, nf, sizeof(double), cmpd);
    double med = nf ? sorted[nf / 2] : 150;

    double sum = 0; int cnt = 0;
    for (int i = 0; i < nf; i++) if (flow[i] < med * 2.5) { sum += flow[i]; cnt++; }
    double I = cnt ? sum / cnt : med;

    for (int i = 1; i < nkr; i++) {
        if (kr[i - 1].c != L' ' || kr[i].c == 8) continue;
        spaces++;
        if (kr[i].t - kr[i - 1].t > fmax(250, med * 2.5)) pauses++;
    }

    int nch = 0; double chunks[512];
    for (int i = 0; i + 6 <= nf && nch < 512; i += 6) {
        double s = 0; int c2 = 0;
        for (int j = i; j < i + 6; j++) if (flow[j] < med * 2.5) { s += flow[j]; c2++; }
        if (c2 >= 4) chunks[nch++] = s / c2;
    }
    double G = 0; for (int i = 0; i < nch; i++) G += chunks[i];
    G = nch ? G / nch : I;
    double p = 0.5;
    for (int it = 0; it < 8 && nch; it++) {
        double b = G / (0.6 * p + 1.0 * (1 - p)), fast = 0;
        for (int i = 0; i < nch; i++) if (chunks[i] < 0.8 * b) fast++;
        p = fast / nch;
    }

    resTypo = letters ? fmin(30, 100.0 * episodes / letters * 0.5) : 0;
    resBurst = 100 * p;
    resPause = spaces ? 100.0 * pauses / spaces : 0;
    double base = I / (0.6 * p + 1.0 * (1 - p));
    resWpm = base > 0 ? 60000.0 / (base * 5) : wpm;
    if (resWpm > 250) resWpm = 250;
    haveRes = 1;

    swprintf(testResult, 1024,
        L"Measured speed:  %.0f WPM   ·   Accuracy %.0f%%\n"
        L"Typo rate:  %.1f%% of letters  (%d fixes)\n"
        L"Burst rate:  %.0f%% of key chunks are fast spurts\n"
        L"Pause rate:  %.0f%% of words followed by a hesitation\n\n"
        L"Humanizer settings to match you:\n"
        L"WPM %.0f   ·   Typo %.1f%%   ·   Burst %.0f%%   ·   Pause %.0f%%",
        wpm, acc, resTypo, episodes, resBurst, resPause, resWpm, resTypo, resBurst, resPause);
    free(flow); free(sorted); free(fin);
}

static void finishTest(void) {
    if (testDone) return;
    analyzeTest();
    if (haveRes) { testDone = 1; SendDlgItemMessageW(hMain, ID_TEDIT, EM_SETREADONLY, TRUE, 0); }
    InvalidateRect(hMain, NULL, FALSE);
}

static void newPrompt(int advance) {
    if (advance) promptIdx = (promptIdx + 1) % NPROMPTS;
    nkr = 0; testDone = 0; haveRes = 0;
    HWND ed = GetDlgItem(hMain, ID_TEDIT);
    SendMessageW(ed, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(ed, L"");
    wcscpy(testResult, L"Start typing in the box above whenever you're ready — the clock starts on your first key. "
                       L"It finishes automatically when you reach the end of the prompt.");
    InvalidateRect(hMain, NULL, FALSE);
    if (curPage == PG_TEST) SetFocus(ed);
}

static LRESULT CALLBACK TestEditSub(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR rd) {
    (void)id; (void)rd;
    if (m == WM_PASTE) return 0;
    if (m == WM_KEYDOWN && ((wp == 'V' && GetKeyState(VK_CONTROL) < 0) || (wp == VK_INSERT && GetKeyState(VK_SHIFT) < 0))) return 0;
    if (m == WM_CHAR) {
        if (wp == 0x16 || wp == 0x7F) return 0;
        if (!testDone && (wp == 8 || wp >= 32)) {
            if (nkr == kcap) { kcap = kcap ? kcap * 2 : 1024; kr = realloc(kr, kcap * sizeof(KeyRec)); }
            kr[nkr].t = nowMs(); kr[nkr].c = (wchar_t)wp; nkr++;
        }
        LRESULT r = DefSubclassProc(h, m, wp, lp);
        if (!testDone && wp != 8 && GetWindowTextLengthW(h) >= (int)wcslen(PROMPTS[promptIdx])) finishTest();
        return r;
    }
    return DefSubclassProc(h, m, wp, lp);
}

/* ---------- macro editor ---------- */

static const wchar_t *EVDESC[EV_COUNT] = {
    L"KD – key down", L"KU – key up", L"MV – mouse move", L"LD – left down", L"LU – left up",
    L"RD – right down", L"RU – right up", L"MD – middle down", L"MU – middle up",
    L"X1D – side 1 down", L"X1U – side 1 up", L"X2D – side 2 down", L"X2U – side 2 up",
    L"WH – wheel", L"HW – h-wheel", L"TYPE – type text",
};

static int isKeyEv(int t) { return t == EV_KD || t == EV_KU; }
static int isWheelEv(int t) { return t == EV_WH || t == EV_HW; }

static int isExtVk(int vk) {
    return (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_RCONTROL ||
           vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS || vk == VK_NUMLOCK || vk == VK_DIVIDE;
}

static void keyLabel(int vk, int scan, int ext, wchar_t *out, int n) {
    out[0] = 0;
    if (vk >= VK_F1 && vk <= VK_F24) { swprintf(out, n, L"F%d", vk - VK_F1 + 1); return; }
    if (!scan) scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (!GetKeyNameTextW((scan << 16) | (ext << 24), out, n) || !out[0]) swprintf(out, n, L"vk %d", vk);
}

static const struct { const wchar_t *n; int vk; } KEYNAMES[] = {
    { L"enter", VK_RETURN }, { L"return", VK_RETURN }, { L"space", VK_SPACE }, { L"tab", VK_TAB },
    { L"esc", VK_ESCAPE }, { L"escape", VK_ESCAPE }, { L"backspace", VK_BACK }, { L"bksp", VK_BACK },
    { L"delete", VK_DELETE }, { L"del", VK_DELETE }, { L"insert", VK_INSERT }, { L"ins", VK_INSERT },
    { L"home", VK_HOME }, { L"end", VK_END }, { L"pageup", VK_PRIOR }, { L"pgup", VK_PRIOR },
    { L"pagedown", VK_NEXT }, { L"pgdn", VK_NEXT }, { L"up", VK_UP }, { L"down", VK_DOWN },
    { L"left", VK_LEFT }, { L"right", VK_RIGHT }, { L"shift", VK_LSHIFT }, { L"lshift", VK_LSHIFT },
    { L"rshift", VK_RSHIFT }, { L"ctrl", VK_LCONTROL }, { L"control", VK_LCONTROL }, { L"lctrl", VK_LCONTROL },
    { L"rctrl", VK_RCONTROL }, { L"alt", VK_LMENU }, { L"lalt", VK_LMENU }, { L"ralt", VK_RMENU },
    { L"win", VK_LWIN }, { L"capslock", VK_CAPITAL }, { L"caps", VK_CAPITAL }, { L"menu", VK_APPS },
    { L"printscreen", VK_SNAPSHOT }, { L"prtsc", VK_SNAPSHOT }, { L"pause", VK_PAUSE },
};

static int nameToVk(const wchar_t *s) {
    for (unsigned i = 0; i < sizeof KEYNAMES / sizeof KEYNAMES[0]; i++) if (!_wcsicmp(s, KEYNAMES[i].n)) return KEYNAMES[i].vk;
    if ((s[0] == L'f' || s[0] == L'F') && iswdigit(s[1])) { int f = _wtoi(s + 1); if (f >= 1 && f <= 24) return VK_F1 + f - 1; }
    if (s[0] && !s[1]) { SHORT v = VkKeyScanW(s[0]); if (v != -1) return LOBYTE(v); }
    return 0;
}

static void evDetails(Ev *e, wchar_t *out, int n) {
    if (isKeyEv(e->type)) {
        wchar_t k[64]; keyLabel(e->a, e->b, e->c, k, 64);
        swprintf(out, n, L"%s   (vk %d, scan %d%s)", k, e->a, e->b, e->c ? L", ext" : L"");
    } else if (isWheelEv(e->type)) swprintf(out, n, L"at %d, %d   Δ %d", e->a, e->b, e->c);
    else if (e->type == EV_TYPE) {
        int k = 0;
        for (const wchar_t *p = e->text ? e->text : L""; *p && k < n - 2; p++) out[k++] = *p == L'\n' ? L'⏎' : *p;
        out[k] = 0;
    } else swprintf(out, n, L"at %d, %d", e->a, e->b);
}

static void evArgs(Ev *e, wchar_t *out, int n) {
    if (isKeyEv(e->type)) swprintf(out, n, L"%d %d %d", e->a, e->b, e->c);
    else if (isWheelEv(e->type)) swprintf(out, n, L"%d %d %d", e->a, e->b, e->c);
    else if (e->type == EV_TYPE) {
        int k = 0;
        for (const wchar_t *p = e->text ? e->text : L""; *p && k < n - 3; p++) {
            if (*p == L'\n') { out[k++] = L'\\'; out[k++] = L'n'; }
            else out[k++] = *p;
        }
        out[k] = 0;
    } else swprintf(out, n, L"%d %d", e->a, e->b);
}

static HWND lv(void) { return GetDlgItem(hMain, ID_LIST); }
static int lvSel(void) { return (int)SendMessageW(lv(), LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); }

static void lvSelect(int i) {
    LVITEMW it = {0};
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(lv(), LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&it);
    if (i < 0 || i >= nev) return;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(lv(), LVM_SETITEMSTATE, i, (LPARAM)&it);
    SendMessageW(lv(), LVM_ENSUREVISIBLE, i, FALSE);
}

static void refreshList(void) {
    HWND l = lv();
    if (!l) return;
    SendMessageW(l, LVM_SETITEMCOUNT, nev, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    InvalidateRect(l, NULL, FALSE);
}

static void fillFields(int i) {
    if (i < 0 || i >= nev) return;
    wchar_t b[1024];
    swprintf(b, 32, L"%lu", (unsigned long)ev[i].delay);
    SetDlgItemTextW(hMain, ID_EDELAY, b);
    SendDlgItemMessageW(hMain, ID_EEVENT, CB_SETCURSEL, ev[i].type, 0);
    evArgs(&ev[i], b, 1024);
    SetDlgItemTextW(hMain, ID_EARGS, b);
}

static int parseFields(Ev *e) {
    wchar_t b[1024];
    memset(e, 0, sizeof *e);
    GetDlgItemTextW(hMain, ID_EDELAY, b, 32);
    e->delay = (DWORD)wcstoul(b, NULL, 10);
    e->type = (int)SendDlgItemMessageW(hMain, ID_EEVENT, CB_GETCURSEL, 0, 0);
    if (e->type < 0) { status(L"Editor: pick an event type."); return 0; }
    GetDlgItemTextW(hMain, ID_EARGS, b, 1024);
    if (e->type == EV_TYPE) {
        wchar_t *o = b;
        for (wchar_t *q = b; *q; q++) {
            if (*q == L'\\' && q[1]) { q++; *o++ = *q == L'n' ? L'\n' : *q == L't' ? L'\t' : *q; }
            else *o++ = *q;
        }
        *o = 0;
        e->text = _wcsdup(b);
        return 1;
    }
    wchar_t *ctx = NULL, *t1 = wcstok(b, L" ,", &ctx), *t2 = wcstok(NULL, L" ,", &ctx), *t3 = wcstok(NULL, L" ,", &ctx);
    if (isKeyEv(e->type)) {
        if (!t1) { status(L"Editor: key events need a key — e.g. Enter, A, F5, or vk scan ext numbers."); return 0; }
        if (iswdigit(t1[0]) && t1[1]) {
            e->a = _wtoi(t1);
            e->b = t2 ? _wtoi(t2) : (int)MapVirtualKeyW(e->a, MAPVK_VK_TO_VSC);
            e->c = t3 ? _wtoi(t3) : isExtVk(e->a);
        } else {
            e->a = nameToVk(t1);
            if (!e->a) { status(L"Editor: don't know the key \"%s\". Try a single character, Enter, Space, F1-F24, Ctrl…", t1); return 0; }
            e->b = (int)MapVirtualKeyW(e->a, MAPVK_VK_TO_VSC);
            e->c = isExtVk(e->a);
        }
        return 1;
    }
    POINT cp; GetCursorPos(&cp);
    e->a = t1 ? _wtoi(t1) : cp.x;
    e->b = t2 ? _wtoi(t2) : cp.y;
    e->c = isWheelEv(e->type) ? (t3 ? _wtoi(t3) : 120) : 0;
    return 1;
}

static void evInsert(int at, Ev e) {
    push(e);
    memmove(ev + at + 1, ev + at, (nev - 1 - at) * sizeof(Ev));
    ev[at] = e;
}

static void evRemove(int i) {
    free(ev[i].text);
    memmove(ev + i, ev + i + 1, (nev - 1 - i) * sizeof(Ev));
    nev--;
}

static void editorCmd(int id) {
    if (busy()) { status(L"Editor: stop recording/playback first."); return; }
    int sel = lvSel();
    Ev e;
    switch (id) {
    case ID_EUPD:
        if (sel < 0) { status(L"Editor: select a row to update."); return; }
        if (!parseFields(&e)) return;
        free(ev[sel].text); ev[sel] = e;
        status(L"Updated event %d.", sel + 1);
        break;
    case ID_EINS: {
        if (!parseFields(&e)) return;
        int at = sel < 0 ? nev : sel + 1;
        evInsert(at, e); sel = at;
        status(L"Inserted event %d.", at + 1);
    } break;
    case ID_EDEL: {
        if (sel < 0) { status(L"Editor: select rows to delete."); return; }
        int cnt = 0;
        for (int i = nev - 1; i >= 0; i--)
            if (SendMessageW(lv(), LVM_GETITEMSTATE, i, LVIS_SELECTED) & LVIS_SELECTED) { evRemove(i); cnt++; }
        if (nev && sel >= nev) sel = nev - 1;
        status(L"Deleted %d event%s.", cnt, cnt == 1 ? L"" : L"s");
    } break;
    case ID_EUP: case ID_EDOWN: {
        int to = sel + (id == ID_EUP ? -1 : 1);
        if (sel < 0 || to < 0 || to >= nev) return;
        Ev t = ev[sel]; ev[sel] = ev[to]; ev[to] = t;
        sel = to;
    } break;
    case ID_ESCALEGO: {
        wchar_t b[32]; GetDlgItemTextW(hMain, ID_ESCALE, b, 32);
        double f = wcstod(b, NULL);
        if (f <= 0) { status(L"Editor: scale must be above 0 (0.5 = twice as fast, 2 = half speed)."); return; }
        for (int i = 0; i < nev; i++) ev[i].delay = (DWORD)(ev[i].delay * f + 0.5);
        status(L"Scaled every delay by ×%g.", f);
    } break;
    case ID_ESTRIP: {
        int k = 0, removed = 0; DWORD carry = 0;
        for (int i = 0; i < nev; i++) {
            if (ev[i].type == EV_MV) { carry += ev[i].delay; removed++; continue; }
            ev[i].delay += carry; carry = 0;
            ev[k++] = ev[i];
        }
        nev = k; sel = -1;
        status(L"Removed %d mouse moves (their timing was kept).", removed);
    } break;
    }
    refreshList();
    lvSelect(sel);
    if (sel >= 0) fillFields(sel);
    InvalidateRect(hMain, NULL, FALSE);
}

static LRESULT listNotify(NMHDR *nh) {
    if (nh->code == LVN_GETDISPINFOW) {
        NMLVDISPINFOW *di = (NMLVDISPINFOW *)nh;
        int i = di->item.iItem;
        if (!(di->item.mask & LVIF_TEXT) || i < 0 || i >= nev) return 0;
        wchar_t *o = di->item.pszText; int n = di->item.cchTextMax;
        switch (di->item.iSubItem) {
        case 0: swprintf(o, n, L"%d", i + 1); break;
        case 1: swprintf(o, n, L"%lu", (unsigned long)ev[i].delay); break;
        case 2: swprintf(o, n, L"%hs", EVN[ev[i].type]); break;
        default: evDetails(&ev[i], o, n);
        }
    } else if (nh->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *nm = (NMLISTVIEW *)nh;
        if (nm->iItem >= 0 && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) fillFields(nm->iItem);
    }
    return 0;
}

/* ---------- presets ---------- */

static void presetsReload(const wchar_t *select) {
    HWND cb = GetDlgItem(hMain, ID_PRESET);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    static wchar_t names[8192];
    DWORD n = GetPrivateProfileStringW(L"Presets", NULL, L"", names, 8192, iniPath);
    for (wchar_t *p = names; n && *p; p += wcslen(p) + 1) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)p);
    if (select) {
        LRESULT i = SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)select);
        if (i >= 0) SendMessageW(cb, CB_SETCURSEL, i, 0); else SetWindowTextW(cb, select);
    }
}

static void presetsSeed(void) {
    wchar_t names[64];
    if (GetPrivateProfileStringW(L"Presets", NULL, L"", names, 64, iniPath)) return;
    WritePrivateProfileStringW(L"Presets", L"Me", L"100,1.4,34,14,1,1", iniPath);
    WritePrivateProfileStringW(L"Presets", L"Tired me", L"55,3.5,15,30,1,1", iniPath);
    WritePrivateProfileStringW(L"Presets", L"Focused me", L"115,0.8,50,6,1,1", iniPath);
}

static void presetApply(const wchar_t *name) {
    wchar_t v[128];
    if (!GetPrivateProfileStringW(L"Presets", name, L"", v, 128, iniPath) || !v[0]) return;
    double w = 60, ty = DEF_TYPO, bu = DEF_BURST, pa = DEF_PAUSE; int typos = 1, docs = 1;
    swscanf(v, L"%lf,%lf,%lf,%lf,%d,%d", &w, &ty, &bu, &pa, &typos, &docs);
    wchar_t b[32];
    swprintf(b, 32, L"%g", w);  SetDlgItemTextW(hMain, ID_WPM, b);
    swprintf(b, 32, L"%g", ty); SetDlgItemTextW(hMain, ID_TYPOP, b);
    swprintf(b, 32, L"%g", bu); SetDlgItemTextW(hMain, ID_BURSTP, b);
    swprintf(b, 32, L"%g", pa); SetDlgItemTextW(hMain, ID_PAUSEP, b);
    tog[TG_FIXWPM].on = tog[TG_FIXMS].on = 0; tog[TG_HUMAN].on = 1;
    tog[TG_TYPOS].on = typos != 0; tog[TG_DOCS].on = docs != 0;
    InvalidateRect(hMain, NULL, FALSE);
    status(L"Loaded preset \"%s\": %g WPM · typo %g%% · burst %g%% · pause %g%%", name, w, ty, bu, pa);
}

static void presetCmd(int id) {
    wchar_t name[128];
    GetDlgItemTextW(hMain, ID_PRESET, name, 128);
    for (wchar_t *p = name; *p; p++) if (*p == L'=' || *p == L'[' || *p == L']') *p = L'-';
    if (!name[0]) { status(L"Presets: type a name in the preset box first."); return; }
    if (id == ID_PSAVE) {
        wchar_t v[128];
        swprintf(v, 128, L"%g,%g,%g,%g,%d,%d", getDbl(ID_WPM, 60), getPct(ID_TYPOP, DEF_TYPO), getPct(ID_BURSTP, DEF_BURST),
                 getPct(ID_PAUSEP, DEF_PAUSE), tog[TG_TYPOS].on, tog[TG_DOCS].on);
        WritePrivateProfileStringW(L"Presets", name, v, iniPath);
        presetsReload(name);
        status(L"Saved preset \"%s\".", name);
    } else {
        WritePrivateProfileStringW(L"Presets", name, NULL, iniPath);
        presetsReload(NULL);
        SetDlgItemTextW(hMain, ID_PRESET, L"");
        status(L"Deleted preset \"%s\".", name);
    }
}

/* ---------- clipboard typing ---------- */

static void startClipType(void) {
    if (busy()) return;
    wchar_t *text = NULL;
    if (OpenClipboard(hMain)) {
        HANDLE d = GetClipboardData(CF_UNICODETEXT);
        wchar_t *t = d ? GlobalLock(d) : NULL;
        if (t) {
            text = calloc(wcslen(t) + 1, sizeof(wchar_t));
            wchar_t *o = text;
            for (wchar_t *q = t; *q; q++) if (*q != L'\r') *o++ = *q;
            GlobalUnlock(d);
        }
        CloseClipboard();
    }
    if (!text || !text[0]) { free(text); status(L"Clipboard has no text to type."); return; }
    TypeJob *j = calloc(1, sizeof *j);
    j->text = text; j->clip = 1;
    j->mode = tog[TG_HUMAN].on ? ID_HUMAN : tog[TG_FIXMS].on ? ID_FIXMS : ID_FIXWPM;
    j->wpm = (int)getDbl(ID_WPM, 60); if (j->wpm < 1) j->wpm = 1;
    j->ms = (int)getDbl(ID_MS, 50); if (j->ms < 0) j->ms = 0;
    j->typos = tog[TG_TYPOS].on;
    j->docsSafe = tog[TG_DOCS].on;
    j->typoP = getPct(ID_TYPOP, DEF_TYPO);
    j->burstP = getPct(ID_BURSTP, DEF_BURST);
    j->pauseP = getPct(ID_PAUSEP, DEF_PAUSE);
    stopFlag = 0; typing = 1;
    CloseHandle(CreateThread(NULL, 0, TypeThread, j, 0, NULL));
}


/* ---------- disable keyboard ---------- */

#define WM_KBTOGGLE (WM_APP + 8)
#define WM_MAGIC (WM_APP + 9)
static HHOOK hkGuard;

static int blockableVk(DWORD vk) {
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) ||
           vk == VK_SPACE || (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
}

static LRESULT CALLBACK GuardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (!(k->flags & LLKHF_INJECTED)) {
            int down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            if (k->vkCode == VK_OEM_3 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                if (down) PostMessageW(hMain, WM_KBTOGGLE, 0, 0);
                return 1;
            }
            if (magicOn) {
                DWORD vk = k->vkCode;
                int mod = (GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000;
                if (!mod && (blockableVk(vk) || vk == VK_RETURN || vk == VK_BACK || vk == VK_TAB)) {
                    if (down) PostMessageW(hMain, WM_MAGIC, vk == VK_BACK, 0);
                    return 1;
                }
            }
            if (kbBlocked && blockableVk(k->vkCode)) return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static wchar_t *magicText; static int magicIdx, magicLen;

static void setMagicLabel(void) {
    HWND b = GetDlgItem(hMain, ID_MAGIC);
    SetWindowTextW(b, magicOn ? L"Magic ON" : L"Magic");
    InvalidateRect(b, NULL, FALSE);
}

static void magicStop(int finished) {
    if (!magicOn) return;
    magicOn = 0;
    HWND te = GetDlgItem(hMain, ID_TEXT);
    if (finished || magicIdx >= magicLen) { resumeIdx = 0; resumePending = 0; highlight(-1); }
    else {
        resumeIdx = magicIdx; resumePending = 0;
        free(resumeText); resumeText = _wcsdup(magicText);
        highlight(magicIdx);
    }
    free(magicText); magicText = NULL;
    SendMessageW(te, EM_SETREADONLY, FALSE, 0);
    setMagicLabel(); setTypeLabel();
    if (finished) status(L"Magic Type finished the whole text ✨");
    else status(L"Magic Type off — stopped at character %d of %d (Type/Resume or Magic picks up there).", resumeIdx, magicLen);
}

static void magicStart(void) {
    if (busy()) return;
    HWND te = GetDlgItem(hMain, ID_TEXT);
    wchar_t *text = getRichText(te);
    int n = (int)wcslen(text);
    if (!n) { free(text); status(L"Magic Type: the Instruct text box is empty."); return; }
    magicIdx = (resumeValid(text) && resumeIdx < n) ? resumeIdx : 0;
    magicText = text; magicLen = n;
    resetTextFormat(te);
    hlPrev = -1;
    highlight(magicIdx);
    SendMessageW(te, EM_SETREADONLY, TRUE, 0);
    magicOn = 1;
    setMagicLabel();
    status(L"Magic Type on — click into your doc and mash any keys. Each one types the next character. Click Magic again to stop.");
}

static void magicKey(int back) {
    if (!magicOn) return;
    if (back) {
        if (magicIdx > 0) { sendChar(8); magicIdx--; highlight(magicIdx); }
        return;
    }
    if (magicIdx >= magicLen) { magicStop(1); return; }
    sendChar(magicText[magicIdx++]);
    if (magicIdx >= magicLen) magicStop(1);
    else {
        highlight(magicIdx);
        status(L"✨ Magic Type — %d of %d characters", magicIdx, magicLen);
    }
}

static void setKbLabel(void) {
    HWND b = GetDlgItem(hMain, ID_KBLOCK);
    SetWindowTextW(b, kbBlocked ? L"Keys OFF" : L"Disable keys");
    InvalidateRect(b, NULL, FALSE);
}

static void toggleKb(void) {
    kbBlocked = !kbBlocked;
    setKbLabel();
    status(kbBlocked ? L"Keyboard disabled — letters, numbers & symbols are blocked (FrutigerMacro can still type). Ctrl+Shift+~ turns it back on."
                     : L"Keyboard enabled again.");
}

/* ---------- controls & pages ---------- */

typedef struct { HWND h; int page, x, y, w, hh; } Ctl;
static Ctl ctls[96]; static int nctl;
static int buildPage = PG_GLOBAL;

static HWND mk(DWORD ex, const wchar_t *cls, const wchar_t *txt, DWORD st, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | st, S(x), S(y), S(w), S(h),
                             hMain, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)hFont, TRUE);
    if (nctl < 96) { Ctl k = { c, buildPage, x, y, w, h }; ctls[nctl++] = k; }
    return c;
}
static HWND BTN(const wchar_t *t, int x, int y, int w, int h, int id) {
    HWND b = mk(0, L"BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, id);
    SetWindowSubclass(b, BtnSub, 1, 0);
    return b;
}
#define NUM(t, x, y, w, id) mk(WS_EX_CLIENTEDGE, L"EDIT", t, WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 22, id)

static LRESULT CALLBACK HkSub(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR rd) {
    (void)id; (void)rd;
    if (m == WM_SETFOCUS) unregisterAll();
    else if (m == WM_KILLFOCUS) {
        int other = 0;
        for (int i = 0; i < HK_N; i++) if ((HWND)wp == hHk[i]) other = 1;
        if (!other) registerAll();
    }
    return DefSubclassProc(h, m, wp, lp);
}

static void showPage(int p) {
    curPage = p;
    hoverBtn = NULL;
    for (int i = 0; i < nctl; i++) ShowWindow(ctls[i].h, onPage(ctls[i].page, p) ? SW_SHOWNA : SW_HIDE);
    InvalidateRect(hMain, NULL, FALSE);
    if (p == PG_TEST) SetFocus(GetDlgItem(hMain, ID_TEDIT));
    else if (p == PG_INSTR) SetFocus(GetDlgItem(hMain, ID_TEXT));
    else if (p == PG_EDIT) { refreshList(); SetFocus(lv()); }
    else SetFocus(GetDlgItem(hMain, ID_REC));
}

static const struct { const wchar_t *t; int w; } COLS[] = { { L"#", 46 }, { L"Delay ms", 64 }, { L"Event", 52 }, { L"Details", 214 } };

static void buildUI(void) {
    buildPage = PG_MACRO;
    hBtn[HK_REC] = BTN(L"Record", 20, 72, 92, 28, ID_REC);
    BTN(L"Stop rec", 116, 72, 62, 28, ID_STOP);
    hBtn[HK_PLAY] = BTN(L"Play", 182, 72, 92, 28, ID_PLAY);
    BTN(L"Save", 278, 72, 46, 28, ID_SAVE);
    BTN(L"Load", 328, 72, 46, 28, ID_LOAD);
    BTN(L"Clear", 378, 72, 44, 28, ID_CLEAR);
    NUM(L"1", 112, 112, 42, ID_REPEAT);
    NUM(L"1.0", 212, 112, 42, ID_SPEED);

    buildPage = PG_INSTR;
    HWND te = mk(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                 20, 70, 400, 110, ID_TEXT);
    SendMessageW(te, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);
    SendMessageW(te, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
    SendMessageW(te, EM_SETTARGETDEVICE, 0, 0);
    resetTextFormat(te);
    SetWindowSubclass(te, RichSub, 2, 0);
    mk(0, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT, 66, 188, 170, 200, ID_PRESET);
    BTN(L"Save preset", 240, 186, 88, 26, ID_PSAVE);
    BTN(L"Delete", 332, 186, 88, 26, ID_PDEL);
    NUM(L"100", 62, 248, 44, ID_WPM);
    NUM(L"50", 174, 248, 44, ID_MS);
    NUM(L"3", 330, 248, 40, ID_DELAY);
    NUM(L"1.4", 74, 280, 38, ID_TYPOP);
    NUM(L"34", 176, 280, 38, ID_BURSTP);
    NUM(L"14", 282, 280, 38, ID_PAUSEP);
    hBtn[HK_TYPE] = BTN(L"Type", 20, 314, 100, 30, ID_TYPE);
    hBtn[HK_STOP] = BTN(L"Stop", 124, 314, 76, 30, ID_TSTOP);
    BTN(L"Restart", 204, 314, 70, 30, ID_TRESTART);
    BTN(L"Clear", 278, 314, 58, 30, ID_TCLEAR);
    BTN(L"Magic", 340, 314, 80, 30, ID_MAGIC);
    SendMessageW(te, EM_SETEVENTMASK, 0, ENM_CHANGE);

    buildPage = PG_EDIT;
    HWND l = mk(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 20, 70, 400, 170, ID_LIST);
    SendMessageW(l, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(l, L"Explorer", NULL);
    SendMessageW(l, LVM_SETTEXTCOLOR, 0, NAVY);
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.cx = S(COLS[i].w); col.pszText = (wchar_t *)COLS[i].t; col.iSubItem = i;
        SendMessageW(l, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }
    NUM(L"", 60, 248, 58, ID_EDELAY);
    HWND evc = mk(0, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 164, 248, 96, 300, ID_EEVENT);
    SendMessageW(evc, CB_SETDROPPEDWIDTH, S(150), 0);
    for (int i = 0; i < EV_COUNT; i++) SendMessageW(evc, CB_ADDSTRING, 0, (LPARAM)EVDESC[i]);
    SendMessageW(evc, CB_SETCURSEL, EV_TYPE, 0);
    NUM(L"", 300, 248, 120, ID_EARGS);
    BTN(L"Update", 20, 280, 74, 28, ID_EUPD);
    BTN(L"Insert", 98, 280, 74, 28, ID_EINS);
    BTN(L"Delete", 176, 280, 74, 28, ID_EDEL);
    BTN(L"Move up", 254, 280, 80, 28, ID_EUP);
    BTN(L"Move down", 338, 280, 82, 28, ID_EDOWN);
    NUM(L"1.0", 110, 316, 46, ID_ESCALE);
    BTN(L"Apply", 160, 314, 64, 28, ID_ESCALEGO);
    BTN(L"Strip mouse moves", 228, 314, 132, 28, ID_ESTRIP);
    BTN(L"Save…", 364, 314, 56, 28, ID_ESAVE);

    buildPage = PG_UPDATE;
    HWND nb = mk(0, L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 26, 190, 388, 94, ID_UNOTES);
    (void)nb;
    BTN(L"Check now", 20, 326, 110, 28, ID_UCHECK);
    BTN(L"Install update", 134, 326, 140, 28, ID_UINSTALL);
    BTN(L"Open GitHub page", 278, 326, 142, 28, ID_UPAGE);

    buildPage = PG_TEST;
    HWND ed = mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, 20, 202, 400, 110, ID_TEDIT);
    SendMessageW(ed, WM_SETFONT, (WPARAM)hPrompt, TRUE);
    SetWindowSubclass(ed, TestEditSub, 3, 0);
    BTN(L"New prompt", 20, 488, 110, 30, ID_TNEW);
    BTN(L"Finish", 136, 488, 90, 30, ID_TFIN);
    BTN(L"Apply to Humanizer", 232, 488, 188, 30, ID_TAPPLY);

    buildPage = PG_GLOBAL;
    for (int i = 0; i < HK_N; i++) {
        int x = (i & 1) ? 222 : 20, y = 400 + (i / 2) * 28;
        hHk[i] = mk(0, HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, x + 64, y, (i & 1) ? 134 : 128, 22, ID_HK0 + i);
        SetWindowSubclass(hHk[i], HkSub, 4, 0);
    }
    BTN(L"Apply hotkeys", 222, 455, 198, 26, ID_HKAPPLY);
    BTN(L"Disable keys", 318, 504, 104, 28, ID_KBLOCK);
}

static void makeFonts(void) {
    HFONT old[6] = { hFont, hBold, hTitle, hSmall, hPrompt, hBtnSmall };
    hFont   = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL,   0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    hBold   = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    hTitle  = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    hSmall  = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL,   1, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    hPrompt = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL,   0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    hBtnSmall = CreateFontW(-S(11), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    for (int i = 0; i < 6; i++) if (old[i]) DeleteObject(old[i]);
}

static void makeCanvases(void) {
    int cw = S(440), ch = S(546), fw = cw + 2 * S(FR), fh = ch + S(TT) + S(FR);
    for (int p = 0; p < PG_N; p++) { if (pageC[p].dc) freeCanvas(&pageC[p]); pageC[p] = mkCanvas(cw, ch); }
    if (backC.dc) freeCanvas(&backC);
    if (frameC.dc) freeCanvas(&frameC);
    if (frameBack.dc) freeCanvas(&frameBack);
    backC = mkCanvas(cw, ch);
    frameC = mkCanvas(fw, fh);
    frameBack = mkCanvas(fw, fh);
    renderPages();
    renderFrame();
}

static void rescaleAll(void) {
    makeFonts();
    makeCanvases();
    rescaling = 1;
    RECT wr; GetWindowRect(hFrame, &wr);
    SetWindowPos(hFrame, NULL, wr.left, wr.top, frameC.w, frameC.h, SWP_NOZORDER | SWP_NOACTIVATE);
    rescaling = 0;
    SetWindowPos(hMain, NULL, S(FR), S(TT), backC.w, backC.h, SWP_NOZORDER | SWP_NOACTIVATE);
    for (int i = 0; i < nctl; i++) {
        SetWindowPos(ctls[i].h, NULL, S(ctls[i].x), S(ctls[i].y), S(ctls[i].w), S(ctls[i].hh), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(ctls[i].h, WM_SETFONT, (WPARAM)(GetDlgCtrlID(ctls[i].h) == ID_TEDIT ? hPrompt : hFont), TRUE);
    }
    for (int i = 0; i < 4; i++) SendMessageW(lv(), LVM_SETCOLUMNWIDTH, i, S(COLS[i].w));
    resetTextFormat(GetDlgItem(hMain, ID_TEXT));
    ShowWindow(hMain, SW_SHOW);
    showPage(curPage);
    RedrawWindow(hFrame, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    wchar_t b[32]; swprintf(b, 32, L"%.3f", uiScale);
    WritePrivateProfileStringW(L"Settings", L"Scale", b, iniPath);
}

static const struct { int id; const wchar_t *key; } SAVED[] = {
    { ID_REPEAT, L"Repeat" }, { ID_SPEED, L"Speed" }, { ID_WPM, L"WPM" }, { ID_MS, L"MsPerChar" },
    { ID_DELAY, L"StartDelay" }, { ID_TYPOP, L"TypoPct" }, { ID_BURSTP, L"BurstPct" }, { ID_PAUSEP, L"PausePct" },
};
static const wchar_t *TOGKEY[TG_N] = { L"RecordMoves", L"ModeFixedWPM", L"ModeFixedMs", L"ModeHumanizer", L"Typos", L"DocsSafe", L"UpdateCheck", L"UpdateAuto" };

static void loadSettings(void) {
    wchar_t b[64];
    for (unsigned i = 0; i < sizeof SAVED / sizeof SAVED[0]; i++)
        if (GetPrivateProfileStringW(L"Settings", SAVED[i].key, L"\x01", b, 64, iniPath) && b[0] != 1)
            SetDlgItemTextW(hMain, SAVED[i].id, b);
    for (int i = 0; i < TG_N; i++) tog[i].on = GetPrivateProfileIntW(L"Settings", TOGKEY[i], tog[i].on, iniPath) != 0;
}

static void saveSettings(void) {
    wchar_t b[64];
    for (unsigned i = 0; i < sizeof SAVED / sizeof SAVED[0]; i++) {
        GetDlgItemTextW(hMain, SAVED[i].id, b, 64);
        WritePrivateProfileStringW(L"Settings", SAVED[i].key, b, iniPath);
    }
    for (int i = 0; i < TG_N; i++) WritePrivateProfileStringW(L"Settings", TOGKEY[i], tog[i].on ? L"1" : L"0", iniPath);
}

static void applyTest(void) {
    if (!haveRes) { finishTest(); if (!haveRes) return; }
    wchar_t b[32];
    swprintf(b, 32, L"%.0f", resWpm);   SetDlgItemTextW(hMain, ID_WPM, b);
    swprintf(b, 32, L"%.1f", resTypo);  SetDlgItemTextW(hMain, ID_TYPOP, b);
    swprintf(b, 32, L"%.0f", resBurst); SetDlgItemTextW(hMain, ID_BURSTP, b);
    swprintf(b, 32, L"%.0f", resPause); SetDlgItemTextW(hMain, ID_PAUSEP, b);
    tog[TG_FIXWPM].on = tog[TG_FIXMS].on = 0; tog[TG_HUMAN].on = 1; tog[TG_TYPOS].on = 1;
    showPage(PG_INSTR);
    status(L"Applied your typing profile to the Humanizer: %.0f WPM · typo %.1f%% · burst %.0f%% · pause %.0f%%",
           resWpm, resTypo, resBurst, resPause);
}

static int tabAt(int mx, int my) {
    POINT p = { mx, my };
    for (int i = 0; i < PG_N; i++) { RECT r = tabRect(i); if (PtInRect(&r, p)) return i; }
    return -1;
}

static void clickToggle(int mx, int my) {
    for (int i = 0; i < TG_N; i++) {
        if (tog[i].page != curPage) continue;
        RECT r = RS(tog[i].x, tog[i].y, tog[i].w, 22);
        if (mx < r.left || mx >= r.right || my < r.top || my >= r.bottom) continue;
        if (tog[i].radio) { for (int k = 0; k < TG_N; k++) if (tog[k].radio) tog[k].on = 0; tog[i].on = 1; }
        else tog[i].on = !tog[i].on;
        InvalidateRect(hMain, NULL, FALSE);
        return;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REC: startRec(); break;
        case ID_STOP: stopRec(); break;
        case ID_PLAY: startPlay(); break;
        case ID_TYPE: startType(); break;
        case ID_TSTOP: stopAll(); break;
        case ID_CLEAR: if (!busy()) { clearEv(); curFile[0] = 0; idleStatus(); } break;
        case ID_HKAPPLY: applyHotkeys(1); break;
        case ID_TNEW: newPrompt(1); break;
        case ID_UCHECK: startCheck(); break;
        case ID_UINSTALL: startInstall(); break;
        case ID_UPAGE: {
            wchar_t url[600];
            if (updPage[0]) wcscpy(url, updPage); else swprintf(url, 600, L"https://github.com/%s", updRepo);
            ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        } break;
        case ID_KBLOCK: toggleKb(); break;
        case ID_MAGIC: if (magicOn) magicStop(0); else magicStart(); break;
        case ID_EUPD: case ID_EINS: case ID_EDEL: case ID_EUP: case ID_EDOWN: case ID_ESCALEGO: case ID_ESTRIP:
            editorCmd(LOWORD(wp)); break;
        case ID_ESAVE: SendMessageW(h, WM_COMMAND, ID_SAVE, 0); break;
        case ID_PSAVE: case ID_PDEL: presetCmd(LOWORD(wp)); break;
        case ID_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                wchar_t nm[128]; LRESULT i = SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (i >= 0 && SendMessageW((HWND)lp, CB_GETLBTEXTLEN, i, 0) < 127) {
                    SendMessageW((HWND)lp, CB_GETLBTEXT, i, (LPARAM)nm);
                    presetApply(nm);
                }
            }
            break;
        case ID_TRESTART: if (!busy()) { resetResume(); startType(); } break;
        case ID_TCLEAR:
            if (!busy()) { SetWindowTextW(GetDlgItem(h, ID_TEXT), L""); hlPrev = -1; resetResume(); idleStatus(); }
            break;
        case ID_TEXT:
            if (HIWORD(wp) == EN_CHANGE && !typing && resumeIdx > 0) {
                wchar_t *t = getRichText((HWND)lp);
                if (!resumeValid(t)) { hlPrev = -1; resetTextFormat((HWND)lp); resetResume(); }
                free(t);
            }
            break;
        case ID_TFIN: finishTest(); break;
        case ID_TAPPLY: applyTest(); break;
        case ID_SAVE: {
            wchar_t p[MAX_PATH];
            if (busy()) break;
            if (!nev) { status(L"Nothing to save yet."); break; }
            if (fileDlg(p, 1)) {
                if (saveMacro(p)) { wcscpy(curFile, p); status(L"Saved %d events → %s", nev, p); }
                else status(L"Couldn't save to %s", p);
            }
        } break;
        case ID_LOAD: {
            wchar_t p[MAX_PATH];
            if (busy()) break;
            if (fileDlg(p, 0)) {
                if (loadMacro(p)) { wcscpy(curFile, p); status(L"Loaded %d events from %s", nev, p); }
                else status(L"Couldn't open %s", p);
            }
        } break;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), t = tabAt(x, y);
        if (t >= 0) { if (t != curPage) showPage(t); }
        else clickToggle(x, y);
    } return 0;
    case WM_MOUSEMOVE: {
        int t = tabAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (t != tabHover) {
            tabHover = t;
            TRACKMOUSEEVENT tm = { sizeof tm, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tm);
            RECT r = RS(10, 8, 420, 32); InvalidateRect(h, &r, FALSE);
        }
    } return 0;
    case WM_MOUSELEAVE: if (tabHover >= 0) { tabHover = -1; RECT r = RS(10, 8, 420, 32); InvalidateRect(h, &r, FALSE); } return 0;
    case WM_DRAWITEM: drawButton((DRAWITEMSTRUCT *)lp); return TRUE;
    case WM_NOTIFY: if (((NMHDR *)lp)->idFrom == ID_LIST) return listNotify((NMHDR *)lp); break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, NAVY); SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        paintMain(dc);
        EndPaint(h, &ps);
    } return 0;
    case WM_HOTKEY: {
        wchar_t cls[32] = L"";
        GetClassNameW(GetFocus(), cls, 32);
        if (GetForegroundWindow() == hFrame && (curPage == PG_TEST || !_wcsicmp(cls, HOTKEY_CLASSW))) return 0;
        switch ((int)wp - 1) {
        case HK_REC: recording ? stopRec() : startRec(); break;
        case HK_PLAY: playing ? (void)(stopFlag = 1) : startPlay(); break;
        case HK_TYPE: typing ? (void)(stopFlag = 1) : startType(); break;
        case HK_STOP: stopAll(); break;
        case HK_CLIP: typing ? (void)(stopFlag = 1) : startClipType(); break;
        }
    } return 0;
    case WM_STATUS:
        wcsncpy(statusText, (wchar_t *)lp, 511); free((void *)lp);
        if (!recording && curPage == PG_EDIT) refreshList();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DONE:
        if (!typing) SendMessageW(GetDlgItem(h, ID_TEXT), EM_SETREADONLY, FALSE, 0);
        setTypeLabel();
        if (!busy()) {
            if (resumeIdx > 0 && resumeText)
                status(L"Paused at character %d of %d — Resume picks up right there, Restart starts over.",
                       resumeIdx, (int)wcslen(resumeText));
            else idleStatus();
        }
        return 0;
    case WM_HL: highlight((int)(INT_PTR)wp); return 0;
    case WM_KBTOGGLE: toggleKb(); return 0;
    case WM_MAGIC: magicKey((int)wp); return 0;
    case WM_UPDCHECKED:
        fillNotes();
        InvalidateRect(h, NULL, FALSE);
        if (updAvail == 1) {
            if (tog[TG_UAUTO].on && !busy()) startInstall();
            else status(L"Update %s is available — open the Update tab to install it.", updLatest);
        }
        return 0;
    case WM_UPDFAIL: {
        wchar_t *m = (wchar_t *)lp;
        updErr = 1;
        _snwprintf(updConnMsg, 159, L"%s  Try restarting the macro.", m); updConnMsg[159] = 0;
        status(L"%s  Try restarting the macro.", m); free(m); InvalidateRect(h, NULL, FALSE);
    } return 0;
    case WM_UPDRESTART: restartUpdated(); return 0;
    case WM_DESTROY:
        saveSettings();
        stopAll();
        for (int i = 0; i < HK_N; i++) UnregisterHotKey(h, i + 1);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

#define IDI_MAIL 2

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    LoadLibraryW(L"Msftedit.dll");
    timeBeginPeriod(1);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpf = (double)f.QuadPart;

    HDC dc = GetDC(NULL); dpi = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f; ReleaseDC(NULL, dc);
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wcscpy(iniPath, exePath);
    wchar_t *sl = wcsrchr(iniPath, L'\\'); if (sl) sl[1] = 0;
    wcscat(iniPath, L"FrutigerMacro.ini");
    {
        wchar_t oldIni[MAX_PATH]; wcscpy(oldIni, iniPath);
        wchar_t *s2 = wcsrchr(oldIni, L'\\'); if (s2) s2[1] = 0;
        wcscat(oldIni, L"DoogMacro.ini");
        if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(oldIni) != INVALID_FILE_ATTRIBUTES)
            MoveFileW(oldIni, iniPath);
    }
    {
        wchar_t old[MAX_PATH + 8]; swprintf(old, MAX_PATH + 8, L"%s.old", exePath);
        for (int i = 0; i < 20 && GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(old); i++) Sleep(150);
        justUpdated = wcsstr(GetCommandLineW(), L"--updated") != NULL;
        wchar_t b[64];
        GetPrivateProfileStringW(L"Settings", L"Scale", L"1", b, 64, iniPath);
        uiScale = (float)_wtof(b);
        if (uiScale < 0.7f || uiScale > 2.2f) uiScale = 1.0f;
        GetPrivateProfileStringW(L"Update", L"Repo", updRepo, updRepo, 128, iniPath);
    }
    makeFonts();
    for (int i = 0; i < HK_N; i++) hkVal[i] = (WORD)GetPrivateProfileIntW(L"Hotkeys", HKKEY[i], hkVal[i], iniPath);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"DoogMacro";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW fc = {0};
    fc.style = CS_HREDRAW | CS_VREDRAW;
    fc.lpfnWndProc = FrameProc; fc.hInstance = hi; fc.lpszClassName = L"DoogFrame";
    fc.hCursor = LoadCursor(NULL, IDC_ARROW);
    fc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_MAIL));
    RegisterClassW(&fc);

    makeCanvases();
    int cw = backC.w, ch = backC.h, fw = frameC.w, fh = frameC.h;

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    hFrame = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, L"DoogFrame", L"FrutigerMacro",
                             WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                             wa.left + (wa.right - wa.left - fw) / 2, wa.top + (wa.bottom - wa.top - fh) / 2, fw, fh,
                             NULL, NULL, hi, NULL);
    SendMessageW(hFrame, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(hi, MAKEINTRESOURCEW(IDI_MAIL), IMAGE_ICON,
                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    SendMessageW(hFrame, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(hi, MAKEINTRESOURCEW(IDI_MAIL), IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    DWORD pref = 2;
    DwmSetWindowAttribute(hFrame, 33, &pref, sizeof pref);
    MARGINS mg = { 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hFrame, &mg);
    SetWindowPos(hFrame, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    hMain = CreateWindowExW(WS_EX_CONTROLPARENT, L"DoogMacro", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                            S(FR), S(TT), cw, ch, hFrame, NULL, hi, NULL);
    buildUI();
    presetsSeed();
    presetsReload(L"Me");
    loadSettings();
    applyHotkeys(0);
    newPrompt(0);
    fillNotes();
    showPage(PG_INSTR);
    idleStatus();
    setKbLabel();
    hkGuard = SetWindowsHookExW(WH_KEYBOARD_LL, GuardProc, GetModuleHandleW(NULL), 0);
    if (justUpdated) status(L"Updated to v%s — enjoy!", APP_VERSION_W);
    if (tog[TG_UCHECK].on) startCheck();
    ShowWindow(hFrame, show);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hMain, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    timeEndPeriod(1);
    return 0;
}
