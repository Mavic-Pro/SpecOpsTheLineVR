#include "vr_menu.h"
#include "control.h"
#include "logger.h"
#include "vr_session.h"

#include <windows.h>
#include <openvr.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace vr::menu {

namespace {

constexpr int kW = 1100, kH = 820;
constexpr int kRowH = 58, kTop = 86, kRowsPerPage = 11;

enum class Kind { OnOff, Choice, Number, Text };

struct Item {
    std::string key;      // chiave dell'ini (comando)
    std::string label;    // titolo
    std::string help;     // una riga di spiegazione
    Kind kind;
    std::vector<std::string> choices;  // Choice
    float step = 0.0f, lo = 0.0f, hi = 0.0f;  // Number
    std::string value;    // valore attuale (testo dopo la chiave)
};

// Scheda Basic: le scelte che un giocatore tocca davvero.
std::vector<Item> BasicItems() {
    return {
        {"hand", "Dominant hand", "Which controller holds the gun. Left-handed: the gun buttons move to the left controller and Walker holds the gun in his left hand.", Kind::Choice, {"right", "left"}},
        {"mirrorbody", "Mirrored body", "Walker's body mirrored so the gun is in his left hand (auto = with the left dominant hand).", Kind::Choice, {"auto", "on", "off"}},
        {"cinemalaser", "Laser on the screen", "Point the gun controller at the virtual screen to move the game's cursor on its menus; trigger = click.", Kind::OnOff},
        {"wristhud", "Wrist HUD", "Ammo, weapon and health on the support-hand wrist, like a watch.", Kind::OnOff},
        {"commandaim", "Squad orders aim", "Who the squad order targets while the squad button is held: along the gun barrel, the centre of the view (game), or where the support hand points.", Kind::Choice, {"gun", "head", "support"}},
        {"height", "Standing height", "auto = your real height measured while playing; hold both sticks 3 s to set it.", Kind::Choice, {"auto", "fixed"}},
        {"snapturn", "Turning", "0 = smooth turning with the right stick, 30/45 = snap turns in degrees.", Kind::Choice, {"0", "30", "45"}},
        {"vignette", "Comfort vignette", "Darkens the edges while moving: 0 off, 0.5 light, 1 strong.", Kind::Choice, {"0", "0.5", "1"}},
        {"hud", "HUD size", "Scale of the game's HUD (ammo, objectives) in your view.", Kind::Choice, {"0.3 100", "0.4 100", "0.5 100", "0.6 100"}},
        {"fpbody", "Walker's body", "Show Walker's body and arms (off = floating hands and gun).", Kind::OnOff},
        {"reload", "Reload", "physical = grip the magazine, belt, push it in; simple = press X.", Kind::Choice, {"physical", "simple"}},
        {"twohand", "Two-handed aiming", "Left hand on the barrel steadies the gun (12 cm sideways, 8 cm vertical).", Kind::OnOff},
        {"marker", "Green aim marker", "A marker where the barrel points. Off: use the weapon's own sights.", Kind::OnOff},
        {"laser", "Laser beam", "A visible beam from the barrel to the point of impact.", Kind::OnOff},
        {"spread", "Bullet spread", "on = the game's spread scaled by stance; off = laser-straight shots.", Kind::OnOff},
        {"roomscale", "Room-scale", "Walker follows your real steps (collisions included).", Kind::OnOff},
        {"seated", "Seated play", "Playing sitting down: the view keeps Walker's standing eye height from where your head is when you recenter.", Kind::OnOff},
        {"hints", "Controller hints", "Short hints in the headset during the first sessions.", Kind::Choice, {"auto", "on", "off"}},
        {"vrmenu", "This menu at start", "Open this menu on the game's main menu at every start.", Kind::Choice, {"on", "off"}},
    };
}

const char* const kControls[] = {
    "LEFT STICK: move          RIGHT STICK: turn          STICK CLICKS: LS / RS",
    "RIGHT TRIGGER: fire        LEFT TRIGGER: aim (or bring the gun to your eye)",
    "A: action / cover          B: melee (punch or hit with the gun)",
    "X: reload                  Y: next weapon",
    "LEFT GRIP: grenade (quick throw with the left hand) / hold the gun by the barrel",
    "RIGHT GRIP: squad orders (aim the gun at a target) / grab ropes, ziplines, turrets",
    "LEFT MENU BUTTON: pause     RIGHT STICK HELD + direction: D-pad",
    "BOTH STICKS 1 s: recenter   BOTH STICKS 3 s: also save your standing height",
    "B + Y 1 s: gunstock calibration (aim at something far, look at it)",
    "RELOAD BY HAND: grip the magazine under the gun, pull it out, grip at your belt, push it in",
    "WEAPON SWAP: right grip over your shoulder or at your hip",
    "COVER: duck quickly next to cover; lean out with your body to pop out and aim",
};

std::mutex g_mutex;
std::vector<Item> g_basic = BasicItems();
std::vector<Item> g_advanced;
int g_tab = 0;      // 0 Basic, 1 Advanced, 2 Controls
int g_scroll = 0;
bool g_open = false;
bool g_shownAtStart = false;
ULONGLONG g_cinemaSince = 0;
std::atomic<int> g_request{0};  // 1 apri, -1 chiudi
vr::VROverlayHandle_t g_overlay = vr::k_ulOverlayHandleInvalid;
unsigned g_overlayGen = 0;
int g_hoverX = -1, g_hoverY = -1;
bool g_dirty = true;
float g_placedYaw = 0.0f;
ULONGLONG g_awaySince = 0;

struct Hit {
    RECT r;
    int action;  // 1 tab basic, 2 advanced, 3 controls, 4 play, 5 scroll up, 6 scroll down, 10+i riga i (toggle/cycle), 1000+i meno, 2000+i piu'
};
std::vector<Hit> g_hits;

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Righe dell'ini accanto alla dll: valori attuali e, per Advanced, i commenti
// che precedono ogni chiave come spiegazione.
void LoadIni() {
    std::ifstream f(vr::control::GameDirectory() + "\\specopsvr.ini");
    std::vector<std::pair<std::string, std::string>> kv;  // chiave, resto
    std::vector<Item> adv;
    std::string line, help;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (t.empty()) {
            help.clear();
            continue;
        }
        if (t[0] == '#') {
            std::string c = Trim(t.substr(1));
            if (!help.empty()) help += " ";
            help += c;
            continue;
        }
        std::istringstream in(t);
        std::string key, rest;
        in >> key;
        std::getline(in, rest);
        rest = Trim(rest);
        // Mai nel menu (richiesta 21/10): gli aggiornamenti non vanno spenti per sbaglio.
        if (key == "configversion" || key == "hintsshown" || key == "autoupdate" || key == "updatechannel" || key == "updaterepo") {
            help.clear();
            continue;
        }
        kv.push_back({key, rest});
        Item it;
        it.key = key;
        it.label = key;
        it.help = help.size() > 160 ? help.substr(0, 157) + "..." : help;
        it.value = rest;
        std::string low;
        for (char ch : rest) low += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (low == "on" || low == "off") it.kind = Kind::OnOff;
        else {
            char* e = nullptr;
            std::strtof(rest.c_str(), &e);
            it.kind = e != rest.c_str() ? Kind::Number : Kind::Text;
            float v = static_cast<float>(std::atof(rest.c_str()));
            it.step = std::fabs(v) >= 10.0f ? 1.0f : (std::fabs(v) >= 1.0f ? 0.1f : 0.05f);
        }
        adv.push_back(it);
        help.clear();
    }
    for (Item& b : g_basic) {
        b.value.clear();
        for (auto& p : kv)
            if (p.first == b.key) b.value = p.second;
        if (b.value.empty()) {
            if (b.kind == Kind::OnOff) b.value = "on";
            else if (!b.choices.empty()) b.value = b.choices[0];
        }
    }
    g_advanced = adv;
}

void Apply(const Item& it, const std::string& value) {
    vr::control::ExecuteLine(it.key + " " + value);
    vr::control::SaveIniValue(it.key, value);
    vr::log::Line("VRMENU: " + it.key + " " + value);
}

std::string FirstWord(const std::string& s) {
    std::istringstream in(s);
    std::string w;
    in >> w;
    return w;
}

void Change(Item& it, int dir) {
    std::string low;
    for (char ch : it.value) low += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (it.kind == Kind::OnOff) {
        it.value = FirstWord(low) == "on" ? "off" : "on";
    } else if (it.kind == Kind::Choice) {
        int idx = 0;
        for (size_t i = 0; i < it.choices.size(); ++i)
            if (it.choices[i] == low || FirstWord(it.choices[i]) == FirstWord(low)) idx = static_cast<int>(i);
        idx = (idx + dir + static_cast<int>(it.choices.size())) % static_cast<int>(it.choices.size());
        it.value = it.choices[idx];
    } else if (it.kind == Kind::Number) {
        std::istringstream in(it.value);
        float v = 0.0f;
        in >> v;
        std::string rest;
        std::getline(in, rest);
        v += dir * it.step;
        char b[32];
        std::snprintf(b, sizeof(b), it.step >= 1.0f ? "%.0f" : "%.2f", v);
        it.value = std::string(b) + rest;
    } else {
        return;
    }
    Apply(it, it.value);
}

// --- Disegno (GDI) -------------------------------------------------------------
HFONT MakeFont(int h, int weight) {
    return CreateFontA(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

void Button(HDC dc, RECT r, const char* text, bool active, int action, HFONT font) {
    HBRUSH b = CreateSolidBrush(active ? RGB(70, 130, 90) : RGB(45, 48, 58));
    FillRect(dc, &r, b);
    DeleteObject(b);
    if (g_hoverX >= r.left && g_hoverX < r.right && g_hoverY >= r.top && g_hoverY < r.bottom) {
        HBRUSH hb = CreateSolidBrush(RGB(255, 255, 255));
        FrameRect(dc, &r, hb);
        DeleteObject(hb);
    }
    SelectObject(dc, font);
    SetTextColor(dc, RGB(240, 240, 240));
    DrawTextA(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    g_hits.push_back({r, action});
}

bool Render(std::vector<uint32_t>* rgba) {
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
    HFONT title = MakeFont(34, FW_SEMIBOLD), body = MakeFont(26, FW_NORMAL), fontSmall = MakeFont(19, FW_NORMAL),
          mono = MakeFont(22, FW_NORMAL);
    g_hits.clear();
    RECT all = {0, 0, kW, kH};
    HBRUSH bg = CreateSolidBrush(RGB(22, 24, 30));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    // Schede
    const char* tabs[] = {"Basic", "Advanced", "Controls"};
    for (int i = 0; i < 3; ++i) {
        RECT r = {20 + i * 190, 16, 20 + i * 190 + 175, 64};
        Button(dc, r, tabs[i], g_tab == i, 1 + i, title);
    }
    Button(dc, {kW - 200, 16, kW - 20, 64}, "PLAY", true, 4, title);
    SelectObject(dc, fontSmall);
    SetTextColor(dc, RGB(150, 150, 160));
    RECT hint = {620, 22, kW - 210, 60};
    DrawTextA(dc, "SpecOpsVR - point and click with a controller", -1, &hint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // Avviso per Virtual Desktop (richiesta 21/10): cosa spegnere per non
    // giocare a meta' frequenza.
    LONG topY = kTop;
    if (vr::control::g_vdDetected) {
        // Testo corto e a capo automatico (prima due righe lunghe tagliate).
        RECT ra = {20, kTop - 14, kW - 20, kTop + 92};
        HBRUSH ab = CreateSolidBrush(RGB(110, 70, 20));
        FillRect(dc, &ra, ab);
        DeleteObject(ab);
        SelectObject(dc, fontSmall);
        SetTextColor(dc, RGB(255, 235, 200));
        RECT r1 = {34, kTop - 8, kW - 34, kTop + 88};
        DrawTextA(dc,
                  "VIRTUAL DESKTOP: to avoid half frame rate, in the VD menu (Streaming tab) set Synchronous Spacewarp = Disabled, "
                  "VR Frame Rate = 72, VR Graphics Quality = High or lower, and connect the PC to the router with an Ethernet cable. "
                  "Changes apply at the next start.",
                  -1, &r1, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        topY = kTop + 106;
    }
    if (g_tab == 2) {
        SelectObject(dc, mono);
        SetTextColor(dc, RGB(230, 230, 230));
        for (size_t i = 0; i < sizeof(kControls) / sizeof(kControls[0]); ++i) {
            RECT r = {30, topY + static_cast<LONG>(i) * 52, kW - 30, topY + static_cast<LONG>(i) * 52 + 48};
            DrawTextA(dc, kControls[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    } else {
        std::vector<Item>& items = g_tab == 0 ? g_basic : g_advanced;
        int n = static_cast<int>(items.size());
        int rows = topY > kTop ? kRowsPerPage - 2 : kRowsPerPage;
        if (g_scroll > n - rows) g_scroll = n - rows;
        if (g_scroll < 0) g_scroll = 0;
        for (int row = 0; row < rows && g_scroll + row < n; ++row) {
            int i = g_scroll + row;
            Item& it = items[i];
            LONG y = topY + row * kRowH;
            SelectObject(dc, body);
            SetTextColor(dc, RGB(240, 240, 240));
            RECT rl = {30, y, 620, y + 30};
            DrawTextA(dc, it.label.c_str(), -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, fontSmall);
            SetTextColor(dc, RGB(165, 170, 180));
            RECT rh = {30, y + 30, 640, y + kRowH - 2};
            DrawTextA(dc, it.help.c_str(), -1, &rh, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            std::string val = it.value.empty() ? "-" : it.value;
            if (it.kind == Kind::OnOff) {
                std::string low = FirstWord(val);
                for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                Button(dc, {kW - 260, y + 6, kW - 40, y + 50}, low == "on" ? "ON" : "OFF", low == "on", 10 + i, body);
            } else if (it.kind == Kind::Choice) {
                Button(dc, {kW - 320, y + 6, kW - 40, y + 50}, ("<  " + val + "  >").c_str(), false, 10 + i, body);
            } else if (it.kind == Kind::Number) {
                Button(dc, {kW - 360, y + 6, kW - 300, y + 50}, "-", false, 1000 + i, body);
                RECT rv = {kW - 295, y + 6, kW - 105, y + 50};
                SelectObject(dc, body);
                SetTextColor(dc, RGB(240, 240, 240));
                DrawTextA(dc, val.c_str(), -1, &rv, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                Button(dc, {kW - 100, y + 6, kW - 40, y + 50}, "+", false, 2000 + i, body);
            } else {
                RECT rv = {kW - 360, y + 6, kW - 40, y + 50};
                SelectObject(dc, body);
                SetTextColor(dc, RGB(200, 200, 210));
                DrawTextA(dc, val.c_str(), -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }
        if (n > rows) {
            Button(dc, {kW - 200, kH - 60, kW - 110, kH - 16}, "UP", false, 5, body);
            Button(dc, {kW - 100, kH - 60, kW - 20, kH - 16}, "DOWN", false, 6, body);
            SelectObject(dc, fontSmall);
            SetTextColor(dc, RGB(150, 150, 160));
            char pg[64];
            std::snprintf(pg, sizeof(pg), "%d-%d of %d", g_scroll + 1, (std::min)(n, g_scroll + rows), n);
            RECT rp = {30, kH - 56, 600, kH - 20};
            DrawTextA(dc, pg, -1, &rp, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    GdiFlush();
    rgba->resize(static_cast<size_t>(kW) * kH);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < rgba->size(); ++i) {
        uint32_t p = src[i];
        (*rgba)[i] = (p & 0xFF0000) >> 16 | (p & 0xFF00) | (p & 0xFF) << 16 | 0xFF000000u;  // opaco: leggibile
    }
    for (HFONT f : {title, body, fontSmall, mono}) DeleteObject(f);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

bool EnsureOverlay() {
    vr::IVROverlay* ov = vr::VROverlay();
    vr::IVRSystem* sys = vr::VRSystem();
    if (!ov || !sys) return false;
    unsigned gen = vr::session::Generation();
    if (gen != g_overlayGen) {
        g_overlayGen = gen;
        g_overlay = vr::k_ulOverlayHandleInvalid;
    }
    if (g_overlay != vr::k_ulOverlayHandleInvalid) return true;
    if (ov->CreateOverlay("specopsvr.menu", "SpecOpsVR settings", &g_overlay) != vr::VROverlayError_None) {
        g_overlay = vr::k_ulOverlayHandleInvalid;
        vr::log::Line("VRMENU: CreateOverlay failed.");
        return false;
    }
    ov->SetOverlayWidthInMeters(g_overlay, 1.5f);
    ov->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale = {static_cast<float>(kW), static_cast<float>(kH)};
    ov->SetOverlayMouseScale(g_overlay, &scale);
    ov->SetOverlayFlag(g_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    ov->SetOverlayFlag(g_overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    ov->SetOverlaySortOrder(g_overlay, 900);
    return true;
}

// Davanti a dove guarda il giocatore (solo yaw), a 1,6 m, all'altezza degli occhi.
void Place() {
    vr::IVROverlay* ov = vr::VROverlay();
    vr::IVRSystem* sys = vr::VRSystem();
    if (!ov || !sys || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    vr::TrackedDevicePose_t pose;
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
    const vr::HmdMatrix34_t& m = pose.mDeviceToAbsoluteTracking;
    float fx = -m.m[0][2], fz = -m.m[2][2];
    float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-3f) { fx = 0.0f; fz = -1.0f; fl = 1.0f; }
    fx /= fl;
    fz /= fl;
    float dist = 1.6f;
    g_placedYaw = std::atan2(fx, -fz);  // direzione in cui e' stato messo (per rimetterlo davanti se ci si gira)
    float zx = -fx, zz = -fz;
    vr::HmdMatrix34_t t = {};
    t.m[0][0] = zz;   t.m[0][1] = 0.0f; t.m[0][2] = zx; t.m[0][3] = m.m[0][3] + fx * dist;
    t.m[1][0] = 0.0f; t.m[1][1] = 1.0f; t.m[1][2] = 0.0f; t.m[1][3] = pose.bPoseIsValid ? m.m[1][3] - 0.1f : 1.5f;
    t.m[2][0] = -zx;  t.m[2][1] = 0.0f; t.m[2][2] = zz; t.m[2][3] = m.m[2][3] + fz * dist;
    ov->SetOverlayTransformAbsolute(g_overlay, vr::TrackingUniverseStanding, &t);
}

void Redraw() {
    std::vector<uint32_t> px;
    if (!Render(&px) || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    vr::VROverlay()->SetOverlayRaw(g_overlay, px.data(), kW, kH, 4);
    g_dirty = false;
}

void Click(int x, int y) {
    for (const Hit& h : g_hits) {
        if (x < h.r.left || x >= h.r.right || y < h.r.top || y >= h.r.bottom) continue;
        int a = h.action;
        if (a >= 1 && a <= 3) {
            g_tab = a - 1;
            g_scroll = 0;
        } else if (a == 4) {
            Show(false);
        } else if (a == 5) {
            g_scroll -= kRowsPerPage;
        } else if (a == 6) {
            g_scroll += kRowsPerPage;
        } else if (a >= 2000) {
            std::vector<Item>& items = g_tab == 0 ? g_basic : g_advanced;
            if (a - 2000 < static_cast<int>(items.size())) Change(items[a - 2000], +1);
        } else if (a >= 1000) {
            std::vector<Item>& items = g_tab == 0 ? g_basic : g_advanced;
            if (a - 1000 < static_cast<int>(items.size())) Change(items[a - 1000], -1);
        } else if (a >= 10) {
            std::vector<Item>& items = g_tab == 0 ? g_basic : g_advanced;
            if (a - 10 < static_cast<int>(items.size())) Change(items[a - 10], +1);
        }
        g_dirty = true;
        return;
    }
}

void DoShow(bool show) {
    if (show == g_open) return;
    if (show) {
        if (!EnsureOverlay()) return;
        LoadIni();
        g_tab = 0;
        g_scroll = 0;
        Place();
        g_dirty = true;
        Redraw();
        // Puntatore di SteamVR solo con il menu aperto: da chiuso l'overlay
        // tratteneva il focus dei controller e i tasti (A) non arrivavano al
        // gioco finche' non si premeva Meta (21/10).
        vr::VROverlay()->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlayFlag(g_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        vr::VROverlay()->ShowOverlay(g_overlay);
        g_open = true;
        vr::control::g_vrMenuOpen = true;
        vr::log::Line("VRMENU: shown.");
    } else {
        if (vr::VROverlay() && g_overlay != vr::k_ulOverlayHandleInvalid) {
            vr::VROverlay()->SetOverlayFlag(g_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
            vr::VROverlay()->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_None);
            vr::VROverlay()->HideOverlay(g_overlay);
        }
        g_open = false;
        vr::control::g_vrMenuOpen = false;
        vr::log::Line("VRMENU: closed.");
    }
}

}  // namespace

void Show(bool show) { g_request = show ? 1 : -1; }
bool IsOpen() { return g_open; }

void Tick(bool cinema) {
    // Dal worker dell'uscita VR (ogni frame: puntatore reattivo) e dal thread
    // della sessione (ogni 0,5 s, riserva senza visore). Uno alla volta, con
    // il lock condiviso della sessione per gli overlay; chi trova occupato salta.
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    vr::session::Shared session(true);
    if (!session.ok()) return;
    int req = g_request.exchange(0);
    if (req) DoShow(req > 0);
    // Apertura automatica: sul primo schermo virtuale (menu del gioco), dopo 2 s.
    int mode = vr::control::g_vrMenu.load();
    if (!g_open && !g_shownAtStart && mode == 1 && vr::session::Ready() && vr::session::HmdPresent()) {  // mai senza visore (test headless)
        // Prima del gioco: 1,5 s dopo l'inizio della sessione nel visore,
        // qualunque cosa stia facendo il gioco (lo schermo virtuale resta
        // nascosto finche' il menu e' aperto, vedi vr_output).
        (void)cinema;
        ULONGLONG now = GetTickCount64();
        if (!g_cinemaSince) g_cinemaSince = now;
        if (now - g_cinemaSince > 1500) {
            g_shownAtStart = true;
            DoShow(true);
        }
    }
    if (!g_open || !vr::VROverlay() || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    // Menu fuori dalla vista (testa girata di oltre 50 gradi per 0,7 s, p.es.
    // visore indossato guardando altrove): lo si rimette davanti.
    {
        vr::IVRSystem* sys = vr::VRSystem();
        vr::TrackedDevicePose_t pose;
        if (sys) {
            sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
            const vr::HmdMatrix34_t& m = pose.mDeviceToAbsoluteTracking;
            float yaw = std::atan2(-m.m[0][2], m.m[2][2]);
            float d = std::fabs(std::remainder(yaw - g_placedYaw, 6.28318530718f));
            ULONGLONG nowP = GetTickCount64();
            if (pose.bPoseIsValid && d > 50.0f * 0.01745329252f) {
                if (!g_awaySince) g_awaySince = nowP;
                if (nowP - g_awaySince > 700) {
                    g_awaySince = 0;
                    Place();
                }
            } else {
                g_awaySince = 0;
            }
        }
    }
    vr::VREvent_t ev;
    while (vr::VROverlay()->PollNextOverlayEvent(g_overlay, &ev, sizeof(ev))) {
        // Coordinate del mouse dell'overlay con SetOverlayMouseScale: origine
        // in basso a sinistra (verificato nel visore 21/10: senza inversione
        // nessun clic arrivava ai pulsanti); "vrmenu flipy off" per provare.
        int x = static_cast<int>(ev.data.mouse.x), y = static_cast<int>(ev.data.mouse.y);
        if (vr::control::g_vrMenuFlipY) y = kH - y;
        switch (ev.eventType) {
        case vr::VREvent_MouseMove: {
            // Ridisegno solo quando cambia il pulsante sotto il puntatore:
            // un SetOverlayRaw a ogni movimento faceva lampeggiare il menu.
            auto hitAt = [&](int px, int py) {
                for (size_t i = 0; i < g_hits.size(); ++i) {
                    const RECT& r = g_hits[i].r;
                    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) return static_cast<int>(i);
                }
                return -1;
            };
            int before = hitAt(g_hoverX, g_hoverY), after = hitAt(x, y);
            g_hoverX = x;
            g_hoverY = y;
            if (before != after) g_dirty = true;
            break;
        }
        case vr::VREvent_MouseButtonDown:
            Click(x, y);
            break;
        case vr::VREvent_ScrollDiscrete:
            g_scroll += ev.data.scroll.ydelta < 0 ? 3 : -3;
            g_dirty = true;
            break;
        default:
            break;
        }
    }
    if (g_open && g_dirty) Redraw();
}

}  // namespace vr::menu
