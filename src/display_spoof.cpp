#include "game_addrs.h"
#include "display_spoof.h"
#include "logger.h"
#include "control.h"
#include "vr_session.h"

#include <windows.h>
#include <cstdio>
#include <cwctype>
#include <intrin.h>
#include <string>

namespace vr::display {

namespace {

// Voci della IAT di SpecOpsTheLine.exe (USER32), vedi README.
const uintptr_t& kIatGetSystemMetrics = vr::addr::g[vr::addr::IatGetSystemMetrics];
const uintptr_t& kIatSystemParametersInfoW = vr::addr::g[vr::addr::IatSystemParametersInfoW];
const uintptr_t& kIatSetWindowPos = vr::addr::g[vr::addr::IatSetWindowPos];
const uintptr_t& kIatMoveWindow = vr::addr::g[vr::addr::IatMoveWindow];
const uintptr_t& kIatGetClientRect = vr::addr::g[vr::addr::IatGetClientRect];
// Solo collaudi (specopsvr_keepfocus): chi e' in primo piano (import dell'exe, 25/10).
const uintptr_t& kIatGetForegroundWindow = vr::addr::g[vr::addr::IatGetForegroundWindow];
const uintptr_t& kIatGetActiveWindow = vr::addr::g[vr::addr::IatGetActiveWindow];
const uintptr_t& kIatGetFocus = vr::addr::g[vr::addr::IatGetFocus];
const uintptr_t& kIatShowWindow = vr::addr::g[vr::addr::IatShowWindow];
const uintptr_t& kIatSetWindowPlacement = vr::addr::g[vr::addr::IatSetWindowPlacement];
const uintptr_t& kIatGetCommandLineW = vr::addr::g[vr::addr::IatGetCommandLineW];

// Desktop minimo riportato al gioco. La finestra reale puo' uscire dallo
// schermo: conta solo la dimensione del back buffer.
constexpr int kMinWidth = 4096;
constexpr int kMinHeight = 4096;

typedef int(WINAPI* GetSystemMetrics_t)(int index);
typedef BOOL(WINAPI* SystemParametersInfoW_t)(UINT action, UINT param, PVOID pv, UINT winIni);
GetSystemMetrics_t g_GetSystemMetrics_Real = nullptr;
SystemParametersInfoW_t g_SystemParametersInfoW_Real = nullptr;

int Grow(int value, int minimum) { return value < minimum ? minimum : value; }

int WINAPI Hooked_GetSystemMetrics(int index) {
    int v = g_GetSystemMetrics_Real(index);
    switch (index) {
        case SM_CXSCREEN:
        case SM_CXVIRTUALSCREEN:
        case SM_CXFULLSCREEN:
        case SM_CXMAXIMIZED:
        case SM_CXMAXTRACK:
            return Grow(v, kMinWidth);
        case SM_CYSCREEN:
        case SM_CYVIRTUALSCREEN:
        case SM_CYFULLSCREEN:
        case SM_CYMAXIMIZED:
        case SM_CYMAXTRACK:
            return Grow(v, kMinHeight);
        default:
            return v;
    }
}

BOOL WINAPI Hooked_SystemParametersInfoW(UINT action, UINT param, PVOID pv, UINT winIni) {
    BOOL ok = g_SystemParametersInfoW_Real(action, param, pv, winIni);
    if (ok && action == SPI_GETWORKAREA && pv) {
        RECT* r = static_cast<RECT*>(pv);
        r->right = r->left + Grow(r->right - r->left, kMinWidth);
        r->bottom = r->top + Grow(r->bottom - r->top, kMinHeight);
    }
    return ok;
}

bool Patch(uintptr_t slot, const char* name, void* replacement, void** outOriginal, const char* dll = "user32.dll") {
    HMODULE module = GetModuleHandleA(dll);
    void* expected = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    void** entry = reinterpret_cast<void**>(slot);
    if (!slot || !expected || *entry != expected) {
        vr::log::Line(std::string("DISPLAY: IAT entry ") + name + " unexpected, leaving it alone.");
        return false;
    }
    DWORD old;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *outOriginal = expected;
    *entry = replacement;
    VirtualProtect(entry, sizeof(void*), old, &old);
    return true;
}

// Gioco sempre in finestra (28/10, issue #3): in fullscreen esclusivo il device
// D3D9Ex non nasceva (CreateDeviceEx senza display mode -> D3DERR_INVALIDCALL) e
// il gioco renderizzava alla risoluzione del monitor invece che a quella del
// visore; la risoluzione automatica usa comunque "setres ...w". UE3 legge la
// riga di comando con GetCommandLineW in WinMain: "-windowed" vince anche
// sull'impostazione Fullscreen salvata dal gioco; un "-fullscreen" esplicito
// diventa "-windowed".
typedef LPWSTR(WINAPI* GetCommandLineW_t)();
GetCommandLineW_t g_GetCommandLineW_Real = nullptr;

LPWSTR WINAPI Hooked_GetCommandLineW() {
    static std::wstring line = [] {
        std::wstring s = g_GetCommandLineW_Real();
        std::wstring lower = s;
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        const std::wstring fs = L"-fullscreen";
        for (size_t at = lower.find(fs); at != std::wstring::npos; at = lower.find(fs, at + 1)) {
            s.replace(at, fs.size(), L"-windowed  ");
            lower.replace(at, fs.size(), L"-windowed  ");
        }
        if (lower.find(L"-windowed") == std::wstring::npos) s += L" -windowed";
        return s;
    }();
    return &line[0];
}

// Diagnostica: chi ridimensiona la finestra del gioco (e a quanto)?
typedef BOOL(WINAPI* SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI* MoveWindow_t)(HWND, int, int, int, int, BOOL);
SetWindowPos_t g_SetWindowPos_Real = nullptr;
MoveWindow_t g_MoveWindow_Real = nullptr;

void LogResize(const char* api, int x, int y, int cx, int cy, UINT flags, void* caller) {
    static int budget = 40;
    if (budget-- <= 0) return;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "DISPLAY: %s pos=(%d,%d) size=%dx%d flags=0x%X caller=0x%p",
                  api, x, y, cx, cy, flags, caller);
    vr::log::Line(buf);
}

bool SafeRead(const uint32_t* p, uint32_t* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

BOOL WINAPI Hooked_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(flags & SWP_NOSIZE)) {
        LogResize("SetWindowPos", x, y, cx, cy, flags, _ReturnAddress());
        // Stack grezzo: indirizzi nel .text dell'exe presenti sullo stack.
        std::string stack = "DISPLAY:   stack:";
        uint32_t* sp = reinterpret_cast<uint32_t*>(_AddressOfReturnAddress());
        int found = 0;
        for (int i = 0; i < 400 && found < 14; ++i) {
            uint32_t v = 0;
            if (!SafeRead(sp + i, &v)) break;
            if (v >= vr::addr::TextBegin() && v < vr::addr::TextEnd()) {
                char b[16];
                std::snprintf(b, sizeof(b), " %08X", v);
                stack += b;
                ++found;
            }
        }
        vr::log::Line(stack);
    }
    return g_SetWindowPos_Real(h, after, x, y, cx, cy, flags);
}

BOOL WINAPI Hooked_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL repaint) {
    LogResize("MoveWindow", x, y, cx, cy, 0, _ReturnAddress());
    return g_MoveWindow_Real(h, x, y, cx, cy, repaint);
}

// UE3 confronta periodicamente il viewport con GetClientRect della finestra
// e si ridimensiona a quella (0x00BE0770). Windows pero' riduce la finestra
// quando la ripristina da minimizzata (1940x1066 sul monitor di sviluppo),
// ignorando WM_GETMINMAXINFO. Per la finestra del gioco riportiamo quindi la
// dimensione richiesta: D3D9 in finestra scala da solo il back buffer.
typedef BOOL(WINAPI* GetClientRect_t)(HWND, LPRECT);
GetClientRect_t g_GetClientRect_Real = nullptr;
HWND g_gameWindow = nullptr;
int g_wantedWidth = 0;
int g_wantedHeight = 0;

// Finestra minimizzata: senza visore si lascia il comportamento normale (UE3
// vede 0x0). Col visore in uso invece no: il 20/10, con Virtual Desktop, la
// finestra e' stata minimizzata alla chiusura della dashboard di SteamVR, UE3
// ha ridotto il viewport a 0x0 (SetWindowPos 6x40) e il video Bink del menu e'
// andato in crash in binkw32.dll scrivendo oltre il suo buffer ("Fatal error").
// Visore in uso: sessione Scene e visore indossato. Solo Scene non bastava:
// la mod resta in Scene per tutta la partita, e tolto il visore la finestra
// non si poteva piu' minimizzare (audit 20/10).
bool HeadsetInUse() { return vr::session::IsScene() && vr::session::HmdPresent(); }

// Solo collaudi: con il file "specopsvr_keepfocus" accanto alla dll la finestra
// tiene il focus anche senza visore. Con due copie del gioco aperte insieme il
// focus preso da una metteva in pausa l'altra e i collaudi andavano persi (25/10).
bool TestKeepFocus() {
    static int cached = -1;
    if (cached < 0) {
        char path[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&TestKeepFocus), &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        std::string flag(path);
        flag = flag.substr(0, flag.find_last_of("\\/")) + "\\specopsvr_keepfocus";
        cached = GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
        if (cached) vr::log::Line("DISPLAY: specopsvr_keepfocus present: the window keeps the focus without a headset (tests).");
    }
    return cached == 1;
}

BOOL WINAPI Hooked_GetClientRect(HWND h, LPRECT r) {
    BOOL ok = g_GetClientRect_Real(h, r);
    // Anche ridotta a icona e senza visore: il rettangolo vero 0x0 faceva
    // ridurre la viewport a 0x0 e il video del menu (binkw32) scriveva oltre
    // il suo buffer, "Fatal error" (visto il 21/10 col mod senza OpenVR).
    if (ok && r && h == g_gameWindow && g_wantedWidth > 0) {
        r->left = r->top = 0;
        r->right = g_wantedWidth;
        r->bottom = g_wantedHeight;
    }
    return ok;
}

WNDPROC g_originalWndProc = nullptr;
LONG volatile g_wndMessages = 0;

// Diagnostica (21/10): senza OpenVR la finestra del gioco finiva ridotta a
// icona (viewport 0x0, crash del video del menu) mentre senza mod no. Si
// registrano ShowWindow/SetWindowPlacement del gioco con lo stack.
typedef BOOL(WINAPI* ShowWindow_t)(HWND, int);
typedef BOOL(WINAPI* SetWindowPlacement_t)(HWND, const WINDOWPLACEMENT*);
ShowWindow_t g_ShowWindow_Real = nullptr;
SetWindowPlacement_t g_SetWindowPlacement_Real = nullptr;

void LogStack(const char* what, int arg) {
    void* frames[10] = {};
    USHORT n = CaptureStackBackTrace(1, 10, frames, nullptr);
    char b[260];
    int len = std::snprintf(b, sizeof(b), "DISPLAY: %s(%d) called from", what, arg);
    for (USHORT i = 0; i < n && len < 240; ++i) len += std::snprintf(b + len, sizeof(b) - len, " %p", frames[i]);
    vr::log::Line(b);
}

BOOL WINAPI Hooked_ShowWindow(HWND h, int cmd) {
    static int budget = 4;
    if (budget-- > 0 || cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)
        LogStack("ShowWindow", cmd);
    return g_ShowWindow_Real(h, cmd);
}

BOOL WINAPI Hooked_SetWindowPlacement(HWND h, const WINDOWPLACEMENT* p) {
    LogStack("SetWindowPlacement", p ? static_cast<int>(p->showCmd) : -1);
    return g_SetWindowPlacement_Real(h, p);
}

// Messaggio della mod: prendi il focus della tastiera (SetFocus vale solo
// dal thread della finestra; KeepSubclass gira su quello di rendering).
constexpr UINT kMsgTakeFocus = WM_APP + 0x51;

LRESULT CALLBACK UnclampedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    InterlockedIncrement(&g_wndMessages);
    if (msg == WM_CLOSE || msg == WM_DESTROY || (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE)) {
        if (!vr::control::g_exiting.exchange(true)) vr::log::Line("DISPLAY: game window closing, the mod stops touching the game.");
    }
    if (msg == WM_NCDESTROY && g_originalWndProc) {
        // Finestra distrutta: si rimette la procedura del gioco e ci si toglie.
        WNDPROC orig = g_originalWndProc;
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        return CallWindowProcW(orig, hwnd, msg, wp, lp);
    }
    // Col visore in uso la finestra del gioco che perde il focus (clic sul
    // desktop di Virtual Desktop, una notifica) non deve mettere in pausa il
    // gioco ne' fermare l'input: i messaggi di disattivazione non arrivano
    // al gioco ("keepfocus off" per il comportamento normale; audit 19/10).
    if (vr::control::g_keepFocus && (HeadsetInUse() || TestKeepFocus())) {
        bool deactivate = (msg == WM_ACTIVATEAPP && wp == FALSE) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) ||
                          msg == WM_KILLFOCUS;
        if (deactivate) {
            static int logged = 0;
            if (logged++ < 3) vr::log::Line("DISPLAY: game window lost focus, ignored while the headset is in use.");
            return 0;
        }
        // Minimizzata da fuori (Virtual Desktop, Win+D, un'altra app a schermo
        // intero): il gioco non deve vederlo; la finestra torna com'era senza
        // prendere il focus (vedi Hooked_GetClientRect, crash Bink 20/10).
        if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MINIMIZE) return 0;
        if (msg == WM_SIZE && wp == SIZE_MINIMIZED) {
            static int logged = 0;
            if (logged++ < 3) vr::log::Line("DISPLAY: game window minimized while the headset is in use, restored.");
            ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
            return 0;
        }
    }
    // Diagnostica: attivazione e riduzione a icona (chi la chiede: stack).
    if (msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE || msg == WM_KILLFOCUS || msg == WM_SETFOCUS ||
        (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MINIMIZE) || msg == WM_SHOWWINDOW) {
        static int budget = 24;
        if (budget-- > 0) {
            char b[120];
            std::snprintf(b, sizeof(b), "DISPLAY: msg 0x%04X wp=0x%X lp=0x%X foreground=%s", msg, (unsigned)wp, (unsigned)lp,
                          GetForegroundWindow() == hwnd ? "game" : "other");
            vr::log::Line(b);
        }
    }
    if (msg == WM_SIZE && wp == SIZE_MINIMIZED) {
        void* frames[12] = {};
        USHORT n = CaptureStackBackTrace(0, 12, frames, nullptr);
        std::string s = "DISPLAY: minimized, call stack:";
        for (USHORT i = 0; i < n; ++i) {
            char a[16];
            std::snprintf(a, sizeof(a), " %p", frames[i]);
            s += a;
        }
        vr::log::Line(s);
    }
    // Attivazione all'avvio (lancio da Steam / dal visore): arrivava
    // WM_ACTIVATE con il flag "ridotta a icona" (wp 0x200001) e mai
    // WM_SETFOCUS, cosi' il gioco ignorava i comandi finche' non si cliccava
    // sulla finestra (22/10, nel visore). Finestra non ridotta: il flag si
    // toglie, e il focus della tastiera si prende qui (thread della finestra).
    if (msg == kMsgTakeFocus) {
        if (GetForegroundWindow() == hwnd && GetFocus() != hwnd) {
            SetFocus(hwnd);
            vr::log::Line("DISPLAY: game in the foreground without keyboard focus, focus taken.");
        }
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) {
        if (HIWORD(wp) && !IsIconic(hwnd)) wp = MAKEWPARAM(LOWORD(wp), 0);
        LRESULT ra = CallWindowProcW(g_originalWndProc, hwnd, msg, wp, lp);
        if (GetFocus() != hwnd) {
            SetFocus(hwnd);
            vr::log::Line("DISPLAY: window activated without keyboard focus, focus taken.");
        }
        return ra;
    }
    LRESULT r = CallWindowProcW(g_originalWndProc, hwnd, msg, wp, lp);
    if (msg == WM_SIZE) {
        static int budget = 20;
        if (budget-- > 0) {
            vr::log::Line("DISPLAY: WM_SIZE " + std::to_string(LOWORD(lp)) + "x" + std::to_string(HIWORD(lp)) +
                          " type=" + std::to_string(wp));
        }
    }
    if (msg == WM_GETMINMAXINFO) {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMaxTrackSize.x = Grow(mmi->ptMaxTrackSize.x, kMinWidth + 64);
        mmi->ptMaxTrackSize.y = Grow(mmi->ptMaxTrackSize.y, kMinHeight + 64);
        mmi->ptMaxSize.x = Grow(mmi->ptMaxSize.x, kMinWidth + 64);
        mmi->ptMaxSize.y = Grow(mmi->ptMaxSize.y, kMinHeight + 64);
    }
    return r;
}

}  // namespace

HWND GameWindow() { return g_gameWindow; }

// Con OpenXR nessuno riporta il gioco davanti (SteamVR lo faceva da solo):
// 22/10 nel visore il pad era morto perche' davanti c'era un terminale.
// SetForegroundWindow da un processo in secondo piano e' permesso solo con
// l'input agganciato al thread in primo piano (o dopo un tasto simulato).
void BringToForeground(const char* why) {
    HWND h = g_gameWindow;
    if (!h || vr::control::g_exiting || GetForegroundWindow() == h) return;
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = fgTid && fgTid != me && AttachThreadInput(me, fgTid, TRUE);
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    bool ok = SetForegroundWindow(h) != FALSE;
    if (!ok) {
        keybd_event(VK_MENU, 0, 0, 0);  // un tasto simulato sblocca SetForegroundWindow
        ok = SetForegroundWindow(h) != FALSE;
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    }
    BringWindowToTop(h);
    if (attached) AttachThreadInput(me, fgTid, FALSE);
    PostMessageW(h, kMsgTakeFocus, 0, 0);
    vr::log::Line(std::string("DISPLAY: game window brought to the foreground (") + why + (ok ? ")." : "): refused by Windows."));
}

// 20/10: con "wndinfo" si e' visto che poco dopo l'avvio il gioco installa
// una sua procedura (0x00F59DF0) al posto della nostra e non la richiama piu'
// (10 messaggi ricevuti in tutto): keepfocus, WM_GETMINMAXINFO e il ripristino
// dalla minimizzazione non funzionavano mai. Ci si rimette in testa,
// concatenando la procedura nuova (che non richiama la nostra: niente cicli).
void KeepSubclass() {
    HWND h = g_gameWindow;
    if (!h || !g_originalWndProc || vr::control::g_exiting) return;
    {
        // Stato della finestra, una volta al secondo per i primi 30 s.
        static ULONGLONG last = 0, first = 0;
        ULONGLONG now = GetTickCount64();
        if (!first) first = now;
        if (now - first < 30000 && now - last >= 1000) {
            last = now;
            RECT r = {};
            GetWindowRect(h, &r);
            LONG style = static_cast<LONG>(GetWindowLongPtrW(h, GWL_STYLE));
            char b[200];
            std::snprintf(b, sizeof(b), "DISPLAY: window iconic=%d visible=%d rect=(%ld,%ld)-(%ld,%ld) style=0x%08lX foreground=%s",
                          IsIconic(h), IsWindowVisible(h), r.left, r.top, r.right, r.bottom, style,
                          GetForegroundWindow() == h ? "game" : "other");
            vr::log::Line(b);
        }
    }
    {
        // In primo piano ma senza il focus della tastiera (lancio dal visore,
        // 22/10): i comandi non arrivavano al gioco finche' non si cliccava.
        static ULONGLONG lastFocus = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastFocus >= 1000 && GetForegroundWindow() == h) {
            lastFocus = now;
            GUITHREADINFO gti = {sizeof(gti)};
            DWORD tid = GetWindowThreadProcessId(h, nullptr);
            if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus != h) PostMessageW(h, kMsgTakeFocus, 0, 0);
        }
    }
    WNDPROC cur = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_WNDPROC));
    if (!cur || cur == &UnclampedWndProc) return;
    // Solo procedure dell'exe del gioco: un altro programma che sottoclassa
    // dopo di noi (Special K, rimappatori) chiama gia' la nostra; rimetterci
    // davanti creava un ciclo infinito fino all'esaurimento dello stack
    // (audit 20/10). In quel caso si lascia stare, una volta per tutte.
    uintptr_t a = reinterpret_cast<uintptr_t>(cur);
    if (a < vr::addr::TextBegin() || a >= vr::addr::TextEnd()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            char b[120];
            std::snprintf(b, sizeof(b), "DISPLAY: window procedure %p belongs to another program, left in front.",
                          reinterpret_cast<void*>(cur));
            vr::log::Line(b);
        }
        return;
    }
    g_originalWndProc = cur;
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UnclampedWndProc));
    static int logged = 0;
    if (logged++ < 5) {
        char b[96];
        std::snprintf(b, sizeof(b), "DISPLAY: window procedure replaced by the game (%p), ours put back in front.",
                      reinterpret_cast<void*>(cur));
        vr::log::Line(b);
    }
}

void SetWantedClientSize(int w, int h) {
    if (g_gameWindow && w > 0 && h > 0) {
        g_wantedWidth = w;
        g_wantedHeight = h;
    }
}

void LogWindowInfo() {
    HWND h = g_gameWindow;
    char b[300];
    std::snprintf(b, sizeof(b),
                  "DISPLAY: wndinfo hwnd=%p unicode=%d wndprocW=%p wndprocA=%p ours=%p original=%p messages=%ld iconic=%d",
                  h, h ? IsWindowUnicode(h) : -1, h ? reinterpret_cast<void*>(GetWindowLongPtrW(h, GWLP_WNDPROC)) : nullptr,
                  h ? reinterpret_cast<void*>(GetWindowLongPtrA(h, GWLP_WNDPROC)) : nullptr,
                  reinterpret_cast<void*>(&UnclampedWndProc), reinterpret_cast<void*>(g_originalWndProc), g_wndMessages,
                  h ? IsIconic(h) : -1);
    vr::log::Line(b);
}

void UnclampWindow(HWND window, int clientWidth, int clientHeight) {
    if (!window || g_originalWndProc) return;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UnclampedWndProc)));
    vr::log::Line(std::string("DISPLAY: window size limit removed") +
                  (g_originalWndProc ? "." : " (FAILED)."));
    if (!g_originalWndProc || clientWidth <= 0 || clientHeight <= 0) return;
    g_gameWindow = window;
    g_wantedWidth = clientWidth;
    g_wantedHeight = clientHeight;
    // Il gioco crea la finestra ridotta a icona (ShowWindow(SW_SHOWMINIMIZED)
    // in FUN_00de8b00) e la mostra con SW_SHOW, che non la ripristina; con
    // le dimensioni riportate dal mod il ripristino del gioco non scattava e
    // senza visore il video del menu andava in crash con la viewport 0x0
    // (21/10). Qui la viewport c'e': la finestra torna normale.
    if (IsIconic(window)) {
        ShowWindowAsync(window, SW_SHOWNOACTIVATE);
        vr::log::Line("DISPLAY: game window was minimized at viewport creation, restored.");
    }

    RECT client;
    GetClientRect(window, &client);
    if (client.right - client.left >= clientWidth && client.bottom - client.top >= clientHeight) return;
    RECT r = {0, 0, clientWidth, clientHeight};
    AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)));
    g_SetWindowPos_Real(window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    vr::log::Line("DISPLAY: window reported as client " + std::to_string(clientWidth) + "x" +
                  std::to_string(clientHeight) + " (was " + std::to_string(client.right - client.left) + "x" +
                  std::to_string(client.bottom - client.top) + ").");
}

typedef HWND(WINAPI* HwndQuery_t)();
HwndQuery_t g_GetForegroundWindow_Real = nullptr, g_GetActiveWindow_Real = nullptr, g_GetFocus_Real = nullptr;
// Solo collaudi: con due copie del gioco aperte, quella senza focus smetteva di leggere il pad (il gioco
// chiede a Windows chi e' in primo piano) anche con specopsvr_keepfocus; il gioco vede sempre se stesso.
HWND WINAPI Hooked_GetForegroundWindow() {
    HWND r = g_GetForegroundWindow_Real();
    return g_gameWindow && !vr::control::g_exiting ? g_gameWindow : r;
}
HWND WINAPI Hooked_GetActiveWindow() {
    HWND r = g_GetActiveWindow_Real();
    return g_gameWindow && !vr::control::g_exiting ? g_gameWindow : r;
}
// La lettura del pad del motore (FUN_006d3340) richiede GetFocus() == finestra del gioco. Col
// visore il gioco non vede la perdita del focus (WM_KILLFOCUS filtrato) e continua a girare, ma
// senza questo i controller erano morti con un'altra finestra davanti: testa e mani si
// muovevano, Walker no (30/10, riprodotto con il blocco note in primo piano).
HWND WINAPI Hooked_GetFocus() {
    HWND r = g_GetFocus_Real();
    if (!g_gameWindow || vr::control::g_exiting) return r;
    return TestKeepFocus() || (vr::control::g_keepFocus && HeadsetInUse()) ? g_gameWindow : r;
}

void InstallDesktopSizeSpoof() {
    if (TestKeepFocus()) {
        Patch(kIatGetForegroundWindow, "GetForegroundWindow", reinterpret_cast<void*>(&Hooked_GetForegroundWindow),
              reinterpret_cast<void**>(&g_GetForegroundWindow_Real));
        Patch(kIatGetActiveWindow, "GetActiveWindow", reinterpret_cast<void*>(&Hooked_GetActiveWindow),
              reinterpret_cast<void**>(&g_GetActiveWindow_Real));
        vr::log::Line("DISPLAY: test mode: the game always sees its own window in the foreground (specopsvr_keepfocus).");
    }
    Patch(kIatGetFocus, "GetFocus", reinterpret_cast<void*>(&Hooked_GetFocus), reinterpret_cast<void**>(&g_GetFocus_Real));
    bool a = Patch(kIatGetSystemMetrics, "GetSystemMetrics",
                   reinterpret_cast<void*>(&Hooked_GetSystemMetrics),
                   reinterpret_cast<void**>(&g_GetSystemMetrics_Real));
    bool b = Patch(kIatSystemParametersInfoW, "SystemParametersInfoW",
                   reinterpret_cast<void*>(&Hooked_SystemParametersInfoW),
                   reinterpret_cast<void**>(&g_SystemParametersInfoW_Real));
    Patch(kIatGetClientRect, "GetClientRect", reinterpret_cast<void*>(&Hooked_GetClientRect),
          reinterpret_cast<void**>(&g_GetClientRect_Real));
    Patch(kIatSetWindowPos, "SetWindowPos", reinterpret_cast<void*>(&Hooked_SetWindowPos),
          reinterpret_cast<void**>(&g_SetWindowPos_Real));
    Patch(kIatMoveWindow, "MoveWindow", reinterpret_cast<void*>(&Hooked_MoveWindow),
          reinterpret_cast<void**>(&g_MoveWindow_Real));
    Patch(kIatShowWindow, "ShowWindow", reinterpret_cast<void*>(&Hooked_ShowWindow),
          reinterpret_cast<void**>(&g_ShowWindow_Real));
    Patch(kIatSetWindowPlacement, "SetWindowPlacement", reinterpret_cast<void*>(&Hooked_SetWindowPlacement),
          reinterpret_cast<void**>(&g_SetWindowPlacement_Real));
    if (Patch(kIatGetCommandLineW, "GetCommandLineW", reinterpret_cast<void*>(&Hooked_GetCommandLineW),
              reinterpret_cast<void**>(&g_GetCommandLineW_Real), "kernel32.dll"))
        vr::log::Line("DISPLAY: game forced to windowed mode (-windowed on its command line; exclusive fullscreen "
                      "breaks the GPU path and the headset resolution).");
    vr::log::Line(std::string("DISPLAY: desktop reported to the game >= ") + std::to_string(kMinWidth) + "x" +
                  std::to_string(kMinHeight) + (a && b ? " (active)" : " (NOT active)"));
}

}  // namespace vr::display
