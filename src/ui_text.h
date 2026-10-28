#pragma once
// Testi dell'interfaccia nel visore (avvisi, suggerimenti, pagina Controls del
// menu) e impaginazione degli avvisi, solo GDI: li usano vr_toast.cpp,
// vr_menu.cpp, hooks.cpp e i test offline (unit_tests: ogni testo deve
// entrare intero, misurato con lo stesso DrawText del visore).
//
// Review WI-4c (round 1, alto): gli avvisi erano una riga sola con i puntini
// (DT_SINGLELINE | DT_END_ELLIPSIS, ~38 caratteri a 40 px): il suggerimento 0
// si leggeva "Aim with the gun in your right hand; l..." (il tester: "the hint
// to use right hand only") e la guida della torretta non si vedeva mai. Ora a
// capo automatico: 40 px su una o due righe, se no 36 / 32 px (fino a tre
// righe), e il tempo a schermo cresce con la lunghezza.
#ifndef NOMINMAX
#define NOMINMAX  // std::min / std::max nei file che lo includono per primo (unit_tests)
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr::ui {

// --- Avvisi (vr_toast) ---------------------------------------------------------
constexpr int kToastW = 768, kToastH = 150;  // pixel; overlay largo 0,55 m
constexpr int kToastPadX = 24, kToastPadY = 6;

// Guida alla prima torretta della sessione (hooks.cpp TurretAimStep): una riga
// a 40 px, letta in un attimo (review WI-4c round 1: prima tagliate a ~38 caratteri).
// Il dettaglio (una mano sola mira) nel suggerimento kHintTurretNormal.
constexpr const char* kToastTurretNormal = "Turret: move both handles to aim";
constexpr const char* kToastTurretGunstock = "Turret: aim along your stock";

// Suggerimenti sui controller (vr_toast HintsTick), in ordine. Parlano dell'arma
// in mano, a piedi: su una torretta aspettano (HintSequence qui sotto).
// WI-4c: "gun hand", non "right hand" (mancini; sulle torrette il tester aveva
// mirato solo con la destra dopo questo suggerimento, tagliato a "right hand; l...").
constexpr const char* kHints[] = {
    "Aim with your gun hand. Other hand on the barrel (grip or rest it) = two-handed",
    "Right trigger: fire. Left trigger or gun to the eye: aim down the sights",
    "A: action / cover. B: melee (or punch / hit with the gun). X: reload. Y: next weapon",
    "Left grip: grenade (quick throw with the left hand). Right grip: squad orders",
    "Reload by hand: grip the magazine under the gun, pull it, grip at your belt, push it in",
    "Right stick held + direction: D-pad. Both sticks held 1 s: recenter (3 s: standing height)",
    "Gunstock: aim at something far, look at it, hold B + Y for 1 s",
};
constexpr size_t kHintCount = sizeof(kHints) / sizeof(kHints[0]);

// Suggerimento sulla torretta, uno per presa (review WI-4c round 2, medio): sulla
// minigun del prologo la serie di prima mostrava "Other hand on the barrel =
// two-handed" (falso su una torretta: la presa a due mani e' vietata li') e poi
// mira, ricarica a mano e calcio. Ora sulla torretta la serie aspetta e si vede
// questo; dice anche che una mano sola mira (quello che il tester ha dovuto scoprire).
constexpr const char* kHintTurretNormal = "Turret: both hands on the handles, move them to aim like a real machine gun. One hand alone also aims";
constexpr const char* kHintTurretGunstock = "Turret: aim along your stock with your gun hand, as with your guns";

// Sequenza dei suggerimenti (pura, per i test): dopo 15 s di gioco, uno ogni
// 7 s. Su una torretta (turret >= 0) la serie dell'arma aspetta di scendere e si
// mostra una volta il suggerimento della presa della torretta (0 NORMAL, 1
// GUNSTOCK; 2 = torretta mirata in altro modo, stick o "mode off": nessuno).
struct HintSequence {
    unsigned long long gameplaySince = 0, lastHint = 0;
    size_t next = 0;
    bool turretShown[2] = {false, false};
    // Testo da mostrare adesso o nullptr; *weapon = true se e' della serie (conta la sessione).
    const char* Step(unsigned long long nowMs, bool gameplay, int turret, bool* weapon = nullptr) {
        if (weapon) *weapon = false;
        if (!gameplay) {
            gameplaySince = 0;
            return nullptr;
        }
        if (!gameplaySince) gameplaySince = nowMs;
        if (nowMs - gameplaySince < 15000 || (lastHint && nowMs - lastHint < 7000)) return nullptr;
        if (turret >= 0) {
            if (turret > 1 || turretShown[turret]) return nullptr;
            turretShown[turret] = true;
            lastHint = nowMs;
            return turret == 0 ? kHintTurretNormal : kHintTurretGunstock;
        }
        if (next >= kHintCount) return nullptr;
        lastHint = nowMs;
        if (weapon) *weapon = true;
        return kHints[next++];
    }
    bool WeaponDone() const { return next >= kHintCount; }
};

// Suggerimenti contestuali sugli oggetti (regressione finale del gameplay, 27/10;
// chiesti dall'utente: il collaudatore ha scoperto le funzioni solo dai
// suggerimenti, e la serie qui sopra parla solo dei comandi). Uno per
// situazione, la prima volta che succede (bit delle situazioni da props.cpp
// HintSituations), al massimo uno ogni 7 s insieme alla serie qui sopra, solo a
// piedi nel gioco 3D (niente scene, pausa o torrette), mai con l'opzione della
// funzione spenta. Con "hints auto" una volta per profilo (bit in "objecthints"
// nell'ini), con "hints on" una volta per sessione, con "hints off" mai.
enum ObjHint : int { kObjHintNear = 0, kObjHintHeld, kObjHintMelee, kObjHintGrenade, kObjHintGun, kObjHintExplosive, kObjHintCount };
// Opzioni che decidono se e quale testo (ObjHintText).
enum : unsigned {
    kObjOptGrab = 1u << 0,       // grabprops
    kObjOptGloves = 1u << 1,     // gravitygloves
    kObjOptThrowDmg = 1u << 2,   // propcombat + propthrowdamage
    kObjOptShield = 1u << 3,     // propshield
    kObjOptMelee = 1u << 4,      // propcombat + propmelee
    kObjOptDisarm = 1u << 5,     // disarm
    kObjOptGrenade = 1u << 6,    // grenadeback
    kObjOptGun = 1u << 7,        // weaponpickup
    kObjOptExplosive = 1u << 8,  // propexplosive
};
constexpr const char* kObjHintNearGloves = "Left grip near an object: grab it. Point at it and flick: pull it to you";
constexpr const char* kObjHintNearPlain = "Left grip near an object: grab it";
constexpr const char* kObjHintHeldBoth = "Throw it at enemies: heavy objects knock them down. Hold it up to stop bullets";
constexpr const char* kObjHintHeldThrow = "Throw it at enemies: heavy objects knock them down";
constexpr const char* kObjHintHeldShield = "Hold it up to stop enemy bullets";
constexpr const char* kObjHintMeleeBoth = "Swing it at an enemy, or hit his gun to disarm him";
constexpr const char* kObjHintMeleeSwing = "Swing it hard at an enemy";
constexpr const char* kObjHintMeleeDisarm = "Hit an enemy's gun with it to disarm him";
constexpr const char* kObjHintGrenadeText = "Grab the grenade (red corners) and throw it back";
constexpr const char* kObjHintGunText = "Grab a dropped gun, bring it to your gun hand to take it";
constexpr const char* kObjHintExplosiveText = "Explosive: it blows up if shot or dropped hard";

// Testo del suggerimento con le opzioni di adesso, o nullptr (funzione spenta).
inline const char* ObjHintText(int which, unsigned opt) {
    if (!(opt & kObjOptGrab)) return nullptr;  // grabprops off: niente oggetti in mano
    switch (which) {
    case kObjHintNear:
        return opt & kObjOptGloves ? kObjHintNearGloves : kObjHintNearPlain;
    case kObjHintHeld:
        if ((opt & kObjOptThrowDmg) && (opt & kObjOptShield)) return kObjHintHeldBoth;
        return opt & kObjOptThrowDmg ? kObjHintHeldThrow : (opt & kObjOptShield ? kObjHintHeldShield : nullptr);
    case kObjHintMelee:
        if ((opt & kObjOptMelee) && (opt & kObjOptDisarm)) return kObjHintMeleeBoth;
        return opt & kObjOptMelee ? kObjHintMeleeSwing : (opt & kObjOptDisarm ? kObjHintMeleeDisarm : nullptr);
    case kObjHintGrenade:
        return opt & kObjOptGrenade ? kObjHintGrenadeText : nullptr;
    case kObjHintGun:
        return opt & kObjOptGun ? kObjHintGunText : nullptr;
    case kObjHintExplosive:
        return opt & kObjOptExplosive ? kObjHintExplosiveText : nullptr;
    default:
        return nullptr;
    }
}

// Ordine quando piu' situazioni sono in corso insieme: il pericolo prima.
constexpr int kObjHintOrder[kObjHintCount] = {kObjHintGrenade, kObjHintExplosive, kObjHintMelee,
                                              kObjHintGun,     kObjHintHeld,      kObjHintNear};

// Il suggerimento da mostrare adesso (indice ObjHint) o -1: situazione in corso
// (bit 1 << indice in sit), non ancora visto (shown), testo per le opzioni di
// adesso, a piedi nel gioco (non sulla torretta) e almeno 7 s dopo l'ultimo
// suggerimento di qualunque serie (lastAnyMs, 0 = nessuno).
inline int ObjHintStep(unsigned long long nowMs, bool gameplay, bool turret, unsigned sit, unsigned opt, unsigned shown,
                       unsigned long long lastAnyMs) {
    if (!gameplay || turret || !sit) return -1;
    if (lastAnyMs && nowMs - lastAnyMs < 7000) return -1;
    for (int which : kObjHintOrder)
        if ((sit & (1u << which)) && !(shown & (1u << which)) && ObjHintText(which, opt)) return which;
    return -1;
}

// Avvisi del calcio (hooks.cpp: avviso della migrazione e suggerimento senza
// "stockmount"). Review WI-4c round 2 (basso): prima mezze frasi in due avvisi da
// 3,5 s ("Stock: look along it at a far target," / "then hold B + Y 1 s, left
// hand on it"), quando un avviso era una riga sola; ora un avviso a capo.
constexpr const char* kToastMigrationKept = "Old aim kept: gunstock, ";  // + stockangle + " deg"
constexpr const char* kToastMigrationNoStock = "No stock? Set Controller hold: normal";
constexpr const char* kToastStockHint = "Stock: look along it at a far target, then hold B + Y 1 s with your left hand on it";
// 0.10.0: l'ini di una versione precedente rimesso ai predefiniti (control.cpp, kResetConfigVersion).
constexpr const char* kToastSettingsReset = "New version: settings back to the defaults (old file kept as specopsvr.ini.bak)";

// Tempo a schermo: 3,5 s fino a 40 caratteri, poi 50 ms per carattere, al
// massimo 7 s (prima 3,5 s per qualunque lunghezza).
inline unsigned ToastShowMs(size_t chars) {
    const size_t ms = chars <= 40 ? 3500 : 3500 + (chars - 40) * 50;
    return static_cast<unsigned>(ms > 7000 ? 7000 : ms);
}

struct ToastLayout {
    int fontPx = 0;  // dimensione usata
    int lines = 0;
    int textW = 0, textH = 0;  // ingombro del testo (pixel)
    bool fits = false;         // tutto il testo dentro, senza puntini
};

inline HFONT ToastFont(int px) {
    return CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

// Ingombro col font gia' scelto nel dc, a capo automatico nella larghezza utile.
inline ToastLayout MeasureToast(HDC dc, const std::string& text, int px) {
    ToastLayout l;
    l.fontPx = px;
    RECT r = {0, 0, kToastW - 2 * kToastPadX, 0};
    DrawTextA(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT);
    TEXTMETRICA tm = {};
    GetTextMetricsA(dc, &tm);
    l.textW = r.right - r.left;
    l.textH = r.bottom - r.top;
    l.lines = tm.tmHeight > 0 ? (l.textH + tm.tmHeight / 2) / tm.tmHeight : 0;
    l.fits = l.textW <= kToastW - 2 * kToastPadX && l.textH <= kToastH - 2 * kToastPadY;
    return l;
}

// Disegna l'avviso (testo chiaro su fondo scuro) in BGRA dall'alto in basso,
// kToastW x kToastH; il testo centrato in verticale. Prova 40, 36, 32 px e
// usa il primo che fa entrare tutto il testo (se nessuno: 32 px, ultima riga
// con i puntini).
inline bool RenderToastBgra(const std::string& text, std::vector<uint32_t>* bgra, ToastLayout* layout = nullptr) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kToastW;
    bi.bmiHeader.biHeight = -kToastH;  // dall'alto in basso
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
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    ToastLayout l;
    for (int px : {40, 36, 32}) {
        if (font) {
            SelectObject(dc, oldFont);
            DeleteObject(font);
        }
        font = ToastFont(px);
        oldFont = font ? SelectObject(dc, font) : nullptr;
        l = MeasureToast(dc, text, px);
        if (l.fits) break;
    }
    RECT r = {0, 0, kToastW, kToastH};
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    const int h = l.textH < kToastH - 2 * kToastPadY ? l.textH : kToastH - 2 * kToastPadY;
    RECT tr = {kToastPadX, (kToastH - h) / 2, kToastW - kToastPadX, (kToastH - h) / 2 + h};
    DrawTextA(dc, text.c_str(), -1, &tr, DT_WORDBREAK | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL);
    GdiFlush();
    bgra->assign(static_cast<const uint32_t*>(bits), static_cast<const uint32_t*>(bits) + static_cast<size_t>(kToastW) * kToastH);
    if (font) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (layout) *layout = l;
    return true;
}

// --- Pagina Controls del menu (vr_menu) ------------------------------------------
// Font 22 px (Segoe UI), area utile 30..kW-30 = 1040 px; righe di 52 px, piu'
// strette quando l'avviso di Virtual Desktop sposta l'inizio in giu' (review
// WI-4c: con l'avviso la 13a riga finiva sotto il pannello, a y 816-864 di 820).
constexpr int kControlsFontPx = 22, kControlsWidthPx = 1040, kControlsRowPx = 52;
constexpr const char* kControls[] = {
    "LEFT STICK: move          RIGHT STICK: turn          STICK CLICKS: LS / RS",
    "RIGHT TRIGGER: fire        LEFT TRIGGER: aim (or bring the gun to your eye)",
    "A: action / cover          B: melee (punch or hit with the gun)",
    "X: reload                  Y: next weapon",
    "LEFT GRIP: grenade (left-hand throw) / on the barrel: two-handed grip (or just rest the hand there)",
    "RIGHT GRIP: squad orders (look at the target) / grab ropes, ziplines, turrets",
    "LEFT MENU BUTTON: pause (then LS click: this menu)     RIGHT STICK HELD + direction: D-pad",
    "BOTH STICKS 1 s: recenter   BOTH STICKS 3 s: also save your standing height",
    "B + Y 1 s: gunstock calibration (stock at your cheek, look through the sights at something far)",
    "RELOAD BY HAND: grip the magazine under the gun, pull it out, grip at your belt, push it in",
    "WEAPON SWAP: right grip over your shoulder or at your hip",
    "CROUCH: for real or with the button (view lowers smoothly)   COVER: duck by cover, lean out to aim",
    "TURRET: both hands on the handles, move them to aim (helicopter: grips, handlebars); one hand also works",
};
constexpr size_t kControlsCount = sizeof(kControls) / sizeof(kControls[0]);

// Passo delle righe della pagina Controls: 52 px se entrano tutte fra topY e
// il fondo del pannello (panelH, margine 12), altrimenti quanto basta (mai
// meno di 30: il testo e' alto 22).
inline int ControlsRowPitch(int topY, int panelH, size_t lines) {
    if (lines == 0) return kControlsRowPx;
    const int avail = panelH - 12 - topY;
    int pitch = avail / static_cast<int>(lines);
    if (pitch > kControlsRowPx) pitch = kControlsRowPx;
    if (pitch < 30) pitch = 30;
    return pitch;
}

}  // namespace vr::ui
