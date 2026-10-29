#include "vr_menu.h"
#include "control.h"
#include "logger.h"
#include "pure_utils.h"
#include "ui_text.h"
#include "vr_session.h"

#include <windows.h>
#include <openvr.h>
#include "vr_runtime.h"

#include <algorithm>
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

// Lista in alto (kListH), sotto il riquadro con la descrizione intera della
// voce puntata (29/10: le descrizioni lunghe erano tagliate a una riga).
constexpr int kW = 1100, kListH = 820, kH = 1000;
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

// Voce "Gun angle" (review WI-4a, round 1): mostra e cambia l'angolo della
// presa ATTIVA. Prima cambiava sempre gunangle, che con "holdmode gunstock"
// (anche dopo la migrazione di un vecchio aimpitch calibrato) non conta: il
// clic non faceva niente e nessuno lo diceva. B + Y nella presa normale non
// e' piu' nell'aiuto: allineando l'arma DISEGNATA (gia' inclinata di gunangle)
// la misura restituisce gunangle stesso (circolare); col calcio vale, la
// guancia fissa il controller rispetto all'occhio. Verifica nel visore.
const char* const kGunAngleHelp =
    "Normal hold: tilt of the gun above where the controller points (0 = like the menu laser). Raise it if the gun points too "
    "low, lower it if it points too high.";
const char* const kStockAngleHelp =
    "Gunstock hold: tilt of the gun above where the controller points. B + Y held 1 s measures it (stock at your cheek, look "
    "through the sights at something far). No stock: Controller hold > normal.";

// Scheda Basic: le scelte che un giocatore tocca davvero.
std::vector<Item> BasicItems() {
    return {
        {"hand", "Dominant hand", "Which controller holds the gun. Left-handed: the gun buttons move to the left controller and Walker holds the gun in his left hand.", Kind::Choice, {"right", "left"}},
        // WI-4b: la presa a due mani segue la presa: normal = la sinistra sulla canna gira l'arma; gunstock = la mira resta del calcio.
        {"holdmode", "Controller hold", "normal = bare controllers: the gun points like the menu laser (Gun angle); left hand on the barrel = two-handed, both hands aim. gunstock = controllers on a stock: the gun follows the stock (B + Y measures it).", Kind::Choice, {"normal", "gunstock"}},
        // Con "holdmode gunstock" questa voce diventa stockangle (SyncHoldAngleRow).
        {"gunangle", "Gun angle", kGunAngleHelp, Kind::Choice, {"-15", "-10", "-5", "0", "5", "10", "15"}},
        // WI-4c (bug 1 del tester): torrette a due mani, i due sistemi di mira.
        // L'aiuto si vede su una riga di 610 px: l'essenziale per primo (review WI-4c round 1).
        {"turretgrip", "Turret grip", "normal = both hands on the handles: move them to aim like a real machine gun (helicopter minigun: grips, handlebars); gunstock = aim along the stock; auto = as Controller hold. One hand alone also aims.", Kind::Choice, {"auto", "normal", "gunstock"}},
        {"wristhud", "Wrist HUD", "Ammo, weapon and health on the support-hand wrist, like a watch.", Kind::OnOff},
        {"height", "Standing height", "auto = your real height measured while playing (hold both sticks 3 s to save it); fixed = Walker's eye height whatever your height.", Kind::Choice, {"auto", "fixed"}},
        {"snapturn", "Turning", "0 = smooth turning with the right stick, 30/45 = snap turns in degrees.", Kind::Choice, {"0", "30", "45"}},
        {"vignette", "Comfort vignette", "Darkens the edges while moving: 0 off, 0.5 light, 1 strong.", Kind::Choice, {"0", "0.5", "1"}},
        {"hud", "HUD size", "Scale of the game's HUD (ammo, objectives) towards the centre of your view: lower = ammo and weapon icons closer to the centre, easier to see, but smaller.", Kind::Choice, {"0.25 100", "0.3 100", "0.35 100", "0.4 100", "0.5 100", "0.6 100"}},
        {"fpbody", "Walker's body", "full = Walker's body under you, his eyes on yours and his neck under your neck; arms = only his arms, hands and gun (torso and legs hidden).", Kind::Choice, {"full", "arms"}},
        // Review 2 del follow-up (27/10): il corpo che torna sotto la testa cambia cio' che il corpo fa dopo ogni sguardo: Basic.
        {"bodyalign", "Body under the head", "Seconds your head must stay still before Walker's body turns back under it, so looking down shows your chest, not his shoulder (0 = off: it can stay up to 45 deg off). Only with tracked controllers; it waits while you aim or near a wall.", Kind::Choice, {"0", "0.4", "1"}},
        {"reload", "Reload", "physical = grip the magazine, belt, push it in; simple = left hand on the magazine, then pull down. X always reloads.", Kind::Choice, {"physical", "simple"}},
        {"marker", "Green aim marker", "A green marker where the barrel points (on by default). Off: use only the weapon's own sights.", Kind::OnOff},
        {"laser", "Laser beam", "A visible beam from the barrel to the point of impact.", Kind::OnOff},
        {"spread", "Bullet spread", "on = the game's spread scaled by stance; off = laser-straight shots.", Kind::OnOff},
        {"physicalcrouch", "Crouch for real", "Crouch 30 cm in your room and Walker crouches with you (fits through low passages); stand up and he stands. Off = only the crouch button.", Kind::OnOff},
        {"crouchview", "Crouch view", "auto = the crouch button lowers your view smoothly to Walker's crouch (crouching for real moves only your own head, never twice); real = only your real head; game = the view drops with the game.", Kind::Choice, {"auto", "real", "game"}},
        {"grabprops", "Grab objects", "Close the grip next to a physics object to pick it up; open it to drop or throw it.", Kind::OnOff},
        {"gravitygloves", "Gravity gloves", "Point your left hand at an object or a dropped gun, close the grip and flick your wrist: it flies to your hand (like Half-Life: Alyx).", Kind::OnOff},
        {"grabhighlight", "Highlight objects", "Glowing corners around the object you point at or can reach (gold when locked).", Kind::OnOff},
        {"throwboost", "Throw strength", "Thrown objects leave at your hand's speed times this (1.1 = SteamVR's default).", Kind::Choice, {"1", "1.1", "1.3", "1.6", "2"}},
        {"throwmode", "Throw style", "peak = objects leave at the fastest speed of your throw and spin with your wrist; heavy ones lob; a slow release just puts them down. classic = your hand's average speed at release (older).", Kind::Choice, {"peak", "classic"}},
        {"objectweight", "Object weight", "Heavy objects in your free hand lag, sag and turn slowly (full) or a little (light); off = no weight. Everything can still be lifted.", Kind::Choice, {"light", "full", "off"}},
        {"analoggrip", "Analog grip", "With Object weight: squeeze harder for more control. A loose grip gives your hand less pull and twist (objects trail further and turn back slowly); a full squeeze holds them tight. Everything can still be lifted.", Kind::OnOff},
        {"solidhands", "Solid hands", "Free hand drawn where physics puts it: on what you hold (with Object weight: lags, sags, stays on an object pressed into a crate) and on door handles; full = empty hand also stops on objects with physics. Gun unchanged.", Kind::Choice, {"held", "full", "off"}},
        {"impacthaptics", "Impact haptics", "Pulses when what you hold hits something or your empty hand slaps it, scaled by how hard (full) or at half strength (low); off = fixed pulses only.", Kind::Choice, {"full", "low", "off"}},
        {"propcombat", "Objects as weapons", "Objects you throw or swing count as hits on enemies, glass and crates.", Kind::OnOff},
        {"propthrowdamage", "Thrown objects hurt", "An object you throw at an enemy hurts him: light ones a little (never a kill), heavy ones or a hit on the head make him stagger or fall like your melee. Needs Objects as weapons.", Kind::OnOff},
        {"propmelee", "Swing objects at enemies", "An object you hold and swing hard at an enemy hits him like your melee: from a fast swing (or on the head) he staggers or falls, then B finishes him; a bottle or glass breaks on him. Damage never kills; head hits count double. Needs Objects as weapons.", Kind::OnOff},
        {"propglass", "Objects break glass", "Objects you throw or swing crack or smash glass panes, crates and the windows holding back sand (a thrown radio can bury enemies under the avalanche; a bottle only cracks it). Needs Objects as weapons.", Kind::OnOff},
        {"propexplosive", "Explosive bottles", "Propane and diving bottles can be picked up and thrown (also with the gravity gloves); shoot one (a rifle needs two hits) or drop it from a roof and it explodes where it is, with the game's own blast and damage. Off = they stay in place, as before.", Kind::OnOff},
        {"fakegrenade", "Fake grenades", "A small object you throw (a can, a bottle, a rock) that lands within 3 m of enemies makes them think it is a grenade: the nearest one shouts \"grenade!\" and their AI gets the same warning as for a real one. Once every 10 s per group; not when it hits an enemy, or lands as close to Adams or Lugo. Off = thrown objects are just objects.", Kind::OnOff},
        {"propnoise", "Objects make noise", "Experimental, not verified. An object you throw, drop or knock over that lands hard (or a bottle you smash) sends the enemies' AI the game's fist-fight noise at the spot where it lands: within 3-15 m depending on how heavy it is and how hard it lands (breaking glass is louder). Tests in a firefight showed no clear effect (the enemies already knew where you were), and it was not tested on enemies that are not fighting yet: the noise may draw them to you rather than to the object. Off by default.", Kind::OnOff},
        {"grenadeback", "Throw back grenades", "An enemy grenade that has come to rest on the ground within 8 m gets red corners (thicker when your left hand can reach it). Close your left hand on it to pick it up and throw it back. Its fuse keeps burning (enemy frag grenades: 4 s from the throw), so if you hold it too long it explodes in your hand and kills you. Once thrown back it counts as your grenade: the enemies it kills are your kills. Grenades still flying cannot be caught, and a grenade with under 1 s of fuse left gets no corners and cannot be picked up. Your squad's grenades are left alone. Off = enemy grenades cannot be picked up.", Kind::OnOff},
        {"corpsedrag", "Drag bodies", "Close your left hand on a dead enemy's arm, leg or body to grab it and drag it (to clear a doorway, or to move it out of sight); a fast swing of your free hand also shoves a body. A body is heavy: it lags when you pull hard and the pull grows while it does not come, so it always moves; yank it away fast and it slips out of your hand. Only dead enemies: never squad mates, story characters or civilians, never the living or enemies still dying. Off = bodies stay where they fall.", Kind::OnOff},
        {"weaponpickup", "Pick up enemy weapons", "Close your left (support) hand on a gun an enemy dropped to pick it up, then bring it to your gun hand to take it (the gun you held falls to the ground if you already carry two). Touching a gun like one you carry takes its ammo instead. Off = press A next to it, as in the game.", Kind::OnOff},
        {"disarm", "Disarm enemies", "Hit an enemy's gun hard (a fast slap with your free hand, a swung object, or a thrown one) and he staggers; harder and the gun falls out of his hands, where you can pick it up. A disarmed enemy stops shooting; once back on his feet (5-10 s) he grabs his gun again if it is still next to him and you did not take it. Needs Objects as weapons.", Kind::OnOff},
        {"propshield", "Objects stop bullets", "An object you hold (a suitcase, a box, a radio) stops enemy bullets: each hit pushes it and you feel it; after a few hits it flies out of your hand (plates and books 2, suitcases and radios about 6, metal boxes and sandbags 15). Off = bullets go through what you hold.", Kind::OnOff},
        {"coverlean", "Lean out of cover", "When Walker leans out of cover to aim, your view and gun go with him around the edge (off = only your real head moves).", Kind::OnOff},
        {"roomscale", "Room-scale", "Walker follows your real steps (collisions included).", Kind::OnOff},
        {"headcollision", "Head in walls", "push = leaning into a wall, crate or squadmate stops your view at the surface (sliding along it stays 1:1); off = the view goes through.", Kind::Choice, {"push", "off"}},
        {"seated", "Seated play", "auto = detected when you put the headset on or recenter (sitting: your eyes go to Walker's eye height, no real crouch); on / off force it.", Kind::Choice, {"auto", "on", "off"}},
        {"mirrorwin", "Recording window", "A 1920x1080 window on the PC showing your view (right eye, 16:9) for OBS: add a Window Capture of \"SpecOpsVR mirror\" (Windows 10 capture method). It stays behind your other windows and never takes the focus.", Kind::OnOff},
        {"hints", "Controller hints", "Short hints in the headset during the first sessions, and one per object situation (auto: once per profile; on: every session).", Kind::Choice, {"auto", "on", "off"}},
        {"vrmenu", "This menu at start", "Open this menu on the game's main menu at every start.", Kind::Choice, {"on", "off"}},
        {"runtime", "VR runtime", "steamvr = through SteamVR; openxr = Quest Link or Virtual Desktop (VDXR) directly, no SteamVR; auto (default) = Virtual Desktop (VDXR) when its Streamer runs, otherwise SteamVR. Restart the game.", Kind::Choice, {"steamvr", "openxr", "auto"}},
    };
}

// Righe della pagina Controls: ui_text.h (misurate nei test offline: ognuna
// entro i 1040 px della pagina; review WI-4c round 1).
using vr::ui::kControls;


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
// Righe disegnate (rettangolo e indice della voce) per la voce puntata.
struct RowHit {
    RECT r;
    int item;
};
std::vector<RowHit> g_rows;
int RowAt(int x, int y) {
    for (const RowHit& h : g_rows)
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return h.item;
    return -1;
}
bool g_dirty = true;
float g_placedYaw = 0.0f;
ULONGLONG g_awaySince = 0;

struct Hit {
    RECT r;
    int action;  // 1 tab basic, 2 advanced, 3 controls, 4 play, 5 scroll up, 6 scroll down, 10+i riga i (toggle/cycle), 1000+i meno, 2000+i piu'
};
std::vector<Hit> g_hits;
// Ultima pagina Controls disegnata (collaudo "vrmenu dump"): riga piu' larga, fondo dell'ultima, passo.
struct ControlsMetrics {
    int widest = 0, bottom = 0, pitch = 0;
};
ControlsMetrics g_lastControls;
int g_forceVd = -1;  // collaudo: -1 come e', 0 / 1 avviso di Virtual Desktop forzato
struct DumpReq {
    std::string path;
    int tab;
    int vd;
    int hoverRow = -1;  // "basic@3": il laser sulla riga 3 della pagina (riquadro della descrizione)
};
std::vector<DumpReq> g_dumpReq;

// Voce "Gun angle" sulla presa attiva (kGunAngleHelp): chiave, aiuto e
// scelte; con withValue anche il valore, dai globali (dopo un clic su
// "Controller hold", che cambia la presa senza ricaricare l'ini).
void SyncHoldAngleRow(bool withValue) {
    const bool stock = vr::control::g_holdMode.load() == 1;
    for (Item& it : g_basic) {
        if (it.key != "gunangle" && it.key != "stockangle") continue;
        it.key = stock ? "stockangle" : "gunangle";
        it.help = stock ? kStockAngleHelp : kGunAngleHelp;
        it.choices = stock ? std::vector<std::string>{"0", "5", "10", "15", "20", "25", "30", "35", "40"}
                           : std::vector<std::string>{"-15", "-10", "-5", "0", "5", "10", "15"};
        if (withValue)
            it.value = vr::pure::FormatConfigNumber(stock ? vr::control::g_stockAngleDeg.load() : vr::control::g_gunAngleDeg.load(), 1);
    }
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string FirstWord(const std::string& s) {
    std::istringstream in(s);
    std::string w;
    in >> w;
    return w;
}

// Righe dell'ini accanto alla dll: valori attuali e, per Advanced, i commenti
// che precedono ogni chiave come spiegazione. Dopo, le voci dei predefiniti
// distribuiti che l'ini dell'utente non ha (vr::pure::DefaultLinesMissingFrom):
// col valore predefinito, che vale gia' (ApplyDefaultsMissingFrom); cambiarle
// le scrive nell'ini (SaveIniValue aggiunge la riga).
void LoadIni() {
    std::vector<std::string> lines;
    {
        std::ifstream f(vr::control::GameDirectory() + "\\specopsvr.ini");
        std::string l;
        while (std::getline(f, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (lines.empty() && l.rfind("\xEF\xBB\xBF", 0) == 0) l.erase(0, 3);  // BOM UTF-8 (come LoadConfigFile)
            lines.push_back(l);
        }
    }
    for (std::string& l : vr::pure::DefaultLinesMissingFrom(lines, vr::control::DefaultIniText())) lines.push_back(l);
    std::vector<std::pair<std::string, std::string>> kv;  // chiave, resto
    std::vector<Item> adv;
    std::string help;
    for (const std::string& line : lines) {
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
        if (key == "configversion" || key == "hintsshown" || key == "objecthints" || key == "autoupdate" || key == "updatechannel" || key == "updaterepo") {
            help.clear();
            continue;
        }
        kv.push_back({key, rest});
        Item it;
        it.key = key;
        it.label = key;
        it.help = help;  // intera: la riga la taglia, il riquadro sotto la mostra tutta
        it.value = rest;
        std::string low;
        for (char ch : rest) low += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        // Valori a parole noti (vr::pure::AdvancedWordChoices, review 2 WI-1):
        // si scorrono come le voci Choice di Basic (prima il testo restava
        // fisso, es. "predictframe auto" per l'A/B nel visore). Prima della
        // regola on/off: "predictframe on" deve tornare a auto, non diventare
        // un interruttore on/off (e "predict" non ha "on": scriveva un valore
        // non valido; collaudo 26/10).
        const std::vector<std::string>* words = vr::pure::AdvancedWordChoices(key);
        if (words && std::find(words->begin(), words->end(), FirstWord(low)) != words->end()) {
            it.kind = Kind::Choice;
            it.choices = *words;
            it.value = FirstWord(low);
        } else if (low == "on" || low == "off") {
            it.kind = Kind::OnOff;
        } else {
            char* e = nullptr;
            std::strtof(rest.c_str(), &e);
            it.kind = e != rest.c_str() ? Kind::Number : Kind::Text;
            it.step = vr::pure::MenuStepFor(key, static_cast<float>(std::atof(rest.c_str()))).step;
        }
        adv.push_back(it);
        help.clear();
    }
    SyncHoldAngleRow(false);  // "Gun angle" = gunangle o stockangle secondo la presa
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

void Change(Item& it, int dir) {
    std::string low;
    for (char ch : it.value) low += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (it.kind == Kind::OnOff) {
        it.value = FirstWord(low) == "on" ? "off" : "on";
    } else if (it.kind == Kind::Choice) {
        int idx = -1;
        for (size_t i = 0; i < it.choices.size(); ++i)
            if (it.choices[i] == low || FirstWord(it.choices[i]) == FirstWord(low)) idx = static_cast<int>(i);
        const int n = static_cast<int>(it.choices.size());
        if (idx < 0) {
            // Valore numerico fuori dalle scelte (es. "gunangle 2.5" da B+Y,
            // WI-4a): la scelta successiva nel verso del clic, non la prima.
            idx = vr::pure::NextNumericChoice(it.choices, low, dir);
        } else {
            idx = (idx + dir + n) % n;
        }
        it.value = it.choices[idx];
    } else if (it.kind == Kind::Number) {
        // Passo per chiave (vr::pure::MenuStepFor): gli angoli a 1 grado per
        // clic, non 0,05 come veniva dal valore (aimpitch 0, 25/10).
        it.value = vr::pure::StepConfigNumber(it.key, it.value, dir, it.step);
    } else {
        return;
    }
    Apply(it, it.value);
    if (it.key == "holdmode") SyncHoldAngleRow(true);  // la voce "Gun angle" segue la presa
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
    g_rows.clear();
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
    if (g_forceVd == 1 || (g_forceVd < 0 && vr::control::g_vdDetected)) {
        // Testo corto e a capo automatico (prima due righe lunghe tagliate).
        RECT ra = {20, kTop - 14, kW - 20, kTop + 92};
        HBRUSH ab = CreateSolidBrush(RGB(110, 70, 20));
        FillRect(dc, &ra, ab);
        DeleteObject(ab);
        SelectObject(dc, fontSmall);
        SetTextColor(dc, RGB(255, 235, 200));
        RECT r1 = {34, kTop - 8, kW - 34, kTop + 88};
        // Con VDXR (runtime openxr) il gioco regge anche 120 Hz in Godlike
        // (log 22/10: 117-120 invii/s a 3072x3264 per occhio): i limiti 72 Hz /
        // High valgono solo passando da SteamVR.
        const char* vdText =
            vr::rt::Active() == vr::rt::Backend::OpenXR
                ? "VIRTUAL DESKTOP (VDXR, no SteamVR): any VR Frame Rate and Graphics Quality works, up to 120 Hz on Godlike. "
                  "Only if the game stays at half the refresh rate: VD menu (Streaming tab) > Synchronous Spacewarp = Disabled. "
                  "Ethernet cable from the PC to the router recommended."
                : "VIRTUAL DESKTOP through SteamVR: for full frame rate set VR runtime = openxr (Basic tab, then restart the game; "
                  "up to 120 Hz on Godlike). Staying on SteamVR: VD menu (Streaming tab) > Synchronous Spacewarp = Disabled, "
                  "VR Frame Rate = 72, Graphics Quality = High or lower.";
        DrawTextA(dc, vdText, -1, &r1, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        topY = kTop + 106;
    }
    if (g_tab == 2) {
        SelectObject(dc, mono);
        SetTextColor(dc, RGB(230, 230, 230));
        // Passo delle righe che le fa entrare tutte anche sotto l'avviso di
        // Virtual Desktop (review WI-4c: con l'avviso la 13a riga, TURRET,
        // finiva a y 816-864 in un pannello alto 820).
        const int pitch = vr::ui::ControlsRowPitch(static_cast<int>(topY), kListH, vr::ui::kControlsCount);
        LONG widest = 0, bottom = 0;
        for (size_t i = 0; i < vr::ui::kControlsCount; ++i) {
            RECT r = {30, topY + static_cast<LONG>(i) * pitch, kW - 30, topY + static_cast<LONG>(i) * pitch + pitch - 4};
            DrawTextA(dc, kControls[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SIZE sz = {};
            GetTextExtentPoint32A(dc, kControls[i], static_cast<int>(std::strlen(kControls[i])), &sz);
            widest = (std::max)(widest, sz.cx);
            bottom = r.bottom;
        }
        g_lastControls = {static_cast<int>(widest), static_cast<int>(bottom), pitch};
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
            g_rows.push_back({{20, y, kW - 20, y + kRowH}, i});
            if (RowAt(g_hoverX, g_hoverY) == i) {
                RECT hr = {20, y, kW - 20, y + kRowH - 2};
                HBRUSH hb = CreateSolidBrush(RGB(34, 38, 48));
                FillRect(dc, &hr, hb);
                DeleteObject(hb);
            }
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
            Button(dc, {kW - 200, kListH - 60, kW - 110, kListH - 16}, "UP", false, 5, body);
            Button(dc, {kW - 100, kListH - 60, kW - 20, kListH - 16}, "DOWN", false, 6, body);
            SelectObject(dc, fontSmall);
            SetTextColor(dc, RGB(150, 150, 160));
            char pg[64];
            std::snprintf(pg, sizeof(pg), "%d-%d of %d", g_scroll + 1, (std::min)(n, g_scroll + rows), n);
            RECT rp = {30, kListH - 56, 600, kListH - 20};
            DrawTextA(dc, pg, -1, &rp, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    if (g_tab != 2) {
        // Descrizione intera della voce puntata.
        RECT panel = {20, kListH, kW - 20, kH - 12};
        HBRUSH pb = CreateSolidBrush(RGB(32, 35, 44));
        FillRect(dc, &panel, pb);
        DeleteObject(pb);
        const std::vector<Item>& items = g_tab == 0 ? g_basic : g_advanced;
        const int hovered = RowAt(g_hoverX, g_hoverY);
        RECT rt = {36, kListH + 8, kW - 36, kListH + 40};
        RECT rd = {36, kListH + 42, kW - 36, kH - 20};
        if (hovered >= 0 && hovered < static_cast<int>(items.size())) {
            const Item& it = items[hovered];
            SelectObject(dc, body);
            SetTextColor(dc, RGB(240, 240, 240));
            DrawTextA(dc, it.label.c_str(), -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, fontSmall);
            SetTextColor(dc, RGB(200, 205, 215));
            DrawTextA(dc, it.help.empty() ? "(no description)" : it.help.c_str(), -1, &rd,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);
        } else {
            SelectObject(dc, fontSmall);
            SetTextColor(dc, RGB(150, 150, 160));
            DrawTextA(dc, "Point at an option to read its full description here.", -1, &rt,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
    vr::IVROverlay* ov = vr::rt::Overlay();
    vr::IVRSystem* sys = vr::rt::System();
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
    vr::IVROverlay* ov = vr::rt::Overlay();
    vr::IVRSystem* sys = vr::rt::System();
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
    vr::rt::Overlay()->SetOverlayRaw(g_overlay, px.data(), kW, kH, 4);
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

// Collaudo senza visore (review 2 WI-1): "vrmenu cycle <chiave> [+1|-1]" fa
// quello che fa un clic sulla voce (LoadIni + Change: esegue e salva
// nell'ini), dal thread del menu (Tick), con una riga sul tipo della voce.
std::mutex g_cycleMutex;
std::vector<std::pair<std::string, int>> g_cycleReq;

void CycleNow(const std::string& keyArg, int dir) {
    LoadIni();
    // "basic:<chiave>" (WI-4a): solo la scheda Basic (gunangle e holdmode sono
    // anche in Advanced, che si prova per prima).
    const bool basicOnly = keyArg.rfind("basic:", 0) == 0;
    const std::string key = basicOnly ? keyArg.substr(6) : keyArg;
    for (int tab = basicOnly ? 0 : 1; tab >= 0; --tab) {
        std::vector<Item>& items = tab == 0 ? g_basic : g_advanced;
        for (Item& it : items) {
            // "basic:gunangle" trova la voce "Gun angle" anche con la presa a calcio (chiave stockangle).
            if (it.key != key && !(basicOnly && key == "gunangle" && it.key == "stockangle")) continue;
            static const char* const kKind[] = {"on/off", "choice", "number", "text"};
            std::string choices;
            for (const std::string& c : it.choices) choices += (choices.empty() ? "" : "|") + c;
            const std::string before = it.value;
            Change(it, dir);
            vr::log::Line(std::string("VRMENU: test cycle ") + it.key + " (" + (tab ? "Advanced" : "Basic") + ", " +
                          kKind[static_cast<int>(it.kind)] + (choices.empty() ? "" : " " + choices) + "): \"" + before +
                          "\" -> \"" + it.value + "\"" + (it.kind == Kind::Text ? " (text: not changeable)" : ""));
            g_dirty = true;
            return;
        }
    }
    vr::log::Line("VRMENU: test cycle " + key + ": not in the menu.");
}

// Collaudo senza visore (review WI-4c round 1): "vrmenu dump <file.bmp> [basic|advanced|controls] [vd|novd]"
// disegna la scheda col renderer vero (Render), con l'avviso di Virtual Desktop
// forzato o tolto, e la salva in BMP; per Controls la riga VRMENU dice la riga
// piu' larga, il fondo dell'ultima e il passo.
void DumpNow(const DumpReq& d) {
    LoadIni();
    const int tab = g_tab, scroll = g_scroll, vd = g_forceVd;
    g_tab = d.tab;
    g_scroll = 0;
    g_forceVd = d.vd;
    g_lastControls = ControlsMetrics();
    const int hx = g_hoverX, hy = g_hoverY;
    if (d.hoverRow >= 0) {
        std::vector<uint32_t> first;
        Render(&first);  // righe della pagina (g_rows)
        if (d.hoverRow < static_cast<int>(g_rows.size())) {
            g_hoverX = g_rows[d.hoverRow].r.left + 10;
            g_hoverY = g_rows[d.hoverRow].r.top + 10;
        }
    }
    std::vector<uint32_t> px;
    const bool ok = Render(&px);
    g_hoverX = hx;
    g_hoverY = hy;
    g_tab = tab;
    g_scroll = scroll;
    g_forceVd = vd;
    g_dirty = true;
    if (!ok) {
        vr::log::Line("VRMENU: dump failed (render).");
        return;
    }
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
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size() * 4);
    // Render da' RGBA (overlay): BMP vuole BGRA.
    for (uint32_t& p : px) p = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
    FILE* f = nullptr;
    if (fopen_s(&f, d.path.c_str(), "wb") != 0 || !f) {
        vr::log::Line("VRMENU: dump failed (cannot write " + d.path + ").");
        return;
    }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px.data(), 4, px.size(), f);
    fclose(f);
    char b[300];
    std::snprintf(b, sizeof(b), "VRMENU: dump tab %d, Virtual Desktop notice %s -> %s", d.tab, d.vd == 1 ? "on" : (d.vd == 0 ? "off" : "as is"),
                  d.path.c_str());
    std::string line(b);
    if (d.tab == 2) {
        std::snprintf(b, sizeof(b), " | Controls: %u lines, row pitch %d px, widest %d of %d px, last row bottom %d of %d px", static_cast<unsigned>(vr::ui::kControlsCount),
                      g_lastControls.pitch, g_lastControls.widest, kW - 60, g_lastControls.bottom, kH);
        line += b;
    }
    vr::log::Line(line);
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
        vr::rt::Overlay()->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_Mouse);
        vr::rt::Overlay()->SetOverlayFlag(g_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        vr::rt::Overlay()->ShowOverlay(g_overlay);
        g_open = true;
        vr::control::g_vrMenuOpen = true;
        vr::log::Line("VRMENU: shown.");
    } else {
        if (vr::rt::Overlay() && g_overlay != vr::k_ulOverlayHandleInvalid) {
            vr::rt::Overlay()->SetOverlayFlag(g_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
            vr::rt::Overlay()->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_None);
            vr::rt::Overlay()->HideOverlay(g_overlay);
        }
        g_open = false;
        vr::control::g_vrMenuOpen = false;
        vr::log::Line("VRMENU: closed.");
    }
}

}  // namespace

void Show(bool show) { g_request = show ? 1 : -1; }
void TestCycle(const std::string& key, int dir) {
    std::lock_guard<std::mutex> lock(g_cycleMutex);
    g_cycleReq.push_back({key, dir < 0 ? -1 : 1});
}
void TestDump(const std::string& path, const std::string& tab, const std::string& vd) {
    std::lock_guard<std::mutex> lock(g_cycleMutex);
    std::string t = tab;
    int hover = -1;
    const size_t at = t.find('@');
    if (at != std::string::npos) {
        hover = std::atoi(t.c_str() + at + 1);
        t = t.substr(0, at);
    }
    g_dumpReq.push_back({path, t == "controls" ? 2 : (t == "advanced" ? 1 : 0), vd == "vd" ? 1 : (vd == "novd" ? 0 : -1), hover});
}
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
    {
        std::vector<std::pair<std::string, int>> cyc;
        {
            std::lock_guard<std::mutex> cl(g_cycleMutex);
            cyc.swap(g_cycleReq);
        }
        for (const auto& c : cyc) CycleNow(c.first, c.second);
        std::vector<DumpReq> dumps;
        {
            std::lock_guard<std::mutex> cl(g_cycleMutex);
            dumps.swap(g_dumpReq);
        }
        for (const DumpReq& d : dumps) DumpNow(d);
    }
    // Apertura automatica: sul primo schermo virtuale (menu del gioco), dopo 2 s.
    int mode = vr::control::g_vrMenu.load();
    // Mai senza visore, ne' col runtime finto (collaudi senza visore, 25/10): il menu
    // aperto rende neutro il pad, anche quello finto ("pad fake") dei collaudi.
    if (!g_open && !g_shownAtStart && mode == 1 && vr::session::Ready() && vr::session::HmdPresent() &&
        vr::rt::Active() != vr::rt::Backend::Fake) {
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
    if (!g_open || !vr::rt::Overlay() || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    // Menu fuori dalla vista (testa girata di oltre 50 gradi per 0,7 s, p.es.
    // visore indossato guardando altrove): lo si rimette davanti.
    {
        vr::IVRSystem* sys = vr::rt::System();
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
    while (vr::rt::Overlay()->PollNextOverlayEvent(g_overlay, &ev, sizeof(ev))) {
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
            const int rowBefore = RowAt(g_hoverX, g_hoverY), rowAfter = RowAt(x, y);
            g_hoverX = x;
            g_hoverY = y;
            if (before != after || rowBefore != rowAfter) g_dirty = true;
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
