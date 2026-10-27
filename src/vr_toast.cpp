#include "vr_toast.h"
#include "control.h"
#include "logger.h"
#include "ui_text.h"
#include "vr_session.h"

#include <windows.h>
#include <openvr.h>
#include "vr_runtime.h"

#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace vr::toast {

namespace {

// Impaginazione e testi in ui_text.h (anche per i test offline): a capo
// automatico, 40 px su una o due righe, se no 36 / 32 (review WI-4c round 1).
constexpr int kW = vr::ui::kToastW, kH = vr::ui::kToastH;
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
bool RenderText(const std::string& text, std::vector<uint32_t>* rgba, vr::ui::ToastLayout* layout = nullptr) {
    std::vector<uint32_t> bgra;
    vr::ui::ToastLayout l;
    if (!vr::ui::RenderToastBgra(text, &bgra, &l)) return false;
    rgba->resize(bgra.size());
    for (size_t i = 0; i < bgra.size(); ++i) {
        uint32_t p = bgra[i];  // BGRA (GDI, alfa non scritto)
        uint32_t b = p & 0xFF, g = (p >> 8) & 0xFF, rr = (p >> 16) & 0xFF;
        (*rgba)[i] = rr | (g << 8) | (b << 16) | (0xE0u << 24);  // RGBA, fondo al 88%
    }
    if (!l.fits) vr::log::Line("TOAST: text too long even at " + std::to_string(l.fontPx) + " px, cut: \"" + text + "\"");
    if (layout) *layout = l;
    return true;
}

bool Show(const std::string& text) {
    vr::IVROverlay* ov = vr::rt::Overlay();
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
    g_shownUntil = GetTickCount64() + vr::ui::ToastShowMs(text.size());
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
        if (vr::rt::Overlay() && g_overlay != vr::k_ulOverlayHandleInvalid) vr::rt::Overlay()->HideOverlay(g_overlay);
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

// Opzioni che decidono i suggerimenti sugli oggetti e il loro testo (ui_text.h ObjHintText).
static unsigned ObjHintOptions() {
    using namespace vr::control;
    unsigned o = 0;
    if (g_grabProps) o |= vr::ui::kObjOptGrab;
    if (g_gravityGloves) o |= vr::ui::kObjOptGloves;
    if (g_propCombat && g_propThrowDamage) o |= vr::ui::kObjOptThrowDmg;
    if (g_propShield) o |= vr::ui::kObjOptShield;
    if (g_propCombat && g_propMelee) o |= vr::ui::kObjOptMelee;
    if (g_disarm) o |= vr::ui::kObjOptDisarm;
    if (g_grenadeBack) o |= vr::ui::kObjOptGrenade;
    if (g_weaponPickup) o |= vr::ui::kObjOptGun;
    if (g_propExplosive) o |= vr::ui::kObjOptExplosive;
    return o;
}

void HintsTick(bool gameplay, int turret, unsigned objectSituations) {
    static vr::ui::HintSequence seq;
    static bool done = false, counted = false;
    static int lastMode = -1;
    static unsigned objSession = 0;  // suggerimenti sugli oggetti visti in questa sessione ("hints on")
    int mode = vr::control::g_hints.load();  // 0 off, 1 on, 2 auto
    // "hints on" dal menu o dalla console a sessione iniziata: prima "done"
    // restava fermo dal primo tick (auto con 3 sessioni gia' contate) e non si
    // vedeva niente fino al riavvio.
    if (mode != lastMode) {
        lastMode = mode;
        done = false;
    }
    // Suggerimenti contestuali sugli oggetti (regressione finale del gameplay,
    // 27/10): prima della serie dei comandi, che finita ("done") esce subito.
    // Con "hints auto" una volta per profilo (bit salvati in "objecthints"),
    // anche dopo le 3 sessioni della serie; con "hints on" una volta per
    // sessione. Lo stesso intervallo di 7 s della serie (seq.lastHint).
    if (mode != 0 && objectSituations && vr::control::g_toasts) {
        const unsigned shown = mode == 2 ? vr::control::g_objectHintsShown.load() : objSession;
        const unsigned opt = ObjHintOptions();
        const unsigned long long now = GetTickCount64();
        const int which = vr::ui::ObjHintStep(now, gameplay, turret >= 0, objectSituations, opt, shown, seq.lastHint);
        if (which >= 0) {
            seq.lastHint = now;
            objSession |= 1u << which;
            char b[96];
            std::snprintf(b, sizeof(b), "HINT: object hint %d (situations 0x%02X, options 0x%03X)", which, objectSituations, opt);
            vr::log::Line(b);
            Notify(vr::ui::ObjHintText(which, opt));
            if (mode == 2) {
                const unsigned n = shown | (1u << which);
                vr::control::g_objectHintsShown = n;
                vr::control::SaveIniValue("objecthints", std::to_string(n));
            }
        }
    }
    if (done) return;
    if (mode == 0 || (mode == 2 && vr::control::g_hintsShown.load() >= 3)) {
        done = true;
        return;
    }
    // Testi e sequenza in ui_text.h (HintSequence). Review WI-4c round 2: la
    // serie parla dell'arma in mano, quindi su una torretta aspetta (prima sulla
    // minigun del prologo arrivavano "Other hand on the barrel = two-handed", la
    // mira, la ricarica a mano e il calcio) e si vede il suggerimento della presa
    // della torretta; a piedi la serie riprende da dove era.
    bool weapon = false;
    const char* show = seq.Step(GetTickCount64(), gameplay, turret, &weapon);
    if (show) {
        Notify(show);
        if (weapon && !counted) {
            counted = true;
            if (mode == 2) {
                int n = vr::control::g_hintsShown.load() + 1;
                vr::control::g_hintsShown = n;
                vr::control::SaveIniValue("hintsshown", std::to_string(n));
            }
        }
    }
    if (seq.WeaponDone()) done = true;
}

// Collaudo senza visore ("toastdump <cartella>", review WI-4c round 1): ogni
// suggerimento, le due guide e i due suggerimenti della torretta e gli avvisi
// del calcio (round 2) disegnati dal renderer vero in BMP (toast_00.bmp ...),
// con l'impaginazione nel log.
void DumpForTest(const std::string& dir) {
    std::vector<std::string> texts(vr::ui::kHints, vr::ui::kHints + vr::ui::kHintCount);
    texts.push_back(vr::ui::kToastTurretNormal);
    texts.push_back(vr::ui::kToastTurretGunstock);
    texts.push_back(vr::ui::kHintTurretNormal);
    texts.push_back(vr::ui::kHintTurretGunstock);
    texts.push_back(std::string(vr::ui::kToastMigrationKept) + "15.4 deg");
    texts.push_back(vr::ui::kToastMigrationNoStock);
    texts.push_back(vr::ui::kToastStockHint);
    // Suggerimenti contestuali sugli oggetti, ogni variante (regressione finale del gameplay).
    for (const char* t : {vr::ui::kObjHintNearGloves, vr::ui::kObjHintNearPlain, vr::ui::kObjHintHeldBoth, vr::ui::kObjHintHeldThrow,
                          vr::ui::kObjHintHeldShield, vr::ui::kObjHintMeleeBoth, vr::ui::kObjHintMeleeSwing, vr::ui::kObjHintMeleeDisarm,
                          vr::ui::kObjHintGrenadeText, vr::ui::kObjHintGunText, vr::ui::kObjHintExplosiveText})
        texts.push_back(t);
    int bad = 0;
    for (size_t i = 0; i < texts.size(); ++i) {
        std::vector<uint32_t> bgra;
        vr::ui::ToastLayout l;
        if (!vr::ui::RenderToastBgra(texts[i], &bgra, &l)) continue;
        char name[64];
        std::snprintf(name, sizeof(name), "\\toast_%02u.bmp", static_cast<unsigned>(i));
        const std::string path = dir + name;
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        ih.biSize = sizeof(ih);
        ih.biWidth = kW;
        ih.biHeight = -kH;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = BI_RGB;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + static_cast<DWORD>(bgra.size() * 4);
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            fwrite(bgra.data(), 4, bgra.size(), f);
            fclose(f);
        }
        bad += l.fits ? 0 : 1;
        char b[400];
        std::snprintf(b, sizeof(b), "TOASTDUMP: %s %d px, %d line(s), text %dx%d of %dx%d, %s, shown %.1f s: \"%s\"", name + 1, l.fontPx,
                      l.lines, l.textW, l.textH, kW - 2 * vr::ui::kToastPadX, kH - 2 * vr::ui::kToastPadY, l.fits ? "fits" : "CUT",
                      vr::ui::ToastShowMs(texts[i].size()) / 1000.0, texts[i].c_str());
        vr::log::Line(b);
    }
    vr::log::Line("TOASTDUMP: " + std::to_string(texts.size()) + " toasts, " + std::to_string(bad) + " cut, in " + dir);
}

}  // namespace vr::toast
