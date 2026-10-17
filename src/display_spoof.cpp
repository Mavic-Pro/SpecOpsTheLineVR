#include "display_spoof.h"
#include "logger.h"

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
        vr::log::Line(std::string("DISPLAY: voce IAT ") + name + " inattesa, non tocco nulla.");
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
    std::snprintf(buf, sizeof(buf), "DISPLAY: %s pos=(%d,%d) size=%dx%d flags=0x%X chiamante=0x%p",
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

BOOL WINAPI Hooked_GetClientRect(HWND h, LPRECT r) {
    BOOL ok = g_GetClientRect_Real(h, r);
    if (ok && r && h == g_gameWindow && g_wantedWidth > 0 && !IsIconic(h)) {
        r->left = r->top = 0;
        r->right = g_wantedWidth;
        r->bottom = g_wantedHeight;
    }
    return ok;
}

WNDPROC g_originalWndProc = nullptr;

LRESULT CALLBACK UnclampedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT r = CallWindowProcW(g_originalWndProc, hwnd, msg, wp, lp);
    if (msg == WM_SIZE) {
        static int budget = 20;
        if (budget-- > 0) {
            vr::log::Line("DISPLAY: WM_SIZE " + std::to_string(LOWORD(lp)) + "x" + std::to_string(HIWORD(lp)) +
                          " tipo=" + std::to_string(wp));
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

void UnclampWindow(HWND window, int clientWidth, int clientHeight) {
    if (!window || g_originalWndProc) return;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UnclampedWndProc)));
    vr::log::Line(std::string("DISPLAY: limite di dimensione della finestra rimosso") +
                  (g_originalWndProc ? "." : " (FALLITO)."));
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
    vr::log::Line("DISPLAY: finestra riportata a client " + std::to_string(clientWidth) + "x" +
                  std::to_string(clientHeight) + " (era " + std::to_string(client.right - client.left) + "x" +
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
    vr::log::Line(std::string("DISPLAY: desktop riportato al gioco >= ") + std::to_string(kMinWidth) + "x" +
                  std::to_string(kMinHeight) + (a && b ? " (attivo)" : " (NON attivo)"));
}

}  // namespace vr::display
