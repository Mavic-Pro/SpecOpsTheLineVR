#include "display_spoof.h"
#include "logger.h"
#include "control.h"
#include "vr_session.h"

#include <windows.h>
#include <cstdio>
#include <intrin.h>
#include <string>

namespace vr::display {

namespace {

// Voci della IAT di SpecOpsTheLine.exe (USER32), vedi README.
constexpr uintptr_t kIatGetSystemMetrics = 0x01109660;
constexpr uintptr_t kIatSystemParametersInfoW = 0x01109664;
constexpr uintptr_t kIatSetWindowPos = 0x011096C4;
constexpr uintptr_t kIatMoveWindow = 0x01109570;
constexpr uintptr_t kIatGetClientRect = 0x01109614;

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

bool Patch(uintptr_t slot, const char* name, void* replacement, void** outOriginal) {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    void* expected = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, name)) : nullptr;
    void** entry = reinterpret_cast<void**>(slot);
    if (!expected || *entry != expected) {
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
            if (v >= 0x00401000 && v < 0x01109000) {
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

BOOL WINAPI Hooked_GetClientRect(HWND h, LPRECT r) {
    BOOL ok = g_GetClientRect_Real(h, r);
    if (ok && r && h == g_gameWindow && g_wantedWidth > 0 && (!IsIconic(h) || HeadsetInUse())) {
        r->left = r->top = 0;
        r->right = g_wantedWidth;
        r->bottom = g_wantedHeight;
    }
    return ok;
}

WNDPROC g_originalWndProc = nullptr;
LONG volatile g_wndMessages = 0;

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
    if (vr::control::g_keepFocus && HeadsetInUse()) {
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

// 20/10: con "wndinfo" si e' visto che poco dopo l'avvio il gioco installa
// una sua procedura (0x00F59DF0) al posto della nostra e non la richiama piu'
// (10 messaggi ricevuti in tutto): keepfocus, WM_GETMINMAXINFO e il ripristino
// dalla minimizzazione non funzionavano mai. Ci si rimette in testa,
// concatenando la procedura nuova (che non richiama la nostra: niente cicli).
void KeepSubclass() {
    HWND h = g_gameWindow;
    if (!h || !g_originalWndProc || vr::control::g_exiting) return;
    WNDPROC cur = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_WNDPROC));
    if (!cur || cur == &UnclampedWndProc) return;
    // Solo procedure dell'exe del gioco: un altro programma che sottoclassa
    // dopo di noi (Special K, rimappatori) chiama gia' la nostra; rimetterci
    // davanti creava un ciclo infinito fino all'esaurimento dello stack
    // (audit 20/10). In quel caso si lascia stare, una volta per tutte.
    uintptr_t a = reinterpret_cast<uintptr_t>(cur);
    if (a < 0x00401000 || a >= 0x01109000) {
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

void InstallDesktopSizeSpoof() {
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
    vr::log::Line(std::string("DISPLAY: desktop reported to the game >= ") + std::to_string(kMinWidth) + "x" +
                  std::to_string(kMinHeight) + (a && b ? " (active)" : " (NOT active)"));
}

}  // namespace vr::display
