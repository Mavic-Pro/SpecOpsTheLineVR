#include "vr_wrist.h"
#include "control.h"
#include "logger.h"
#include "vr_session.h"

#include <windows.h>
#include <openvr.h>

#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace vr::wrist {

namespace {

constexpr int kW = 512, kH = 256;

std::mutex g_mutex;
struct Data {
    bool show = false;
    int clip = -1, reserve = -1, health = -1;
    std::string weapon;
    bool operator==(const Data& o) const {
        return show == o.show && clip == o.clip && reserve == o.reserve && health == o.health && weapon == o.weapon;
    }
};
Data g_data, g_drawn;

vr::VROverlayHandle_t g_overlay = vr::k_ulOverlayHandleInvalid;
unsigned g_overlayGen = 0;
bool g_visible = false;
vr::TrackedDeviceIndex_t g_device = vr::k_unTrackedDeviceIndexInvalid;

// Nome breve dell'arma dalla classe (YWeapon_M4A1 -> M4A1).
std::string ShortWeapon(const std::string& cls) {
    std::string s = cls;
    size_t u = s.find('_');
    if (u != std::string::npos) s = s.substr(u + 1);
    for (char& c : s)
        if (c == '_') c = ' ';
    return s;
}

bool Render(const Data& d, std::vector<uint32_t>* rgba) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;
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
    RECT r = {0, 0, kW, kH};
    HBRUSH bg = CreateSolidBrush(RGB(14, 14, 18));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    HFONT fontBig = CreateFontA(-120, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT fontSmall = CreateFontA(-44, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    // Caricatore grande a sinistra, riserva piu' piccola accanto.
    HGDIOBJ oldFont = SelectObject(dc, fontBig);
    std::string clip = d.clip >= 0 ? std::to_string(d.clip) : "-";
    SetTextColor(dc, d.clip == 0 ? RGB(240, 70, 60) : RGB(245, 245, 245));
    RECT rc = {20, 10, 300, 150};
    DrawTextA(dc, clip.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, fontSmall);
    SetTextColor(dc, RGB(190, 190, 200));
    std::string reserve = d.reserve >= 0 ? "/ " + std::to_string(d.reserve) : "";
    RECT rr = {300, 10, kW - 16, 150};
    DrawTextA(dc, reserve.c_str(), -1, &rr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    // Arma in basso a sinistra, salute in basso a destra.
    SetTextColor(dc, RGB(220, 220, 230));
    RECT rw = {20, 160, 330, kH - 10};
    DrawTextA(dc, ShortWeapon(d.weapon).c_str(), -1, &rw, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (d.health >= 0) {
        int h = d.health > 100 ? 100 : d.health;
        SetTextColor(dc, h < 35 ? RGB(240, 70, 60) : RGB(120, 220, 120));
        std::string hs = "HP " + std::to_string(h);
        RECT rh = {330, 160, kW - 16, kH - 10};
        DrawTextA(dc, hs.c_str(), -1, &rh, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
    GdiFlush();
    rgba->resize(static_cast<size_t>(kW) * kH);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < rgba->size(); ++i) {
        uint32_t p = src[i];
        uint32_t b = p & 0xFF, g = (p >> 8) & 0xFF, rr2 = (p >> 16) & 0xFF;
        (*rgba)[i] = rr2 | (g << 8) | (b << 16) | (0xD0u << 24);
    }
    SelectObject(dc, oldFont);
    DeleteObject(fontBig);
    DeleteObject(fontSmall);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

// Sul polso del controller di supporto: il pannello sta 11 cm verso
// l'avambraccio (+Z del controller), 2 cm sopra, e guarda verso l'alto del
// controller (+Y), con il testo orientato verso la mano.
void Place(vr::IVROverlay* ov, vr::TrackedDeviceIndex_t device) {
    vr::HmdMatrix34_t t = {};
    // Colonne: X overlay = +X controller; Y overlay = -Z controller (verso la
    // mano); Z overlay (normale, verso chi guarda) = +Y controller.
    t.m[0][0] = 1.0f; t.m[0][1] = 0.0f;  t.m[0][2] = 0.0f; t.m[0][3] = 0.0f;
    t.m[1][0] = 0.0f; t.m[1][1] = 0.0f;  t.m[1][2] = 1.0f; t.m[1][3] = 0.02f;
    t.m[2][0] = 0.0f; t.m[2][1] = -1.0f; t.m[2][2] = 0.0f; t.m[2][3] = 0.11f;
    ov->SetOverlayTransformTrackedDeviceRelative(g_overlay, device, &t);
    ov->SetOverlayWidthInMeters(g_overlay, vr::control::g_wristHudWidthM);
}

}  // namespace

void SetData(bool show, int clip, int reserve, int health, const std::string& weapon) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_data.show = show && vr::control::g_wristHud;
    g_data.clip = clip;
    g_data.reserve = reserve;
    g_data.health = health;
    g_data.weapon = weapon;
}

void Tick() {
    static std::mutex tickMutex;
    std::unique_lock<std::mutex> lock(tickMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    vr::session::Shared session(true);
    if (!session.ok() || !vr::session::Ready()) return;
    vr::IVROverlay* ov = vr::VROverlay();
    vr::IVRSystem* sys = vr::VRSystem();
    if (!ov || !sys) return;
    Data d;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        d = g_data;
    }
    unsigned gen = vr::session::Generation();
    if (gen != g_overlayGen) {
        g_overlayGen = gen;
        g_overlay = vr::k_ulOverlayHandleInvalid;
        g_visible = false;
        g_device = vr::k_unTrackedDeviceIndexInvalid;
        g_drawn = Data();
    }
    if (!d.show) {
        if (g_visible && g_overlay != vr::k_ulOverlayHandleInvalid) {
            ov->HideOverlay(g_overlay);
            g_visible = false;
        }
        return;
    }
    if (g_overlay == vr::k_ulOverlayHandleInvalid) {
        vr::EVROverlayError err = ov->CreateOverlay("specopsvr.wrist", "SpecOpsVR wrist HUD", &g_overlay);
        if (err != vr::VROverlayError_None) {
            g_overlay = vr::k_ulOverlayHandleInvalid;
            static bool logged = false;
            if (!logged) vr::log::Line("WRIST: CreateOverlay failed, error " + std::to_string(err));
            logged = true;
            return;
        }
        ov->SetOverlaySortOrder(g_overlay, 500);
        g_drawn = Data();
        vr::log::Line("WRIST: overlay created.");
    }
    // Controller di supporto (ruolo); se il ruolo non e' un controller, il
    // primo altro controller collegato (come in vr_input).
    vr::TrackedDeviceIndex_t dev = sys->GetTrackedDeviceIndexForControllerRole(vr::control::SupportRole());
    if (dev == vr::k_unTrackedDeviceIndexInvalid || sys->GetTrackedDeviceClass(dev) != vr::TrackedDeviceClass_Controller) {
        vr::TrackedDeviceIndex_t gun = sys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole());
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
            if (i != gun && sys->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller && sys->IsTrackedDeviceConnected(i)) {
                dev = i;
                break;
            }
    }
    if (dev == vr::k_unTrackedDeviceIndexInvalid) return;
    static float placedWidth = 0.0f;
    if (dev != g_device || placedWidth != vr::control::g_wristHudWidthM.load()) {
        g_device = dev;
        placedWidth = vr::control::g_wristHudWidthM;
        Place(ov, dev);
    }
    if (!(d == g_drawn)) {
        std::vector<uint32_t> px;
        if (Render(d, &px)) {
            vr::EVROverlayError err = ov->SetOverlayRaw(g_overlay, px.data(), kW, kH, 4);
            if (err == vr::VROverlayError_None) g_drawn = d;
        }
    }
    if (!g_visible) {
        ov->ShowOverlay(g_overlay);
        g_visible = true;
    }
}

}  // namespace vr::wrist
