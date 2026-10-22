#include "vr_toast.h"
#include "control.h"
#include "logger.h"
#include "vr_session.h"

#include <windows.h>
#include <openvr.h>

#include <deque>
#include <mutex>
#include <vector>

namespace vr::toast {

namespace {

constexpr int kW = 768, kH = 112;
constexpr ULONGLONG kShowMs = 3500;
constexpr size_t kMaxQueued = 4;

std::mutex g_mutex;
std::deque<std::string> g_queue;
std::string g_lastText;
ULONGLONG g_lastTextMs = 0;

vr::VROverlayHandle_t g_overlay = vr::k_ulOverlayHandleInvalid;
unsigned g_overlayGen = 0;
ULONGLONG g_shownUntil = 0;
bool g_visible = false;

// Testo bianco su fondo scuro semitrasparente, in RGBA per SetOverlayRaw.
bool RenderText(const std::string& text, std::vector<uint32_t>* rgba) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;  // dall'alto in basso
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontA(-40, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT r = {0, 0, kW, kH};
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    RECT tr = {24, 0, kW - 24, kH};
    DrawTextA(dc, text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    GdiFlush();
    rgba->resize(static_cast<size_t>(kW) * kH);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < rgba->size(); ++i) {
        uint32_t p = src[i];  // BGRA (GDI, alfa non scritto)
        uint32_t b = p & 0xFF, g = (p >> 8) & 0xFF, rr = (p >> 16) & 0xFF;
        (*rgba)[i] = rr | (g << 8) | (b << 16) | (0xE0u << 24);  // RGBA, fondo al 88%
    }
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

bool Show(const std::string& text) {
    vr::IVROverlay* ov = vr::VROverlay();
    if (!ov) return false;
    unsigned gen = vr::session::Generation();
    if (gen != g_overlayGen) {  // nuova connessione a SteamVR: handle vecchio non valido
        g_overlayGen = gen;
        g_overlay = vr::k_ulOverlayHandleInvalid;
        g_visible = false;
    }
    if (g_overlay == vr::k_ulOverlayHandleInvalid) {
        vr::EVROverlayError err = ov->CreateOverlay("specopsvr.status", "SpecOpsVR status", &g_overlay);
        if (err != vr::VROverlayError_None) {
            g_overlay = vr::k_ulOverlayHandleInvalid;
            vr::log::Line("TOAST: CreateOverlay failed, error " + std::to_string(err));
            return false;
        }
        // Agganciato alla testa: 22 cm sotto lo sguardo, a 1 m.
        vr::HmdMatrix34_t t = {};
        t.m[0][0] = 1.0f; t.m[1][1] = 1.0f; t.m[2][2] = 1.0f;
        t.m[0][3] = 0.0f; t.m[1][3] = -0.22f; t.m[2][3] = -1.0f;
        ov->SetOverlayTransformTrackedDeviceRelative(g_overlay, vr::k_unTrackedDeviceIndex_Hmd, &t);
        ov->SetOverlayWidthInMeters(g_overlay, 0.55f);
        ov->SetOverlaySortOrder(g_overlay, 1000);
    }
    std::vector<uint32_t> px;
    if (!RenderText(text, &px)) return false;
    vr::EVROverlayError err = ov->SetOverlayRaw(g_overlay, px.data(), kW, kH, 4);
    if (err != vr::VROverlayError_None) {
        // Dopo il primo messaggio gli aggiornamenti fallivano tutti con 23
        // (RequestFailed, 22/10 nel visore): overlay rifatto e un altro tentativo.
        vr::log::Line("TOAST: SetOverlayRaw failed, error " + std::to_string(err) + ", overlay recreated");
        ov->DestroyOverlay(g_overlay);
        g_overlay = vr::k_ulOverlayHandleInvalid;
        g_visible = false;
        if (ov->CreateOverlay("specopsvr.status", "SpecOpsVR status", &g_overlay) != vr::VROverlayError_None) {
            g_overlay = vr::k_ulOverlayHandleInvalid;
            return false;
        }
        vr::HmdMatrix34_t t = {};
        t.m[0][0] = 1.0f; t.m[1][1] = 1.0f; t.m[2][2] = 1.0f;
        t.m[0][3] = 0.0f; t.m[1][3] = -0.22f; t.m[2][3] = -1.0f;
        ov->SetOverlayTransformTrackedDeviceRelative(g_overlay, vr::k_unTrackedDeviceIndex_Hmd, &t);
        ov->SetOverlayWidthInMeters(g_overlay, 0.55f);
        ov->SetOverlaySortOrder(g_overlay, 1000);
        err = ov->SetOverlayRaw(g_overlay, px.data(), kW, kH, 4);
        if (err != vr::VROverlayError_None) {
            vr::log::Line("TOAST: SetOverlayRaw failed again, error " + std::to_string(err));
            return false;
        }
    }
    ov->ShowOverlay(g_overlay);
    g_visible = true;
    g_shownUntil = GetTickCount64() + kShowMs;
    vr::log::Line("TOAST: \"" + text + "\"");
    return true;
}

}  // namespace

void Notify(const std::string& text) {
    if (!vr::control::g_toasts) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    if (text == g_lastText && now - g_lastTextMs < 10000) return;  // stesso avviso entro 10 s: una volta
    g_lastText = text;
    g_lastTextMs = now;
    if (g_queue.size() >= kMaxQueued) g_queue.pop_front();
    g_queue.push_back(text);
    vr::log::Line("TOAST: queued \"" + text + "\"");
}

void Tick() {
    // Chiamato dal worker dell'uscita VR. Gli overlay si toccano solo con il
    // lock condiviso della sessione: senza, il thread della sessione che
    // passa da Background a Scene (VR_Shutdown + VR_Init) restava bloccato
    // per sempre con la vista nel visore mai partita (21/10, Virtual Desktop).
    // Se il lock non e' libero (switch in corso) si salta il frame.
    static std::mutex tickMutex;
    std::unique_lock<std::mutex> lock(tickMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    vr::session::Shared session(true);
    if (!session.ok() || !vr::session::Ready()) return;
    ULONGLONG now = GetTickCount64();
    if (g_visible && now >= g_shownUntil) {
        if (vr::VROverlay() && g_overlay != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->HideOverlay(g_overlay);
        g_visible = false;
    }
    if (g_visible) return;
    std::string next;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.empty()) return;
        next = g_queue.front();
        g_queue.pop_front();
    }
    Show(next);
}

void HintsTick(bool gameplay) {
    static ULONGLONG gameplaySince = 0, lastHint = 0;
    static size_t next = 0;
    static bool done = false, counted = false;
    if (done) return;
    int mode = vr::control::g_hints.load();  // 0 off, 1 on, 2 auto
    if (mode == 0 || (mode == 2 && vr::control::g_hintsShown.load() >= 3)) {
        done = true;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!gameplay) {
        gameplaySince = 0;
        return;
    }
    if (!gameplaySince) gameplaySince = now;
    if (now - gameplaySince < 15000 || now - lastHint < 7000) return;
    static const char* const kHints[] = {
        "Aim with the gun in your right hand; left hand on the barrel = two-handed",
        "Right trigger: fire. Left trigger or gun to the eye: aim down the sights",
        "A: action / cover. B: melee (or punch / hit with the gun). X: reload. Y: next weapon",
        "Left grip: grenade (quick throw with the left hand). Right grip: squad orders",
        "Reload by hand: grip the magazine under the gun, pull it, grip at your belt, push it in",
        "Right stick held + direction: D-pad. Both sticks held 1 s: recenter (3 s: standing height)",
        "Gunstock: aim at something far, look at it, hold B + Y for 1 s",
    };
    if (next < sizeof(kHints) / sizeof(kHints[0])) {
        Notify(kHints[next++]);
        lastHint = now;
        if (!counted) {
            counted = true;
            if (mode == 2) {
                int n = vr::control::g_hintsShown.load() + 1;
                vr::control::g_hintsShown = n;
                vr::control::SaveIniValue("hintsshown", std::to_string(n));
            }
        }
    } else {
        done = true;
    }
}

}  // namespace vr::toast
