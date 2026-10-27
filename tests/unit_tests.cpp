// Test offline delle funzioni pure (R-T2): nessun gioco, nessun visore.
//   cmake --build build --config Release --target unit_tests && build\Release\unit_tests.exe
#include "pure_utils.h"
#include "twohand_math.h"
#include "ui_text.h"
// windows.h (da ui_text.h) definisce far / near vuoti: i test li usano come nomi.
#undef far
#undef near

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_passed;                                                          \
        } else {                                                                 \
            ++g_failed;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

using namespace vr::pure;

static void TestParseVersion() {
    CHECK(ParseVersion("v0.9.9") == (std::vector<int>{0, 9, 9, 1}));
    CHECK(ParseVersion("0.9.9-beta1") == (std::vector<int>{0, 9, 9, 0}));
    CHECK(ParseVersion("v0.9.9") > ParseVersion("v0.9.9-beta1"));
    CHECK(ParseVersion("v0.9.9-beta1") > ParseVersion("v0.9.8"));
    CHECK(ParseVersion("v0.10.0") > ParseVersion("v0.9.9"));
    CHECK(ParseVersion("1.0") == (std::vector<int>{1, 0, 0, 1}));
    CHECK(ParseVersion("") == (std::vector<int>{0, 0, 0, 1}));
    CHECK(ParseVersion("garbage") == (std::vector<int>{0, 0, 0, 1}));
}

static void TestJson() {
    std::string json = "{\"tag_name\": \"v0.9.9\", \"assets\": [{\"name\":\"d3d9.dll\",\"size\":942592,"
                       "\"digest\":\"sha256:abc\",\"browser_download_url\":\"https://x/y/d3d9.dll\"}]}";
    size_t at = 0;
    CHECK(JsonString(json, "tag_name", 0, &at) == "v0.9.9");
    CHECK(at > 0 && json[at] == '"');
    CHECK(JsonString(json, "missing") == "");
    CHECK(JsonString("{\"a\":1}", "a") == "");  // non e' una stringa
    size_t urlAt = 0;
    std::string url = JsonString(json, "browser_download_url", 0, &urlAt);
    CHECK(url == "https://x/y/d3d9.dll");
    CHECK(JsonNumberBefore(json, "size", 0, urlAt) == 942592);
    CHECK(JsonNumberBefore(json, "size", urlAt, json.size()) == -1);
    size_t d = json.rfind("\"digest\"", urlAt);
    CHECK(d != std::string::npos && JsonString(json, "digest", d) == "sha256:abc");
}

static void TestRevoked() {
    std::string m = "{\"revoked\": [\"0.9.1\", \"v0.9.3\"], \"recommended\": \"v0.9.9\"}";
    CHECK(IsRevoked(m, "0.9.1"));
    CHECK(IsRevoked(m, "0.9.3"));
    CHECK(!IsRevoked(m, "0.9.2"));
    CHECK(!IsRevoked("{}", "0.9.1"));
    CHECK(!IsRevoked("{\"revoked\": [", "0.9.1"));
    CHECK(JsonString(m, "recommended") == "v0.9.9");
}

static void TestCompanions() {
    std::string m = "{\"version\": \"0.9.10\", \"files\": ["
                    "{\"name\": \"openvr_api.dll\", \"size\": 431616, \"sha256\": \"aa\"},"
                    "{\"name\": \"specopsvr.ini\", \"size\": 10, \"sha256\": \"bb\"},"
                    "{\"name\": \"..\\\\evil.bat\", \"size\": 10, \"sha256\": \"cc\"},"
                    "{\"name\": \"sub/x.bat\", \"size\": 10, \"sha256\": \"cc\"},"
                    "{\"name\": \"D3D9.dll\", \"size\": 10, \"sha256\": \"dd\"},"
                    "{\"name\": \"nosha.bat\", \"size\": 10},"
                    "{\"name\": \"rollback.bat\", \"size\": 120, \"sha256\": \"ee\"}]}";
    std::vector<Companion> c = ParseCompanions(m);
    CHECK(c.size() == 2);
    CHECK(c.size() == 2 && c[0].name == "openvr_api.dll" && c[0].size == 431616 && c[0].sha256 == "aa");
    CHECK(c.size() == 2 && c[1].name == "rollback.bat" && c[1].size == 120 && c[1].sha256 == "ee");
    CHECK(ParseCompanions("{}").empty());
    CHECK(ParseCompanions("").empty());
    // Ordine dei campi diverso: sha256 prima di size.
    std::vector<Companion> d = ParseCompanions("{\"files\":[{\"sha256\":\"ff\",\"name\":\"a.ps1\",\"size\":5}]}");
    CHECK(d.size() == 1 && d[0].sha256 == "ff" && d[0].size == 5);
}

static void TestDll32() {
    std::string pe(0x400, '\0');
    pe[0] = 'M';
    pe[1] = 'Z';
    *reinterpret_cast<uint32_t*>(&pe[0x3C]) = 0x100;
    pe[0x100] = 'P';
    pe[0x101] = 'E';
    *reinterpret_cast<uint16_t*>(&pe[0x104]) = 0x014C;
    CHECK(IsDll32(pe));
    *reinterpret_cast<uint16_t*>(&pe[0x104]) = 0x8664;  // x64
    CHECK(!IsDll32(pe));
    CHECK(!IsDll32("MZ"));
    CHECK(!IsDll32(std::string(0x400, 'x')));
}

static void TestNormalize() {
    CHECK(NormalizeConfigLine("Spread ON  # commento") == "spread on");
    CHECK(NormalizeConfigLine("hud 0,5 100") == "hud 0.5 100");
    CHECK(NormalizeConfigLine("marker\tFalse") == "marker off");
    CHECK(NormalizeConfigLine("gesture melee YES") == "gesture melee on");
    CHECK(NormalizeConfigLine("# solo commento") == "# solo commento");
    CHECK(NormalizeConfigLine("ucall pc SetCinematicMode 1 0 0 1 1 1") == "ucall pc SetCinematicMode 1 0 0 1 1 1");
    CHECK(NormalizeConfigLine("giveweapon YWeapon_M4Carbine") == "giveweapon YWeapon_M4Carbine");
    CHECK(NormalizeConfigLine("toast Hello World # keep") == "toast Hello World # keep");
    CHECK(NormalizeConfigLine("") == "");
    CHECK(ConfigKey("gesture grenade on") == "gesture grenade");
    CHECK(ConfigKey("dynres min 65") == "dynres min");
    CHECK(ConfigKey("dynres on") == "dynres");
    CHECK(ConfigKey("hud 0.4 100") == "hud");
}

static void TestMigrate() {
    static const ConfigChange kChanges[] = {{2, "submitpose on", "submitpose off"}, {5, "hud 0.6 150", "hud 0.5 100"}};
    std::vector<std::string> lines = {"submitpose on", "hud 0.6 150"};
    std::string done;
    CHECK(MigrateConfigLines(lines, 5, kChanges, 2, &done));
    CHECK(lines[0] == "submitpose off" && lines[1] == "hud 0.5 100");
    CHECK(lines.back() == "configversion 5");
    CHECK(done.find("submitpose") != std::string::npos);
    // Gia' alla 2: il cambio della 2 non si applica piu', quello della 5 si'.
    lines = {"configversion 2", "submitpose on", "hud 0.6 150"};
    CHECK(MigrateConfigLines(lines, 5, kChanges, 2, &done));
    CHECK(lines[1] == "submitpose on" && lines[2] == "hud 0.5 100" && lines[0] == "configversion 5");
    // Gia' aggiornato: niente da fare.
    lines = {"configversion 5", "hud 0.6 150"};
    CHECK(!MigrateConfigLines(lines, 5, kChanges, 2, &done));
    {
        // configversion 10 (26/10): solidhands off -> held, una scelta diversa resta.
        static const ConfigChange kSolid[] = {{10, "solidhands off", "solidhands held"}};
        std::vector<std::string> a = {"solidhands off", "configversion 9"}, b = {"solidhands full", "configversion 9"},
                                 c = {"solidhands off", "configversion 10"};
        CHECK(MigrateConfigLines(a, 10, kSolid, 1, &done) && a[0] == "solidhands held" && a[1] == "configversion 10");
        CHECK(MigrateConfigLines(b, 10, kSolid, 1, &done) && b[0] == "solidhands full");
        CHECK(!MigrateConfigLines(c, 10, kSolid, 1, &done) && c[0] == "solidhands off");
    }
    {
        std::vector<std::string> l = {"# ---- Arms ----", "# Two-handed aiming: ...", "# more", "twohand on 12 0.08", "hand right",
                                      "# left hand", "leftgrip on 20", "configversion 6"};
        const char* const keys[] = {"twohand", "leftgrip"};
        CHECK(ConfigVersionOf(l) == 6);
        CHECK(RemoveRetiredKeys(l, keys, 2) == 2);
        CHECK(l.size() == 3 && l[0] == "# ---- Arms ----" && l[1] == "hand right" && l[2] == "configversion 6");
        CHECK(RemoveRetiredKeys(l, keys, 2) == 0);
    }
    CHECK(lines[1] == "hud 0.6 150");
}

static void TestDefaultLinesMissing() {
    // Ini vecchio senza masskg: nel menu VR arriva la riga dei predefiniti con il suo commento.
    const std::vector<std::string> user = {"\xEF\xBB\xBF# vecchio", "grabprops off", "dynres min 65", "# nota", "configversion 9"};
    const std::string defaults =
        "# Objects\ngrabprops on\n# Weight of objects\n# second line\nmasskg 40\n\ndynres min 60\ndynres budget 9\n# Settings format\n"
        "configversion 9\n";
    std::vector<std::string> out = DefaultLinesMissingFrom(user, defaults);
    CHECK(out.size() == 6);
    CHECK(out.size() == 6 && out[0] == "" && out[1] == "# Weight of objects" && out[2] == "# second line" && out[3] == "masskg 40");
    CHECK(out.size() == 6 && out[4] == "" && out[5] == "dynres budget 9");  // "dynres min" c'e' gia'
    CHECK(DefaultLinesMissingFrom(user, "grabprops on\r\ndynres min 70\r\n").empty());
}

static void TestFitRecommendedSize() {
    uint32_t w = 2112, h = 2304;
    FitRecommendedSize(w, h, 4032);
    CHECK(w == 2112 && h == 2304);  // Quest 3 al 100%: invariata
    w = 4400; h = 4800;  // cursore di SteamVR alto: proporzioni mantenute
    FitRecommendedSize(w, h, 4032);
    CHECK(h == 4032 && w == 3696);
    w = 5000; h = 3000;
    FitRecommendedSize(w, h, 4032);
    CHECK(w == 4032 && h == 2416);
    w = 2115; h = 2301;
    FitRecommendedSize(w, h, 4032);
    CHECK(w == 2112 && h == 2296);
}

// Vibrazione degli urti (voce 3 del piano Boneworks).
static bool Near(float a, float b, float eps = 1e-4f) { return a - b < eps && b - a < eps; }

static void TestHapticMix() {
    // Senza impulso d'urto l'uscita e' quella di prima, per ogni valore del motore del gioco.
    bool same = true;
    for (unsigned game = 0; game <= 65535; ++game)
        for (int gun = 0; gun < 2; ++gun)
            for (int legacy = 0; legacy < 2; ++legacy) {
                unsigned us = 0;  // UpdateHaptics del 24/10
                if (game && gun) us = 600 + game / 20;
                if (legacy) us = 3500;
                HapticFrameOut o = HapticMix(game, gun != 0, legacy != 0, 0.0, 0.9f, 90.0f);
                const bool on = us != 0;
                const float amp = us > 3900 ? 1.0f : us / 3900.0f;
                const unsigned short lus = static_cast<unsigned short>(us > 3900 ? 3900 : us);
                if (o.on != on || o.impact || (on && (o.amp != amp || o.freq != 150.0f || o.seconds != 0.02f || o.us != lus))) same = false;
            }
    CHECK(same);
    // Impulso d'urto da solo: ampiezza, frequenza e durata che resta (al piu' 20 ms).
    HapticFrameOut a = HapticMix(0, false, false, 0.012, 0.62f, 90.0f);
    CHECK(a.on && a.impact && Near(a.amp, 0.62f) && Near(a.freq, 90.0f) && Near(a.seconds, 0.012f) && a.us == 2418);
    a = HapticMix(0, false, false, 0.044, 0.3f, 0.0f);
    CHECK(a.impact && Near(a.seconds, 0.02f) && Near(a.freq, 150.0f));
    // Mano dell'arma: vince il piu' forte tra la vibrazione del gioco e l'impulso.
    a = HapticMix(65535, true, false, 0.03, 0.5f, 225.0f);  // gioco 3876 us = 0,994
    CHECK(a.on && !a.impact && a.us == 3876 && Near(a.freq, 150.0f));
    a = HapticMix(6000, true, false, 0.03, 0.5f, 225.0f);  // gioco 900 us = 0,23
    CHECK(a.impact && Near(a.amp, 0.5f) && Near(a.freq, 225.0f));
    a = HapticMix(0, true, true, 0.03, 0.95f, 225.0f);  // legacy 3500 us = 0,897: piu' debole
    CHECK(a.impact && Near(a.amp, 0.95f));
    a = HapticMix(0, true, true, 0.03, 0.5f, 225.0f);
    CHECK(!a.impact && a.us == 3500);
    a = HapticMix(0, false, false, 0.03, 0.0f, 225.0f);  // ampiezza 0 (impacthaptics off): niente
    CHECK(!a.on);
}

static void TestImpactPulse() {
    // Formula del piano: 0,2 + J/5, 12 + 8 min(J, 4) ms, 90 Hz sopra i 5 kg.
    HapticPulseSpec s = ImpactPulse(2.1f, 8.0f, 1.0f);
    CHECK(Near(s.amp, 0.62f) && s.ms == 29 && Near(s.freq, 90.0f));
    s = ImpactPulse(0.0f, 1.7f, 1.0f);
    CHECK(Near(s.amp, 0.2f) && s.ms == 12 && Near(s.freq, 225.0f));
    s = ImpactPulse(10.0f, 33.0f, 1.0f);
    CHECK(Near(s.amp, 1.0f) && s.ms == 44);
    s = ImpactPulse(2.1f, 8.0f, 0.5f);  // low
    CHECK(Near(s.amp, 0.31f) && s.ms == 29);
    s = ImpactPulse(2.0f, 5.0f, 1.0f);  // 5 kg (la sfera della mano): tic
    CHECK(Near(s.freq, 225.0f) && Near(s.amp, 0.6f));
    CHECK(Near(ReducedMass(5.0f, 5.0f), 2.5f) && ReducedMass(0.0f, 5.0f) == 0.0f);
    // Presa: conferma con la massa (0,6-0,9, revisione 26/10), piu' forte presa al volo.
    s = GrabPulse(1.7f, 0.0f, 1.0f);
    CHECK(Near(s.amp, 0.6f + 0.3f * 1.7f / 30.0f) && s.ms == 31 && Near(s.freq, 225.0f));
    s = GrabPulse(33.0f, 0.0f, 1.0f);
    CHECK(Near(s.amp, 0.9f) && s.ms == 50 && Near(s.freq, 90.0f));
    s = GrabPulse(8.0f, 4.0f, 1.0f);  // mu 3,08 kg x 4 m/s = 12,3 N s: piena
    CHECK(Near(s.amp, 1.0f) && s.ms == 44);
    // low: la parte d'urto a meta' (0,5) sotto la conferma della radio (0,68), che non si dimezza.
    s = GrabPulse(8.0f, 4.0f, 0.5f);
    CHECK(Near(s.amp, 0.6f + 0.3f * 8.0f / 30.0f) && s.ms == 44);
    s = GrabPulse(0.8f, 0.0f, 0.5f);  // bottiglia con low: 0,61, non 0,21
    CHECK(Near(s.amp, 0.6f + 0.3f * 0.8f / 30.0f) && s.ms == 31);
    // Impulso sentito (revisione): massa ridotta oggetto-mano da 5 kg. Radio da 8 kg alla soglia (0,8 m/s): 0,69, non piena;
    // piena solo da 1,56 m/s (J 4). Schiaffo con 2 kg: 0,8 m/s 0,52, pieno a 2 m/s.
    s = ImpactPulse(ReducedMass(8.0f, 5.0f) * 0.8f, 8.0f, 1.0f);
    CHECK(s.amp > 0.68f && s.amp < 0.70f && Near(s.freq, 90.0f));
    s = ImpactPulse(ReducedMass(8.0f, 5.0f) * 1.2f, 8.0f, 1.0f);
    CHECK(s.amp > 0.93f && s.amp < 0.95f);
    s = ImpactPulse(2.0f * 0.8f, 5.0f, 1.0f);
    CHECK(s.amp > 0.51f && s.amp < 0.53f && Near(s.freq, 225.0f));
    s = ImpactPulse(2.0f * 2.0f, 5.0f, 1.0f);
    CHECK(Near(s.amp, 1.0f) && s.ms == 44);
}

// Un passo del rilevatore: dv verso il basso (m/s), velocita' misurata (m/s).
// speed = velocita' misurata lungo +z (verso l'ostacolo: dv positivo = tolta lungo +z).
static bool EdgeStep(ImpactEdge& e, float dvz, float speed, double t, bool quiet = false, bool* muted = nullptr, float* lost = nullptr,
                     double dt = 1.0 / 144.0) {
    const float d[3] = {0.0f, 0.0f, dvz}, v[3] = {0.0f, 0.0f, speed};
    return e.Step(d, v, t, dt, ImpactEdgeTune(), quiet, muted, lost);
}

static void TestImpactEdge() {
    const double dt = 1.0 / 144.0;
    double t = 0.0;
    int fired = 0;
    ImpactEdge e;
    // 10 s fermo in aria (rumore 0,02 m/s, come misurato): niente.
    for (int i = 0; i < 1440; ++i, t += dt) fired += EdgeStep(e, 0.02f, 0.0f, t);
    CHECK(fired == 0);
    // Giu' a 1,5 m/s, urto secco (1,26 m/s in un tick), poi premuto contro il terreno (0,54 m/s per tick) per 1 s: un impulso.
    for (int i = 0; i < 100; ++i, t += dt) fired += EdgeStep(e, 0.0f, 1.5f, t);
    float lost = 0.0f;
    fired += EdgeStep(e, 1.26f, 0.2f, t, false, nullptr, &lost);
    t += dt;
    CHECK(fired == 1 && lost > 1.25f && lost < 1.27f);
    for (int i = 0; i < 144; ++i, t += dt) fired += EdgeStep(e, i % 2 ? 0.54f : 0.62f, 0.0f, t);
    CHECK(fired == 1);
    // Su di nuovo (staccato 20 ms: meno di rearm) e giu' subito: niente.
    for (int i = 0; i < 3; ++i, t += dt) fired += EdgeStep(e, 0.0f, 1.5f, t);
    fired += EdgeStep(e, 1.3f, 0.2f, t);
    t += dt;
    CHECK(fired == 1);
    // Staccato 0,1 s, spigolo sulla sabbia (contatto 0 del collaudo h3: 0,42-0,75 m/s per tick da 1,5 m/s): scatta al secondo tick.
    for (int i = 0; i < 15; ++i, t += dt) fired += EdgeStep(e, 0.0f, 1.5f, t);
    const float corner[] = {0.42f, 0.45f, 0.45f, 0.47f, 0.46f, 0.53f, 0.7f};
    int at = -1;
    for (int i = 0; i < 7; ++i, t += dt) {
        if (EdgeStep(e, corner[i], 1.5f - 0.2f * i, t, false, nullptr, &lost)) {
            ++fired;
            if (at < 0) at = i;
        }
    }
    CHECK(fired == 2 && at == 1 && lost > 0.86f && lost < 0.88f);
    // Oggetto fermo premuto piu' forte contro il tavolo (dv 0,6-1,5 per tick, velocita' prima 0): mai.
    for (int i = 0; i < 15; ++i, t += dt) fired += EdgeStep(e, 0.0f, 0.0f, t);
    for (int i = 0; i < 144; ++i, t += dt) fired += EdgeStep(e, 0.6f + 0.006f * i, 0.0f, t);
    CHECK(fired == 2);
    // Urto nella finestra muta (salto, assestamento): niente, e niente dopo finche' resta contro.
    for (int i = 0; i < 15; ++i, t += dt) fired += EdgeStep(e, 0.0f, 2.0f, t);
    bool muted = false;
    fired += EdgeStep(e, 2.0f, 0.0f, t, true, &muted);
    t += dt;
    CHECK(muted);
    for (int i = 0; i < 20; ++i, t += dt) fired += EdgeStep(e, 1.2f, 0.0f, t);
    CHECK(fired == 2);
    // Trascinata su un'altra radio (collaudo w5): l'attrito toglie 0,18-0,48 m/s per tick in crescita lenta, a 1,2 m/s: niente.
    ImpactEdge gr;
    int fg = 0;
    for (int i = 0; i < 15; ++i, t += dt) fg += EdgeStep(gr, 0.02f, 1.2f, t);
    const float grind[] = {0.18f, 0.28f, 0.36f, 0.39f, 0.43f, 0.48f, 0.47f, 0.45f, 0.5f, 0.46f, 0.44f, 0.4f};
    for (float x : grind) {
        fg += EdgeStep(gr, x, 1.2f, t);
        t += dt;
    }
    CHECK(fg == 0);
    // ... e un urto vero mentre struscia (da 0,4 a 1,5 m/s in un tick): scatta.
    fg += EdgeStep(gr, 1.5f, 0.1f, t);
    t += dt;
    CHECK(fg == 1);
    // Vibrazione contro un ostacolo (dv alterno +-1 m/s, oggetto quasi fermo): niente.
    ImpactEdge v;
    int fv = 0;
    for (int i = 0; i < 288; ++i, t += dt) {
        const float d[3] = {i % 2 ? 1.0f : -1.0f, 0.0f, 0.0f}, w[3] = {0.3f, 0.0f, 0.0f};
        fv += v.Step(d, w, t, dt, ImpactEdgeTune(), false);
    }
    CHECK(fv <= 1);
    // objectweight off, radio bloccata sul terreno (F2_radio_off): 3 tick in su a 1,2 m/s, uno in giu' a 6,8, poi
    // fermata (6,8 m/s tolti), a ripetizione: mai (si avvicinava al terreno da un tick solo).
    ImpactEdge j;
    int fj = 0;
    for (int i = 0; i < 15; ++i, t += dt) fj += EdgeStep(j, 0.0f, 0.0f, t);
    for (int r = 0; r < 10; ++r) {
        for (int i = 0; i < 3; ++i, t += dt) fj += EdgeStep(j, 0.0f, -1.2f, t);
        fj += EdgeStep(j, 0.0f, 6.8f, t);
        t += dt;
        fj += EdgeStep(j, 6.8f, 0.0f, t);
        t += dt;
        for (int i = 0; i < 6; ++i, t += dt) fj += EdgeStep(j, 0.0f, 0.0f, t);
    }
    CHECK(fj == 0);
    // Urto vero di lato mentre l'oggetto va giu': conta solo la velocita' verso l'ostacolo (x), 1,2 m/s.
    ImpactEdge sx;
    for (int i = 0; i < 15; ++i, t += dt) {
        const float d0[3] = {0, 0, 0}, w0[3] = {1.2f, 0.0f, -1.5f};
        sx.Step(d0, w0, t, dt, ImpactEdgeTune(), false);
    }
    const float dh[3] = {1.2f, 0.0f, 0.0f}, wh[3] = {0.0f, 0.0f, -1.5f};
    float lx = 0.0f;
    CHECK(sx.Step(dh, wh, t, dt, ImpactEdgeTune(), false, nullptr, &lx) && lx > 1.19f && lx < 1.21f);
}

// Revisione (26/10): gli stessi contatti fisici a 45, 90, 120, 144 e 225 Hz. Perdita continua = a x dt per tick
// (premere, attrito, contatto morbido), urto secco = la stessa velocita' in un tick a ogni frequenza.
static void TestImpactEdgeRates() {
    const double rates[] = {45.0, 90.0, 120.0, 144.0, 225.0};
    for (double hz : rates) {
        const double dt = 1.0 / hz;
        const float k = static_cast<float>(dt * 144.0);  // perdita continua per tick rispetto a 144 Hz
        double t = 0.0;
        // 2 s fermo in aria (0,02 m/s): niente.
        ImpactEdge a;
        int fa = 0;
        for (int i = 0; i < static_cast<int>(2.0 * hz); ++i, t += dt) fa += EdgeStep(a, 0.02f, 0.0f, t, false, nullptr, nullptr, dt);
        CHECK(fa == 0);
        // Urto secco da 1,5 m/s (1,3 tolti in un tick), poi premuto 1 s (0,45 m/s per tick a 144 Hz): un impulso.
        for (int i = 0; i < static_cast<int>(0.2 * hz); ++i, t += dt) fa += EdgeStep(a, 0.0f, 1.5f, t, false, nullptr, nullptr, dt);
        float lost = 0.0f;
        fa += EdgeStep(a, 1.3f, 0.2f, t, false, nullptr, &lost, dt);
        t += dt;
        for (int i = 0; i < static_cast<int>(hz); ++i, t += dt) fa += EdgeStep(a, 0.45f * k, 0.0f, t, false, nullptr, nullptr, dt);
        CHECK(fa == 1 && lost > 1.29f && lost < 1.31f);
        // Contatto morbido da 1,22 m/s (B_drag144, 120 Hz: 0,36 poi 0,59 per tick da 8 ms, la radio a 0 in 100 ms):
        // a = 70 m/s^2, il primo tick a meta'. Prima (raddoppio richiesto) non vibrava; ora una volta a ogni frequenza.
        ImpactEdge c;
        int fc = 0;
        for (int i = 0; i < static_cast<int>(0.2 * hz); ++i, t += dt) fc += EdgeStep(c, 0.0f, 1.22f, t, false, nullptr, nullptr, dt);
        float v = 1.22f;
        for (int i = 0; i < static_cast<int>(0.3 * hz); ++i, t += dt) {
            const float d = (i == 0 ? 0.5f : 1.0f) * 70.0f * static_cast<float>(dt);
            v = v - 12.2f * static_cast<float>(dt) > 0.0f ? v - 12.2f * static_cast<float>(dt) : 0.0f;
            fc += EdgeStep(c, d + (v > 0.0f ? 0.0f : 0.45f * k), v, t, false, nullptr, nullptr, dt);
        }
        CHECK(fc == 1);
        if (fc != 1) std::printf("  soft contact at %.0f Hz: %d pulses\n", hz, fc);
        // Spigolo sulla sabbia (h3: 1,5 m/s via in 8 tick da 0,42-0,75 a 144 Hz, ~65 m/s^2) da calmo: una volta.
        ImpactEdge s;
        int fs = 0;
        for (int i = 0; i < static_cast<int>(0.2 * hz); ++i, t += dt) fs += EdgeStep(s, 0.0f, 1.5f, t, false, nullptr, nullptr, dt);
        for (int i = 0; i < static_cast<int>(0.055 * hz) + 1; ++i, t += dt) fs += EdgeStep(s, 0.5f * k, 1.2f, t, false, nullptr, nullptr, dt);
        CHECK(fs == 1);
        // Trascinata sulla sabbia (B_drag144 a 120 Hz: premuta e trascinata a 1 m/s, 0,50-0,54 m/s per tick con punte
        // a 0,9, gia' in corso) e su un'altra radio (w5: l'attrito cresce 0,18-0,5 in 12 tick): mai, a nessuna frequenza.
        ImpactEdge g;
        int fg = 0;
        for (int i = 0; i < static_cast<int>(0.2 * hz); ++i, t += dt) fg += EdgeStep(g, 0.5f * k, 1.0f, t, false, nullptr, nullptr, dt);
        for (int i = 0; i < static_cast<int>(1.0 * hz); ++i, t += dt) {
            const float f = i % 30 == 29 ? 0.9f : 0.5f + 0.04f * static_cast<float>((i * 7) % 10) / 9.0f;
            fg += EdgeStep(g, f * k, 1.0f, t, false, nullptr, nullptr, dt);
        }
        ImpactEdge w;
        for (int i = 0; i < static_cast<int>(0.2 * hz); ++i, t += dt) fg += EdgeStep(w, 0.02f * k, 1.2f, t, false, nullptr, nullptr, dt);
        const float grind[] = {0.18f, 0.28f, 0.36f, 0.39f, 0.43f, 0.48f, 0.47f, 0.45f, 0.5f, 0.46f, 0.44f, 0.4f};
        for (int i = 0; i < static_cast<int>(12.0 * hz / 144.0) + 1; ++i, t += dt) {
            // la stessa rampa nel tempo: a 45 Hz un tick copre 3 tick a 144 Hz (media)
            const double a0 = i * dt * 144.0, a1 = (i + 1) * dt * 144.0;
            float acc = 0.0f;
            int n = 0;
            for (int j = static_cast<int>(a0); j < static_cast<int>(std::ceil(a1)) && j < 12; ++j, ++n) acc += grind[j];
            fg += EdgeStep(w, (n ? acc / n : 0.4f) * k, 1.2f, t, false, nullptr, nullptr, dt);
        }
        // Sotto i 120 Hz l'inizio della rampa (da calmo, 0,55 m/s nel secondo tick a 90 Hz) e' un contatto: un impulso
        // all'inizio, mai durante il trascinamento.
        CHECK(fg == 0 || (fg == 1 && hz < 100.0));
        if (fg != 0) std::printf("  drag at %.0f Hz: %d pulses\n", hz, fg);
        // Storia dell'avvicinamento a tempo: dopo 30 ms in su a 1 m/s e un tick giu' a 3 m/s, fermata (3 m/s tolti).
        // A ogni frequenza i tick in su (entro 25 ms) entrano nella minima: niente (prima, 3 tick: a 45 Hz 67 ms).
        ImpactEdge u;
        int fu = 0;
        for (int i = 0; i < static_cast<int>(0.2 * hz); ++i, t += dt) fu += EdgeStep(u, 0.0f, 0.0f, t, false, nullptr, nullptr, dt);
        for (int i = 0; i < static_cast<int>(0.03 * hz) + 1; ++i, t += dt) fu += EdgeStep(u, 0.0f, -1.0f, t, false, nullptr, nullptr, dt);
        fu += EdgeStep(u, 0.0f, 3.0f, t, false, nullptr, nullptr, dt);
        t += dt;
        fu += EdgeStep(u, 3.0f, 0.0f, t, false, nullptr, nullptr, dt);
        t += dt;
        CHECK(fu == 0);  // a 45 Hz i 2 tick prima (44 ms): il tick in su c'e'
    }
}

// Lancio (voce 4): storia della mano a 144 Hz, piu' recente per prima.
static std::vector<ThrowSample> ThrowRamp(double vUpMs, double tUp, double vEndMs, double tDown, double hz, double wz) {
    // Velocita' lungo x: 0 -> vUpMs in tUp s, poi -> vEndMs in tDown s (m/s; 50 unita' per m). Assi che ruotano
    // attorno a z a wz rad/s.
    std::vector<ThrowSample> out;
    const double T = tUp + tDown, dt = 1.0 / hz;
    double x = 0.0;
    double prevV = 0.0;
    for (double t = 0.0; t <= T + 1e-9; t += dt) {
        const double vv = t < tUp ? vUpMs * t / tUp : vUpMs + (vEndMs - vUpMs) * (t - tUp) / tDown;
        if (t > 0.0) x += 0.5 * (vv + prevV) * dt * 50.0;
        prevV = vv;
        ThrowSample s = {};
        s.p[0] = static_cast<float>(x);
        const float a = static_cast<float>(wz * t);
        const float ca = std::cos(a), sa = std::sin(a);
        const float ax[3][3] = {{ca, sa, 0.0f}, {-sa, ca, 0.0f}, {0.0f, 0.0f, 1.0f}};
        for (int i = 0; i < 3; ++i)
            for (int c = 0; c < 3; ++c) s.ax[i][c] = ax[i][c];
        s.t = 100.0 + t;
        out.push_back(s);
    }
    std::vector<ThrowSample> r(out.rbegin(), out.rend());
    if (r.size() > 16) r.resize(16);
    return r;
}

static void TestPeakThrow() {
    const float zero[3] = {0, 0, 0};
    // Rampa del piano: picco a 4 m/s, 2 m/s al rilascio 60 ms dopo -> 4 +- 0,3 (la media di 60 ms darebbe ~3);
    // a 144 Hz senza rumore 1:2:1 da' 3,85 (120 Hz 3,83, 90 Hz 3,75: il picco cade tra due campioni). Un picco
    // pulito non passa per la mediana (con la sola mediana: 3,80, a 90 Hz 3,70).
    std::vector<ThrowSample> r = ThrowRamp(4.0, 0.15, 2.0, 0.06, 144.0, 0.0);
    PeakThrowOut o = PeakThrow(r.data(), static_cast<int>(r.size()), 0.11, 0.006, 600.0f, zero);
    const float sp = std::sqrt(o.v[0] * o.v[0] + o.v[1] * o.v[1] + o.v[2] * o.v[2]) / 50.0f;
    CHECK(o.ok && sp > 3.8f && sp < 4.0f);
    // A 90 Hz (un campione = 11 ms): ancora entro 4 +- 0,3 (nel gioco a 120 Hz: 3,83 in 6 lanci su 6).
    {
        std::vector<ThrowSample> r90 = ThrowRamp(4.0, 0.15, 2.0, 0.06, 90.0, 0.0);
        PeakThrowOut o90 = PeakThrow(r90.data(), static_cast<int>(r90.size()), 0.11, 0.006, 600.0f, zero);
        const float s90 = std::sqrt(o90.v[0] * o90.v[0] + o90.v[1] * o90.v[1]) / 50.0f;
        CHECK(o90.ok && s90 > 3.7f && s90 < 4.0f);
    }
    CHECK(o.peakAge > 0.04 && o.peakAge < 0.075);
    CHECK(o.nV == 3 && o.rejected == 0);
    CHECK(o.replaced == 0 && o.peakSpeed <= o.rawSpeed);  // un picco pulito non e' toccato dalla mediana
    // Al rilascio (okAvg): la stessa media di classic.
    CHECK(o.okAvg && o.vAvg[0] / 50.0f > 2.8f && o.vAvg[0] / 50.0f < 3.2f);
    // Media "classic" sugli ultimi 60 ms per confronto: ~3 m/s.
    {
        int k = 0;
        while (k + 1 < static_cast<int>(r.size()) && r[0].t - r[k].t < 0.06) ++k;
        const float cl = static_cast<float>((r[0].p[0] - r[k].p[0]) / (r[0].t - r[k].t)) / 50.0f;
        CHECK(cl > 2.8f && cl < 3.2f);
    }
    // Polso a 10 rad/s attorno a z, mano ferma: w = (0, 0, 10) +- 0,1.
    std::vector<ThrowSample> rot = ThrowRamp(0.0, 0.15, 0.0, 0.06, 144.0, 10.0);
    PeakThrowOut ow = PeakThrow(rot.data(), static_cast<int>(rot.size()), 0.11, 0.006, 600.0f, zero);
    CHECK(ow.ok && ow.okW && std::fabs(ow.w[2] - 10.0f) < 0.1f && std::fabs(ow.w[0]) < 0.01f && std::fabs(ow.w[1]) < 0.01f);
    CHECK(ow.okWAvg && std::fabs(ow.wAvg[2] - 10.0f) < 0.1f);  // anche al rilascio (posando: la rotazione di adesso)
    // Tick piu' fitti della posa (225 tick/s, mano a 144 Hz), con minDt 6 ms (il predefinito, griptune peakdt):
    // - come nel gioco (headsample draw): ogni campione ha l'ora della sua posa (g_poseStampMs), due tick con la
    //   stessa posa hanno la stessa ora: 3 m/s costanti -> 3 +- 0,02;
    // - con l'ora del tick (headsample render, o senza ora): una velocita' su 8,9 ms copre una o due pose nuove
    //   (2,3 o 4,7 m/s a 72/28%): i 4,7 isolati prendono la mediana, resta 2,34 (-22%; prima, media 1:2:1 sul
    //   4,7 grezzo, 3,5: +17%). Solo con pose a scatti e ora del tick (synthsmooth off senza visore); nel gioco
    //   ogni tick ha l'ora della sua posa.
    for (int stamped = 1; stamped >= 0; --stamped) {
        std::vector<ThrowSample> st;
        for (int i = 15; i >= 0; --i) {
            ThrowSample s = {};
            const double t = i / 225.0;
            const double tq = std::floor(t * 144.0) / 144.0;  // ultima posa della mano a 144 Hz
            s.p[0] = static_cast<float>(3.0 * 50.0 * tq);
            s.ax[0][0] = s.ax[1][1] = s.ax[2][2] = 1.0f;
            s.t = 50.0 + (stamped ? tq : t);
            st.push_back(s);
        }
        PeakThrowOut os = PeakThrow(st.data(), 16, 0.11, 0.006, 600.0f, zero);
        if (stamped) CHECK(os.ok && std::fabs(os.v[0] / 50.0f - 3.0f) < 0.02f);
        else std::printf("  dense ticks, tick stamps, minDt 6 ms: %.2f m/s for a 3 m/s hand\n", os.v[0] / 50.0f);
        if (!stamped) CHECK(os.ok && os.v[0] / 50.0f > 2.3f && os.v[0] / 50.0f < 3.5f);
    }
    // Salto del tracciamento (50 cm in un tick) nel mezzo di un lancio a 3 m/s: scartato, picco ~3 m/s.
    {
        std::vector<ThrowSample> j = ThrowRamp(3.0, 0.05, 3.0, 0.1, 144.0, 0.0);
        for (int k = 0; k <= 5; ++k) j[k].p[0] += 25.0f;  // dal campione 5 in poi (piu' recenti) +25 unita'
        PeakThrowOut oj = PeakThrow(j.data(), static_cast<int>(j.size()), 0.11, 0.006, 600.0f, zero);
        CHECK(oj.ok && oj.rejected >= 1 && oj.v[0] / 50.0f > 2.9f && oj.v[0] / 50.0f < 3.1f);
    }
    // Di corsa (Walker a 6 m/s): la mano a 14 m/s nel mondo non e' un salto rispetto a Walker.
    {
        std::vector<ThrowSample> run = ThrowRamp(14.0, 0.01, 14.0, 0.2, 144.0, 0.0);
        const float ref[3] = {300.0f, 0.0f, 0.0f};
        PeakThrowOut orr = PeakThrow(run.data(), static_cast<int>(run.size()), 0.11, 0.006, 600.0f, ref);
        CHECK(orr.ok && orr.rejected == 0 && orr.v[0] / 50.0f > 13.5f);
        PeakThrowOut o0 = PeakThrow(run.data(), static_cast<int>(run.size()), 0.11, 0.006, 600.0f, zero);
        CHECK(!o0.ok && o0.rejected > 0);
    }
    // Polso girato di 80 gradi in un tick (synthleftrot, controller ritrovato): non e' una rotazione del lancio.
    {
        std::vector<ThrowSample> jw = ThrowRamp(0.0, 0.15, 0.0, 0.06, 120.0, 0.0);
        const float a = 80.0f * 0.0174533f, ca = std::cos(a), sa = std::sin(a);
        const float ax[3][3] = {{ca, sa, 0.0f}, {-sa, ca, 0.0f}, {0.0f, 0.0f, 1.0f}};
        for (int i = 0; i < 3; ++i)
            for (int c = 0; c < 3; ++c) jw[0].ax[i][c] = ax[i][c];
        PeakThrowOut oj = PeakThrow(jw.data(), static_cast<int>(jw.size()), 0.11, 0.006, 600.0f, zero);
        CHECK(oj.ok && oj.rejectedW == 1 && std::fabs(oj.w[2]) < 0.01f);
        // ...nemmeno al rilascio (posando, revisione 26/10): come differenza tra gli estremi della finestra di 60 ms
        // leggeva ~25 rad/s (sotto i 50 del salto) e la leva lanciava la radio di regrab a 5 m/s.
        CHECK(oj.okWAvg && std::fabs(oj.wAvg[0]) + std::fabs(oj.wAvg[1]) + std::fabs(oj.wAvg[2]) < 0.01f);
        // Il polso a 30 rad/s resta (sotto i 50 del salto).
        std::vector<ThrowSample> w30 = ThrowRamp(0.0, 0.15, 0.0, 0.06, 120.0, 30.0);
        PeakThrowOut o30 = PeakThrow(w30.data(), static_cast<int>(w30.size()), 0.11, 0.006, 600.0f, zero);
        CHECK(o30.okW && o30.rejectedW == 0 && std::fabs(o30.w[2] - 30.0f) < 0.3f);
    }
    // Storia troppo corta o interrotta: niente stima.
    CHECK(!PeakThrow(r.data(), 1, 0.11, 0.006, 600.0f, zero).ok);
    // Revisione 26/10: un campione sbagliato sotto i 12 m/s (controller che torna in vista con 5 cm di scatto, a
    // 90 Hz ~4,5 m/s per un frame) non fa il picco: ne' come scalino ne' come andata e ritorno. Con la media 1:2:1
    // di prima il picco era ~2,25 m/s a mano ferma.
    for (int back = 0; back <= 1; ++back) {
        std::vector<ThrowSample> sj = ThrowRamp(0.0, 0.15, 0.0, 0.06, 90.0, 0.0);
        for (int k = 0; k <= 3; ++k)
            if (!back || k == 3) sj[k].p[0] += 2.5f;  // scalino dal campione 3 in poi, o solo il campione 3
        PeakThrowOut os = PeakThrow(sj.data(), static_cast<int>(sj.size()), 0.11, 0.006, 600.0f, zero);
        CHECK(os.ok && os.rawSpeed / 50.0f > 4.0f && os.replaced >= 1 && os.peakSpeed / 50.0f < 0.05f);
        CHECK(os.okAvg && std::fabs(os.vAvg[0]) / 50.0f < 0.05f);  // anche la media al rilascio (posando)
    }
    // ...e nel mezzo di un lancio non nasconde il picco vero: rampa del piano con un campione a +4 cm nella frenata.
    {
        std::vector<ThrowSample> rg = ThrowRamp(4.0, 0.15, 2.0, 0.06, 144.0, 0.0);
        rg[3].p[0] += 2.0f;
        PeakThrowOut og = PeakThrow(rg.data(), static_cast<int>(rg.size()), 0.11, 0.006, 600.0f, zero);
        CHECK(og.ok && og.rawSpeed / 50.0f > 5.0f && og.replaced >= 1 && og.peakSpeed / 50.0f > 3.8f && og.peakSpeed / 50.0f < 4.0f);
    }
    // Oggetto posato: mano da 0,6 a 0,1 m/s in 100 ms. Picco ~0,6 -> peso 0 (sotto peakmin 1 m/s): parte con la
    // media al rilascio, ~0,22 m/s (col solo picco 0,6, x throwboost).
    {
        std::vector<ThrowSample> pl = ThrowRamp(0.6, 0.05, 0.1, 0.1, 144.0, 0.0);
        PeakThrowOut op = PeakThrow(pl.data(), static_cast<int>(pl.size()), 0.11, 0.006, 600.0f, zero);
        CHECK(op.ok && op.okAvg && op.peakSpeed / 50.0f > 0.55f && op.peakSpeed / 50.0f < 0.62f);
        CHECK(ThrowPeakBlend(op.peakSpeed / 50.0f, 1.0f, 2.0f) == 0.0f);
        CHECK(op.vAvg[0] / 50.0f > 0.18f && op.vAvg[0] / 50.0f < 0.26f);
    }
    // Passaggio morbido tra posare e lanciare: 0 fino a 1 m/s, 1 da 2, smoothstep, monotono.
    CHECK(ThrowPeakBlend(0.5f, 1.0f, 2.0f) == 0.0f && ThrowPeakBlend(1.0f, 1.0f, 2.0f) == 0.0f);
    CHECK(std::fabs(ThrowPeakBlend(1.5f, 1.0f, 2.0f) - 0.5f) < 1e-6f && ThrowPeakBlend(2.0f, 1.0f, 2.0f) == 1.0f);
    CHECK(ThrowPeakBlend(3.0f, 1.0f, 2.0f) == 1.0f && ThrowPeakBlend(1.2f, 1.0f, 1.0f) == 1.0f);
    {
        float prev = 0.0f;
        bool mono = true;
        for (int i = 0; i <= 300; ++i) {
            const float bl = ThrowPeakBlend(i * 0.01f, 1.0f, 2.0f);
            mono = mono && bl >= prev;
            prev = bl;
        }
        CHECK(mono);
    }
    // Limite di energia (85 J, revisione 26/10): barile da 33,2 kg (masskg 40) <= 2,3 m/s; 52 kg 1,8; bottiglia
    // 1,7 kg 10; 150 J (il piano) dava 3,0 al barile.
    CHECK(ThrowEnergyCap(33.2f, 85.0f) <= 2.3f && std::fabs(ThrowEnergyCap(33.2f, 85.0f) - 2.263f) < 0.01f);
    CHECK(std::fabs(ThrowEnergyCap(52.0f, 85.0f) - 1.808f) < 0.01f);
    CHECK(std::fabs(ThrowEnergyCap(1.7f, 85.0f) - 10.0f) < 0.02f);
    CHECK(std::fabs(ThrowEnergyCap(33.2f, 150.0f) - 3.006f) < 0.01f);
    CHECK(ThrowEnergyCap(10.0f, 0.0f) > 1e8f);
}

// Voce 5 (solidhands): zona morta morbida, tetto e filtro della mano disegnata.
static void TestSolidShape() {
    CHECK(SolidShape(0.0f, 2.5f, 25.0f) == 0.0f);
    CHECK(SolidShape(-1.0f, 2.5f, 25.0f) == 0.0f);
    // 1 mm di tremolio quasi sparisce; meta' zona morta = meta' dell'ampiezza (smoothstep 0,5).
    CHECK(SolidShape(0.1f, 2.5f, 25.0f) < 0.0005f);
    CHECK(std::fabs(SolidShape(1.25f, 2.5f, 25.0f) - 0.625f) < 1e-4f);
    // Dal bordo in su intero, senza scalino; tetto.
    CHECK(std::fabs(SolidShape(2.5f, 2.5f, 25.0f) - 2.5f) < 1e-5f);
    CHECK(std::fabs(SolidShape(2.4999f, 2.5f, 25.0f) - 2.4999f) < 1e-3f);
    CHECK(std::fabs(SolidShape(10.0f, 2.5f, 25.0f) - 10.0f) < 1e-5f);
    CHECK(SolidShape(40.0f, 2.5f, 25.0f) == 25.0f);
    // Monotona (niente inversioni del verso della mano).
    float prev = 0.0f;
    bool mono = true;
    for (int i = 1; i < 400; ++i) {
        const float v = SolidShape(i * 0.1f, 2.5f, 25.0f);
        mono = mono && v >= prev;
        prev = v;
    }
    CHECK(mono);
    CHECK(std::fabs(SolidShape(1.0f, 0.0f, 25.0f) - 1.0f) < 1e-6f);  // senza zona morta
    // Filtro: in 40 ms con tau 40 ms il 63%; in uscita (100 ms) piu' lento.
    CHECK(std::fabs(SolidFilterAlpha(0.04f, true, 40.0f, 100.0f) - 0.632f) < 0.002f);
    CHECK(SolidFilterAlpha(0.007f, false, 40.0f, 100.0f) < SolidFilterAlpha(0.007f, true, 40.0f, 100.0f));
    CHECK(SolidFilterAlpha(0.0f, true, 40.0f, 100.0f) == 0.0f);
    CHECK(SolidFilterAlpha(0.01f, true, 0.0f, 100.0f) == 1.0f);
    // Portata del braccio (revisione 26/10): spalla all'origine, portata 30.
    {
        const float sh[3] = {0, 0, 0};
        // Bersaglio dentro, polso dentro: niente.
        const float t1[3] = {20, 0, 0};
        float p[3] = {20, 0, -5};
        CHECK(ReachClamp(sh, t1, 30.0f, p) == 0.0f && p[2] == -5.0f);
        // Bersaglio sul bordo (braccio teso), abbassamento di 5 di lato: resta
        // quasi tutto (prima spariva). Tolta solo la parte che allunga il braccio.
        const float t2[3] = {30, 0, 0};
        float q[3] = {30, 0, -5};
        const float cut = ReachClamp(sh, t2, 30.0f, q);
        CHECK(cut > 0.4f && cut < 0.42f);  // sqrt(925) - 30 = 0,414
        CHECK(std::fabs(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) - 30.0f) < 1e-3f);
        CHECK(q[2] < -4.9f);
        // Bersaglio oltre la portata: limite = la sua distanza, non la portata.
        const float t3[3] = {40, 0, 0};
        float r[3] = {45, 0, 0};
        CHECK(std::fabs(ReachClamp(sh, t3, 30.0f, r) - 5.0f) < 1e-4f && std::fabs(r[0] - 40.0f) < 1e-4f);
        // Continuita': il bersaglio che scorre sul bordo (tremolio del
        // controller) sposta il polso disegnato circa quanto si muove lui.
        float prev[3] = {0, 0, 0};
        float maxStep = 0.0f;
        for (int i = 0; i <= 200; ++i) {
            const float tx = 29.0f + i * 0.01f;
            const float tt[3] = {tx, 0, 0};
            float w[3] = {tx, 0, -5};
            ReachClamp(sh, tt, 30.0f, w);
            if (i) {
                const float d = std::sqrt((w[0] - prev[0]) * (w[0] - prev[0]) + (w[2] - prev[2]) * (w[2] - prev[2]));
                maxStep = d > maxStep ? d : maxStep;
            }
            std::memcpy(prev, w, sizeof(prev));
        }
        CHECK(maxStep < 0.011f);  // passo 0,01: al piu' 0,0101 (prima: 5 di colpo)
        CHECK(ReachClamp(sh, t2, 0.0f, q) == 0.0f);  // portata sconosciuta: niente
    }
}

// Forza della presa (voce 6, "analoggrip"; revisione 26/10).
static void TestAnalogGrip() {
    const float nan = std::nanf("");
    // Asse del controller: grip solo a tasto -> 1 (presa stretta); senza tasto 0.
    CHECK(GripAxisOf(0.0f, 0.0f, true) == 1.0f);
    CHECK(GripAxisOf(0.0f, 0.0f, false) == 0.0f);
    CHECK(GripAxisOf(0.03f, 0.0f, true) == 1.0f);
    // OpenXR: il tasto scatta a 0,5, il valore resta quello dell'asse.
    CHECK(GripAxisOf(0.55f, 0.0f, true) == 0.55f);
    CHECK(GripAxisOf(0.0f, 0.8f, true) == 0.8f);  // binding legacy dei Touch: in y
    // NaN e fuori scala.
    CHECK(GripAxisOf(nan, nan, true) == 1.0f);
    CHECK(GripAxisOf(nan, nan, false) == 0.0f);
    CHECK(GripAxisOf(nan, 0.7f, true) == 0.7f);
    CHECK(GripAxisOf(0.7f, nan, true) == 0.7f);
    CHECK(GripAxisOf(1.3f, 0.0f, false) == 1.0f);
    CHECK(GripAxisOf(-0.2f, 0.0f, true) == 1.0f);
    // Valore per la forza: senza grip premuto (grabsim) 1; asse fermo o NaN 1.
    CHECK(GripHeldValue(false, 0.3f) == 1.0f);
    CHECK(GripHeldValue(true, 0.55f) == 0.55f);
    CHECK(GripHeldValue(true, 0.0f) == 1.0f);
    CHECK(GripHeldValue(true, nan) == 1.0f);
    CHECK(GripHeldValue(true, 1.2f) == 1.0f);
    // Curva: 0,4 fino a 0,6 (0,5-0,6 sotto OpenXR = la presa piu' lenta), 1 da 0,9.
    CHECK(GripFactorOf(0.55f, 0.4f, 0.6f, 0.9f) == 0.4f);
    CHECK(GripFactorOf(0.5f, 0.4f, 0.6f, 0.9f) == 0.4f);
    CHECK(std::fabs(GripFactorOf(0.6f, 0.4f, 0.6f, 0.9f) - 0.4f) < 1e-6f);
    CHECK(std::fabs(GripFactorOf(0.75f, 0.4f, 0.6f, 0.9f) - 0.7f) < 1e-5f);
    CHECK(GripFactorOf(0.9f, 0.4f, 0.6f, 0.9f) == 1.0f);
    CHECK(GripFactorOf(1.0f, 0.4f, 0.6f, 0.9f) == 1.0f);
    CHECK(std::fabs(GripFactorOf(0.8f, 0.4f, 0.6f, 1.0f) - 0.7f) < 1e-5f);  // la curva del piano (hi 1)
    CHECK(GripFactorOf(nan, 0.4f, 0.6f, 0.9f) == 1.0f);
    CHECK(GripFactorOf(0.6f, 0.0f, 0.6f, 0.9f) == 0.05f);  // mai sotto 5%
    CHECK(GripFactorOf(0.7f, 0.4f, 0.8f, 0.5f) == 0.4f);   // hi <= lo: niente divisione per zero
    // Controller a tasto lungo tutta la catena: forza piena.
    CHECK(GripFactorOf(GripHeldValue(true, GripAxisOf(0.0f, 0.0f, true)), 0.4f, 0.6f, 0.9f) == 1.0f);
    CHECK(GripEaseAlpha(0.0f, 0.08f) == 0.0f && GripEaseAlpha(0.01f, 0.0f) == 1.0f);

    const float hold = 0.1f, ease = 0.08f, rise = 0.025f, win = 0.3f;
    // Tempi dell'attesa: da 1 a 0,4 a 120 Hz. Fermo per hold, poi esponenziale.
    {
        GripFilterState s;
        const float dt = 1.0f / 120.0f;
        double t = 0.0;
        float fAt09 = 0.0f, fAt11 = 0.0f, fAt18 = 0.0f, peakAt30 = 0.0f, peakAt42 = 0.0f;
        GripFilterStep(s, 1.0f, t, dt, hold, ease, rise, win);  // presa
        for (int i = 1; i <= 60; ++i) {
            t = i * dt;
            GripFilterStep(s, 0.4f, t, dt, hold, ease, rise, win);
            if (i == 11) fAt09 = s.f;   // 0,092 s
            if (i == 14) fAt11 = s.f;   // 0,117 s
            if (i == 22) fAt18 = s.f;   // 0,183 s
            if (i == 36) peakAt30 = GripPeakOf(s);  // 0,3 s
            if (i == 50) peakAt42 = GripPeakOf(s);  // 0,417 s
        }
        CHECK(fAt09 == 1.0f);
        CHECK(fAt11 < 1.0f && fAt11 > 0.8f);  // 3 tick di discesa: 0,84
        CHECK(fAt18 > 0.55f && fAt18 < 0.70f);  // 0,4 + 0,6 e^-1 = 0,62
        CHECK(s.f < 0.42f);                     // 0,5 s: arrivato
        // Lancio: il massimo degli ultimi 250-300 ms resta la presa stretta
        // (1 fino a 0,1 s); a 0,417 s la finestra (da 0,15 s) l'ha lasciata.
        CHECK(peakAt30 == 1.0f);
        CHECK(peakAt42 < 0.75f);
    }
    // Stringere: niente scalino (prima 0,4 -> 1 in un tick), arrivato in ~0,1 s.
    {
        GripFilterState s;
        double t = 0.0;
        const float dt = 1.0f / 120.0f;
        for (int i = 0; i < 120; ++i, t += dt) GripFilterStep(s, 0.4f, t, dt, hold, ease, rise, win);
        CHECK(std::fabs(s.f - 0.4f) < 1e-6f);
        float maxStep = 0.0f, prev = s.f, fAt10 = 0.0f;
        for (int i = 1; i <= 24; ++i) {
            t += dt;
            GripFilterStep(s, 1.0f, t, dt, hold, ease, rise, win);
            maxStep = std::fmax(maxStep, s.f - prev);
            prev = s.f;
            if (i == 12) fAt10 = s.f;
        }
        CHECK(maxStep < 0.6f * 0.3f);  // un tick: 0,6 x (1 - e^(-8,3/25)) = 0,17
        CHECK(fAt10 > 0.97f);
        CHECK(s.f == 1.0f && s.env == 1.0f);
    }
    // Stesso grip a 90 e a 250 Hz: stessa risposta (esponenziale, non dt / tau).
    {
        float out[2];
        const float hz[2] = {90.0f, 250.0f};
        for (int k = 0; k < 2; ++k) {
            GripFilterState s;
            const float dt = 1.0f / hz[k];
            double t = 0.0;
            while (t < 0.2) {
                GripFilterStep(s, t < 0.01 ? 1.0f : 0.5f, t, dt, hold, ease, rise, win);
                t += dt;
            }
            out[k] = s.f;
        }
        CHECK(std::fabs(out[0] - out[1]) < 0.03f);
    }
    // Grip che trema (0,8 +- 0,05 a ogni tick, f 0,7 +- 0,075): l'inviluppo
    // resta in alto, f varia poco e senza scalini.
    {
        GripFilterState s;
        uint32_t x = 2463534242u;
        double t = 0.0;
        const float dt = 1.0f / 144.0f;
        float lo = 1.0f, hi = 0.0f, maxStep = 0.0f, prev = 1.0f;
        for (int i = 0; i < 144 * 4; ++i, t += dt) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const float v = 0.8f + 0.05f * (static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f);
            GripFilterStep(s, GripFactorOf(v, 0.4f, 0.6f, 1.0f), t, dt, hold, ease, rise, win);
            if (i > 144) {
                lo = std::fmin(lo, s.f);
                hi = std::fmax(hi, s.f);
                maxStep = std::fmax(maxStep, std::fabs(s.f - prev));
            }
            prev = s.f;
        }
        CHECK(lo > 0.70f && hi < 0.776f);  // sopra il valore medio, mai oltre il picco del rumore
        CHECK(hi - lo < 0.075f);           // meta' della banda del rumore (0,15): l'inviluppo ne tiene la parte alta
        CHECK(maxStep < 0.02f);            // 0,012 (senza la salita filtrata: 0,034 di colpo)
    }
    // Finestra nulla: il lancio usa la presa di adesso (come prima della revisione).
    {
        GripFilterState s;
        double t = 0.0;
        const float dt = 1.0f / 120.0f;
        for (int i = 0; i < 60; ++i, t += dt) GripFilterStep(s, i < 2 ? 1.0f : 0.4f, t, dt, 0.0f, 0.01f, rise, 0.0f);
        CHECK(GripPeakOf(s) == s.f && s.f < 0.41f);
    }
    // Un buco lungo (pausa): la finestra riparte.
    {
        GripFilterState s;
        GripFilterStep(s, 1.0f, 0.0, 0.008f, hold, ease, rise, win);
        s.f = s.env = 0.5f;
        GripFilterStep(s, 0.5f, 5.0, 0.008f, hold, ease, rise, win);
        CHECK(GripPeakOf(s) == 0.5f);
    }
}

static void TestThrowHit() {
    const ThrowHitParams p;
    // Sotto 3 m/s o 4 J: niente (rimbalza e basta).
    CHECK(ThrowHitTier(5.0f, 2.9f, true, false, p).tier == 0);
    CHECK(ThrowHitTier(0.4f, 4.0f, false, false, p).tier == 0);  // lattina 0,4 kg a 4 m/s: 3,2 J
    CHECK(ThrowHitTier(0.0f, 8.0f, false, false, p).tier == 0);
    CHECK(ThrowHitTier(1.0f, std::nanf(""), false, false, p).tier == 0);
    // Bottiglia (1,6 kg) al petto: leggero, poco danno; a 10 m/s il massimo leggero.
    ThrowHitOut b6 = ThrowHitTier(1.6f, 6.0f, false, false, p);
    CHECK(b6.tier == 1 && b6.damage == 7 && std::fabs(b6.joules - 28.8f) < 0.01f);
    CHECK(ThrowHitTier(1.6f, 10.0f, false, false, p).damage == 15);
    CHECK(ThrowHitTier(0.4f, 5.0f, false, false, p).damage == 4);  // lattina a 5 m/s (5 J): 3 + 0,75 -> 4
    // Bottiglia in testa: da 15 J pesante (barcolla), sotto leggero x1,5.
    ThrowHitOut bh = ThrowHitTier(1.6f, 6.0f, true, false, p);
    CHECK(bh.tier == 2 && bh.damage == 26);
    ThrowHitOut bl = ThrowHitTier(1.6f, 4.0f, true, false, p);  // 12,8 J
    CHECK(bl.tier == 1 && bl.damage == 7);
    // Lattina leggerissima in testa: sotto 1 kg mai pesante.
    CHECK(ThrowHitTier(0.5f, 10.0f, true, false, p).tier == 1);
    // Radio (5,1 kg) al petto: pesante da 30 J (3,43 m/s); al limite d'energia (85 J) 40 = la mischia di Walker.
    CHECK(ThrowHitTier(5.1f, 3.3f, false, false, p).tier == 1);
    ThrowHitOut r = ThrowHitTier(5.1f, 5.77f, false, false, p);
    CHECK(r.tier == 2 && r.damage == 40);
    CHECK(ThrowHitTier(5.1f, 3.5f, false, false, p).damage == 20);
    // Salto del tracciamento al rilascio: al piu' leggero.
    ThrowHitOut rs = ThrowHitTier(5.1f, 5.77f, false, true, p);
    CHECK(rs.tier == 1 && rs.demoted && rs.damage == 15);
    // Velocita' del danno: l'impatto, ma al piu' il rilascio pulito piu' la caduta.
    CHECK(std::fabs(ThrowDamageSpeed(6.0f, 6.0f, 0.0f, 15.0f, 1.0f, 15.0f) - 6.0f) < 1e-4f);
    CHECK(std::fabs(ThrowDamageSpeed(12.0f, 5.0f, 0.0f, 15.0f, 1.0f, 15.0f) - 6.0f) < 1e-4f);  // spinto oltre: 5 + 1
    CHECK(std::fabs(ThrowDamageSpeed(9.0f, 5.0f, 1.0f, 15.0f, 1.0f, 15.0f) - (std::sqrt(25.0f + 30.0f) + 1.0f)) < 1e-3f);  // caduto 1 m
    CHECK(ThrowDamageSpeed(30.0f, -1.0f, 0.0f, 15.0f, 1.0f, 15.0f) == 15.0f);  // rilascio ignoto: al piu' cap
    CHECK(ThrowDamageSpeed(30.0f, 40.0f, 0.0f, 15.0f, 1.0f, 15.0f) == 15.0f);
    CHECK(ThrowDamageSpeed(-1.0f, 5.0f, 0.0f, 15.0f, 1.0f, 15.0f) == 0.0f);
}

static void TestWorldHit() {
    const WorldHitParams p;
    // Sotto 2 m/s o 3 J niente.
    CHECK(WorldHitDamage(5.0f, 1.9f, false, false, p).damage == 0);
    CHECK(WorldHitDamage(0.4f, 3.5f, false, false, p).damage == 0);  // lattina a 3,5 m/s: 2,45 J
    CHECK(WorldHitDamage(0.0f, 8.0f, false, false, p).damage == 0);
    CHECK(WorldHitDamage(1.0f, std::nanf(""), false, false, p).damage == 0);
    // Bottiglia (1,6 kg) lanciata a 6 m/s: 28,8 J -> 36, incrina il vetro del
    // pullman (salute 60, "Cracked" a 30) senza sfondarlo; a 10 m/s (80 J)
    // resta leggera e tagliata a 40.
    WorldHitOut b6 = WorldHitDamage(1.6f, 6.0f, false, false, p);
    CHECK(b6.damage == 36 && !b6.heavy && !b6.capped && std::fabs(b6.joules - 28.8f) < 0.01f);
    WorldHitOut b10 = WorldHitDamage(1.6f, 10.0f, false, false, p);
    CHECK(b10.damage == 40 && !b10.heavy && b10.capped);
    // Radio (5,1 kg): a 5,77 m/s (85 J, il limite d'energia del lancio) pesante, 106: sfonda (60).
    WorldHitOut r = WorldHitDamage(5.1f, 5.77f, false, false, p);
    CHECK(r.heavy && r.damage == 106);
    // Radio appena lanciata (3,5 m/s, 31 J): non ancora pesante, 39 (incrina).
    WorldHitOut rl = WorldHitDamage(5.1f, 3.5f, false, false, p);
    CHECK(!rl.heavy && rl.damage == 39);
    // Salto del tracciamento al rilascio: al piu' leggero.
    WorldHitOut rs = WorldHitDamage(5.1f, 5.77f, false, true, p);
    CHECK(!rs.heavy && rs.damage == 40 && rs.capped);
    // In mano conta anche il braccio (+2 kg): bottiglia a 4 m/s 28,8 J (36) invece di 12,8 J.
    WorldHitOut bh = WorldHitDamage(1.6f, 4.0f, true, false, p);
    CHECK(bh.damage == 36 && std::fabs(bh.kg - 3.6f) < 1e-4f && !bh.heavy);
    // Bottiglia in mano a 6 m/s (revisione della voce 2): 3,6 kg col braccio,
    // 64,8 J, ma la bottiglia da sola e' leggera: al piu' 40, incrina e basta
    // (prima pesante, 81: sfondava il vetro del pullman da sola). Anche a 8 m/s.
    WorldHitOut bh6 = WorldHitDamage(1.6f, 6.0f, true, false, p);
    CHECK(!bh6.heavy && bh6.capped && bh6.damage == 40 && std::fabs(bh6.joules - 64.8f) < 0.01f);
    CHECK(WorldHitDamage(1.6f, 8.0f, true, false, p).damage == 40);
    // Radio in mano a 4 m/s: 7,1 kg, 56,8 J -> pesante (5,1 kg da sola), 71.
    WorldHitOut rh = WorldHitDamage(5.1f, 4.0f, true, false, p);
    CHECK(rh.heavy && rh.damage == 71);
    // Oggetto di 2,9 kg in mano a 6 m/s (88 J col braccio): leggero, 40.
    CHECK(!WorldHitDamage(2.9f, 6.0f, true, false, p).heavy);
    // Tetto dei pesanti.
    CHECK(WorldHitDamage(33.0f, 10.0f, false, false, p).damage == 250);
}

static void TestMeleeHit() {
    MeleeHitParams p;
    // Sotto 2,5 m/s niente; valori assurdi niente.
    CHECK(MeleeHit(1.6f, 2.4f, true, p).tier == 0);
    CHECK(MeleeHit(0.0f, 5.0f, false, p).tier == 0);
    CHECK(MeleeHit(1.6f, std::nanf(""), false, p).tier == 0);
    CHECK(MeleeHit(1.6f, -3.0f, false, p).tier == 0);
    // Bottiglia (1,6 kg) al corpo: 2,5 m/s leggero (11,25 J col braccio -> 9,5 -> 10), da 3 m/s barcolla (16,2 J -> 11).
    MeleeHitOut b25 = MeleeHit(1.6f, 2.5f, false, p);
    CHECK(b25.tier == 1 && b25.damage == 10 && std::fabs(b25.joules - 11.25f) < 0.01f);
    MeleeHitOut b3 = MeleeHit(1.6f, 3.0f, false, p);
    CHECK(b3.tier == 2 && b3.damage == 11 && !b3.capped);
    // Bottiglia in testa: x2 e barcolla anche sotto 3 m/s; a 4 m/s 33, a 6 m/s il tetto (40, la mischia di Walker).
    MeleeHitOut bh25 = MeleeHit(1.6f, 2.5f, true, p);
    CHECK(bh25.tier == 2 && bh25.damage == 19);
    CHECK(MeleeHit(1.6f, 4.0f, true, p).damage == 33);
    MeleeHitOut bh6 = MeleeHit(1.6f, 6.0f, true, p);
    CHECK(bh6.tier == 2 && bh6.damage == 40 && bh6.capped);
    // Al corpo al piu' 20 (meta' della mischia di Walker): radio (5,1 kg) a 3 m/s 18, a 5 m/s 20.
    CHECK(MeleeHit(5.1f, 3.0f, false, p).damage == 18);
    MeleeHitOut r5 = MeleeHit(5.1f, 5.0f, false, p);
    CHECK(r5.tier == 2 && r5.damage == 20 && r5.capped);
    // Lattina (0,4 kg): mai la mischia di Walker; in testa al piu' il leggero (15).
    MeleeHitOut c4 = MeleeHit(0.4f, 4.0f, false, p);
    CHECK(c4.tier == 1 && c4.damage == 13);
    MeleeHitOut ch = MeleeHit(0.4f, 4.0f, true, p);
    CHECK(ch.tier == 1 && ch.damage == 15 && ch.capped);
    // Tetto della testa da Walker (m_meleeDamagePawns piu' basso).
    p.headMax = 30.0f;
    CHECK(MeleeHit(1.6f, 6.0f, true, p).damage == 30);
    // Revisione della voce 4: colpo vero rispetto a Walker (swing). Camminando
    // (3,8 m/s) con l'oggetto fermo in mano niente, nemmeno in testa.
    MeleeHitParams q;
    MeleeHitOut walk = MeleeHit(5.1f, 3.8f, 0.2f, false, q);
    CHECK(walk.tier == 0 && walk.damage == 0 && walk.joules > 0.0f);
    CHECK(MeleeHit(1.6f, 3.8f, 0.3f, true, q).tier == 0);
    CHECK(MeleeHit(5.1f, 3.8f, std::nanf(""), false, q).tier == 0);
    // Colpo lento (2,6) camminando: leggero, danno dalla velocita' nel mondo (6,4 m/s -> oltre il tetto 15).
    MeleeHitOut ws = MeleeHit(5.1f, 6.4f, 2.6f, false, q);
    CHECK(ws.tier == 1 && ws.damage == 15 && ws.capped);
    // Colpo vero (3,2) camminando: barcolla, al piu' 20.
    MeleeHitOut wh = MeleeHit(5.1f, 4.5f, 3.2f, false, q);
    CHECK(wh.tier == 2 && wh.damage == 20);
    // Indietreggiando: il colpo arriva a 1,8 m/s nel mondo, niente anche se il braccio va a 3.
    CHECK(MeleeHit(1.6f, 1.8f, 3.0f, false, q).tier == 0);
    // Walker fermo: uguale alla forma a una velocita'.
    MeleeHitOut a = MeleeHit(1.6f, 3.0f, 3.0f, false, q), b = MeleeHit(1.6f, 3.0f, false, q);
    CHECK(a.tier == b.tier && a.damage == b.damage && a.joules == b.joules);
}

// Disarmo (voce 10 del piano dei combattimenti, 27/10).
static void TestDisarm() {
    DisarmParams p;
    // Mano vuota: sotto 3 m/s niente, da 3 m/s barcolla (6 kg m/s), da 4 m/s l'arma cade (8).
    DisarmOut h25 = DisarmHit(DisarmHow::kHand, 0.0f, 2.9f, 2.9f, p);
    CHECK(h25.tier == 0 && h25.slow);
    DisarmOut h3 = DisarmHit(DisarmHow::kHand, 0.0f, 3.0f, 3.0f, p);
    CHECK(h3.tier == 1 && std::fabs(h3.impulse - 6.0f) < 1e-4f);
    CHECK(DisarmHit(DisarmHow::kHand, 0.0f, 4.0f, 4.0f, p).tier == 2);
    // Mano ferma mentre Walker cammina (3,8 m/s nel mondo, 0,3 rispetto a lui): niente.
    DisarmOut hw = DisarmHit(DisarmHow::kHand, 0.0f, 3.8f, 0.3f, p);
    CHECK(hw.tier == 0 && hw.slow && std::fabs(hw.speed - 0.3f) < 1e-4f);
    // Oggetto tenuto: bottiglia (1,6 kg) a 2,5 m/s = 9 -> cade; lattina (0,4) a 3 m/s = 7,2 -> barcolla; sotto 2,5 niente.
    CHECK(DisarmHit(DisarmHow::kHeld, 1.6f, 2.5f, 2.5f, p).tier == 2);
    DisarmOut c3 = DisarmHit(DisarmHow::kHeld, 0.4f, 3.0f, 3.0f, p);
    CHECK(c3.tier == 1 && std::fabs(c3.impulse - 7.2f) < 1e-4f);
    CHECK(DisarmHit(DisarmHow::kHeld, 5.1f, 2.4f, 2.4f, p).tier == 0);
    // Tenuto, colpo nel mondo veloce ma lento rispetto a Walker: conta la minore.
    CHECK(DisarmHit(DisarmHow::kHeld, 1.6f, 4.0f, 1.0f, p).tier == 0);
    // Lanciato: bottiglia a 5 m/s = 8 -> cade, a 3 m/s 4,8 -> barcolla; lattina a 8 m/s 3,2 -> niente; sotto 3 m/s niente.
    CHECK(DisarmHit(DisarmHow::kThrown, 1.6f, 5.0f, 0.0f, p).tier == 2);
    CHECK(DisarmHit(DisarmHow::kThrown, 1.6f, 3.0f, 0.0f, p).tier == 1);
    DisarmOut can = DisarmHit(DisarmHow::kThrown, 0.4f, 8.0f, 0.0f, p);
    CHECK(can.tier == 0 && !can.slow);
    CHECK(DisarmHit(DisarmHow::kThrown, 5.1f, 2.9f, 0.0f, p).tier == 0);
    // Valori assurdi: niente.
    CHECK(DisarmHit(DisarmHow::kHeld, 1.6f, std::nanf(""), 3.0f, p).tier == 0);
    CHECK(DisarmHit(DisarmHow::kHeld, 1.6f, 3.0f, std::nanf(""), p).tier == 0);
    CHECK(DisarmHit(DisarmHow::kThrown, -1.0f, 5.0f, 0.0f, p).tier == 0);
    CHECK(DisarmHit(DisarmHow::kHand, 0.0f, -4.0f, -4.0f, p).tier == 0);

    // Segmento contro il box orientato dell'arma.
    const float id[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    const float lc[3] = {0, 0, 0}, le[3] = {10, 2, 5};
    float t = -1.0f;
    const float a0[3] = {-20, 0, 0}, b0[3] = {20, 0, 0};
    CHECK(SegmentHitsObb(a0, b0, id, lc, le, 0.0f, &t) && std::fabs(t - 0.25f) < 1e-4f);
    // Sopra il box (z 8, mezza altezza 5): fuori senza margine, dentro con 4 unita'.
    const float a1[3] = {-20, 0, 8}, b1[3] = {20, 0, 8};
    CHECK(!SegmentHitsObb(a1, b1, id, lc, le, 0.0f, &t));
    CHECK(SegmentHitsObb(a1, b1, id, lc, le, 4.0f, &t));
    // Segmento che si ferma prima del box.
    const float b2[3] = {-12, 0, 0};
    CHECK(!SegmentHitsObb(a0, b2, id, lc, le, 0.0f, &t));
    // Partenza dentro: t = 0.
    const float a3[3] = {1, 0, 0};
    CHECK(SegmentHitsObb(a3, b0, id, lc, le, 0.0f, &t) && t == 0.0f);
    // Box ruotato di 90 gradi attorno a Z (X locale = Y del mondo) e spostato a x 100:
    // lungo 20 in y. Un segmento lungo x a y 8 lo prende, a y 12 no.
    const float rz[4][4] = {{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {100, 0, 0, 1}};
    const float a4[3] = {80, 8, 0}, b4[3] = {120, 8, 0}, a5[3] = {80, 12, 0}, b5[3] = {120, 12, 0};
    CHECK(SegmentHitsObb(a4, b4, rz, lc, le, 0.0f, &t) && std::fabs(t - 0.45f) < 1e-4f);
    CHECK(!SegmentHitsObb(a5, b5, rz, lc, le, 0.0f, &t));
    // Centro locale spostato (le mesh delle armi hanno l'origine all'impugnatura): lc x 11.
    const float lc2[3] = {11, 0, 0};
    const float a6[3] = {80, 20, 0}, b6[3] = {120, 20, 0};
    CHECK(SegmentHitsObb(a6, b6, rz, lc2, le, 0.0f, &t));
    CHECK(!SegmentHitsObb(a5, b5, rz, lc, le, 0.0f, nullptr));
    // Scala 2 nella matrice: le estensioni raddoppiano nel mondo, il margine resta in unita' del mondo.
    const float sc[4][4] = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 2, 0}, {0, 0, 0, 1}};
    const float a7[3] = {-40, 0, 11}, b7[3] = {40, 0, 11};
    CHECK(SegmentHitsObb(a7, b7, sc, lc, le, 1.5f, &t));
    CHECK(!SegmentHitsObb(a7, b7, sc, lc, le, 0.5f, &t));
    // Matrice degenere o NaN: no.
    const float zero[4][4] = {};
    CHECK(!SegmentHitsObb(a0, b0, zero, lc, le, 0.0f, &t));
    const float an[3] = {std::nanf(""), 0, 0};
    CHECK(!SegmentHitsObb(an, b0, id, lc, le, 0.0f, &t));
}

static void TestShield() {
    // Materiale dal nome della mesh (minuscolo).
    CHECK(ShieldMaterialOf("suitcase_02_open") == ShieldMaterial::kNormal);
    CHECK(ShieldMaterialOf("radio_unit_04") == ShieldMaterial::kNormal);
    CHECK(ShieldMaterialOf("plate_01") == ShieldMaterial::kFragile);
    CHECK(ShieldMaterialOf("book_stack") == ShieldMaterial::kFragile);
    CHECK(ShieldMaterialOf("bottle_round02") == ShieldMaterial::kFragile);
    CHECK(ShieldMaterialOf("ammo_box_01") == ShieldMaterial::kMetal);
    CHECK(ShieldMaterialOf("sandbag_single_02") == ShieldMaterial::kMetal);
    CHECK(ShieldMaterialOf("427_fuel_barrel_03") == ShieldMaterial::kMetal);
    CHECK(ShieldMaterialOf("metal_plate_02") == ShieldMaterial::kMetal);  // il metallo vince
    // Durabilita' (il piano: piatti e libri ~2, valigie e radio ~6, casse metalliche ~15).
    CHECK(ShieldDurability(ShieldMaterial::kFragile, 20.0f) == 2);
    CHECK(ShieldDurability(ShieldMaterial::kMetal, 1.0f) == 15);
    CHECK(ShieldDurability(ShieldMaterial::kNormal, 1.6f) == 3);
    CHECK(ShieldDurability(ShieldMaterial::kNormal, 5.1f) == 5);   // radio creata, scala 1
    CHECK(ShieldDurability(ShieldMaterial::kNormal, 8.0f) == 6);   // radio del livello
    CHECK(ShieldDurability(ShieldMaterial::kNormal, 16.8f) == 6);  // valigia
    CHECK(ShieldDurability(ShieldMaterial::kNormal, 33.0f) == 10);
    CHECK(ShieldDurability(ShieldMaterial::kNormal, std::nanf("")) == 3);
    // Usura e impulso dal danno dell'arma.
    CHECK(ShieldWear(30.0f) == 1 && ShieldWear(99.0f) == 1 && ShieldWear(135.0f) == 2 && ShieldWear(365.0f) == 3);
    CHECK(ShieldWear(std::nanf("")) == 1);
    CHECK(std::fabs(ShieldShotImpulse(30.0f) - 1.55f) < 1e-4f);
    CHECK(ShieldShotImpulse(0.0f) == 1.0f && ShieldShotImpulse(1400.0f) == 6.0f && ShieldShotImpulse(-5.0f) == 1.0f);
    // Velocita' e spinta: valigia 16,8 kg e AK47 -> 0,2 m/s (minimo), 0,8 cm; piatto 0,4 kg e Barrett -> 3 m/s, 5 cm.
    CHECK(std::fabs(ShieldDv(2.05f, 16.8f) - 0.2f) < 1e-4f && std::fabs(ShieldKickCm(0.2f) - 0.8f) < 1e-4f);
    CHECK(ShieldDv(6.0f, 0.4f) == 3.0f && ShieldKickCm(3.0f) == 5.0f);
    CHECK(std::fabs(ShieldDv(1.55f, 1.0f) - 1.55f) < 1e-4f && std::fabs(ShieldKickCm(1.55f) - 3.875f) < 1e-4f);
    // Forma della spinta: 0 prima, 1 a tau, torna.
    CHECK(ShieldKickShape(-0.01f, 0.03f) == 0.0f && ShieldKickShape(0.0f, 0.03f) == 0.0f);
    CHECK(std::fabs(ShieldKickShape(0.03f, 0.03f) - 1.0f) < 1e-5f);
    CHECK(ShieldKickShape(0.015f, 0.03f) < 1.0f && ShieldKickShape(0.06f, 0.03f) < 1.0f && ShieldKickShape(0.06f, 0.03f) > 0.5f);
    CHECK(ShieldKickShape(0.25f, 0.03f) == 0.0f);
    // Segmento contro la scatola orientata: asse x, scatola di mezze estensioni 10, 5, 5 in (100, 0, 0).
    const float ax[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, c[3] = {100, 0, 0}, e[3] = {10, 5, 5};
    float t = 0.0f, n[3] = {0, 0, 0};
    const float a[3] = {0, 0, 0}, b[3] = {200, 0, 0};
    CHECK(SegmentObbEntry(a, b, c, ax, e, &t, n) && std::fabs(t - 0.45f) < 1e-5f && n[0] == -1.0f && n[1] == 0.0f);
    const float b2[3] = {80, 0, 0};  // si ferma prima
    CHECK(!SegmentObbEntry(a, b2, c, ax, e, &t, n));
    const float a3[3] = {0, 6, 0}, b3[3] = {200, 6, 0};  // di lato
    CHECK(!SegmentObbEntry(a3, b3, c, ax, e, &t, n));
    const float a4[3] = {100, 0, 0};  // parte dentro
    CHECK(!SegmentObbEntry(a4, b, c, ax, e, &t, n));
    const float a5[3] = {100, 0, 50}, b5[3] = {100, 0, -50};  // dall'alto: faccia +z
    CHECK(SegmentObbEntry(a5, b5, c, ax, e, &t, n) && std::fabs(t - 0.45f) < 1e-5f && n[2] == 1.0f);
    // Scatola ruotata di 45 gradi attorno a z: entrata sullo spigolo piu' vicino.
    const float r = 0.70710678f;
    const float axr[3][3] = {{r, r, 0}, {-r, r, 0}, {0, 0, 1}}, er[3] = {5, 5, 5};
    CHECK(SegmentObbEntry(a, b, c, axr, er, &t, n) && std::fabs(t * 200.0f - (100.0f - 5.0f * 1.41421356f)) < 1e-2f);
    // Oggetto che vola via (revisione della voce 3): velocita' dalla spinta, mai verso la testa.
    CHECK(std::fabs(ShieldKnockSpeed(0.2f) - 1.04f) < 1e-4f);  // valigia
    CHECK(std::fabs(ShieldKnockSpeed(0.41f) - 1.292f) < 1e-4f);  // radio da 5 kg, AK47
    CHECK(ShieldKnockSpeed(3.0f) == 3.5f && ShieldKnockSpeed(std::nanf("")) == 0.8f && ShieldKnockSpeed(-1.0f) == 0.8f);
    {
        // Testa nell'origine, oggetto 15 unita' davanti agli occhi e 3 sotto,
        // colpo da davanti (verso la testa): niente indietro, di lato dalla
        // parte della mano (sinistra = -Y), in basso.
        const float head[3] = {0, 0, 0}, obj[3] = {15, 0, -3}, d[3] = {-1, 0, 0};
        float o[3];
        ShieldKnockDir(d, obj, head, -1.0f, o);
        CHECK(std::fabs(o[0]) < 1e-5f && o[1] < -0.7f && o[2] < -0.4f);
        CHECK(std::fabs(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - 1.0f) < 1e-4f);
        ShieldKnockDir(d, obj, head, 1.0f, o);  // mano destra: a destra
        CHECK(o[1] > 0.7f);
        // Colpo da destra verso sinistra: resta lungo il colpo (a sinistra).
        const float dl[3] = {0, -1, 0};
        ShieldKnockDir(dl, obj, head, 1.0f, o);
        CHECK(o[1] < -0.1f && o[0] >= -1e-5f);
        // Oggetto sopra gli occhi: verso l'alto, non sulla testa.
        const float objUp[3] = {10, 0, 20};
        ShieldKnockDir(d, objUp, head, -1.0f, o);
        CHECK(o[2] > 0.3f && o[0] >= -1e-5f);
        // Oggetto proprio sopra la testa: avanti = contro il colpo.
        const float objTop[3] = {0, 0, 25};
        ShieldKnockDir(d, objTop, head, -1.0f, o);
        CHECK(o[0] >= -1e-5f && o[2] > 0.3f);
        // Molte direzioni e posizioni: mai una parte orizzontale verso la testa.
        bool ok = true;
        for (int i = 0; i < 2000 && ok; ++i) {
            const float u = static_cast<float>(i);
            const float di[3] = {std::sin(u * 1.3f), std::cos(u * 0.7f), std::sin(u * 2.1f) * 0.5f};
            const float ob[3] = {30.0f * std::cos(u * 0.37f), 30.0f * std::sin(u * 0.37f), 10.0f * std::sin(u * 0.11f)};
            ShieldKnockDir(di, ob, head, (i & 1) ? 1.0f : -1.0f, o);
            const float fl = std::sqrt(ob[0] * ob[0] + ob[1] * ob[1]);
            if (fl > 1.0f && (o[0] * ob[0] + o[1] * ob[1]) / fl < -1e-4f) ok = false;
            if (std::fabs(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - 1.0f) > 1e-3f) ok = false;
        }
        CHECK(ok);
    }
}

// Granate nemiche rilanciate (voce 8): velocita' in mano, lancio, "a terra", previsione.
static void TestCorpseDrag() {
    using vr::pure::BodyShape;
    float n[3];
    BodyShape cap;
    cap.kind = 2;
    cap.ax[0][0] = cap.ax[1][1] = cap.ax[2][2] = 1.0f;
    cap.a = 3.0f;
    cap.b = 5.0f;
    const float q0[3] = {0, 0, 0}, q1[3] = {10, 0, 0}, q2[3] = {0, 0, 9};
    CHECK(std::fabs(vr::pure::BodyShapeDist(cap, 1.0f, q0, n) + 3.0f) < 1e-4f);
    CHECK(std::fabs(vr::pure::BodyShapeDist(cap, 1.0f, q1, n) - 7.0f) < 1e-4f && std::fabs(n[0] - 3.0f) < 1e-4f && std::fabs(n[2]) < 1e-4f);
    CHECK(std::fabs(vr::pure::BodyShapeDist(cap, 1.0f, q2, n) - 1.0f) < 1e-4f && std::fabs(n[2] - 8.0f) < 1e-4f);  // oltre la calotta
    CHECK(std::fabs(vr::pure::BodyShapeDist(cap, 2.0f, q1, n) - 4.0f) < 1e-4f);  // scala 2: raggio 6
    // Avambraccio del manichino (Ellbow_Left): capsula r 3, lunga 8,9, origine (6,8 0 1), asse -X dell'osso; il palmo sull'osso e' dentro.
    BodyShape arm;
    arm.kind = 2;
    arm.ax[0][2] = 1.0f;
    arm.ax[1][1] = 1.0f;
    arm.ax[2][0] = -1.0f;
    arm.o[0] = 6.8f;
    arm.o[2] = 1.0f;
    arm.a = 3.0f;
    arm.b = 4.45f;
    const float d = vr::pure::BodyShapeDist(arm, 1.0f, q0, n);
    CHECK(d < 0.0f && d > -1.0f);
    BodyShape box;
    box.kind = 1;
    box.ax[0][0] = box.ax[1][1] = box.ax[2][2] = 1.0f;
    box.a = 2.0f;
    box.b = 3.0f;
    box.c = 4.0f;
    const float b1[3] = {5, 0, 0}, b2[3] = {1, 0, 0}, b3[3] = {3, 4, 0};
    CHECK(std::fabs(vr::pure::BodyShapeDist(box, 1.0f, b1, n) - 3.0f) < 1e-4f && std::fabs(n[0] - 2.0f) < 1e-4f);
    CHECK(std::fabs(vr::pure::BodyShapeDist(box, 1.0f, b2, n) + 1.0f) < 1e-4f && std::fabs(n[0] - 2.0f) < 1e-4f);  // dentro: faccia x
    CHECK(std::fabs(vr::pure::BodyShapeDist(box, 1.0f, b3, n) - std::sqrt(2.0f)) < 1e-4f);  // spigolo
    BodyShape sph;
    sph.kind = 0;
    sph.ax[0][0] = sph.ax[1][1] = sph.ax[2][2] = 1.0f;
    sph.o[0] = sph.o[1] = sph.o[2] = 1.0f;
    sph.a = 2.0f;
    const float s1[3] = {1, 1, 4};
    CHECK(std::fabs(vr::pure::BodyShapeDist(sph, 1.0f, s1, n) - 1.0f) < 1e-4f && std::fabs(n[2] - 3.0f) < 1e-4f);
    // Impulsi: braccio da 0,4 kg (0,01) di un ragdoll da 30 kg (0,75): quasi tutto alla radice, al piu' il limite.
    vr::pure::CorpseImpulse ji = vr::pure::CorpseImpulseSplit(0.01f, 0.75f, 100.0f, 3.0f);
    CHECK(std::fabs(ji.limb - 1.0f) < 1e-4f && std::fabs(ji.root - 2.0f) < 1e-4f);
    ji = vr::pure::CorpseImpulseSplit(0.125f, 0.75f, 100.0f, 3.0f);  // busto da 5 kg: tutto il limite al busto
    CHECK(std::fabs(ji.limb - 3.0f) < 1e-4f && ji.root == 0.0f);
    ji = vr::pure::CorpseImpulseSplit(0.01f, 0.75f, 1.0f, 3.0f);  // poco da fare: sotto il limite
    CHECK(std::fabs(ji.limb - 0.01f) < 1e-6f && std::fabs(ji.root - 0.74f) < 1e-5f);
    ji = vr::pure::CorpseImpulseSplit(0.01f, 0.75f, std::nanf(""), 3.0f);
    CHECK(ji.limb == 0.0f && ji.root == 0.0f);
    // Forza che cresce: x2 ogni 0,4 s col punto lontano e fermo, al piu' x4; vicino torna 1.
    vr::pure::CorpsePull pull;
    const float s = 50.0f;
    vr::pure::CorpsePullStep(pull, 10.0f, 10.0f, s, 0.0);  // 20 cm, fermo: parte il conto
    CHECK(pull.mul == 1.0f && std::fabs(pull.growAt - 0.4) < 1e-9);
    vr::pure::CorpsePullStep(pull, 10.0f, 10.0f, s, 0.41);
    CHECK(pull.mul == 2.0f);
    vr::pure::CorpsePullStep(pull, 10.0f, 10.0f, s, 0.82);
    vr::pure::CorpsePullStep(pull, 10.0f, 10.0f, s, 1.30);
    CHECK(pull.mul == 4.0f);
    vr::pure::CorpsePullStep(pull, 8.0f, 10.0f, s, 1.40);  // si avvicina: niente crescita, resta x4
    CHECK(pull.mul == 4.0f && pull.growAt < 0.0);
    vr::pure::CorpsePullStep(pull, 2.0f, 8.0f, s, 1.50);  // entro 6 cm: di nuovo la forza della mano
    CHECK(pull.mul == 1.0f);
}

static void TestGrenadeBack() {
    float o[3];
    {
        const float err[3] = {11.0f, 0.0f, -5.5f}, hv[3] = {100.0f, 0.0f, 0.0f};
        const float s = GrenadeHoldVel(err, 0.011f, hv, 2000.0f, o);  // 1000 + 100, -500
        CHECK(std::fabs(o[0] - 1100.0f) < 0.01f && o[1] == 0.0f && std::fabs(o[2] + 500.0f) < 0.01f);
        CHECK(std::fabs(s - std::sqrt(1100.0f * 1100.0f + 500.0f * 500.0f)) < 0.05f);
        GrenadeHoldVel(err, 0.0f, hv, 0.0f, o);  // dt non valido: 11 ms, senza tetto
        CHECK(std::fabs(o[0] - 1100.0f) < 0.01f);
        const float big[3] = {100.0f, 0.0f, 0.0f}, still[3] = {0.0f, 0.0f, 0.0f};
        CHECK(GrenadeHoldVel(big, 0.007f, still, 2000.0f, o) == 2000.0f && std::fabs(o[0] - 2000.0f) < 0.01f);  // tetto 40 m/s
        const float bad[3] = {std::nanf(""), 1.0f, 0.0f};
        GrenadeHoldVel(bad, 0.01f, still, 2000.0f, o);
        CHECK(o[0] == 0.0f && std::fabs(o[1] - 100.0f) < 0.01f);
    }
    {
        const float hand[3] = {400.0f, 0.0f, 300.0f}, still[3] = {0.0f, 0.0f, 0.0f};  // 10 m/s
        const float s = GrenadeThrowVel(hand, still, 1.3f, 1300.0f, o);
        CHECK(std::fabs(s - 650.0f) < 0.01f && std::fabs(o[0] - 520.0f) < 0.01f && std::fabs(o[2] - 390.0f) < 0.01f);
        const float fast[3] = {1500.0f, 0.0f, 0.0f};  // 30 m/s x 1,3: tetto 26 m/s
        CHECK(GrenadeThrowVel(fast, still, 1.3f, 1300.0f, o) == 1300.0f && std::fabs(o[0] - 1300.0f) < 0.01f);
        CHECK(GrenadeThrowVel(hand, still, std::nanf(""), 0.0f, o) == 500.0f);  // throwboost non valido: 1
        // Revisione: Walker corre a 6 m/s (300 unita'/s) e la lascia senza
        // muovere la mano: parte a 6 m/s, non 7,8 (throwboost solo sulla mano).
        const float run[3] = {300.0f, 0.0f, 0.0f};
        CHECK(std::fabs(GrenadeThrowVel(still, run, 1.3f, 1300.0f, o) - 300.0f) < 0.01f && std::fabs(o[0] - 300.0f) < 0.01f);
        // Correndo con un lancio da 10 m/s in avanti: 13 + 6 m/s.
        const float fwd[3] = {500.0f, 0.0f, 0.0f};
        CHECK(std::fabs(GrenadeThrowVel(fwd, run, 1.3f, 1300.0f, o) - 950.0f) < 0.01f);
        // Il tetto vale sulla mano, rispetto a Walker: 26 m/s + la corsa.
        CHECK(std::fabs(GrenadeThrowVel(fast, run, 1.3f, 1300.0f, o) - 1600.0f) < 0.01f);
        const float badBase[3] = {std::nanf(""), 0.0f, 0.0f};
        CHECK(std::fabs(GrenadeThrowVel(hand, badBase, 1.3f, 1300.0f, o) - 650.0f) < 0.01f);  // base non finita: 0
    }
    // Miccia minima per angoli e presa (revisione): 1 s.
    CHECK(GrenadeFuseGrabbable(3.8f, 1.0f) && GrenadeFuseGrabbable(1.0f, 1.0f));
    CHECK(!GrenadeFuseGrabbable(0.9f, 1.0f) && !GrenadeFuseGrabbable(0.0f, 1.0f));  // stun a terra (0,9 s), fosforo
    CHECK(GrenadeFuseGrabbable(-1.0f, 1.0f));  // miccia non letta: come prima
    CHECK(!GrenadeFuseGrabbable(std::nanf(""), 1.0f));
    CHECK(GrenadeOnGround(1, false, 10.0f, 200.0f));
    CHECK(GrenadeOnGround(0, true, 150.0f, 200.0f));
    CHECK(!GrenadeOnGround(0, false, 0.0f, 200.0f));  // ancora in volo (mai rimbalzata)
    CHECK(!GrenadeOnGround(3, false, 230.0f, 200.0f));  // tra due rimbalzi
    CHECK(!GrenadeOnGround(3, true, std::nanf(""), 200.0f));
    CHECK(GrenadePredTime(102.5f, 100.0f, 4.0f) == 2.5f);
    CHECK(GrenadePredTime(99.0f, 100.0f, 4.0f) == 0.0f && GrenadePredTime(110.0f, 100.0f, 4.0f) == 4.0f);
    CHECK(GrenadePredTime(std::nanf(""), 100.0f, 4.0f) == 0.0f && GrenadePredTime(102.0f, 100.0f, 0.0f) == 0.0f);
}

// Rumore per distrarre (voce 9): fattore del raggio d'udito dall'energia dell'urto.
static void TestPropNoise() {
    CHECK(std::fabs(PropNoiseLoudness(0.73f, 11.0f, false) - std::sqrt(0.73f * 121.0f / 80.0f)) < 1e-4f);  // bottiglia lanciata: 44 J
    CHECK(std::fabs(PropNoiseLoudness(0.73f, 8.0f, true) - 1.5f * std::sqrt(0.73f * 64.0f / 80.0f)) < 1e-4f);  // rotta: x1,5
    CHECK(PropNoiseLoudness(0.3f, 4.0f, false) == 0.5f);  // lattina: il minimo
    CHECK(PropNoiseLoudness(5.14f, 11.2f, false) == 2.0f);  // radio: il massimo senza vetro
    CHECK(PropNoiseLoudness(5.0f, 15.0f, true) == 2.5f);    // massimo col vetro
    CHECK(PropNoiseLoudness(0.1f, 3.0f, true) == 0.75f);    // il minimo, rotto
    CHECK(PropNoiseLoudness(-1.0f, 5.0f, false) == 1.0f && PropNoiseLoudness(1.0f, std::nanf(""), true) == 1.5f &&
          PropNoiseLoudness(INFINITY, 5.0f, false) == 1.0f && PropNoiseLoudness(1.0f, 0.0f, false) == 1.0f);
    // Cresce con l'energia (tra i limiti).
    bool mono = true;
    float last = 0.0f;
    for (int i = 1; i <= 100; ++i) {
        const float l = PropNoiseLoudness(1.0f, 0.3f * static_cast<float>(i), false);
        if (l < last) mono = false;
        last = l;
    }
    CHECK(mono && last == 2.0f);
}

// --- Angoli delle pose, palmo, filtro, migrazioni calcolate, passi del menu (25/10) ---

static const double kDeg = 57.29577951308232;

// Rotazione casuale uniforme (quaternione normalizzato) come matrice OpenVR:
// indipendente da PoseFromEuler, cosi' il giro non si conferma da solo.
static void RandomPose(std::mt19937& rng, float m[3][4]) {
    std::normal_distribution<double> n(0.0, 1.0);
    double w = n(rng), x = n(rng), y = n(rng), z = n(rng);
    const double l = std::sqrt(w * w + x * x + y * y + z * z);
    w /= l; x /= l; y /= l; z /= l;
    m[0][0] = float(1 - 2 * (y * y + z * z)); m[0][1] = float(2 * (x * y - w * z)); m[0][2] = float(2 * (x * z + w * y));
    m[1][0] = float(2 * (x * y + w * z)); m[1][1] = float(1 - 2 * (x * x + z * z)); m[1][2] = float(2 * (y * z - w * x));
    m[2][0] = float(2 * (x * z - w * y)); m[2][1] = float(2 * (y * z + w * x)); m[2][2] = float(1 - 2 * (x * x + y * y));
    std::uniform_real_distribution<float> p(-2.0f, 2.0f);
    for (int i = 0; i < 3; ++i) m[i][3] = p(rng);
}

// Errore (gradi) della vista composta da FRotationMatrix con gli angoli dati
// rispetto alla posa vera, negli assi di UE3.
static double ViewErrorDeg(const float m[3][4], float pitch, float yaw, float roll) {
    float want[3][3], got[3][3];
    UeAxesFromPose(m, want[0], want[1], want[2]);
    UeAxesFromRotator(pitch, yaw, roll, got[0], got[1], got[2]);
    return AxesAngleDeg(want, got);
}

static void TestEulerRoundTrip() {
    std::mt19937 rng(25102026u);
    double worst = 0.0, worstBack = 0.0;
    int tested = 0;
    // 10k pose di testa e controller: yaw +-180, pitch +-89.5, roll +-60
    // (piano WI-0), poi altre 10k con roll +-90 e pitch +-75 (test T1 del bug 7).
    for (int pass = 0; pass < 2; ++pass) {
        const double maxPitch = pass == 0 ? 89.5 : 75.0, maxRoll = pass == 0 ? 60.0 : 90.0;
        int n = 0;
        while (n < 10000) {
            float m[3][4];
            RandomPose(rng, m);
            Euler e = ExactEuler(m);
            if (std::fabs(e.pitch * kDeg) > maxPitch || std::fabs(e.roll * kDeg) > maxRoll) continue;
            ++n;
            worst = std::fmax(worst, ViewErrorDeg(m, e.pitch, e.yaw, e.roll));
            float back[3][4];
            PoseFromEuler(e.pitch, e.yaw, e.roll, back);
            worstBack = std::fmax(worstBack, PoseAngleDeg(m, back));
        }
        tested += n;
    }
    std::printf("euler round trip: %d poses, max view error %.5f deg, PoseFromEuler max %.5f deg\n", tested, worst, worstBack);
    CHECK(tested == 20000);
    CHECK(worst < 0.01);
    CHECK(worstBack < 0.01);
    // Angoli noti.
    float m[3][4];
    PoseFromEuler(-30.0f / float(kDeg), 40.0f / float(kDeg), 10.0f / float(kDeg), m);
    Euler e = ExactEuler(m);
    CHECK(std::fabs(e.pitch * kDeg + 30.0) < 1e-3 && std::fabs(e.yaw * kDeg - 40.0) < 1e-3 && std::fabs(e.roll * kDeg - 10.0) < 1e-3);
    // Stesso pitch di PitchFromOpenVrPose (asin) e yaw uguale al vecchio a testa dritta.
    CHECK(std::fabs(AsinPitch(m) - e.pitch) < 1e-6f);
    PoseFromEuler(-35.0f / float(kDeg), 70.0f / float(kDeg), 0.0f, m);
    CHECK(std::fabs(WrapPi(BlendedYaw(m) - ExactEuler(m).yaw)) < 1e-5f);
}

// Baseline (bug 2 e 6): lo yaw miscelato di YawFromOpenVrPose con la testa o
// il controller inclinati di lato. Questi controlli documentano il difetto del
// codice di oggi (BlendedYaw resta per le direzioni, non per la vista).
static void TestBlendedYawBaseline() {
    struct Case {
        float pitch, yaw, roll;
        double minErr, maxErr;
    } cases[] = {{-30, 0, 10, 4.40, 4.55}, {-30, 40, 10, 4.40, 4.55}, {-20, 0, 30, 9.25, 9.40}, {0, 0, 30, 0.0, 0.001}};
    for (const Case& c : cases) {
        float m[3][4];
        PoseFromEuler(c.pitch / float(kDeg), c.yaw / float(kDeg), c.roll / float(kDeg), m);
        double oldErr = ViewErrorDeg(m, AsinPitch(m), BlendedYaw(m), std::atan2(-m[1][0], m[1][1]));
        Euler e = ExactEuler(m);
        double newErr = ViewErrorDeg(m, e.pitch, e.yaw, e.roll);
        std::printf("yaw baseline pitch %.0f yaw %.0f roll %.0f: blended (today) %.3f deg, exact %.5f deg\n", c.pitch, c.yaw,
                    c.roll, oldErr, newErr);
        CHECK(oldErr >= c.minErr && oldErr <= c.maxErr);
        CHECK(newErr < 0.01);
    }
}

static void TestPalmOf() {
    // openxr_grip dei modelli di SteamVR (oculus_quest2_controller_left/right.json,
    // oculus_quest_plus_controller_*.json): origine in metri, rotate_xyz (20.6, 0, 0).
    const float jsonOrigin[2][3] = {{0.007f, -0.00182941f, 0.1019482f}, {-0.007f, -0.00182941f, 0.1019482f}};
    std::mt19937 rng(7u);
    double worstJson = 0.0, worstShim = 0.0, worstOff = 0.0, worstRot = 0.0;
    for (int i = 0; i < 2000; ++i) {
        for (int h = 0; h < 2; ++h) {
            const bool left = h == 0;
            float raw[3][4];
            RandomPose(rng, raw);
            // 1) Palmo = raw * openxr_grip (origine del componente).
            float palm[3], want[3];
            PalmOf(raw, left, true, palm);
            for (int k = 0; k < 3; ++k)
                want[k] = raw[k][3] + raw[k][0] * jsonOrigin[h][0] + raw[k][1] * jsonOrigin[h][1] + raw[k][2] * jsonOrigin[h][2];
            double d = 0.0;
            for (int k = 0; k < 3; ++k) d += (palm[k] - want[k]) * (palm[k] - want[k]);
            worstJson = std::fmax(worstJson, std::sqrt(d));
            // 2) Catena del backend OpenXR: posa grip G -> raw = G * inverse(GripInRaw): il palmo torna l'origine di G.
            float grip[3][4], c[3][4], inv[3][4], rawShim[3][4];
            RandomPose(rng, grip);
            GripInRaw(left, kGripPitchDeg, c);
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) inv[r][k] = c[k][r];
            for (int r = 0; r < 3; ++r) inv[r][3] = -(inv[r][0] * c[0][3] + inv[r][1] * c[1][3] + inv[r][2] * c[2][3]);
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 4; ++k) {
                    float v = grip[r][0] * inv[0][k] + grip[r][1] * inv[1][k] + grip[r][2] * inv[2][k];
                    if (k == 3) v += grip[r][3];
                    rawShim[r][k] = v;
                }
            PalmOf(rawShim, left, true, palm);
            d = 0.0;
            for (int k = 0; k < 3; ++k) d += (palm[k] - grip[k][3]) * (palm[k] - grip[k][3]);
            worstShim = std::fmax(worstShim, std::sqrt(d));
            // La rotazione di raw * GripInRaw torna quella di G (rotate_xyz 20.6).
            float back[3][4];
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 4; ++k) {
                    float v = rawShim[r][0] * c[0][k] + rawShim[r][1] * c[1][k] + rawShim[r][2] * c[2][k];
                    if (k == 3) v += rawShim[r][3];
                    back[r][k] = v;
                }
            worstRot = std::fmax(worstRot, PoseAngleDeg(back, grip));
            // 3) openxrgrip off: la posa e' gia' la grip, il palmo e' la sua origine.
            PalmOf(grip, left, false, palm);
            d = 0.0;
            for (int k = 0; k < 3; ++k) d += (palm[k] - grip[k][3]) * (palm[k] - grip[k][3]);
            worstOff = std::fmax(worstOff, std::sqrt(d));
        }
    }
    std::printf("palm: vs openxr_grip %.4f mm, OpenXR chain %.4f mm (rotation %.4f deg), openxrgrip off %.4f mm\n",
                worstJson * 1000.0, worstShim * 1000.0, worstRot, worstOff * 1000.0);
    CHECK(worstJson < 0.001);
    CHECK(worstShim < 0.001);
    CHECK(worstRot < 0.01);
    CHECK(worstOff < 0.001);
    // A riposo (raw = identita') il palmo e' 10 cm dietro e 7 mm verso l'interno.
    float id[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, p[3];
    PalmOf(id, true, true, p);
    CHECK(std::fabs(p[0] - 0.007f) < 1e-6f && std::fabs(p[2] - 0.1019482f) < 1e-6f);
    PalmOf(id, false, true, p);
    CHECK(std::fabs(p[0] + 0.007f) < 1e-6f);
}

// Quaternione dei log (viewlog): la matrice si ricostruisce identica.
static void TestQuatFromPose() {
    std::mt19937 rng(11u);
    double worst = 0.0;
    for (int i = 0; i < 5000; ++i) {
        float m[3][4], q[4], back[3][4];
        RandomPose(rng, m);
        QuatFromPose(m, q);
        const double w = q[0], x = q[1], y = q[2], z = q[3];
        back[0][0] = float(1 - 2 * (y * y + z * z)); back[0][1] = float(2 * (x * y - w * z)); back[0][2] = float(2 * (x * z + w * y));
        back[1][0] = float(2 * (x * y + w * z)); back[1][1] = float(1 - 2 * (x * x + z * z)); back[1][2] = float(2 * (y * z - w * x));
        back[2][0] = float(2 * (x * z - w * y)); back[2][1] = float(2 * (y * z + w * x)); back[2][2] = float(1 - 2 * (x * x + y * y));
        worst = std::fmax(worst, PoseAngleDeg(m, back));
    }
    CHECK(worst < 0.01);
}

static void TestWrapPi() {
    CHECK(std::fabs(WrapPi(3.0f * kPi) - kPi) < 1e-4f || std::fabs(WrapPi(3.0f * kPi) + kPi) < 1e-4f);
    CHECK(std::fabs(WrapPi(0.5f + 4.0f * kPi) - 0.5f) < 1e-4f);
    CHECK(std::fabs(WrapPi(-0.5f - 2.0f * kPi) + 0.5f) < 1e-4f);
    CHECK(std::fabs(WrapPi(179.0f / float(kDeg) - (-179.0f / float(kDeg))) + 2.0f / float(kDeg)) < 1e-4f);
}

static void TestOneEuro() {
    OneEuro f;
    f.minCutoff = 1.0f;
    f.beta = 0.0f;
    CHECK(f.Step(5.0f, 0.011f) == 5.0f);  // primo campione: passa com'e'
    for (int i = 0; i < 100; ++i) f.Step(5.0f, 0.011f);
    CHECK(std::fabs(f.x - 5.0f) < 1e-5f);
    // Rumore bianco 0,5 rms su un valore fermo: fuori molto meno.
    std::mt19937 rng(3u);
    std::normal_distribution<float> n(0.0f, 0.5f);
    OneEuro g;
    g.minCutoff = 1.5f;
    g.beta = 0.0f;
    double s2 = 0.0;
    int cnt = 0;
    for (int i = 0; i < 2000; ++i) {
        float y = g.Step(n(rng), 1.0f / 90.0f);
        if (i > 200) {
            s2 += y * y;
            ++cnt;
        }
    }
    const double rms = std::sqrt(s2 / cnt);
    // Rampa veloce (180/s) con beta: il ritardo resta piccolo.
    OneEuro r;
    r.minCutoff = 1.5f;
    r.beta = 0.5f;
    float lag = 0.0f;
    for (int i = 0; i < 180; ++i) lag = i * 2.0f - r.Step(i * 2.0f, 1.0f / 90.0f);
    std::printf("one euro: noise 0.5 rms -> %.3f rms, 180/s ramp lag %.2f\n", rms, lag);
    CHECK(rms < 0.2);
    CHECK(lag >= 0.0f && lag < 4.0f);
    // dt nullo: stato azzerato, nessuna divisione per zero.
    CHECK(r.Step(1.0f, 0.0f) == 1.0f);
}

// Migrazione calcolata di esempio (non registrata in control.cpp): "aimpitch X"
// diverso da 0 diventa calcio con stockangle X + 39.4 (piano WI-4a).
static bool ExampleAimpitchToStock(const std::string& line, std::vector<std::string>* out) {
    std::istringstream in(line);
    std::string k;
    double v = 0.0;
    if (!(in >> k >> v) || v == 0.0) return false;
    out->push_back("holdmode gunstock");
    out->push_back("stockangle " + FormatConfigNumber(v + 39.4, 1));
    return true;
}

static void TestComputedMigration() {
    CHECK(FormatConfigNumber(15.4000001, 1) == "15.4");
    CHECK(FormatConfigNumber(-0.04, 1) == "0");
    CHECK(FormatConfigNumber(20.0, 2) == "20");
    CHECK(FormatConfigNumber(1.25, 2) == "1.25");
    static const ConfigChange kChanges[] = {{10, "seated off", "seated auto"}};
    static const ComputedChange kComputed[] = {{10, "aimpitch", &ExampleAimpitchToStock}};
    std::vector<std::string> lines = {"# calcio", "aimpitch -24.0", "seated off", "hud 0.4 100", "configversion 9"};
    std::string done;
    CHECK(MigrateConfigLines(lines, 10, kChanges, 1, &done, kComputed, 1));
    CHECK(lines.size() == 6);
    CHECK(lines.size() == 6 && lines[0] == "# calcio" && lines[1] == "holdmode gunstock" && lines[2] == "stockangle 15.4" &&
          lines[3] == "seated auto" && lines[4] == "hud 0.4 100" && lines[5] == "configversion 10");
    CHECK(done.find("aimpitch -24.0") != std::string::npos && done.find("stockangle 15.4") != std::string::npos);
    // aimpitch 0: la funzione non cambia nulla.
    lines = {"aimpitch 0", "configversion 9"};
    CHECK(MigrateConfigLines(lines, 10, kChanges, 1, &done, kComputed, 1));
    CHECK(lines[0] == "aimpitch 0" && lines[1] == "configversion 10");
    // Gia' alla 10: niente da fare (idempotente).
    lines = {"aimpitch -24.0", "configversion 10"};
    CHECK(!MigrateConfigLines(lines, 10, kChanges, 1, &done, kComputed, 1));
    CHECK(lines[0] == "aimpitch -24.0");
    // Senza cambi calcolati il comportamento e' quello di prima.
    lines = {"aimpitch -24.0", "seated off"};
    CHECK(MigrateConfigLines(lines, 10, kChanges, 1, &done));
    CHECK(lines[0] == "aimpitch -24.0" && lines[1] == "seated auto" && lines.back() == "configversion 10");
}

static void TestMenuSteps() {
    CHECK(StepConfigNumber("aimpitch", "0", 1) == "1.0");
    CHECK(StepConfigNumber("aimpitch", "-24.0", 1) == "-23.0");
    CHECK(StepConfigNumber("aimpitch", "0.5", -1) == "-0.5");
    CHECK(StepConfigNumber("openxrgrip", "20.6", 1) == "21.1");
    CHECK(StepConfigNumber("turnwithhead", "45", -1) == "40");
    CHECK(StepConfigNumber("throwboost", "1.3", 1) == "1.4");
    // Chiavi senza regola: come prima (0,05 sotto 1, 0,1 fino a 10, 1 sopra).
    CHECK(StepConfigNumber("hud", "0.4 100", 1) == "0.45 100");
    CHECK(StepConfigNumber("posscale", "50", 1) == "51");
    CHECK(StepConfigNumber("unknownkey", "2.5", -1) == "2.40");
    CHECK(MenuStepFor("aimpitch", 0.0f).step == 1.0f);
    // Passo fissato alla costruzione del menu (Item::step): avanti e indietro
    // tornano al valore di partenza anche attraversando 10 e 1 (review 25/10).
    CHECK(StepConfigNumber("unknownkey", "10", -1, 1.0f) == "9");
    CHECK(StepConfigNumber("unknownkey", "9", 1, 1.0f) == "10");
    CHECK(StepConfigNumber("unknownkey", "1.00", -1, 0.1f) == "0.90");
    CHECK(StepConfigNumber("unknownkey", "0.90", 1, 0.1f) == "1.00");
    // Angoli e centimetri a 0: un grado / mezzo centimetro a clic, non 0,05.
    CHECK(StepConfigNumber("armrot", "0 0 0", 1, 0.05f) == "1 0 0");
    CHECK(StepConfigNumber("armoffset", "0 0 0", -1, 0.05f) == "-0.5 0 0");
    CHECK(StepConfigNumber("gunoffset", "0 0 0", 1, 0.05f) == "0.5 0 0");
    // Chiavi di taratura del WI-1 (review): passi utili nel menu Advanced.
    CHECK(StepConfigNumber("xroverscan", "1.5", 1) == "1.75");
    CHECK(StepConfigNumber("dynresup", "0.65", -1) == "0.60");
    CHECK(StepConfigNumber("predictmax", "150", 1) == "160");
}

// --- WI-1 (25/10): rotazione della testa e consegna dei frame -------------------

// Vista come la compone la mod (UpdateHeadTracking + GetPlayerViewPoint): yaw
// esatto meno il riferimento, pitch asin, roll atan2, in unita' Unreal
// arrotondate; nello spazio del tracciamento la vista e' (pitch, rif + yaw,
// roll). Criterio del piano: vista disegnata contro testa vera < 0,01 gradi.
static void TestHeadViewUnits() {
    std::mt19937 rng(20261025u);
    std::uniform_real_distribution<float> refD(-3.14159f, 3.14159f);
    const float k = 6.283185307179586f / 65536.0f;
    double worstNew = 0.0, worstOld = 0.0;
    int n = 0;
    while (n < 10000) {
        float m[3][4];
        RandomPose(rng, m);
        Euler e = ExactEuler(m);
        if (std::fabs(e.pitch * kDeg) > 89.5 || std::fabs(e.roll * kDeg) > 60.0) continue;
        ++n;
        const float ref = refD(rng);
        const int32_t yawU = RadiansToUnrealRounded(WrapPi(e.yaw - ref));
        const int32_t pitchU = RadiansToUnrealRounded(AsinPitch(m));
        const int32_t rollU = RadiansToUnrealRounded(std::atan2(-m[1][0], m[1][1]));
        worstNew = std::fmax(worstNew, ViewErrorDeg(m, pitchU * k, ref + yawU * k, rollU * k));
        // Prima: yaw miscelato e unita' troncate.
        const int32_t yawOld = static_cast<int32_t>((WrapPi(BlendedYaw(m) - ref) / 6.283185307179586f) * 65536.0f);
        const int32_t pitchOld = static_cast<int32_t>((AsinPitch(m) / 6.283185307179586f) * 65536.0f);
        const int32_t rollOld = static_cast<int32_t>((std::atan2(-m[1][0], m[1][1]) / 6.283185307179586f) * 65536.0f);
        worstOld = std::fmax(worstOld, ViewErrorDeg(m, pitchOld * k, ref + yawOld * k, rollOld * k));
    }
    std::printf("head view in units: %d poses, max drawn vs head %.5f deg (before WI-1: %.3f deg)\n", n, worstNew, worstOld);
    CHECK(worstNew < 0.01);
    CHECK(worstOld > 4.0);
    CHECK(RadiansToUnrealRounded(-0.5f * 6.2831853f / 65536.0f * 1.2f) == -1);  // arrotondato, non troncato
}

// Rilevatore dei salti della vista (dissolvenza): volo a 3000 unita'/s con
// tick lunghi di 20-83 ms = nessun nero (prima: nero a ogni tick lungo), un
// teletrasporto di 17700 unita' si', un salto a piedi si'.
static void TestViewJump() {
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> hitch(0.020f, 0.083f), u(0.0f, 1.0f);
    for (int mount = 0; mount < 2; ++mount) {
        ViewJumpDetector d;
        int jumps = 0, oldJumps = 0;
        float prevDj = 0.0f;
        for (int i = 0; i < 5000; ++i) {
            const float dt = u(rng) < 0.15f ? hitch(rng) : 0.0139f;
            const float dj = 3000.0f * dt;
            jumps += d.Step(dj, dt, mount != 0) != ViewJumpDetector::kNone;
            oldJumps += dj > 80.0f && dj > 3.0f * prevDj + 40.0f;  // regola di prima
            prevDj = dj;
        }
        std::printf("view jump, flight at 3000 u/s with 20-83 ms ticks (%s): %d blinks (before WI-1: %d)\n",
                    mount ? "on a vehicle" : "firm ground", jumps, oldJumps);
        CHECK(jumps == 0);
        CHECK(oldJumps > 50);
        CHECK(d.Step(17700.0f, 0.0139f, mount != 0) == ViewJumpDetector::kTeleport);
    }
    ViewJumpDetector w;
    for (int i = 0; i < 50; ++i) CHECK(w.Step(5.0f, 0.0139f, false) == ViewJumpDetector::kNone);  // a piedi, ~360 u/s
    CHECK(w.Step(500.0f, 0.0139f, false) == ViewJumpDetector::kJump);                     // checkpoint vicino
    ViewJumpDetector v;
    for (int i = 0; i < 50; ++i) v.Step(42.0f, 0.0139f, true);
    CHECK(v.Step(500.0f, 0.0139f, true) == ViewJumpDetector::kNone);  // su un mezzo solo il teletrasporto
    // Review WI-1: a piedi un salto di 150-800 unita' in un tick lungo (scatto
    // con una riposizione scriptata) deve dare il nero; camminando o correndo
    // in un tick lungo no, e nemmeno un moto veloce continuo (caduta a 1500 u/s).
    int longJumps = 0, longWalk = 0, fall = 0, wi1Rule = 0;
    for (float dt : {0.1f, 0.2f, 0.4f, 0.8f}) {
        for (float dj : {150.0f, 300.0f, 800.0f}) {
            if (dj < 1200.0f * dt + 40.0f) continue;  // piu' corto della corsa in quel tempo
            ViewJumpDetector f;
            for (int i = 0; i < 20; ++i) f.Step(5.0f, 0.0139f, false);
            longJumps += f.Step(dj, dt, false) == ViewJumpDetector::kJump;
            const float d = dt > 0.25f ? 0.25f : dt;
            wi1Rule += dj / d > 3.0f * (5.0f / 0.0139f) + 3600.0f;  // sola regola delle velocita' (WI-1)
            ++longWalk;
        }
        ViewJumpDetector r;
        for (int i = 0; i < 20; ++i) r.Step(600.0f * 0.0139f, 0.0139f, false);
        fall += r.Step(600.0f * dt, dt, false) != ViewJumpDetector::kNone;  // corsa a 600 u/s nel tick lungo
        ViewJumpDetector c;
        for (int i = 0; i < 20; ++i) c.Step(1500.0f * 0.0139f, 0.0139f, false);
        fall += c.Step(1500.0f * dt, dt, false) != ViewJumpDetector::kNone;  // caduta a 1500 u/s
    }
    std::printf("view jump on foot in long ticks (0.1-0.8 s): %d of %d jumps blink (speed rule alone: %d); running or falling "
                "through a long tick: %d blinks\n",
                longJumps, longWalk, wi1Rule, fall);
    CHECK(longJumps == longWalk && longWalk >= 5);
    CHECK(wi1Rule < longWalk);
    CHECK(fall == 0);
}

// Passo della torretta: errore avvolto, guadagno al massimo 0,5, parte rifiutata al netto della base.
static void TestTurretMath() {
    const int32_t deg = 182;  // ~1 grado in unita'
    CHECK(std::fabs(TurretGain(0.083f) - 0.5f) < 1e-6f);
    CHECK(std::fabs(TurretGain(0.0139f) - (1.0f - std::exp(-0.0139f / 0.03f))) < 1e-6f);
    // Controller a -175 gradi, riferimento a +175: errore grezzo -350 gradi = +10 avvolto.
    const int32_t raw = static_cast<int32_t>((-350.0 / 360.0) * 65536.0);
    const int32_t step = TurretYawStep(raw, 0.0139f);
    CHECK(step > 0 && step < 10 * deg);
    CHECK(TurretYawStep(30 * deg, 0.2f) == 15 * deg);  // tick lunghissimo: meta' dell'errore
    // Il gioco porta il PlayerController con la base: niente rifiuto.
    CHECK(TurretRefused(1000, 1000 + 500, 500) == 0);
    // Limite: il gioco riporta indietro 300 unita' del passo, la base gira di +500.
    CHECK(TurretRefused(1000, 1000 + 500 - 300, 500) == 300);
    // Attraverso il giro (65535 -> 10).
    CHECK(TurretRefused(65530, 65536 + 10, 16) == 0);
    // Parte eseguita: al massimo il passo, nel suo verso.
    CHECK(TurretExecuted(500, 0) == 500 && TurretExecuted(500, 500) == 0 && TurretExecuted(500, 800) == 0);
    CHECK(TurretExecuted(500, -200) == 500 && TurretExecuted(-500, -100) == -400 && TurretExecuted(-500, 100) == -500);
    // Termine della base (review WI-1): 0 quando la base gira col passo (base = torretta).
    CHECK(TurretBaseFactor(500, 500) == 0.0f && TurretBaseFactor(500, 450) == 0.0f && TurretBaseFactor(-300, -310) == 0.0f);
    CHECK(TurretBaseFactor(500, 0) == 1.0f && TurretBaseFactor(500, -500) == 1.0f && TurretBaseFactor(500, 200) == 1.0f);
    CHECK(TurretBaseFactor(3, -7) == 1.0f && TurretBaseFactor(0, 40) == 1.0f && TurretBaseFactor(3, 4) == 0.0f);
}

// Modello a tick della torretta (test 8 del piano b1): testa ferma, colpo di
// polso di 30 gradi, elicottero che gira di 20 gradi/s, limite del gioco a
// +-45 gradi dalla base. Vista = corpo letto con la testa + (testa - rif).
// Con WI-1 la vista rispetto all'elicottero resta ferma (anche al limite) e la
// torretta raggiunge la mano; prima la vista girava col passo di ogni tick.
// baseIsTurret: la base del Pawn e' la torretta, che ruota col passo eseguito
// e non porta il PlayerController (caso non misurato, review WI-1). factorRule:
// termine della base: 0 = sempre 1 (come il WI-1), 1 = TurretBaseFactor, 2 =
// TurretBaseTerm, la regola del gioco (review 2 WI-1: Physics della base 7 per
// l'elicottero; base = torretta: Physics 7 anche lei, come lo YTurretActor della
// M2E2 misurato, ma 0 perche' e' la torretta stessa). follow (base = torretta): 0 segue il
// PlayerController nello stesso tick, 1 un tick dopo, 2 a 60 gradi/s al massimo.
static double TurretViewSwing(bool wi1, double ctrlDeg, double limitDeg, double* turretRelOut, bool baseIsTurret = false,
                              int factorRule = 1, int follow = 0) {
    const double u2d = 360.0 / 65536.0;
    const float dt = 0.0139f;
    int32_t pc = 0, body = 0, base = 0, ref = 0, head = 0;
    int32_t pcAfter = 0, baseAtWrite = 0, step = 0;
    bool pending = false;
    int32_t lastBody = 0;
    int32_t ownTurret = 0;
    int32_t prevPc = 0;  // follow 1: yaw del PlayerController al tick prima
    double viewMin = 1e9, viewMax = -1e9;
    for (int t = 0; t < 400; ++t) {
        const int32_t ctrl = t < 50 ? 0 : static_cast<int32_t>(ctrlDeg / u2d);
        // Inizio del tick: compensazione.
        if (wi1) {
            if (pending) {
                const int32_t bd = static_cast<int16_t>(base - baseAtWrite);
                const float k = factorRule == 0   ? 1.0f
                                : factorRule == 1 ? TurretBaseFactor(step, bd)
                                                  : TurretBaseTerm(kPhysInterpolating, step, bd, baseIsTurret);
                const int32_t refused = TurretRefused(pcAfter, pc, bd, k);
                ref += TurretExecuted(step, refused);
                pending = false;
            }
        } else if (ownTurret != 0) {
            const int32_t actual = static_cast<int16_t>(body - lastBody);
            int32_t comp = 0;
            if ((actual > 0) == (ownTurret > 0)) comp = std::abs(actual) < std::abs(ownTurret) ? actual : ownTurret;
            ref += comp;
            ownTurret = 0;
        }
        lastBody = body;
        // Posa della testa e vista di questo tick (corpo letto ora).
        const int32_t deltaYaw = head - ref;
        // Passo della torretta (dopo la testa).
        const int32_t aim = ctrl - ref;
        if (wi1) {
            step = TurretYawStep(aim, dt);
            pc += step;
            pcAfter = pc;
            baseAtWrite = base;
            pending = true;
        } else {
            const int32_t dy = static_cast<int32_t>(aim * (1.0f - std::exp(-dt / 0.03f)));
            pc += dy;
            body += dy;  // AddControllerYaw: anche il corpo della vista
            ownTurret = dy;
        }
        const int32_t view = body + deltaYaw;  // gli occhi leggono il corpo dopo il passo
        const double viewRel = static_cast<int16_t>(view - (baseIsTurret ? 0 : base)) * u2d;
        if (t > 40) {
            viewMin = std::fmin(viewMin, viewRel);
            viewMax = std::fmax(viewMax, viewRel);
        }
        if (baseIsTurret) {
            // Torretta a terra come base: ruota con lo yaw del PlayerController, che non porta.
            const int32_t rel = static_cast<int16_t>(pc), lim = static_cast<int32_t>(limitDeg / u2d);
            if (rel > lim) pc = lim;
            if (rel < -lim) pc = -lim;
            if (follow == 0) {
                base = pc;
            } else if (follow == 1) {
                base = prevPc;
                prevPc = pc;
            } else {
                const int32_t maxStep = static_cast<int32_t>(60.0 * dt / u2d);
                const int32_t d = static_cast<int16_t>(pc - base);
                base += d > maxStep ? maxStep : (d < -maxStep ? -maxStep : d);
            }
            body = pc;
            continue;
        }
        // Tick del gioco: l'elicottero gira, porta il PlayerController, limite rispetto alla base.
        const int32_t baseStep = static_cast<int32_t>(20.0 * dt / u2d);
        base += baseStep;
        pc += baseStep;
        const int32_t rel = static_cast<int16_t>(pc - base), lim = static_cast<int32_t>(limitDeg / u2d);
        if (rel > lim) pc = base + lim;
        if (rel < -lim) pc = base - lim;
        body = pc;
    }
    if (turretRelOut) *turretRelOut = static_cast<int16_t>(pc - (baseIsTurret ? 0 : base)) * u2d;
    return viewMax - viewMin;
}

static void TestTurretBookkeeping() {
    double relNew = 0.0, relOld = 0.0, relLimit = 0.0;
    const double swingNew = TurretViewSwing(true, 30.0, 45.0, &relNew);
    const double swingOld = TurretViewSwing(false, 30.0, 45.0, &relOld);
    const double swingLimit = TurretViewSwing(true, 120.0, 45.0, &relLimit);
    std::printf("turret model (helicopter turning 20 deg/s): view swing vs the helicopter %.3f deg with WI-1 (turret at %.1f), "
                "%.3f before; hand at 120 with a 45 deg limit: swing %.3f, turret at %.1f\n",
                swingNew, relNew, swingOld, swingLimit, relLimit);
    CHECK(swingNew < 0.02);
    CHECK(swingOld > 2.0);
    CHECK(swingLimit < 0.02);
    CHECK(std::fabs(relLimit - 45.0) < 0.1);
    // Base = la torretta stessa (review WI-1, M2E2 non misurata): col fattore 1
    // fisso tutto il passo risultava rifiutato e la vista girava col passo.
    double relT = 0.0, relT1 = 0.0;
    const double swingT = TurretViewSwing(true, 30.0, 45.0, &relT, true, true);
    const double swingT1 = TurretViewSwing(true, 30.0, 45.0, &relT1, true, false);
    std::printf("turret model, base = the turret (not carrying the PlayerController): view swing %.3f deg with the base factor "
                "rule, %.3f with the factor fixed at 1 (turret at %.1f / %.1f)\n",
                swingT, swingT1, relT, relT1);
    CHECK(swingT < 0.02);
    CHECK(swingT1 > 20.0);
    CHECK(std::fabs(relT - 30.0) < 0.5);
    // Review 2 WI-1: la base (torretta) segue il PlayerController un tick dopo o
    // a velocita' limitata. TurretBaseFactor vede solo la base nello stesso tick
    // e la vista girava; con la base = l'attore della torretta (Physics 7 come
    // lo YTurretActor della M2E2) il termine e' 0 in tutti i casi, e l'elicottero
    // (Physics 7, non la torretta) resta fermo.
    const char* const kFollow[3] = {"same tick", "one tick late", "60 deg/s"};
    for (int f = 0; f < 3; ++f) {
        double relG = 0.0, relR = 0.0;
        const double swingG = TurretViewSwing(true, 30.0, 45.0, &relG, true, 2, f);
        const double swingR = TurretViewSwing(true, 30.0, 45.0, &relR, true, 1, f);
        std::printf("turret model, base = the turret following %s: view swing %.3f deg with the game rule (turret at %.1f), "
                    "%.3f with TurretBaseFactor alone\n",
                    kFollow[f], swingG, relG, swingR);
        CHECK(swingG < 0.02);
        CHECK(std::fabs(relG - 30.0) < 0.5);
        if (f > 0) CHECK(swingR > 10.0);  // il caso del reviewer (18,8 / 35,8 gradi)
    }
    double relH = 0.0, relHL = 0.0;
    const double swingH = TurretViewSwing(true, 30.0, 45.0, &relH, false, 2);
    const double swingHL = TurretViewSwing(true, 120.0, 45.0, &relHL, false, 2);
    std::printf("turret model, helicopter (base Physics 7) with the game rule: view swing %.3f deg (turret at %.1f), "
                "hand at 120: %.3f deg (turret at %.1f)\n",
                swingH, relH, swingHL, relHL);
    CHECK(swingH < 0.02 && swingHL < 0.02);
    CHECK(std::fabs(relHL - 45.0) < 0.1);
    // Termine della base: la regola del gioco prima, TurretBaseFactor come guardia.
    CHECK(TurretBaseTerm(7, 500, 0) == 1.0f && TurretBaseTerm(7, 500, -500) == 1.0f && TurretBaseTerm(7, 500, 500) == 0.0f);
    CHECK(TurretBaseTerm(0, 500, 0) == 0.0f && TurretBaseTerm(0, 3, -7) == 0.0f && TurretBaseTerm(1, 500, 200) == 0.0f);
    CHECK(TurretBaseTerm(-1, 500, 200) == 1.0f && TurretBaseTerm(-1, 500, 500) == 0.0f);
    CHECK(TurretBaseTerm(7, 500, 200, true) == 0.0f && TurretBaseTerm(7, 500, -500, true) == 0.0f &&
          TurretBaseTerm(-1, 500, 0, true) == 0.0f);
}

// --- WI-4c: torretta, presa a manubrio, limiti (test 1-9 del piano b1) ---

// Corpo (avanti, destra, su; cm) -> spazio del tracciamento OpenVR (metri), con
// il corpo girato di yawDeg (+ = destra) attorno al perno (cm, nel corpo).
static void BarPoint(float f, float r, float u, float yawDeg, float pf, float pr, float out[3]) {
    const float a = yawDeg * 0.01745329252f, ca = std::cos(a), sa = std::sin(a);
    const float df = f - pf, dr = r - pr;
    const float rf = pf + df * ca - dr * sa, rr = pr + df * sa + dr * ca;
    // avanti (yaw a) = (sin a, 0, -cos a) nel tracciamento; destra = (cos a, 0, sin a)
    out[0] = rr * 0.01f;
    out[1] = u * 0.01f;
    out[2] = -rf * 0.01f;
}

static HandlebarIn BarInput(float lf, float lr, float lu, float rf, float rr, float ru, float yawDeg, float pf = 0.0f, float pr = 0.0f) {
    HandlebarIn in;
    in.dt = 0.0111f;
    in.allowed = true;
    in.rightValid = in.leftValid = true;
    BarPoint(lf, lr, lu, yawDeg, pf, pr, in.palmL);
    BarPoint(rf, rr, ru, yawDeg, pf, pr, in.palmR);
    const float a = yawDeg * 0.01745329252f;
    in.fwdR[0] = in.fwdL[0] = std::sin(a);
    in.fwdR[2] = in.fwdL[2] = -std::cos(a);
    in.gripR = in.gripL = true;
    in.oneYaw = 0.0f;
    return in;
}

static void TestHandlebar() {
    const double k = 57.29577951;
    // 1. Mani a (35, -18, -25) e (35, +18, -25): agganciato, yaw 0.
    {
        Handlebar hb;
        HandlebarOut o;
        for (int t = 0; t < 30; ++t) o = hb.Step(BarInput(35, -18, -25, 35, 18, -25, 0.0f));
        CHECK(o.engaged && o.weight == 1.0f && std::fabs(o.barYaw * k) < 0.1 && std::fabs(o.yaw * k) < 0.1);
        CHECK(std::fabs(o.sepM - 0.36f) < 1e-3f);
    }
    // 2. Rotazione rigida della coppia di +-20 / +-40 gradi attorno al corpo, a un
    // perno 35 cm davanti e a uno 30 cm dietro: yaw = rotazione +- 0,2.
    double worstPivot = 0.0;
    for (float pivot : {0.0f, 35.0f, -30.0f}) {
        for (float rot : {20.0f, -20.0f, 40.0f, -40.0f}) {
            Handlebar hb;
            HandlebarOut o;
            for (int t = 0; t < 40; ++t) o = hb.Step(BarInput(35, -18, -25, 35, 18, -25, rot, pivot, 0.0f));
            const double e = std::fabs(o.barYaw * k - rot);
            worstPivot = e > worstPivot ? e : worstPivot;
            CHECK(o.engaged && e < 0.2);
        }
    }
    // 3. Le due mani spostate di 10 cm a destra o in su: yaw invariato.
    {
        Handlebar hb;
        HandlebarOut o;
        for (int t = 0; t < 30; ++t) o = hb.Step(BarInput(35, -8, -25, 35, 28, -25, 0.0f));
        CHECK(o.engaged && std::fabs(o.barYaw * k) < 0.01);
        Handlebar hb2;
        for (int t = 0; t < 30; ++t) o = hb2.Step(BarInput(35, -18, -15, 35, 18, -15, 20.0f));
        CHECK(o.engaged && std::fabs(o.barYaw * k - 20.0) < 0.01);
    }
    // 4. Rifiuti: mani incrociate, posa in linea (sinistra 40 cm davanti),
    // distanza 10 / 100 cm, inclinazione 50 gradi, un grip solo; isteresi.
    {
        auto engagedAfter = [](const HandlebarIn& in) {
            Handlebar hb;
            HandlebarOut o;
            for (int t = 0; t < 20; ++t) o = hb.Step(in);
            return o.engaged;
        };
        CHECK(!engagedAfter(BarInput(35, 18, -25, 35, -18, -25, 0.0f)));   // sinistra a destra
        CHECK(!engagedAfter(BarInput(60, 0, -25, 20, 0, -25, 0.0f)));      // fucile: sinistra 40 cm davanti
        CHECK(!engagedAfter(BarInput(35, -5, -25, 35, 5, -25, 0.0f)));     // 10 cm
        CHECK(!engagedAfter(BarInput(35, -50, -25, 35, 50, -25, 0.0f)));   // 100 cm
        CHECK(!engagedAfter(BarInput(35, -15, -43, 35, 15, -7, 0.0f)));    // pendenza 50 gradi
        HandlebarIn one = BarInput(35, -18, -25, 35, 18, -25, 0.0f);
        one.gripL = false;
        CHECK(!engagedAfter(one));
        HandlebarIn na = BarInput(35, -18, -25, 35, 18, -25, 0.0f);
        na.allowed = false;
        CHECK(!engagedAfter(na));
        // Isteresi: aggancio sotto 90 cm, tenuta fino a 100; 22 cm tenuto, 22 cm non aggancia.
        Handlebar hb;
        HandlebarOut o;
        for (int t = 0; t < 10; ++t) o = hb.Step(BarInput(35, -18, -25, 35, 18, -25, 0.0f));
        o = hb.Step(BarInput(35, -47, -25, 35, 47, -25, 0.0f));
        CHECK(o.engaged);  // 94 cm: tenuto
        o = hb.Step(BarInput(35, -11, -25, 35, 11, -25, 0.0f));
        CHECK(o.engaged);  // 22 cm: tenuto
        CHECK(!engagedAfter(BarInput(35, -11, -25, 35, 11, -25, 0.0f)));  // 22 cm: non aggancia
        CHECK(!engagedAfter(BarInput(35, -47, -25, 35, 47, -25, 0.0f)));  // 94 cm: non aggancia
        // Controller girati di 40 gradi rispetto alla linea: tenuto (50), non agganciato (35).
        HandlebarIn tw = BarInput(35, -18, -25, 35, 18, -25, 0.0f);
        tw.fwdR[0] = tw.fwdL[0] = std::sin(0.698f);
        tw.fwdR[2] = tw.fwdL[2] = -std::cos(0.698f);
        CHECK(!engagedAfter(tw));
        o = hb.Step(tw);
        CHECK(o.engaged);
    }
    // 5. Mancini: il manubrio usa le mani FISICHE (hooks scambia arma/supporto),
    // quindi le stesse posizioni danno lo stesso yaw (qui: stessa funzione).
    {
        const float l[3] = {-0.18f, -0.25f, -0.35f}, r[3] = {0.18f, -0.25f, -0.35f};
        CHECK(std::fabs(HandlebarYawFromPalms(l, r)) < 1e-6f);
    }
    // 6. Tremolio +-2 mm sulle posizioni: deviazione dello yaw < 0,5 gradi.
    double stdYaw = 0.0, rawStd = 0.0;
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> n(-0.002f, 0.002f);
        Handlebar hb;
        std::vector<double> ys, raws;
        for (int t = 0; t < 600; ++t) {
            HandlebarIn in = BarInput(35, -18, -25, 35, 18, -25, 10.0f);
            for (int i = 0; i < 3; ++i) {
                in.palmL[i] += n(rng);
                in.palmR[i] += n(rng);
            }
            const HandlebarOut o = hb.Step(in);
            if (t > 60) {
                ys.push_back(o.barYaw * k);
                raws.push_back(HandlebarYawFromPalms(in.palmL, in.palmR) * k);
            }
        }
        auto sd = [](const std::vector<double>& v) {
            double m = 0.0, s = 0.0;
            for (double x : v) m += x;
            m /= v.size();
            for (double x : v) s += (x - m) * (x - m);
            return std::sqrt(s / v.size());
        };
        stdYaw = sd(ys);
        rawStd = sd(raws);
        CHECK(stdYaw < 0.5);
    }
    // 7. Beccheggio = media dei due; controller perso -> una mano in 100 ms,
    // passi piccoli con manubrio e mano quasi d'accordo; due grip -> manubrio.
    double maxFallStep = 0.0;
    float fallMs = 0.0f;
    {
        Handlebar hb;
        HandlebarIn in = BarInput(35, -18, -25, 35, 18, -25, 2.0f);
        in.pitchR = -0.10f;
        in.pitchL = -0.20f;
        in.oneYaw = 0.0f;
        in.onePitch = -0.10f;
        HandlebarOut o;
        for (int t = 0; t < 40; ++t) o = hb.Step(in);
        CHECK(std::fabs(o.pitch + 0.15f) < 1e-3f && std::fabs(o.yaw * k - 2.0) < 0.05);
        in.leftValid = false;
        double prev = o.yaw * k;
        int ticks = 0;
        do {
            o = hb.Step(in);
            maxFallStep = std::fmax(maxFallStep, std::fabs(o.yaw * k - prev));
            prev = o.yaw * k;
            ++ticks;
        } while (o.weight > 0.0f && ticks < 100);
        fallMs = ticks * in.dt * 1000.0f;
        CHECK(!o.engaged && o.yaw == in.oneYaw && o.pitch == in.onePitch);
        CHECK(maxFallStep < 1.0 && fallMs <= 0.1f * 1000.0f + 12.0f);
    }
    // 8b. Avvolgimento: coppia che gira da 170 a 190 gradi, yaw continuo.
    double maxWrapStep = 0.0;
    {
        Handlebar hb;
        double prev = 0.0;
        bool have = false;
        for (int t = 0; t <= 200; ++t) {
            const float rot = 170.0f + 0.1f * t;
            const HandlebarOut o = hb.Step(BarInput(35, -18, -25, 35, 18, -25, rot));
            const double y = o.barYaw * k;
            if (have) {
                double d = std::fmod(y - prev + 540.0, 360.0) - 180.0;
                maxWrapStep = std::fmax(maxWrapStep, std::fabs(d));
            }
            prev = y;
            have = true;
        }
        CHECK(maxWrapStep < 1.0);
        CHECK(std::fabs(std::fmod(prev + 360.0, 360.0) - 190.0) < 0.5);
    }
    // Grip mode: auto segue holdmode.
    CHECK(ResolveTurretGrip(0, 0) == 0 && ResolveTurretGrip(0, 1) == 1 && ResolveTurretGrip(1, 1) == 0 && ResolveTurretGrip(2, 0) == 1);
    std::printf("handlebar: pivot independence worst %.4f deg (body, 35 ahead, 30 behind; +-20/40), jitter +-2 mm yaw sd %.3f deg "
                "(raw %.3f), fallback to one hand %.0f ms with steps <= %.3f deg, wrap through 180 max step %.3f deg\n",
                worstPivot, stdYaw, rawStd, fallMs, maxFallStep, maxWrapStep);
}

// Review WI-4c round 1 (medio): dopo un buco (passo non chiamato per >= 100 ms)
// il peso non deve andare 0 -> 1 in un passo. Caso "normal_again": manubrio
// 24,66 gradi a destra della mano dell'arma, la presa torna NORMAL coi grip stretti.
static void TestHandlebarGap() {
    const double k = 57.29577951;
    // Parametri dalle chiavi: con i predefiniti uguali a quelli di prima.
    {
        const HandlebarParams a, b = HandlebarParamsFor(25.0f, 90.0f, 35.0f, 40.0f, 0.10f, 1.0f, 4.0f);
        CHECK(std::fabs(a.engageMinM - b.engageMinM) < 1e-6f && std::fabs(a.engageMaxM - b.engageMaxM) < 1e-6f &&
              std::fabs(a.holdMinM - b.holdMinM) < 1e-6f && std::fabs(a.holdMaxM - b.holdMaxM) < 1e-6f &&
              a.engageAlignDeg == b.engageAlignDeg && a.holdAlignDeg == b.holdAlignDeg && a.engageSlopeDeg == b.engageSlopeDeg &&
              a.holdSlopeDeg == b.holdSlopeDeg && a.blendSec == b.blendSec && a.minCutoffHz == b.minCutoffHz && a.beta == b.beta);
        const HandlebarParams c = HandlebarParamsFor(30.0f, 70.0f, 45.0f, 30.0f, 0.2f, 2.0f, 1.0f);
        CHECK(std::fabs(c.engageMinM - 0.30f) < 1e-6f && std::fabs(c.holdMinM - 0.25f) < 1e-6f && std::fabs(c.holdMaxM - 0.80f) < 1e-6f &&
              c.holdAlignDeg == 60.0f && c.holdSlopeDeg == 40.0f);
    }
    const float offset = 24.66f;
    auto run = [&](float firstDt, float* firstW, double* maxStep) {
        Handlebar hb;
        HandlebarIn in = BarInput(35, -18, -25, 35, 18, -25, 0.0f);
        in.oneYaw = static_cast<float>(-offset / k);
        double prev = in.oneYaw * k;
        *maxStep = 0.0;
        for (int t = 0; t < 30; ++t) {
            in.dt = t == 0 ? firstDt : 1.0f / 72.0f;
            const HandlebarOut o = hb.Step(in);
            if (t == 0) *firstW = o.weight;
            *maxStep = std::fmax(*maxStep, std::fabs(o.yaw * k - prev));
            prev = o.yaw * k;
        }
    };
    float w01 = 0.0f, wNom = 0.0f;
    double step01 = 0.0, stepNom = 0.0;
    run(0.1f, &w01, &step01);      // dt del buco limitato a 0,1 (hooks di prima): ora 1/30 nel passo
    run(0.011f, &wNom, &stepNom);  // hooks ora: dt nominale dopo un buco
    // Prima: peso 1 al primo passo, salto di tutto lo scarto.
    HandlebarParams old;
    old.maxDtSec = 1.0f;
    Handlebar ho;
    ho.p = old;
    HandlebarIn in0 = BarInput(35, -18, -25, 35, 18, -25, 0.0f);
    in0.oneYaw = static_cast<float>(-offset / k);
    in0.dt = 0.1f;
    const HandlebarOut oOld = ho.Step(in0);
    const double oldJump = std::fabs(oOld.yaw * k + offset);
    std::printf("handlebar after a gap (bar %.2f deg from the gun hand): first step weight %.3f with dt 0.1 (was %.3f, aim jump %.2f deg), "
                "max aim step per tick %.2f deg; hooks' nominal dt: weight %.3f, max step %.2f deg\n",
                offset, w01, oOld.weight, oldJump, step01, wNom, stepNom);
    CHECK(oOld.weight == 1.0f && oldJump > 24.0);
    CHECK(w01 < 0.3f && step01 < 7.0);
    CHECK(wNom < 0.05f && stepNom < 5.5);
}

// Torretta a terra tenuta per le maniglie (visore 27/10): mitragliatrice vera,
// maniglie dietro il perno: mani a sinistra -> canna a destra, mani giu' -> su.
static void TestTurretHandles() {
    namespace vp = vr::pure;
    const float d2r = 0.01745329252f, dt = 1.0f / 90.0f;
    vp::TurretHandles th;
    // Palmi a 15 cm, all'altezza di 1 m, 40 cm davanti (spazio OpenVR: -z avanti).
    float r[3] = {0.075f, 1.0f, -0.4f}, l[3] = {-0.075f, 1.0f, -0.4f};
    // Aggancio con la mira a una mano a +10 gradi: nessun salto.
    vp::TurretHandlesOut o = th.Step(true, r, l, 10.0f * d2r, 0.0f, dt, 0.35f);
    CHECK(o.event == 1 && o.engaged && Near(o.yaw, 10.0f * d2r, 1e-4f) && Near(o.pivotYaw, 10.0f * d2r, 1e-4f));
    for (int i = 0; i < 30; ++i) o = th.Step(true, r, l, 10.0f * d2r, 0.0f, dt, 0.35f);
    CHECK(Near(o.weight, 1.0f) && Near(o.yaw, 10.0f * d2r, 1e-4f) && Near(o.lever, 0.35f, 1e-4f));
    // Mani 10 cm a sinistra: la canna va a destra (yaw cresce), indipendente dalla mira a una mano.
    float r2[3] = {r[0] - 0.1f, r[1], r[2]}, l2[3] = {l[0] - 0.1f, l[1], l[2]};
    o = th.Step(true, r2, l2, -30.0f * d2r, 0.0f, dt, 0.35f);
    CHECK(o.yaw > 24.0f * d2r && o.yaw < 28.0f * d2r);  // 10 + circa 15,7
    // Mani 10 cm giu': la canna sale.
    float r3[3] = {r[0], r[1] - 0.1f, r[2]}, l3[3] = {l[0], l[1] - 0.1f, l[2]};
    o = th.Step(true, r3, l3, 0.0f, 0.0f, dt, 0.35f);
    CHECK(o.pitch > 14.0f * d2r && o.pitch < 17.0f * d2r);
    // Ruotare i polsi non conta: la funzione vede solo i palmi (stessi palmi, stessa mira).
    const float yBefore = o.yaw;
    o = th.Step(true, r3, l3, 50.0f * d2r, 20.0f * d2r, dt, 0.35f);
    CHECK(Near(o.yaw, yBefore, 1e-4f));
    // Rilascio: torna alla mira con una mano in 0,15 s senza salti; riagganciando il perno
    // torna davanti alle mani lungo la mira di quel momento.
    float prev = o.yaw, maxStep = 0.0f;
    int ticks = 0;
    for (; ticks < 40; ++ticks) {
        o = th.Step(false, r3, l3, 0.0f, 0.0f, dt, 0.35f);
        maxStep = (std::max)(maxStep, std::fabs(vp::WrapPi(o.yaw - prev)));
        prev = o.yaw;
        if (o.weight <= 0.0f) break;
    }
    CHECK(!o.engaged && o.weight <= 0.0f && Near(o.yaw, 0.0f, 1e-4f) && ticks <= 15 && maxStep < 2.5f * d2r);
    o = th.Step(true, r, l, 0.0f, 0.0f, dt, 0.35f);
    CHECK(o.event == 1 && Near(o.yaw, 0.0f, 1e-4f) && Near(o.pitch, 0.0f, 1e-4f));
    // Ricentramento assorbito: la mira tenuta gira con il riferimento, niente salto.
    for (int i = 0; i < 30; ++i) o = th.Step(true, r, l, 0.0f, 0.0f, dt, 0.35f);
    th.Reanchor(0.5f);
    float r4[3] = {0.3f, 1.0f, 0.2f}, l4[3] = {0.15f, 1.0f, 0.2f};  // le mani saltate con lo spazio
    o = th.Step(true, r4, l4, 0.5f, 0.0f, dt, 0.35f);
    CHECK(Near(o.yaw, 0.5f, 1e-4f));
    // Mani sul perno (sotto 5 cm): resta l'ultima mira.
    vp::TurretHandles t2;
    t2.Step(true, r, l, 0.0f, 0.0f, dt, 0.35f);
    float rp[3] = {0.075f, 1.0f, -0.75f}, lp[3] = {-0.075f, 1.0f, -0.75f};
    o = t2.Step(true, rp, lp, 0.0f, 0.0f, dt, 0.35f);
    CHECK(o.lever < 0.05f && Near(o.pivotYaw, 0.0f, 1e-4f));
}

// Review WI-4c round 1 (basso): colpetto al limite una volta, non a ogni
// attraversamento mentre il supporto si inclina in virata.
static void TestTurretLimitBuzz() {
    const double dt = 1.0 / 72.0;
    // Mano ferma vicino al limite, supporto che oscilla: margine -0,5..+1,5 gradi, periodo 1,5 s, 10 s.
    TurretLimitBuzz bz;
    int nNew = 0, nOld = 0;
    bool wasClamped = false;
    double lastBump = -10.0;
    for (int t = 0; t < 720; ++t) {
        const double now = t * dt;
        const float margin = static_cast<float>(0.5 + 1.0 * std::sin(now * 6.28318530718 / 1.5));
        const bool clamped = margin < 0.0f;
        nNew += bz.Step(clamped, margin, now) ? 1 : 0;
        if (clamped && !wasClamped && now - lastBump > 0.6) {
            lastBump = now;
            ++nOld;
        }
        wasClamped = clamped;
    }
    std::printf("turret limit buzz, hand still at the limit while the mount banks (10 s): %d buzz (old rule %d)\n", nNew, nOld);
    CHECK(nNew == 1 && nOld >= 5);
    // Fuori dal limite, poi 3 gradi dentro per 0,6 s, poi di nuovo al limite: due colpetti;
    // dentro solo 0,3 s: uno.
    for (double inside : {0.6, 0.3}) {
        TurretLimitBuzz b;
        int n = 0;
        double now = 0.0;
        for (; now < 0.5; now += dt) n += b.Step(true, -1.0f, now) ? 1 : 0;
        const double until = now + inside;
        for (; now < until; now += dt) n += b.Step(false, 3.0f, now) ? 1 : 0;
        for (int i = 0; i < 20; ++i, now += dt) n += b.Step(true, -1.0f, now) ? 1 : 0;
        CHECK(n == (inside > 0.5 ? 2 : 1));
    }
    // Margine dai limiti nel telaio del supporto.
    const TurretLimits L = kTurretLimitsFlight;
    CHECK(std::fabs(TurretLimitMarginDeg(48.0f, 0.0f, L) - 2.0f) < 1e-4f && std::fabs(TurretLimitMarginDeg(0.0f, -24.0f, L) - 1.0f) < 1e-4f &&
          TurretLimitMarginDeg(-70.0f, 0.0f, L) < 0.0f);
}

// Review WI-4c round 1 (alto): ogni suggerimento e le due guide della torretta
// entrano interi nell'avviso (stesso DrawText del visore), e la pagina Controls
// entra nel pannello anche sotto l'avviso di Virtual Desktop.
static void TestUiTextFits() {
    std::vector<std::string> texts(vr::ui::kHints, vr::ui::kHints + vr::ui::kHintCount);
    texts.push_back(vr::ui::kToastTurretNormal);
    texts.push_back(vr::ui::kToastTurretGunstock);
    // Round 2: suggerimenti della torretta e avvisi del calcio (prima mezze frasi in due avvisi).
    texts.push_back(vr::ui::kHintTurretNormal);
    texts.push_back(vr::ui::kHintTurretGunstock);
    texts.push_back(std::string(vr::ui::kToastMigrationKept) + "15.4 deg");
    texts.push_back(vr::ui::kToastMigrationNoStock);
    texts.push_back(vr::ui::kToastStockHint);
    texts.push_back(vr::ui::kToastSettingsReset);
    // Regressione finale del gameplay: i suggerimenti contestuali sugli oggetti, ogni variante.
    for (const char* t : {vr::ui::kObjHintNearGloves, vr::ui::kObjHintNearPlain, vr::ui::kObjHintHeldBoth, vr::ui::kObjHintHeldThrow,
                          vr::ui::kObjHintHeldShield, vr::ui::kObjHintMeleeBoth, vr::ui::kObjHintMeleeSwing, vr::ui::kObjHintMeleeDisarm,
                          vr::ui::kObjHintGrenadeText, vr::ui::kObjHintGunText, vr::ui::kObjHintExplosiveText})
        texts.push_back(t);
    int cut = 0, maxLines = 0, minPx = 99;
    for (const std::string& t : texts) {
        std::vector<uint32_t> px;
        vr::ui::ToastLayout l;
        CHECK(vr::ui::RenderToastBgra(t, &px, &l));
        CHECK(px.size() == static_cast<size_t>(vr::ui::kToastW) * vr::ui::kToastH);
        cut += l.fits ? 0 : 1;
        maxLines = l.lines > maxLines ? l.lines : maxLines;
        minPx = l.fontPx < minPx ? l.fontPx : minPx;
        std::printf("  toast %d px %d line(s) %4d x %3d %s: %s\n", l.fontPx, l.lines, l.textW, l.textH, l.fits ? "fits" : "CUT ", t.c_str());
    }
    // Le guide della torretta su una riga a 40 px.
    for (const char* t : {vr::ui::kToastTurretNormal, vr::ui::kToastTurretGunstock}) {
        std::vector<uint32_t> px;
        vr::ui::ToastLayout l;
        vr::ui::RenderToastBgra(t, &px, &l);
        CHECK(l.fits && l.fontPx == 40 && l.lines == 1);
    }
    HDC dc = CreateCompatibleDC(nullptr);
    // Il suggerimento 0 di prima su una riga sola: tagliato (il caso del tester).
    HFONT f = vr::ui::ToastFont(40);
    HGDIOBJ of = SelectObject(dc, f);
    const char* oldHint = "Aim with the gun in your right hand; left hand on the barrel (grip or rest it) = two-handed";
    SIZE sz = {};
    GetTextExtentPoint32A(dc, oldHint, static_cast<int>(std::strlen(oldHint)), &sz);
    CHECK(sz.cx > vr::ui::kToastW - 2 * vr::ui::kToastPadX);
    SelectObject(dc, of);
    DeleteObject(f);
    // Pagina Controls: 22 px, ogni riga entro 1040 px.
    HFONT m = CreateFontA(-vr::ui::kControlsFontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    of = SelectObject(dc, m);
    LONG widest = 0;
    for (size_t i = 0; i < vr::ui::kControlsCount; ++i) {
        GetTextExtentPoint32A(dc, vr::ui::kControls[i], static_cast<int>(std::strlen(vr::ui::kControls[i])), &sz);
        widest = sz.cx > widest ? sz.cx : widest;
        if (sz.cx > vr::ui::kControlsWidthPx) std::printf("FAIL controls line %ld px: %s\n", sz.cx, vr::ui::kControls[i]);
    }
    SelectObject(dc, of);
    DeleteObject(m);
    DeleteDC(dc);
    CHECK(widest <= vr::ui::kControlsWidthPx);
    // Con l'avviso di VD (topY 192) e senza (86), pannello alto 820.
    const int n = static_cast<int>(vr::ui::kControlsCount);
    const int pVd = vr::ui::ControlsRowPitch(192, 820, vr::ui::kControlsCount), pNo = vr::ui::ControlsRowPitch(86, 820, vr::ui::kControlsCount);
    const int bottomVd = 192 + n * pVd - 4, bottomNo = 86 + n * pNo - 4;
    CHECK(pNo == 52 && bottomNo <= 820 && bottomVd <= 820 && pVd >= 40);
    std::printf("ui text: %u toasts, %d cut, up to %d lines, smallest font %d px; Controls widest %ld of %d px, rows %d px (VD notice: %d px, "
                "last row ends at %d of 820; before: 52 px, 864)\n",
                static_cast<unsigned>(texts.size()), cut, maxLines, minPx, widest, vr::ui::kControlsWidthPx, pNo, pVd, bottomVd);
    CHECK(cut == 0 && maxLines <= 3);
    CHECK(vr::ui::ToastShowMs(30) == 3500 && vr::ui::ToastShowMs(90) == 6000 && vr::ui::ToastShowMs(200) == 7000);
}

// Review WI-4c round 2 (medio): la serie dei suggerimenti parla dell'arma in mano.
// Prologo (60 s sulla torretta NORMAL) e poi 60 s a piedi: sulla torretta solo il
// suggerimento della torretta, a piedi la serie dall'inizio (prima: 0, 1, 2, 4,
// 5, 6 sulla minigun, con "= two-handed", e il 3 rimandato a un "a piedi" che con
// le braccia non arrivava mai).
static void TestHintSequence() {
    using vr::ui::HintSequence;
    const double tickMs = 1000.0 / 72.0;
    HintSequence s;
    unsigned long long now = 1000000ull;  // ms, un passo per tick a 72 Hz
    auto steps = [&](double sec, bool gameplay, int turret, std::vector<std::string>* out, int* weapon) {
        const unsigned long long end = now + static_cast<unsigned long long>(sec * 1000.0);
        double acc = 0.0;
        while (now < end) {
            acc += tickMs;
            const unsigned long long d = static_cast<unsigned long long>(acc);
            acc -= static_cast<double>(d);
            now += d;
            bool w = false;
            if (const char* h = s.Step(now, gameplay, turret, &w)) {
                if (out) out->push_back(h);
                if (weapon) *weapon += w ? 1 : 0;
            }
        }
    };
    std::vector<std::string> turretShown, footShown;
    int turretWeapon = 0, footWeapon = 0;
    steps(60.0, true, 0, &turretShown, &turretWeapon);  // prologo: minigun, presa NORMAL
    steps(5.0, false, -1, nullptr, nullptr);            // schianto, caricamento
    steps(60.0, true, -1, &footShown, &footWeapon);     // a piedi
    CHECK(turretShown.size() == 1 && turretShown[0] == vr::ui::kHintTurretNormal && turretWeapon == 0);
    CHECK(footShown.size() == vr::ui::kHintCount && footWeapon == static_cast<int>(vr::ui::kHintCount));
    for (size_t i = 0; i < footShown.size() && i < vr::ui::kHintCount; ++i) CHECK(footShown[i] == vr::ui::kHints[i]);
    CHECK(s.WeaponDone());
    // Torretta col calcio a meta' serie: la serie si ferma, il suggerimento del calcio, poi riprende.
    s = HintSequence();
    std::vector<std::string> a, b, c;
    int wa = 0, wb = 0, wc = 0;
    steps(30.0, true, -1, &a, &wa);  // a 15, 22, 29 s: 0, 1, 2
    steps(30.0, true, 1, &b, &wb);   // torretta GUNSTOCK: solo il suo
    steps(30.0, true, 2, &b, &wb);   // torretta con lo stick: niente
    steps(40.0, true, -1, &c, &wc);  // di nuovo a piedi: 3, 4, 5, 6
    CHECK(a.size() == 3 && wa == 3 && a[0] == vr::ui::kHints[0]);
    CHECK(b.size() == 1 && wb == 0 && b[0] == vr::ui::kHintTurretGunstock);
    CHECK(c.size() == 4 && wc == 4 && c[0] == vr::ui::kHints[3] && c[3] == vr::ui::kHints[6]);
    std::printf("hints: prologue turret 60 s -> %u (turret hint only), on foot 60 s -> %u of %u weapon hints in order; "
                "mid-sequence gunstock turret -> %u turret hint, then %u more\n",
                static_cast<unsigned>(turretShown.size()), static_cast<unsigned>(footShown.size()),
                static_cast<unsigned>(vr::ui::kHintCount), static_cast<unsigned>(b.size()), static_cast<unsigned>(c.size()));
}

// Regressione finale del gameplay (27/10): suggerimenti contestuali sugli oggetti
// insieme alla serie dei comandi, come in vr_toast HintsTick con "hints auto":
// uno per situazione e una volta sola, il pericolo prima, mai due a meno di 7 s
// (anche con la serie dei comandi), niente sulla torretta o fuori dal gioco,
// niente (e non contato) con l'opzione spenta, testo secondo le opzioni.
static void TestObjHints() {
    using namespace vr::ui;
    const unsigned all = kObjOptGrab | kObjOptGloves | kObjOptThrowDmg | kObjOptShield | kObjOptMelee | kObjOptDisarm | kObjOptGrenade |
                         kObjOptGun | kObjOptExplosive;
    // Testi secondo le opzioni.
    CHECK(ObjHintText(kObjHintNear, all) == kObjHintNearGloves && ObjHintText(kObjHintNear, all & ~kObjOptGloves) == kObjHintNearPlain);
    CHECK(ObjHintText(kObjHintHeld, all) == kObjHintHeldBoth && ObjHintText(kObjHintHeld, all & ~kObjOptShield) == kObjHintHeldThrow &&
          ObjHintText(kObjHintHeld, all & ~kObjOptThrowDmg) == kObjHintHeldShield &&
          ObjHintText(kObjHintHeld, all & ~(kObjOptThrowDmg | kObjOptShield)) == nullptr);
    CHECK(ObjHintText(kObjHintMelee, all) == kObjHintMeleeBoth && ObjHintText(kObjHintMelee, all & ~kObjOptDisarm) == kObjHintMeleeSwing &&
          ObjHintText(kObjHintMelee, all & ~kObjOptMelee) == kObjHintMeleeDisarm &&
          ObjHintText(kObjHintMelee, all & ~(kObjOptMelee | kObjOptDisarm)) == nullptr);
    CHECK(ObjHintText(kObjHintGrenade, all & ~kObjOptGrenade) == nullptr && ObjHintText(kObjHintGun, all & ~kObjOptGun) == nullptr &&
          ObjHintText(kObjHintExplosive, all & ~kObjOptExplosive) == nullptr);
    for (int w = 0; w < kObjHintCount; ++w) CHECK(ObjHintText(w, all & ~kObjOptGrab) == nullptr);  // grabprops off: nessuno
    // Simulazione a 72 Hz: serie dei comandi + suggerimenti sugli oggetti (stesso ordine di HintsTick).
    struct Shown {
        double t;
        std::string text;
        int obj;
    };
    HintSequence seq;
    unsigned shownMask = 0;
    std::vector<Shown> out;
    double t = 0.0;
    const unsigned long long t0 = 5000000ull;
    auto run = [&](double sec, bool gameplay, int turret, unsigned sit, unsigned opt) {
        const double end = t + sec;
        for (; t < end; t += 1.0 / 72.0) {
            const unsigned long long now = t0 + static_cast<unsigned long long>(t * 1000.0);
            const int w = ObjHintStep(now, gameplay, turret >= 0, sit, opt, shownMask, seq.lastHint);
            if (w >= 0) {
                seq.lastHint = now;
                shownMask |= 1u << w;
                out.push_back({t, ObjHintText(w, opt), w});
            }
            if (const char* h = seq.Step(now, gameplay, turret)) out.push_back({t, h, -1});
        }
    };
    const unsigned near = 1u << kObjHintNear, held = 1u << kObjHintHeld, melee = 1u << kObjHintMelee, gren = 1u << kObjHintGrenade,
                   gun = 1u << kObjHintGun, expl = 1u << kObjHintExplosive;
    run(2.0, true, -1, 0, all);             // 0-2 s: niente
    run(1.0, true, -1, near, all);          // 2 s: mano vicino a un oggetto -> subito
    run(9.0, true, -1, held, all);          // 3-12 s: in mano -> a 9 s (7 s dopo)
    run(3.0, true, -1, 0, all);             // 12-15 s
    run(10.0, true, -1, held | expl | melee, all);  // 15-25 s: bombola in mano, nemico vicino: esplosiva prima, poi il nemico
    run(5.0, false, -1, gren, all);         // scena: niente
    run(20.0, true, 0, gren | gun, all);    // torretta: niente oggetti (solo il suggerimento della torretta)
    run(8.0, true, -1, gun, all & ~kObjOptGun);  // weaponpickup off: niente, non contato
    run(20.0, true, -1, near | held | gun | gren, all);  // granata e arma: una volta ciascuna; vicino e in mano gia' visti
    run(60.0, true, -1, near | held | melee | gun | gren | expl, all);  // tutto gia' visto: solo la serie dei comandi
    int objCount[kObjHintCount] = {};
    double minGap = 1e9;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].obj >= 0) ++objCount[out[i].obj];
        if (i) minGap = (std::min)(minGap, out[i].t - out[i - 1].t);
    }
    for (int w = 0; w < kObjHintCount; ++w) CHECK(objCount[w] == 1);
    CHECK(minGap >= 6.99);
    auto firstObj = [&](int w) {
        for (const Shown& s : out)
            if (s.obj == w) return s.t;
        return -1.0;
    };
    CHECK(firstObj(kObjHintNear) < 2.05 && std::fabs(firstObj(kObjHintHeld) - 9.0) < 0.05);
    CHECK(firstObj(kObjHintExplosive) < firstObj(kObjHintMelee) && firstObj(kObjHintExplosive) >= 15.0);
    // Granata (pericolo) prima dell'arma, solo dopo scena, torretta e opzione spenta (58 s), a 7 s dal comando di 52 s.
    CHECK(firstObj(kObjHintGrenade) >= 58.0 && firstObj(kObjHintGun) - firstObj(kObjHintGrenade) >= 6.99);
    int weaponHints = 0;
    for (const Shown& s : out) weaponHints += s.obj < 0 && s.text != kHintTurretNormal ? 1 : 0;
    CHECK(weaponHints == static_cast<int>(kHintCount));  // la serie dei comandi arriva comunque tutta
    std::printf("object hints: %u shown (near %.1f s, held %.1f, explosive %.1f, melee %.1f, grenade %.1f, gun %.1f), %u hints in all, "
                "smallest gap %.2f s\n",
                objCount[0] + objCount[1] + objCount[2] + objCount[3] + objCount[4] + objCount[5], firstObj(kObjHintNear),
                firstObj(kObjHintHeld), firstObj(kObjHintExplosive), firstObj(kObjHintMelee), firstObj(kObjHintGrenade),
                firstObj(kObjHintGun), static_cast<unsigned>(out.size()), minGap);
    // Fuori dal gioco, sulla torretta, senza situazioni o con tutto visto: -1.
    CHECK(ObjHintStep(t0, false, false, near, all, 0, 0) == -1 && ObjHintStep(t0, true, true, near, all, 0, 0) == -1 &&
          ObjHintStep(t0, true, false, 0, all, 0, 0) == -1 && ObjHintStep(t0, true, false, near, all, near, 0) == -1);
    CHECK(ObjHintStep(t0 + 6999, true, false, near, all, 0, t0) == -1 && ObjHintStep(t0 + 7000, true, false, near, all, 0, t0) == kObjHintNear);
}

// Review WI-4c round 2 (medio): grip stretti sulle maniglie della torretta NORMAL.
// Modello del gioco: il tasto che riceve = premuto && !tolto; conta i fronti di
// salita (una pressione nuova = ordine alla squadra / granata). Prima (prova a ogni
// lettura) le maniglie spente a meta' pressione davano un fronte.
static void TestGripStripLatch() {
    using vr::pure::GripStripLatch;
    struct Ev {
        bool pressed, handles, use;
        bool ext;  // un'altra regola lo toglie (es. i gesti per LB)
    };
    auto edges = [](const std::vector<Ev>& seq, bool latch) {
        GripStripLatch l;
        bool was = false;
        int n = 0;
        for (const Ev& e : seq) {
            const bool strip = latch ? l.Step(e.pressed, e.handles, e.use) : (e.handles || e.use);
            const bool out = e.pressed && !strip && !e.ext;
            if (latch) l.Delivered(out);
            n += (out && !was) ? 1 : 0;
            was = out;
        }
        return n;
    };
    auto seq = [](std::initializer_list<std::pair<int, Ev>> parts) {
        std::vector<Ev> v;
        for (const auto& p : parts)
            for (int i = 0; i < p.first; ++i) v.push_back(p.second);
        return v;
    };
    // 1. Pressione sulle maniglie, maniglie spente (fine torretta / pausa / dashboard /
    //    scena / controller persi / presa cambiata), poi rilascio: nessun fronte.
    const auto off = seq({{20, {false, true, false, false}}, {30, {true, true, false, false}}, {40, {true, false, false, false}}, {10, {false, false, false, false}}});
    CHECK(edges(off, false) == 1 && edges(off, true) == 0);
    // ... e una nuova pressione dopo il rilascio arriva (un fronte).
    auto off2 = off;
    for (int i = 0; i < 20; ++i) off2.push_back({true, false, false, false});
    CHECK(edges(off2, true) == 1);
    // 2. Pressione cominciata in pausa (maniglie spente): al menu del gioco, e alla
    //    ripresa resta com'e' (nessun rilascio a meta': granata).
    const auto pause = seq({{20, {true, false, false, false}}, {40, {true, true, false, false}}, {5, {false, true, false, false}}});
    {
        GripStripLatch l;
        int falls = 0;
        bool was = false;
        for (const Ev& e : pause) {
            const bool out = e.pressed && !l.Step(e.pressed, e.handles, e.use) && !e.ext;
            l.Delivered(out);
            falls += (was && !out && e.pressed) ? 1 : 0;
            was = out;
        }
        CHECK(falls == 0 && edges(pause, true) == 1);
    }
    // 3. Salire con il grip della presa d'uso (A), zona finita, maniglie dopo 1 s:
    //    il gioco non vede RB in nessun momento.
    const auto board = seq({{10, {true, false, true, false}}, {72, {true, false, false, false}}, {100, {true, true, false, false}}, {10, {false, true, false, false}}});
    CHECK(edges(board, false) == 1 && edges(board, true) == 0);
    // 4. Pressione mai vista dal gioco (tolta da un'altra regola, es. LB dei gesti) e
    //    maniglie che si accendono: tolta fino al rilascio.
    const auto late = seq({{10, {true, false, false, true}}, {10, {true, true, false, false}}, {30, {true, false, false, false}}});
    CHECK(edges(late, false) == 1 && edges(late, true) == 0);
    std::printf("grip latch: handles off mid-press %d -> %d new presses, boarding with the use grip %d -> %d, paused press kept to the "
                "menu (%d edge, 0 releases)\n",
                edges(off, false), edges(off, true), edges(board, false), edges(board, true), edges(pause, true));
}

// Yaw fermo con la canna quasi verticale: isteresi 70 / 65.
static void TestTurretYawHold() {
    TurretYawHold h;
    bool ch = false;
    CHECK(!h.Step(69.9f, &ch) && !ch);
    CHECK(h.Step(70.1f, &ch) && ch);
    CHECK(h.Step(68.0f, &ch) && !ch);     // sotto 70 ma sopra 65: ancora fermo
    CHECK(h.Step(-66.0f, &ch) && !ch);
    CHECK(!h.Step(64.9f, &ch) && ch);
    // Tremolio attorno a 70 (69-71): senza isteresi ~1 cambio per tick; ora uno solo.
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> n(69.0f, 71.0f);
    int changes = 0, oldChanges = 0;
    bool oldWas = false;
    h.Reset();
    for (int t = 0; t < 500; ++t) {
        const float p = n(rng);
        h.Step(p, &ch);
        changes += ch ? 1 : 0;
        const bool o = TurretYawHeld(p);
        oldChanges += o != oldWas ? 1 : 0;
        oldWas = o;
    }
    std::printf("turret yaw hold with the barrel trembling 69-71 deg: %d state changes in 500 ticks (was %d)\n", changes, oldChanges);
    CHECK(changes == 1 && oldChanges > 100);
}

// Limiti nel telaio del supporto (misurati nel prologo: yaw +50/-65, su +18).
static void TestTurretClamp() {
    const double u = 65536.0 / 360.0;
    auto U = [&](double deg) { return static_cast<int32_t>(std::lround(deg * u)); };
    const TurretLimits L = kTurretLimitsFlight;
    int32_t p = 0, y = 0;
    float ly = 0.0f, lp = 0.0f;
    // Supporto a yaw 30: mira a 100 -> 80 (destra +50), a -60 -> -35 (sinistra -65), dentro: invariata.
    const int32_t m0[3] = {0, U(30), 0};
    CHECK(ClampAimInMount(0, U(100), m0, L, &p, &y, &ly, &lp) && std::abs(y - U(80)) <= 2 && std::abs(p) <= 2);
    CHECK(ClampAimInMount(0, U(-60), m0, L, &p, &y) && std::abs(y - U(-35)) <= 2);
    CHECK(!ClampAimInMount(U(-10), U(60), m0, L, &p, &y) && y == U(60) && p == U(-10));
    // Beccheggio: +30 -> +18, -40 -> -25 (la mira del gioco si ferma li' con le scritture assolute).
    CHECK(ClampAimInMount(U(30), U(30), m0, L, &p, &y) && std::abs(p - U(18)) <= 2 && std::abs(y - U(30)) <= 2);
    CHECK(ClampAimInMount(U(-40), U(30), m0, L, &p, &y) && std::abs(p - U(-25)) <= 2);
    // Giri accumulati del PlayerController: stesso giro in uscita.
    CHECK(ClampAimInMount(0, 3 * 65536 + U(100), m0, L, &p, &y) && std::abs(y - (3 * 65536 + U(80))) <= 2);
    // Supporto inclinato come l'elicottero del campione (pitch 0,35, yaw -97,21,
    // roll -12,96): la mira del gioco allo stick (pitch 6,4, yaw -162,97) era
    // a +18 nel supporto: limitata appena sopra, ferma sotto.
    const int32_t mh[3] = {U(0.35), U(-97.21), U(-12.96)};
    ClampAimInMount(U(6.4), U(-162.97), mh, L, &p, &y, &ly, &lp);
    std::printf("turret clamp: prologue stick sample in the mount frame yaw %.2f pitch %.2f (game limits +50/-65, up 18)\n", ly, lp);
    CHECK(std::fabs(ly + 64.55) < 0.3 && std::fabs(lp - 18.02) < 0.3);
    CHECK(ClampAimInMount(U(10.0), U(-150.0), mh, L, &p, &y, &ly, &lp));
    float ly2 = 0.0f, lp2 = 0.0f;
    int32_t p2 = 0, y2 = 0;
    ClampAimInMount(p, y, mh, L, &p2, &y2, &ly2, &lp2);
    CHECK(lp2 <= 18.01f && lp2 >= 17.9f);
}

// Modello a tick con i limiti (test 8-9): elicottero che gira a 20 gradi/s e
// porta il PlayerController; gameLimit: il gioco riprende le spinte oltre
// quel limite (0 = mai, come a sinistra nel prologo); modClamp: limiti della
// mod (+50/-65 attorno al supporto = la base). Vista = corpo + (testa - rif).
static double TurretLimitModel(double ctrlDeg, double gameLimitDeg, bool modClamp, bool latch, double* turretRel, int* pushTicks,
                               int* latchOn) {
    const double u2d = 360.0 / 65536.0;
    const float dt = 0.0139f;
    int32_t pc = 0, base = 0, ref = 0, step = 0, pcAfter = 0, baseAtWrite = 0, errStep = 0;
    bool pending = false;
    TurretLimitLatch lt;
    double viewMin = 1e9, viewMax = -1e9;
    *pushTicks = 0;
    *latchOn = 0;
    for (int t = 0; t < 600; ++t) {
        const int32_t ctrl = t < 50 ? 0 : static_cast<int32_t>(ctrlDeg / u2d);
        int32_t refused = 0;
        if (pending) {
            const int32_t bd = static_cast<int16_t>(base - baseAtWrite);
            refused = TurretRefused(pcAfter, pc, bd, 1.0f);
            ref += TurretExecuted(step, refused);
            pending = false;
            if (latch && lt.Count(step, refused, errStep) != 0 && lt.dir != 0) ++*latchOn;
        }
        const int32_t view = pc + (0 - ref);  // testa ferma
        const double viewRel = static_cast<int16_t>(view - base) * u2d;
        if (t > 40) {
            viewMin = std::fmin(viewMin, viewRel);
            viewMax = std::fmax(viewMax, viewRel);
        }
        const int32_t aim = static_cast<int16_t>(ctrl - ref);
        int32_t tgtYaw = pc + aim, tgtPitch = 0;
        if (modClamp) {
            const int32_t mount[3] = {0, base, 0};
            int32_t cp = 0, cy = 0;
            ClampAimInMount(tgtPitch, tgtYaw, mount, kTurretLimitsFlight, &cp, &cy);
            tgtYaw = cy;
        }
        const int32_t err = static_cast<int16_t>(tgtYaw - pc);
        int32_t dy = TurretYawStep(err, dt);
        if (latch) dy = lt.Filter(dy, err, t * dt);
        pc += dy;
        step = dy;
        errStep = err;
        pcAfter = pc;
        baseAtWrite = base;
        pending = true;
        // Tick del gioco: l'elicottero gira e porta il PlayerController; il
        // gioco riprende cio' che supera il suo limite (solo spinte oltre).
        const int32_t bs = static_cast<int32_t>(20.0 * dt / u2d);
        base += bs;
        pc += bs;
        if (gameLimitDeg > 0.0) {
            const int32_t rel = static_cast<int16_t>(pc - base), lim = static_cast<int32_t>(gameLimitDeg / u2d);
            if (rel > lim) pc = base + lim;
        }
        if (std::abs(dy) > 9 && std::abs(static_cast<int16_t>(pc - base)) * u2d > gameLimitDeg - 0.01 && gameLimitDeg > 0.0) ++*pushTicks;
    }
    *turretRel = static_cast<int16_t>(pc - base) * u2d;
    return viewMax - viewMin;
}

static void TestTurretLimitsModel() {
    double rel = 0.0;
    int push = 0, on = 0;
    // Mano a +120 e -120, il gioco non riporta indietro: la mod si ferma ai suoi limiti, vista ferma.
    const double swR = TurretLimitModel(120.0, 0.0, true, true, &rel, &push, &on);
    CHECK(swR < 0.02 && std::fabs(rel - 50.0) < 0.2);
    const double relR = rel;
    const double swL = TurretLimitModel(-120.0, 0.0, true, true, &rel, &push, &on);
    CHECK(swL < 0.02 && std::fabs(rel + 65.0) < 0.2);
    const double relL = rel;
    // Senza i limiti della mod, a -120 la torretta va a -120 dalla base (come nel prologo).
    TurretLimitModel(-120.0, 0.0, false, false, &rel, &push, &on);
    CHECK(std::fabs(rel + 120.0) < 0.5);
    const double relNoClamp = rel;
    // Il gioco si ferma a +45 (prima dei +50 della mod): senza il blocco la mod
    // spinge a ogni tick, col blocco smette dopo 2 tick (prove ogni 0,5 s).
    int pushNo = 0, onNo = 0, pushLatch = 0, onLatch = 0;
    double relNo = 0.0, relLatch = 0.0;
    const double swNo = TurretLimitModel(120.0, 45.0, true, false, &relNo, &pushNo, &onNo);
    const double swLatch = TurretLimitModel(120.0, 45.0, true, true, &relLatch, &pushLatch, &onLatch);
    std::printf("turret limits model (helicopter 20 deg/s): hand +-120 -> turret %+.1f / %+.1f deg from the mount, view swing %.3f / %.3f "
                "(no mod limits: %+.1f); game stops at +45: push ticks %d without the latch, %d with it (latch on %d), view swing %.3f / %.3f\n",
                relR, relL, swR, swL, relNoClamp, pushNo, pushLatch, onLatch, swNo, swLatch);
    CHECK(swNo < 0.02 && swLatch < 0.02);
    CHECK(onLatch >= 1 && pushLatch * 5 < pushNo);
    // Blocco: scatta dopo 3 tick rifiutati con la mira ad almeno 1 grado oltre (isteresi, 27/10), si scioglie quando
    // l'errore torna indietro di 0,25 gradi o la prova passa.
    TurretLimitLatch lt;
    CHECK(lt.Count(200, 180, 500) == 0 && lt.Count(200, 190, 500) == 0 && lt.Count(200, 190, 500) == 1 && lt.dir == 1);
    CHECK(lt.Filter(200, 500, 10.0) == 200 && lt.probing);             // prima prova subito
    CHECK(lt.Filter(200, 500, 10.1) == 0);                            // poi niente spinta
    CHECK(lt.Filter(-150, -300, 10.2) == -150 && lt.dir == 0);         // errore tornato: libero
    TurretLimitLatch l2;
    l2.Count(-300, -280, -900);
    l2.Count(-300, -290, -900);
    l2.Count(-300, -290, -900);
    CHECK(l2.dir == -1);
    CHECK(l2.Filter(-300, -900, 20.0) == -300 && l2.Count(-300, 0, -900) == -2 && l2.dir == 0);  // prova accettata
    TurretLimitLatch l3;
    CHECK(l3.Count(200, 100, 500) == 0 && l3.Count(200, 180, 500) == 0 && l3.Count(-200, -190, -500) == 0 && l3.dir == 0);  // non di seguito
    CHECK(l3.Count(5, 5, 500) == 0 && l3.Count(5, 5, 500) == 0 && l3.Count(5, 5, 500) == 0 && l3.dir == 0);  // passi minuscoli non contano
}

// Risoluzione dinamica su un modello a gradini della curva del log del tester
// (VDXR 23/10, 120 Hz; non la curva registrata): GPU 5,0 ms all'80%, 8,5
// all'85% (misura contaminata), rumore +-5%. Prima: 80 <-> 85 ogni 0,7-1,4 s.
// Ora: dopo l'assestamento al piu' una prova ogni 20-320 s (salita e ridiscesa
// a ~2 s, quindi 2 cambi in 10 s), in media (regime) meno di 1 cambio ogni 10 s.
static float TesterGpuMs(int pct) {
    if (pct >= 100) return 12.0f;
    if (pct >= 95) return 10.8f;
    if (pct >= 90) return 9.8f;
    if (pct >= 85) return 8.5f;
    if (pct >= 80) return 5.0f;
    return 5.0f * (pct / 80.0f) * (pct / 80.0f);
}

static void TestDynRes() {
    const float budget = 1000.0f / 120.0f;
    std::mt19937 rng(3u);
    std::uniform_real_distribution<float> noise(0.95f, 1.05f);
    // Nuovo controllore.
    DynResController c;
    c.Reset(100, 0.0);
    std::vector<double> changes;
    int cur = 100;
    for (int i = 0; i < 120 * 600; ++i) {
        const double t = i / 120.0;
        c.AddSample(t, TesterGpuMs(cur) * noise(rng), cur);
        if (i % 12 == 0) {
            const int want = c.Update(t, budget);
            if (want != cur) {
                cur = want;
                c.Reset(cur, t);
                changes.push_back(t);
            }
        }
    }
    int after = 0, window = 0;
    for (size_t i = 0; i < changes.size(); ++i) {
        if (changes[i] >= 60.0) ++after;
        int w = 0;
        for (size_t j = i; j < changes.size() && changes[j] - changes[i] < 10.0; ++j) ++w;
        if (changes[i] >= 60.0) window = std::max(window, w);
    }
    // Vecchio controllore (EMA 0,1, ogni 0,7 s, banda 0,70-0,92, +5/-10).
    int oldCur = 100, oldChanges = 0;
    float ema = 0.0f;
    double last = 0.0;
    for (int i = 0; i < 120 * 600; ++i) {
        const double t = i / 120.0;
        const float g = TesterGpuMs(oldCur) * noise(rng);
        ema = ema <= 0.0f ? g : ema + 0.1f * (g - ema);
        if (t - last > 0.7) {
            last = t;
            if (ema > 0.92f * budget || (ema < 0.70f * budget && oldCur < 100)) {
                float next = oldCur * std::sqrt(0.85f * budget / ema);
                next = std::max(next, oldCur - 10.0f);
                next = std::min(next, oldCur + 5.0f);
                int want = std::max(65, std::min(100, static_cast<int>(next / 5.0f + 0.5f) * 5));
                if (want != oldCur) {
                    oldCur = want;
                    ++oldChanges;
                }
            }
        }
    }
    std::printf("dynres on a step model of the tester's curve, 600 s at 120 Hz: %d changes (%d after the first 60 s, %.2f per 10 s "
                "steady-state average, at most %d in any 10 s: a probe and its failure), ends at %d%%; before WI-1: %d changes\n",
                static_cast<int>(changes.size()), after, after / 54.0, window, cur, oldChanges);
    CHECK(after / 54.0 <= 1.0);
    CHECK(window <= 2);
    CHECK(cur == 80 || cur == 85);
    CHECK(oldChanges > 300);
    // Scena leggera: nessun cambio dal 100%.
    DynResController l;
    l.Reset(100, 0.0);
    int lightChanges = 0;
    for (int i = 0; i < 120 * 60; ++i) {
        const double t = i / 120.0;
        l.AddSample(t, 0.5f * budget * noise(rng), 100);
        if (i % 12 == 0 && l.Update(t, budget) != 100) ++lightChanges;
    }
    CHECK(lightChanges == 0);
    // Scena pesante dall'inizio: giu' entro pochi secondi, poi ferma.
    DynResController h;
    h.Reset(100, 0.0);
    int hc = 100, steps = 0;
    double firstDown = -1.0;
    for (int i = 0; i < 120 * 30; ++i) {
        const double t = i / 120.0;
        h.AddSample(t, 2.0f * budget * (hc / 100.0f) * (hc / 100.0f) * noise(rng), hc);
        if (i % 12 == 0) {
            const int want = h.Update(t, budget);
            if (want != hc) {
                if (firstDown < 0.0) firstDown = t;
                hc = want;
                h.Reset(hc, t);
                ++steps;
            }
        }
    }
    std::printf("dynres heavy scene (2x the budget at 100%%): first step down after %.1f s, %d steps, ends at %d%%\n", firstDown,
                steps, hc);
    CHECK(firstDown > 0.0 && firstDown <= 2.5);
    CHECK(hc <= 70);
    // GPU condivisa (collaudo con due copie del gioco, 25/10): di solito 0,37
    // volte il budget, ogni 4-12 s un picco di 0,3-0,6 s a 5 volte. Una prova
    // fallita torna al livello di prima: niente discese di due livelli.
    DynResController b;
    b.Reset(65, 0.0);
    int bc = 65, bChanges = 0, bWindow = 0, twoLevelDrops = 0;
    std::vector<double> bt;
    std::uniform_real_distribution<float> gap(4.0f, 12.0f), len(0.3f, 0.6f);
    double nextBurst = gap(rng), burstEnd = -1.0;
    for (int i = 0; i < 120 * 300; ++i) {
        const double t = i / 120.0;
        if (t >= nextBurst) {
            burstEnd = t + len(rng);
            nextBurst = burstEnd + gap(rng);
        }
        const float ms = (t < burstEnd ? 5.0f : 0.37f) * budget * noise(rng);
        b.AddSample(t, ms, bc);
        if (i % 12 == 0) {
            const int want = b.Update(t, budget);
            if (want != bc) {
                if (want <= bc - 10) ++twoLevelDrops;
                bc = want;
                b.Reset(bc, t);
                bt.push_back(t);
                ++bChanges;
            }
        }
    }
    for (size_t i = 0; i < bt.size(); ++i) {
        int w = 0;
        for (size_t j = i; j < bt.size() && bt[j] - bt[i] < 10.0; ++j) ++w;
        bWindow = std::max(bWindow, w);
    }
    std::printf("dynres with GPU bursts (5x the budget for 0.3-0.6 s every 4-12 s), 300 s: %d changes (%.2f per 10 s), at most %d "
                "in any 10 s, %d drops of two levels, ends at %d%%\n",
                bChanges, bChanges / 30.0, bWindow, twoLevelDrops, bc);
    CHECK(bChanges / 30.0 <= 1.0);
    CHECK(bWindow <= 2);
    CHECK(twoLevelDrops == 0);
    // Soglie dall'ini (review WI-1): i predefiniti sono quelli di prima; con
    // "dynresup 0.5" la scena leggera a 0,55 del budget non sale piu'.
    DynResController::Params def;
    CHECK(def.down == 1.0f && def.slow == 0.92f && def.up == 0.65f && def.dwell == 4.0 && def.backoff == 20.0);
    for (int pass = 0; pass < 2; ++pass) {
        DynResController u;
        if (pass == 1) u.p.up = 0.5f;
        u.Reset(80, 0.0);
        int uc = 80;
        for (int i = 0; i < 120 * 30; ++i) {
            const double t = i / 120.0;
            u.AddSample(t, 0.55f * budget, uc);
            if (i % 12 == 0) {
                const int want = u.Update(t, budget);
                if (want != uc) {
                    uc = want;
                    u.Reset(uc, t);
                }
            }
        }
        CHECK(pass == 0 ? uc > 80 : uc == 80);
    }
    // "dynresbackoff 5": il livello fallito torna disponibile dopo 5 s, non 20.
    DynResController k;
    k.p.backoff = 5.0;
    k.Reset(100, 0.0);
    double downAt = -1.0;
    for (int i = 0; i < 120 * 10 && downAt < 0.0; ++i) {
        const double t = i / 120.0;
        k.AddSample(t, 2.0f * budget, 100);
        if (i % 12 == 0 && k.Update(t, budget) != 100) downAt = t;
    }
    const DynResController::Fail& kf = k.fail[DynResController::Slot(100)];
    CHECK(downAt > 0.0 && std::fabs((kf.until - downAt) - 5.0) < 1e-6);
}

// "predictframe auto" (review WI-1): due finestre POSEAGE di fila a -1 frame
// accendono il frame in piu', una sola no; dopo il cambio la finestra seguente
// si salta; finestre con pochi frame in rotazione non contano; +1 frame lo spegne.
static void TestPredictFrameAuto() {
    const float frame = 1000.0f / 72.0f;
    PredictFrameAuto a;
    CHECK(!a.Window(-13.5f, 200, frame) && !a.on);    // una finestra
    CHECK(!a.Window(-1.0f, 200, frame) && !a.on);     // interrotta
    CHECK(!a.Window(-13.5f, 200, frame));
    CHECK(!a.Window(-13.5f, 10, frame) && !a.on);     // muta: pochi frame in rotazione
    CHECK(a.Window(-13.9f, 200, frame) && a.on);      // seconda di fila
    CHECK(!a.Window(+13.9f, 200, frame) && a.on);     // saltata dopo il cambio
    CHECK(!a.Window(+0.5f, 200, frame) && a.on);      // giusta: resta acceso
    CHECK(!a.Window(+13.0f, 200, frame) && a.on);
    CHECK(a.Window(+13.0f, 200, frame) && !a.on);     // un frame di troppo per due finestre: spento
    CHECK(!a.Window(std::nanf(""), 200, frame) && !a.on);
    PredictFrameAuto b;  // runtime che aspetta gia' giusto: mai acceso
    int changes = 0;
    std::mt19937 rng(11u);
    std::uniform_real_distribution<float> n(-3.0f, 3.0f);
    for (int i = 0; i < 200; ++i) changes += b.Window(n(rng), 120, frame);
    CHECK(changes == 0 && !b.on);
}

// Vettore di rotazione relativo (riga POSEAGE): A = B ruotata di 0,02 rad attorno a y.
static void TestRelativeRotationVector() {
    float b[3][4], a[3][4];
    PoseFromEuler(-0.3f, 0.5f, 0.1f, b);
    const float r[3][3] = {{std::cos(0.02f), 0, std::sin(0.02f)}, {0, 1, 0}, {-std::sin(0.02f), 0, std::cos(0.02f)}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) a[i][j] = r[i][0] * b[0][j] + r[i][1] * b[1][j] + r[i][2] * b[2][j];
        a[i][3] = 0.0f;
    }
    float v[3];
    RelativeRotationVector(a, b, v);
    CHECK(std::fabs(v[0]) < 1e-4f && std::fabs(v[1] - 0.02f) < 1e-4f && std::fabs(v[2]) < 1e-4f);
}

// --- WI-2: posizione della vista, altezza degli occhi, ricentramenti ---------------

static void TestStableEyeHeight() {
    // Senza limiti (in piedi): la vista e' la testa vera, qualunque sia il riferimento.
    // Limite inferiore = gli occhi misurati di Walker (WI-3 fix 1: era 30, il suo collo).
    const float s = 50.0f, lo = kWalkerEyeStand;
    for (float ref : {1.65f, 1.7f, 1.9f})
        for (float trk : {ref, ref + 0.05f, ref + 0.12f})
            for (float pos : {ref - 0.6f, ref - 0.1f, ref, ref + 0.05f}) {
                const float view = StableEyeHeight(ref, trk, s, lo, 55.0f) + (pos - trk) * s;
                CHECK(std::fabs(view - (pos * s - 45.0f)) < 1e-3f);
            }
    // Limitata (seduti con "seated off", testa a 1,20 m): il massimo che sale a
    // 1,30 e scende di 1 cm/s (il vecchio posRef[1]) non sposta piu' la vista;
    // con la formula di prima la vista saliva di 5 unita' in 10 s.
    float worstNew = 0.0f, worstOld = 0.0f;
    const float pos = 1.20f, ref = 1.20f;
    const float v0 = StableEyeHeight(ref, 1.30f, s, lo, 55.0f) + (pos - 1.30f) * s;
    const float o0 = std::fmax(25.0f, std::fmin(55.0f, 1.30f * s - 45.0f)) + (pos - 1.30f) * s;
    for (float trk = 1.30f; trk >= 1.20f; trk -= 0.001f) {
        const float v = StableEyeHeight(ref, trk, s, lo, 55.0f) + (pos - trk) * s;
        const float o = std::fmax(25.0f, std::fmin(55.0f, trk * s - 45.0f)) + (pos - trk) * s;
        worstNew = std::fmax(worstNew, std::fabs(v - v0));
        worstOld = std::fmax(worstOld, std::fabs(o - o0));
    }
    CHECK(worstNew < 1e-3f);
    CHECK(worstOld > 4.9f);
    CHECK(std::fabs(v0 - kWalkerEyeStand) < 1e-3f);  // minimo: gli occhi di Walker (36,25)
    // Solo il riferimento e' limitato, mai lo spostamento del frame: accucciarsi di 30 cm abbassa di 15 unita'.
    const float crouch = StableEyeHeight(ref, ref, s, lo, 55.0f) + (0.90f - ref) * s;
    CHECK(std::fabs(crouch - (kWalkerEyeStand - 15.0f)) < 1e-3f);
    std::printf("stable eye height: clamped head still while the max decays 1.30 -> 1.20 m: drift %.4f units (old formula %.2f)\n",
                worstNew, worstOld);
}

static void TestWallPullStep() {
    const float dt = 1.0f / 72.0f;
    // Verso il muro: al massimo 150 unita'/s...
    float p = 0.0f;
    p = WallPullStep(p, 20.0f, 0.0f, dt);
    CHECK(std::fabs(p - 150.0f * dt) < 1e-4f);
    // ...ma la parte "dura" (occhi entro 4 unita' dal muro) subito.
    p = WallPullStep(0.0f, 20.0f, 12.0f, dt);
    CHECK(std::fabs(p - 12.0f) < 1e-4f);
    // Via dal muro: tau 0,2 s.
    p = WallPullStep(10.0f, 0.0f, 0.0f, dt);
    CHECK(std::fabs(p - 10.0f * std::exp(-dt / 0.2f)) < 1e-3f);
    // hard oltre want: limitato a want.
    CHECK(std::fabs(WallPullStep(0.0f, 5.0f, 9.0f, dt) - 5.0f) < 1e-4f);
}

static void TestLowProbeLatch() {
    // Bordo di una cassa a quota 0 (rispetto all'occhio di riferimento); la
    // trace bassa sta a u - 10 (- isteresi): colpisce sotto il bordo. La testa
    // ondeggia di +-2 unita' a 1 Hz attorno al punto in cui la trace sfiora il
    // bordo (u = 10): senza isteresi la trace entra ed esce a ogni mezzo ciclo.
    const float dt = 1.0f / 72.0f;
    LowProbeLatch latch;
    int engages = 0, releases = 0, oldToggles = 0;
    bool was = false, oldWas = false;
    for (int i = 0; i < 72 * 6; ++i) {
        const float t = i * dt;
        const float u = 10.5f + 2.0f * std::sin(6.2831853f * t);
        const bool oldHit = u - 10.0f < 0.0f;
        if (oldHit != oldWas && i > 0) ++oldToggles;
        oldWas = oldHit;
        const bool hit = u - 10.0f - latch.Drop() < 0.0f;
        latch.Step(hit, 15.0f, dt);
        if (latch.engaged && !was) ++engages;
        if (!latch.engaged && was) ++releases;
        was = latch.engaged;
    }
    CHECK(engages == 1 && releases == 0);
    CHECK(oldToggles >= 10);
    // Alzandosi davvero (u 18, 8 unita' oltre il bordo): rilascio dopo 0,4 s.
    int ticks = 0;
    while (latch.engaged && ticks < 200) {
        latch.Step(18.0f - 10.0f - latch.Drop() < 0.0f, 15.0f, dt);
        ++ticks;
    }
    CHECK(!latch.engaged && std::fabs(ticks * dt - LowProbeLatch::kHold) < 2.0f * dt);
    std::printf("low probe latch: head bobbing +-2 units across a crate edge, 6 s: %d engage, %d release (without: %d toggles); "
                "standing up: released after %.2f s\n", engages, releases, oldToggles, ticks * dt);
}

static void TestClampIntoPlane() {
    // Muro x = 10, normale verso la base (-1, 0, 0), margine 8.
    const float hit[3] = {10.0f, 0.0f, 0.0f}, n[3] = {-1.0f, 0.0f, 0.0f}, base[3] = {0.0f, 0.0f, 0.0f};
    float maxSide = 0.0f;
    for (float y = -10.0f; y <= 10.0f; y += 0.5f) {
        float t[3] = {8.0f, y, 3.0f};
        CHECK(ClampIntoPlane(base, t, hit, n, 8.0f));
        CHECK(std::fabs(t[0] - 2.0f) < 1e-4f && std::fabs(t[2] - 3.0f) < 1e-4f);
        maxSide = std::fmax(maxSide, std::fabs(t[1] - y));  // di lato 1:1
    }
    CHECK(maxSide < 1e-5f);
    // Davanti al margine: nessuna correzione.
    float free[3] = {1.5f, 4.0f, 0.0f};
    CHECK(!ClampIntoPlane(base, free, hit, n, 8.0f) && free[0] == 1.5f);
    // Base gia' a 5 unita' (meno del margine): l'occhio resta alla distanza della base, non dietro.
    const float base5[3] = {5.0f, 0.0f, 0.0f};
    float t5[3] = {9.0f, 2.0f, 0.0f};
    CHECK(ClampIntoPlane(base5, t5, hit, n, 8.0f) && std::fabs(t5[0] - 5.0f) < 1e-4f && std::fabs(t5[1] - 2.0f) < 1e-4f);
}

static void TestSpaceShift() {
    std::mt19937 rng(25102026u);
    std::uniform_real_distribution<float> ang(-3.1f, 3.1f), lin(-2.0f, 2.0f);
    double worstOff = 0.0, worstYaw = 0.0, worstInv = 0.0, worstXr = 0.0;
    for (int k = 0; k < 2000; ++k) {
        SpaceShift sh;
        sh.yaw = ang(rng);
        sh.tx = lin(rng);
        sh.tz = lin(rng);
        float ref = ang(rng), prx = lin(rng), prz = lin(rng);
        const float hx = lin(rng), hz = lin(rng), hyaw = ang(rng);
        // Spostamento (avanti, destra) e yaw della testa rispetto ai riferimenti, come UpdateHeadTracking.
        auto offs = [](float r, float px, float pz, float x, float z, float* f, float* rt) {
            const float dx = x - px, dz = z - pz, sy = std::sin(r), cy = std::cos(r);
            *f = dx * sy - dz * cy;
            *rt = dx * cy + dz * sy;
        };
        float f0, r0, f1, r1, nhx, nhz;
        offs(ref, prx, prz, hx, hz, &f0, &r0);
        const float y0 = WrapPi(hyaw - ref);
        sh.Apply(hx, hz, &nhx, &nhz);
        const float nhyaw = WrapPi(hyaw + sh.yaw);
        RebaseReferences(sh, &ref, &prx, &prz);
        offs(ref, prx, prz, nhx, nhz, &f1, &r1);
        worstOff = std::fmax(worstOff, std::fmax(std::fabs(f1 - f0), std::fabs(r1 - r0)));
        worstYaw = std::fmax(worstYaw, std::fabs(WrapPi(WrapPi(nhyaw - ref) - y0)));
        // Inversa e composizione.
        const SpaceShift id = SpaceShift::Then(sh, sh.Inverse());
        worstInv = std::fmax(worstInv, std::fmax(std::fabs(WrapPi(id.yaw)), std::fmax(std::fabs(id.tx), std::fabs(id.tz))));
        // ShiftFromPoses ritrova la trasformazione; poseInPreviousSpace (OpenXR) pure.
        const SpaceShift fromPoses = ShiftFromPoses(hyaw, hx, hz, nhyaw, nhx, nhz);
        float m[3][4];
        const SpaceShift toOld = sh.Inverse();  // origine del nuovo spazio nel vecchio
        PoseFromEuler(0.0f, toOld.yaw, 0.0f, m);
        m[0][3] = toOld.tx;
        m[1][3] = 0.0f;
        m[2][3] = toOld.tz;
        const SpaceShift fromXr = ShiftFromPreviousSpacePose(m);
        for (const SpaceShift* s2 : {&fromPoses, &fromXr})
            worstXr = std::fmax(worstXr, std::fmax(std::fabs(WrapPi(s2->yaw - sh.yaw)),
                                                   std::fmax(std::fabs(s2->tx - sh.tx), std::fabs(s2->tz - sh.tz))));
    }
    CHECK(worstOff < 1e-4 && worstYaw < 1e-4 && worstInv < 1e-4 && worstXr < 1e-3);
    std::printf("space shift: 2000 random recenters, head offset kept within %.2g m, view yaw within %.2g rad, "
                "inverse %.2g, from poses / OpenXR poseInPreviousSpace %.2g\n", worstOff, worstYaw, worstInv, worstXr);
}

// Rilevatore del 25/10 (solo yaw esatto, salto tolto subito, storia dal campione
// saltato): solo per il confronto prima/dopo della review WI-2.
struct OldHeadJumpDetector {
    double t[2] = {0.0, 0.0};
    float yaw[2] = {0, 0}, x[2] = {0, 0}, z[2] = {0, 0};
    int n = 0;
    bool Step(double now, float yawNow, float xNow, float zNow) {
        bool jump = false;
        if (n >= 2) {
            const double dt1 = now - t[1], dt0 = t[1] - t[0];
            if (dt1 > 0.0 && dt1 <= 0.15 && dt0 > 0.0 && dt0 <= 0.15) {
                const float k = static_cast<float>(dt1 / dt0);
                const float py = yaw[1] + WrapPi(yaw[1] - yaw[0]) * k;
                const float px = x[1] + (x[1] - x[0]) * k, pz = z[1] + (z[1] - z[0]) * k;
                const float lp = std::sqrt((xNow - px) * (xNow - px) + (zNow - pz) * (zNow - pz));
                const float ly = std::fabs(WrapPi(yawNow - py)) * 57.2957795f;
                const float d2 = static_cast<float>(dt1 * dt1);
                jump = lp > 0.05f + 30.0f * d2 || ly > 5.0f + 3000.0f * d2;
            } else {
                n = 0;
            }
        }
        if (jump || n == 0) {
            n = 1;
        } else {
            t[0] = t[1];
            yaw[0] = yaw[1];
            x[0] = x[1];
            z[0] = z[1];
            n = 2;
        }
        t[1] = now;
        yaw[1] = yawNow;
        x[1] = xNow;
        z[1] = zNow;
        return jump;
    }
};

// Rotazione OpenVR della testa (ExactEuler = pitch, yaw, roll; radianti).
static void HeadRot(float pitch, float yaw, float roll, float r[3][3]) {
    float m[3][4];
    PoseFromEuler(pitch, yaw, roll, m);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = m[i][j];
}

static float ExactYawOf(const float r[3][3]) {
    float m[3][4] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = r[i][j];
    return ExactEuler(m).yaw;
}

// Simulazione del chiamante (UpdateHeadTracking): vista = posa (tenuta nel tick
// kHold) riportata nello spazio di partenza dai riferimenti (kConfirm toglie il
// salto). Errore massimo (gradi) tra la vista e la testa vera, e conteggi.
struct JumpSim {
    HeadJumpDetector det;
    SpaceShift refs;  // salti tolti dai riferimenti (spazio di partenza -> spazio del runtime)
    int holds = 0, confirms = 0, rejects = 0, unsure = 0;
    double worstDeg = 0.0;
    bool unsureAsConfirm = true;  // false: come prima della review WI-2 round 2 (kUnsure ignorato)
    void Step(double t, const float trueR[3][3], const float seenR[3][3], float x, float z) {
        SpaceShift js;
        const int r = det.Step(t, seenR, x, z, &js);
        float shown[3][4] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) shown[i][j] = seenR[i][j];
        if (r == HeadJumpDetector::kHold) {
            ++holds;
            ShiftPose(js.Inverse(), shown);
        } else if (r == HeadJumpDetector::kConfirm) {
            ++confirms;
            refs = SpaceShift::Then(refs, js);
        } else if (r == HeadJumpDetector::kUnsure) {
            ++unsure;
            if (unsureAsConfirm) refs = SpaceShift::Then(refs, js);
        } else if (r == HeadJumpDetector::kRejected) {
            ++rejects;
        }
        ShiftPose(refs.Inverse(), shown);
        float tr[3][4] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) tr[i][j] = trueR[i][j];
        worstDeg = std::fmax(worstDeg, PoseAngleDeg(shown, tr));
    }
};

static void TestHeadJumpDetector() {
    const double dt = 1.0 / 72.0;
    float r[3][3];
    // 1) Testa vera a 72 Hz: giri fino a 400 gradi/s, pitch e rollio, passi di 1 m/s: nessun salto.
    {
        HeadJumpDetector det;
        int any = 0;
        SpaceShift got;
        double t = 0.0;
        for (int i = 0; i < 72 * 20; ++i, t += dt) {
            const float ft = static_cast<float>(t);
            HeadRot(0.6f * std::sin(ft * 3.1f), 1.2f * std::sin(ft * 5.3f) + 0.002f * std::sin(ft * 61.0f), 0.3f * std::sin(ft * 2.3f),
                    r);
            const float x = 0.3f * std::sin(ft * 3.3f), z = 0.2f * std::cos(ft * 2.1f);
            if (det.Step(t, r, x, z, &got) != HeadJumpDetector::kNone) ++any;
        }
        CHECK(any == 0);
        // Tick lunghi (87 ms, VD con la GPU condivisa): nessuna conferma.
        HeadJumpDetector dl;
        int longConfirm = 0;
        for (double tl = 0.0; tl < 20.0; tl += 0.087) {
            const float ft = static_cast<float>(tl);
            HeadRot(0.0f, 1.2f * std::sin(ft * 5.3f), 0.0f, r);
            if (dl.Step(tl, r, 0.3f * std::sin(ft * 3.3f), 0.2f * std::cos(ft * 2.1f), &got) == HeadJumpDetector::kConfirm)
                ++longConfirm;
        }
        CHECK(longConfirm == 0);
        std::printf("head jump detector: 20 s of head motion (up to 364 deg/s, pitch and roll), %d detections; 87 ms ticks: %d confirms\n",
                    any, longConfirm);
    }
    // 2) Ricentramento vero (spazio girato di 30 gradi e spostato di 20 cm): tenuto un
    //    tick, confermato il tick dopo con la trasformazione giusta; vista continua.
    {
        SpaceShift sh;
        sh.yaw = 0.5236f;
        sh.tx = 0.15f;
        sh.tz = -0.13f;
        JumpSim sim;
        SpaceShift found;
        double t = 0.0;
        for (int i = 0; i < 72 * 3; ++i, t += dt) {
            const float ft = static_cast<float>(t);
            float tr[3][3], seen[3][3];
            HeadRot(0.2f * std::sin(ft * 1.3f), 0.8f * std::sin(ft * 2.0f), 0.05f, tr);
            float x = 0.1f * std::sin(ft * 1.7f), z = 0.05f;
            std::memcpy(seen, tr, sizeof(seen));
            if (i >= 72) {
                float m[3][4] = {};
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) m[a][b] = tr[a][b];
                m[0][3] = x;
                m[2][3] = z;
                ShiftPose(sh, m);
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) seen[a][b] = m[a][b];
                x = m[0][3];
                z = m[2][3];
            }
            const int before = sim.confirms;
            sim.Step(t, tr, seen, x, z);
            if (sim.confirms != before) found = sim.refs;
        }
        CHECK(sim.holds == 1 && sim.confirms == 1);
        CHECK(std::fabs(WrapPi(found.yaw - sh.yaw)) < 0.01f && std::fabs(found.tx - sh.tx) < 0.01f &&
              std::fabs(found.tz - sh.tz) < 0.01f);
        CHECK(sim.worstDeg < 0.5);
        std::printf("runtime recenter 30 deg / 20 cm: %d hold, %d confirm, found %.2f deg (%.3f, %.3f) m, view error max %.3f deg\n",
                    sim.holds, sim.confirms, found.yaw * 57.2958f, found.tx, found.tz, sim.worstDeg);
    }
    // 3) Un campione sbagliato di 10 gradi (yaw, poi pitch) girando a 29 gradi/s:
    //    nessuna conferma; vista mai spostata di piu' del campione (prima: 20 gradi per 1 s).
    for (int axis = 0; axis < 2; ++axis) {
        JumpSim sim;
        OldHeadJumpDetector old;
        int oldJumps = 0;
        double t = 0.0;
        for (int i = 0; i < 300; ++i, t += dt) {
            const float yaw = 0.5f * static_cast<float>(t);
            float tr[3][3], seen[3][3];
            HeadRot(0.0f, yaw, 0.0f, tr);
            const float spike = i == 100 ? 0.1745f : 0.0f;
            HeadRot(axis == 1 ? spike : 0.0f, yaw + (axis == 0 ? spike : 0.0f), 0.0f, seen);
            sim.Step(t, tr, seen, 0.0f, 0.0f);
            if (old.Step(t, ExactYawOf(seen), 0.0f, 0.0f)) ++oldJumps;
        }
        CHECK(sim.confirms == 0);
        CHECK(axis == 0 ? sim.worstDeg < 0.2 : sim.worstDeg < 10.5);  // yaw: vista tenuta; pitch: un tick, come prima del WI-2
        std::printf("one bad sample of 10 deg (%s): %d holds, %d confirms, %d rejected; view error max %.2f deg (old detector: %d jumps)\n",
                    axis == 0 ? "yaw" : "pitch", sim.holds, sim.confirms, sim.rejects, sim.worstDeg, oldJumps);
    }
    // 4) Campione ripetuto (posa vecchia) girando a 300 gradi/s dopo un tick di 30 ms: nessuna conferma.
    {
        JumpSim sim;
        OldHeadJumpDetector old;
        int oldJumps = 0;
        double t = 0.0;
        float prevSeen[3][3];
        HeadRot(0, 0, 0, prevSeen);
        for (int i = 0; i < 100; ++i) {
            t += i == 41 ? 0.030 : dt;
            float tr[3][3], seen[3][3];
            HeadRot(0.0f, 5.236f * static_cast<float>(t), 0.0f, tr);
            if (i == 41) std::memcpy(seen, prevSeen, sizeof(seen));
            else std::memcpy(seen, tr, sizeof(seen));
            sim.Step(t, tr, seen, 0.0f, 0.0f);
            if (old.Step(t, ExactYawOf(seen), 0.0f, 0.0f)) ++oldJumps;
            std::memcpy(prevSeen, seen, sizeof(prevSeen));
        }
        CHECK(sim.confirms == 0);
        std::printf("repeated sample after a 30 ms tick in a 300 deg/s turn: %d holds, %d confirms, view error max %.2f deg (old: %d jumps)\n",
                    sim.holds, sim.confirms, sim.worstDeg, oldJumps);
    }
    // 5) Guardando dritto in basso (cenni attraverso la verticale, con un po' di yaw):
    //    lo yaw esatto si gira di 180 gradi; la rotazione completa no.
    {
        HeadJumpDetector det;
        OldHeadJumpDetector old;
        int detections = 0, oldJumps = 0;
        SpaceShift got;
        double t = 0.0;
        for (int i = 0; i < 72 * 10; ++i, t += dt) {
            const float ft = static_cast<float>(t);
            const float pitch = -1.5708f + 0.35f * std::sin(ft * 6.2832f), yaw = 0.1f * std::sin(ft * 1.1f);
            const double cy = std::cos(-yaw), sy = std::sin(-yaw), cp = std::cos(pitch), sp = std::sin(pitch);
            const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}, rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
            double rr[3][3];
            rot3::Mul(ry, rx, rr);
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) r[a][b] = static_cast<float>(rr[a][b]);
            if (det.Step(t, r, 0.0f, 0.0f, &got) != HeadJumpDetector::kNone) ++detections;
            if (old.Step(t, ExactYawOf(r), 0.0f, 0.0f)) ++oldJumps;
        }
        CHECK(detections == 0 && oldJumps >= 5);
        std::printf("nodding across straight down for 10 s: %d detections (old detector, exact yaw: %d false jumps)\n", detections,
                    oldJumps);
    }
    // 6) Giro rapido (60 gradi in 250 ms, picco 450 gradi/s, minimum jerk) con un tick
    //    lungo di 30-60 ms in 60 posizioni: pose senza predizione (come ora) e predette
    //    80 ms (VDXR, il caso della review): nessuna conferma.
    {
        auto mj = [](double tt, double& v) {
            const double A = 60.0 / 57.2958, T = 0.25;
            if (tt <= 0.0) {
                v = 0.0;
                return 0.0;
            }
            if (tt >= T) {
                v = 0.0;
                return A;
            }
            const double u = tt / T;
            v = A * (30 * u * u - 60 * u * u * u + 30 * u * u * u * u) / T;
            return A * (10 * u * u * u - 15 * u * u * u * u + 6 * u * u * u * u * u);
        };
        for (double pred : {0.0, 0.080}) {
            for (double hitch : {0.030, 0.045, 0.060}) {
                int confirms = 0, holds = 0, oldRuns = 0;
                for (int off = 0; off < 60; ++off) {
                    HeadJumpDetector det;
                    OldHeadJumpDetector old;
                    bool oldHit = false, done = false;
                    double t = 0.0;
                    SpaceShift got;
                    while (t < 2.0) {
                        double v;
                        const double p = mj(t - 1.0, v) + v * pred;
                        HeadRot(0.0f, static_cast<float>(p), 0.0f, r);
                        const int res = det.Step(t, r, 0.0f, 0.0f, &got);
                        confirms += res == HeadJumpDetector::kConfirm;
                        holds += res == HeadJumpDetector::kHold;
                        oldHit = old.Step(t, static_cast<float>(p), 0.0f, 0.0f) || oldHit;
                        if (!done && t >= 0.95 + off * 0.005) {
                            t += hitch;
                            done = true;
                        } else {
                            t += dt;
                        }
                    }
                    oldRuns += oldHit;
                }
                CHECK(confirms == 0);
                std::printf("flick 60 deg / 250 ms, prediction %.0f ms, one %.0f ms tick in 60 positions: %d confirms, %d holds "
                            "(old detector: false jumps in %d runs)\n",
                            pred * 1000.0, hitch * 1000.0, confirms, holds, oldRuns);
            }
        }
    }
    // 7) Sotto soglia (3 cm, 4 gradi): niente; dopo un tick oltre 150 ms si riparte;
    //    un ricentramento di 40 gradi / 36 cm in un tick di 87 ms si vede e si conferma.
    {
        HeadJumpDetector d2;
        SpaceShift got;
        int any = 0;
        for (int i = 0; i < 20; ++i) {
            HeadRot(0.0f, i < 10 ? 0.0f : 0.07f, 0.0f, r);
            any += d2.Step(i * dt, r, i < 10 ? 0.0f : 0.03f, 0.0f, &got) != HeadJumpDetector::kNone;
        }
        CHECK(any == 0);
        HeadJumpDetector d5;
        HeadRot(0, 0, 0, r);
        d5.Step(0.0, r, 0.0f, 0.0f, &got);
        d5.Step(0.014, r, 0.0f, 0.0f, &got);
        HeadRot(0.0f, 1.0f, 0.0f, r);
        CHECK(d5.Step(0.214, r, 0.5f, 0.0f, &got) == HeadJumpDetector::kNone);
        HeadJumpDetector d3;
        HeadRot(0, 0, 0, r);
        d3.Step(0.0, r, 0.0f, 0.0f, &got);
        d3.Step(0.040, r, 0.0f, 0.0f, &got);
        HeadRot(0.0f, 0.698f, 0.0f, r);
        CHECK(d3.Step(0.127, r, 0.3f, -0.2f, &got) == HeadJumpDetector::kHold);
        CHECK(d3.Step(0.214, r, 0.3f, -0.2f, &got) == HeadJumpDetector::kConfirm);
    }
    // 8) Review WI-2 round 2: ricentramento vero (30 gradi / 20 cm) e subito dopo un tick
    //    di 200 ms (menu di sistema del Quest col tasto tenuto premuto). Prima: kRejected,
    //    riferimenti intatti, il ricentramento intero si vedeva di scatto. Ora kUnsure con lo
    //    spostamento del candidato: tolto come un kConfirm, vista continua.
    {
        SpaceShift sh;
        sh.yaw = 0.5236f;
        sh.tx = 0.15f;
        sh.tz = -0.13f;
        double worst[2] = {0, 0};
        int unsure[2] = {0, 0}, holds[2] = {0, 0}, confirms[2] = {0, 0};
        SpaceShift found;
        for (int variant = 0; variant < 2; ++variant) {
            JumpSim sim;
            sim.unsureAsConfirm = variant == 1;
            double t = 0.0;
            for (int i = 0; i < 72 * 3; ++i) {
                const float ft = static_cast<float>(t);
                float tr[3][3], seen[3][3];
                HeadRot(0.2f * std::sin(ft * 1.3f), 0.8f * std::sin(ft * 2.0f), 0.05f, tr);
                float x = 0.1f * std::sin(ft * 1.7f), z = 0.05f;
                std::memcpy(seen, tr, sizeof(seen));
                if (i >= 72) {
                    float m[3][4] = {};
                    for (int a = 0; a < 3; ++a)
                        for (int b = 0; b < 3; ++b) m[a][b] = tr[a][b];
                    m[0][3] = x;
                    m[2][3] = z;
                    ShiftPose(sh, m);
                    for (int a = 0; a < 3; ++a)
                        for (int b = 0; b < 3; ++b) seen[a][b] = m[a][b];
                    x = m[0][3];
                    z = m[2][3];
                }
                const int before = sim.unsure;
                sim.Step(t, tr, seen, x, z);
                if (sim.unsure != before && variant == 1) found = sim.refs;
                t += i == 72 ? 0.200 : dt;  // il tick dopo il salto arriva 200 ms dopo
            }
            worst[variant] = sim.worstDeg;
            unsure[variant] = sim.unsure;
            holds[variant] = sim.holds;
            confirms[variant] = sim.confirms;
        }
        CHECK(holds[1] == 1 && unsure[1] == 1 && confirms[1] == 0);
        CHECK(std::fabs(WrapPi(found.yaw - sh.yaw)) < 0.01f && std::fabs(found.tx - sh.tx) < 0.01f &&
              std::fabs(found.tz - sh.tz) < 0.01f);
        CHECK(worst[1] < 0.5 && worst[0] > 25.0);
        std::printf("runtime recenter 30 deg / 20 cm then a 200 ms tick: %d hold, %d unsure (taken off, kept with the event, "
                    "else blink), found %.2f deg; view error max %.3f deg (as before, unsure ignored: %.1f deg snap)\n",
                    holds[1], unsure[1], found.yaw * 57.2958f, worst[1], worst[0]);
        // Senza candidato valido (salto inclinato): come prima.
        HeadJumpDetector d6;
        SpaceShift got;
        HeadRot(0, 0, 0, r);
        d6.Step(0.0, r, 0.0f, 0.0f, &got);
        d6.Step(dt, r, 0.0f, 0.0f, &got);
        HeadRot(0.35f, 0.0f, 0.0f, r);  // 20 gradi di pitch: inclinato, nessun hold
        CHECK(d6.Step(2 * dt, r, 0.0f, 0.0f, &got) == HeadJumpDetector::kNone);
        CHECK(d6.Step(2 * dt + 0.2, r, 0.0f, 0.0f, &got) == HeadJumpDetector::kRejected);
    }
}

// Review WI-2 round 2: controller nel tick tenuto di un salto del visore (PoseFollowsShift)
// e riferimento delle mani per props.cpp nel primo spazio.
static void TestRecenterHands() {
    const double dt = 1.0 / 72.0;
    auto pose = [](float pitch, float yaw, float roll, float x, float y, float z, float m[3][4]) {
        PoseFromEuler(pitch, yaw, roll, m);
        m[0][3] = x;
        m[1][3] = y;
        m[2][3] = z;
    };
    SpaceShift sh;  // come il ricentramento del runtime finto: 40 gradi, 14 cm
    sh.yaw = 0.698f;
    sh.tx = 0.10f;
    sh.tz = -0.10f;
    // 1) Mani ferme o in moto fino a 2 m/s e 200 gradi/s, ricentramento vero: riportate indietro;
    //    scarto dal tick prima (cm, gradi) senza e con il ritorno.
    float worstRawCm = 1e9f, worstBackCm = 0.0f, worstBackDeg = 0.0f;
    int mapped = 0, cases = 0;
    for (int k = 0; k < 40; ++k) {
        const float v = 0.05f * k, w = 0.087f * k;  // m/s, rad/s
        float prev[3][4], now[3][4];
        pose(-0.3f, 0.4f * k, 0.1f, 0.25f, 1.2f, -0.45f, prev);
        pose(-0.3f, 0.4f * k + w * static_cast<float>(dt), 0.1f, 0.25f + v * static_cast<float>(dt), 1.2f, -0.45f, now);
        float raw[3][4];
        std::memcpy(raw, now, sizeof(raw));
        ShiftPose(sh, raw);  // nello spazio nuovo
        ++cases;
        if (PoseFollowsShift(raw, prev, sh)) {
            ++mapped;
            float back[3][4];
            std::memcpy(back, raw, sizeof(back));
            ShiftPose(sh.Inverse(), back);
            const float d = std::sqrt((back[0][3] - prev[0][3]) * (back[0][3] - prev[0][3]) +
                                      (back[1][3] - prev[1][3]) * (back[1][3] - prev[1][3]) +
                                      (back[2][3] - prev[2][3]) * (back[2][3] - prev[2][3])) * 100.0f;
            worstBackCm = std::fmax(worstBackCm, d);
            worstBackDeg = std::fmax(worstBackDeg, static_cast<float>(PoseAngleDeg(back, prev)));
        }
        worstRawCm = std::fmin(worstRawCm, std::sqrt((raw[0][3] - prev[0][3]) * (raw[0][3] - prev[0][3]) +
                                                     (raw[2][3] - prev[2][3]) * (raw[2][3] - prev[2][3])) * 100.0f);
    }
    CHECK(mapped == cases && worstBackCm < 3.0f && worstBackDeg < 3.0f);
    // 2) Salto solo del visore (campione sbagliato): mani ferme o lente, mai riportate.
    int wrong = 0;
    for (int k = 0; k < 40; ++k) {
        float prev[3][4], now[3][4];
        pose(-0.3f, 0.4f * k, 0.1f, 0.25f, 1.2f, -0.45f, prev);
        pose(-0.3f, 0.4f * k + 0.01f * k, 0.1f, 0.25f + 0.001f * k, 1.2f, -0.45f, now);
        wrong += PoseFollowsShift(now, prev, sh);
    }
    CHECK(wrong == 0);
    // 3) Riferimento delle mani per props nel primo spazio: invariato da un assorbimento
    //    (riferimenti ribasati di js, spazi assorbiti A -> A then js), 2000 casi a caso.
    std::mt19937 rng(2610202602u);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worstC = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        SpaceShift A, js;
        A.yaw = 3.0f * u(rng);
        A.tx = u(rng);
        A.tz = u(rng);
        js.yaw = 3.0f * u(rng);
        js.tx = 0.5f * u(rng);
        js.tz = 0.5f * u(rng);
        float ref = 3.0f * u(rng), px = u(rng), pz = u(rng);
        float c0y = ref, c0x = px, c0z = pz;
        RebaseReferences(A.Inverse(), &c0y, &c0x, &c0z);
        RebaseReferences(js, &ref, &px, &pz);
        const SpaceShift A1 = SpaceShift::Then(A, js);
        float c1y = ref, c1x = px, c1z = pz;
        RebaseReferences(A1.Inverse(), &c1y, &c1x, &c1z);
        worstC = std::fmax(worstC, std::fmax(std::fabs(WrapPi(c1y - c0y)), std::fmax(std::fabs(c1x - c0x), std::fabs(c1z - c0z))));
    }
    CHECK(worstC < 1e-4f);
    std::printf("recenter hands: real 40 deg / 14 cm recenter, hand still to 2 m/s / 200 deg/s: %d of %d mapped back, step from the "
                "previous tick %.2f cm / %.2f deg (raw at least %.1f cm); headset-only jump: %d of 40 mapped; props frame across "
                "an absorb: %.1g\n",
                mapped, cases, worstBackCm, worstBackDeg, worstRawCm, wrong, worstC);
}

static void TestViewHeightRecapture() {
    // Chiamante: riferimento della vista, ripresa applicata il tick dopo (battito).
    const float s = 50.0f;
    auto view = [&](float ref, float pos) { return StableEyeHeight(ref, ref, s, kWalkerEyeStand, 55.0f) + (pos - ref) * s; };
    std::mt19937 rng(26102026u);
    std::normal_distribution<float> noise(0.0f, 0.0003f);  // 0,3 mm
    const double dt = 1.0 / 72.0;
    // 1) Visore messo seduti (1,20 m), poi in piedi (1,70 m) a t = 5 s, con rumore e 3 mm di ondeggiamento.
    {
        ViewHeightRecapture rc;
        float ref = 1.20f, pending = 0.0f;
        int recaptures = 0;
        double atT = -1.0;
        float vMin = 1e9f, vMax = -1e9f;
        for (int i = 0; i < 72 * 45; ++i) {
            const double t = i * dt;
            const float head = (t < 5.0 ? 1.20f : 1.70f) + noise(rng) + 0.003f * std::sin(static_cast<float>(t) * 1.9f);
            if (pending > 0.0f) {
                ref = pending;
                pending = 0.0f;
            }
            const float nr = rc.Step(t, head, ref);
            if (nr > 0.0f) {
                ++recaptures;
                pending = nr;
                atT = t;
            }
            if (t > 15.0) {  // 30 s dopo: deriva della vista oltre il movimento della testa
                const float v = view(ref, head) - (head - 1.70f) * s;
                vMin = std::fmin(vMin, v);
                vMax = std::fmax(vMax, v);
            }
        }
        CHECK(recaptures == 1 && atT > 7.9 && atT < 8.5);
        CHECK(std::fabs(ref - 1.70f) < 0.01f);
        CHECK(vMax - vMin < 0.2f && std::fabs(0.5f * (vMax + vMin) - 40.0f) < 0.5f);
        std::printf("view height re-capture, headset on seated 1.20 m then standing 1.70 m (0.3 mm noise, 3 mm sway): %d re-capture at "
                    "%.2f s, reference %.3f m, view %.2f units, drift over 30 s %.3f units\n",
                    recaptures, atT, ref, 0.5f * (vMax + vMin), vMax - vMin);
    }
    // 2) "seated off" a 1,20 m che si alza di 25 cm per 2 s e torna giu', 5 volte:
    //    riferimento mai cambiato, vista da seduti agli occhi di Walker (36,25; era 30).
    {
        ViewHeightRecapture rc;
        float ref = 1.20f;
        int recaptures = 0;
        for (int i = 0; i < 72 * 60; ++i) {
            const double t = i * dt;
            const double ph = std::fmod(t, 12.0);
            const float head = (ph > 4.0 && ph < 6.0 ? 1.45f : 1.20f) + noise(rng);
            const float nr = rc.Step(t, head, ref);
            if (nr > 0.0f) {
                ++recaptures;
                ref = nr;
            }
        }
        const float vEnd = view(ref, 1.20f);
        CHECK(recaptures == 0 && std::fabs(vEnd - kWalkerEyeStand) < 1e-3f);
        std::printf("seated 1.20 m rising 25 cm for 2 s, 5 times: %d re-captures, seated view %.2f units\n", recaptures, vEnd);
    }
    // 3) Solo rumore e ondeggiamento per 60 s: nessuna ripresa.
    {
        ViewHeightRecapture rc;
        int recaptures = 0;
        for (int i = 0; i < 72 * 60; ++i) {
            const double t = i * dt;
            recaptures += rc.Step(t, 1.20f + noise(rng) + 0.003f * std::sin(static_cast<float>(t) * 1.9f), 1.20f) > 0.0f;
        }
        CHECK(recaptures == 0);
    }
}

static void TestClampIntoPlanes() {
    // Angolo: muri x = 10 (normale -x) e y = 10 (normale -y), margine 8, base all'origine.
    const float hit[2][3] = {{10, 0, 0}, {0, 10, 0}}, n[2][3] = {{-1, 0, 0}, {0, -1, 0}}, base[3] = {0, 0, 0};
    float t[3] = {15.0f, 15.0f, 3.0f};
    CHECK(ClampIntoPlanes(base, t, hit, n, 2, 8.0f));
    CHECK(std::fabs(t[0] - 2.0f) < 1e-4f && std::fabs(t[1] - 2.0f) < 1e-4f && std::fabs(t[2] - 3.0f) < 1e-4f);
    // Lungo il primo muro, lontano dal secondo: di lato 1:1.
    float t2[3] = {9.0f, -5.0f, 0.0f};
    CHECK(ClampIntoPlanes(base, t2, hit, n, 2, 8.0f) && std::fabs(t2[0] - 2.0f) < 1e-4f && std::fabs(t2[1] + 5.0f) < 1e-4f);
    // Angolo acuto (60 gradi): tutti i vincoli rispettati dopo le proiezioni.
    const float n3[2][3] = {{-1, 0, 0}, {-0.5f, -0.8660254f, 0}}, hit3[2][3] = {{10, 0, 0}, {5, 8.660254f, 0}};
    float t3[3] = {12.0f, 6.0f, 0.0f};
    ClampIntoPlanes(base, t3, hit3, n3, 2, 8.0f);
    for (int k = 0; k < 2; ++k) {
        float d = 0.0f;
        for (int i = 0; i < 3; ++i) d += (t3[i] - hit3[k][i]) * n3[k][i];
        CHECK(d >= 8.0f - 0.05f);
    }
}

static void TestModeBlend() {
    // fp 15 -> 0 entrando su una torretta: a 72 Hz al massimo 0,7 unita' per tick, zero dopo 0,3 s.
    ModeBlend b;
    const float d[3] = {15.0f, 0.0f, -4.0f};
    b.Start(d);
    float prev[3] = {15.0f, 0.0f, -4.0f}, o[3];
    float worst = 0.0f;
    for (int i = 0; i < 40; ++i) {
        b.Step(1.0f / 72.0f, o);
        float st = 0.0f;
        for (int c = 0; c < 3; ++c) st += (o[c] - prev[c]) * (o[c] - prev[c]);
        worst = std::fmax(worst, std::sqrt(st));
        std::memcpy(prev, o, sizeof(prev));
    }
    CHECK(worst < 1.0f && o[0] == 0.0f && o[2] == 0.0f);
    CHECK(BaseMovingFast(0.5f, 0.014f) && !BaseMovingFast(0.2f, 0.014f) && !BaseMovingFast(30.0f, 0.6f));
    std::printf("mode blend: 15.5 units eased over 0.3 s, largest tick step %.2f units at 72 Hz\n", worst);
}

// Attesa del mutex OpenXR (review 2 WI-1): istogramma fisso e statistica per
// tick. Caso del reviewer: ~30 chiamate per tick, un'attesa di 3 ms nel 20% dei
// tick. Per chiamata il p99 resta al valore senza contesa (lo 0,7% delle
// chiamate) e il criterio "p99 < 0,5 ms" passava; per tick il 20% dei tick e'
// oltre 1 ms e il criterio "meno dell'1% dei tick oltre 1 ms" lo vede.
static void TestLockWaitStats() {
    WaitHistogram perCall, perTick;
    std::mt19937 rng(7u);
    std::uniform_real_distribution<double> base(0.002, 0.02);
    for (int t = 0; t < 1000; ++t) {
        double tick = 0.0;
        const bool stall = (t % 5) == 0;
        for (int c = 0; c < 30; ++c) {
            const double w = (stall && c == 3) ? 3.0 : base(rng);
            perCall.Add(w);
            tick += w;
        }
        perTick.Add(tick);
    }
    const double pctTicksOver1 = 100.0 * perTick.overOne / perTick.n;
    std::printf("lock wait model (30 calls per tick, 3 ms in 20%% of ticks): per call p99 %.3f ms (%u calls, max %.2f), per tick "
                "p50 %.3f p99 %.3f ms, %.1f%% of ticks over 1 ms\n",
                perCall.Quantile(0.99), perCall.n, perCall.maxMs, perTick.Quantile(0.5), perTick.Quantile(0.99), pctTicksOver1);
    CHECK(perCall.n == 30000 && perCall.overOne == 200);
    CHECK(perCall.Quantile(0.99) < 0.5);   // la statistica di prima non vedeva le attese
    CHECK(perTick.n == 1000 && perTick.overOne == 200 && pctTicksOver1 > 1.0);
    CHECK(perTick.Quantile(0.99) >= 3.0);
    // Niente tetto: con 50000 campioni il max e' l'ultimo (prima: 4096 per finestra).
    WaitHistogram h;
    for (int i = 0; i < 50000; ++i) h.Add(i == 49999 ? 7.5 : 0.01);
    CHECK(h.n == 50000 && h.maxMs == 7.5 && h.overOne == 1 && h.overHalf == 1);
    // Quantili entro la risoluzione del bin (~9%) e mai oltre il max.
    WaitHistogram q;
    for (int i = 1; i <= 1000; ++i) q.Add(i * 0.001);  // 0,001 .. 1,000 ms
    const double p50 = q.Quantile(0.5), p99 = q.Quantile(0.99);
    CHECK(p50 >= 0.5 && p50 <= 0.5 * 1.1);
    CHECK(p99 >= 0.99 && p99 <= 1.0);
    CHECK(q.overHalf == 500 && q.overOne == 0);
    WaitHistogram z;
    z.Add(0.0);
    z.Add(-1.0);
    z.Add(std::nan(""));
    CHECK(z.n == 3 && z.maxMs == 0.0 && z.Quantile(0.99) <= WaitHistogram::kFirstMs);
    h.Reset();
    CHECK(h.n == 0 && h.maxMs == 0.0 && h.Quantile(0.5) == 0.0);
    CHECK(WaitHistogram::BinOf(1e9) == WaitHistogram::kBins - 1);
}

// Voci Advanced a parole (review 2 WI-1): i valori dell'ini distribuito sono
// nella lista (cosi' il menu le fa scorrere) e il predefinito e' il primo.
static void TestAdvancedWordChoices() {
    const std::vector<std::string>* pf = AdvancedWordChoices("predictframe");
    CHECK(pf && pf->size() == 3 && (*pf)[0] == "auto" && (*pf)[1] == "on" && (*pf)[2] == "off");
    const std::vector<std::string>* pr = AdvancedWordChoices("predict");
    CHECK(pr && pr->size() == 2 && (*pr)[0] == "auto" && (*pr)[1] == "off");
    const std::vector<std::string>* hc = AdvancedWordChoices("headcollision");
    CHECK(hc && (*hc)[0] == "push");
    CHECK(AdvancedWordChoices("openxrjson") == nullptr && AdvancedWordChoices("fov") == nullptr &&
          AdvancedWordChoices("vrinput") == nullptr);
    // Ini distribuito (deploy/specopsvr.ini accanto a tests/).
    std::string path = __FILE__;
    const size_t cut = path.find_last_of("\\/");
    path = path.substr(0, cut == std::string::npos ? 0 : cut) + "/../deploy/specopsvr.ini";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::printf("advanced word choices: %s not found, ini check skipped\n", path.c_str());
        return;
    }
    int found = 0, first = 0;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        char key[64] = {}, val[64] = {};
        if (line[0] == '#' || std::sscanf(line, "%63s %63s", key, val) != 2) continue;
        const std::vector<std::string>* w = AdvancedWordChoices(key);
        if (!w) continue;
        ++found;
        bool in = false;
        for (const std::string& s : *w) in = in || s == val;
        CHECK(in);
        if (in && (*w)[0] == val) ++first;
        if (!in) std::printf("advanced word choices: ini value \"%s %s\" not in the list\n", key, val);
    }
    std::fclose(f);
    std::printf("advanced word choices: %d keys of the shipped ini are menu choices, %d at the first (default) value\n", found,
                first);
    CHECK(found >= 10 && first == found);
}

// --- Telaio dell'arma (WI-4a, bug 6) -----------------------------------------------

// Posa OpenXR (quaternione x, y, z, w e posizione in metri) come matrice OpenVR 3x4.
static void PoseFromQuat(double x, double y, double z, double w, double px, double py, double pz, float m[3][4]) {
    m[0][0] = float(1 - 2 * (y * y + z * z)); m[0][1] = float(2 * (x * y - w * z)); m[0][2] = float(2 * (x * z + w * y));
    m[1][0] = float(2 * (x * y + w * z)); m[1][1] = float(1 - 2 * (x * x + z * z)); m[1][2] = float(2 * (y * z - w * x));
    m[2][0] = float(2 * (x * z - w * y)); m[2][1] = float(2 * (y * z + w * x)); m[2][2] = float(1 - 2 * (x * x + y * y));
    m[0][3] = float(px); m[1][3] = float(py); m[2][3] = float(pz);
}

// Prodotto di pose rigide (rotazione + traslazione) e inversa, in doppia precisione.
static void PoseMul(const float a[3][4], const float b[3][4], float o[3][4]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = 0.0;
            for (int k = 0; k < 3; ++k) v += double(a[i][k]) * b[k][j];
            if (j == 3) v += a[i][3];
            o[i][j] = float(v);
        }
}
static void PoseInv(const float a[3][4], float o[3][4]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = a[j][i];
    for (int i = 0; i < 3; ++i) o[i][3] = -(o[i][0] * a[0][3] + o[i][1] * a[1][3] + o[i][2] * a[2][3]);
}
static double Dist3(const float a[3], const float b[3]) {
    return std::sqrt(double(a[0] - b[0]) * (a[0] - b[0]) + double(a[1] - b[1]) * (a[1] - b[1]) + double(a[2] - b[2]) * (a[2] - b[2]));
}

// Raggio di puntamento nel raw: dal modello di SteamVR (openxr_aim) e dalla
// calibrazione di VDXR (action.cpp, pose OVR / grip / aim di un controller
// sinistro, copiate dal sorgente di VDXR, mbucchia/VirtualDesktop-OpenXR)
// attraverso la catena del backend OpenXR della mod (raw = grip * inverse(GripInRaw)).
static void TestAimInRawModel() {
    // SteamVR: raw nel raggio = inversa di openxr_aim -> pitch +39.4.
    float aim[3][4], inv[3][4];
    AimRotInRaw(AimInRaw(), aim);
    PoseInv(aim, inv);
    Euler e = ExactEuler(inv);
    std::printf("gun frame: SteamVR raw in aim pitch %.3f yaw %.3f roll %.3f deg\n", e.pitch * kDeg, e.yaw * kDeg, e.roll * kDeg);
    CHECK(std::fabs(e.pitch * kDeg - 39.4) < 0.05 && std::fabs(e.yaw * kDeg) < 0.05 && std::fabs(e.roll * kDeg) < 0.05);
    // VDXR (lato sinistro).
    float grip[3][4], vaim[3][4];
    PoseFromQuat(-0.406630576, -0.480359077, -0.434706628, 0.644155979, 0.755659044, 0.849373817, 0.065275185, grip);
    PoseFromQuat(-0.674230397, -0.198649853, -0.616646528, 0.354540110, 0.711199582, 0.760591149, 0.057151683, vaim);
    // Shim: raw = grip * inverse(GripInRaw(sinistra, 20.6)); raggio nel raw = inverse(raw) * aim.
    float gir[3][4], girInv[3][4], raw[3][4], rawInv[3][4], aimInRaw[3][4];
    GripInRaw(true, kGripPitchDeg, gir);
    PoseInv(gir, girInv);
    PoseMul(grip, girInv, raw);
    PoseInv(raw, rawInv);
    PoseMul(rawInv, vaim, aimInRaw);
    Euler v = ExactEuler(aimInRaw);
    std::printf("gun frame: VDXR aim in the shim raw pitch %.3f yaw %.3f roll %.3f deg (model %.1f)\n", v.pitch * kDeg, v.yaw * kDeg,
                v.roll * kDeg, kAimPitchInRawDeg);
    CHECK(std::fabs(v.pitch * kDeg - kAimPitchInRawDeg) < 0.05 && std::fabs(v.yaw * kDeg) < 0.05 && std::fabs(v.roll * kDeg) < 0.05);
    // Posa grip nella aim: VDXR contro SteamVR (openxr_grip in openxr_aim, sinistra x +0.007) entro 0,1 gradi e 1 mm.
    float aimInv[3][4], gripInAimV[3][4], saim[3][4], saimInv[3][4], gripInAimS[3][4];
    PoseInv(vaim, aimInv);
    PoseMul(aimInv, grip, gripInAimV);
    AimRotInRaw(AimInRaw(), saim);
    saim[0][3] = 0.007f; saim[1][3] = kAimOriginY; saim[2][3] = kAimOriginZ;
    PoseInv(saim, saimInv);
    PoseMul(saimInv, gir, gripInAimS);
    const double rotErr = PoseAngleDeg(gripInAimV, gripInAimS);
    const float tv[3] = {gripInAimV[0][3], gripInAimV[1][3], gripInAimV[2][3]}, ts[3] = {gripInAimS[0][3], gripInAimS[1][3], gripInAimS[2][3]};
    const double posErr = Dist3(tv, ts);
    Euler gv = ExactEuler(gripInAimV);
    std::printf("gun frame: grip in aim, VDXR pitch %.2f deg (%.1f, %.1f, %.1f) cm vs SteamVR: rotation %.4f deg, position %.3f mm\n",
                gv.pitch * kDeg, tv[0] * 100.0, tv[1] * 100.0, tv[2] * 100.0, rotErr, posErr * 1000.0);
    CHECK(rotErr < 0.1);
    CHECK(posErr < 0.001);
    CHECK(std::fabs(gv.pitch * kDeg - kGripInAimPitchDeg) < 0.05);
    // Senza misura: OpenVR = costante, OpenXR = openxrgrip - 60.
    CHECK(DefaultAimInRaw(false, true, 20.6f).pitch == kAimPitchInRawDeg);
    CHECK(std::fabs(DefaultAimInRaw(true, true, 20.6f).pitch - kAimPitchInRawDeg) < 1e-4f);
    CHECK(DefaultAimInRaw(true, false, 20.6f).pitch == -60.0f);
    CHECK(std::fabs(DefaultAimInRaw(true, true, 25.0f).pitch + 35.0f) < 1e-4f);
    // Plausibilita' della misura OpenXR.
    AimInRaw a;
    CHECK(AimInRawPlausible(a));
    a.pitch = -22.0f;  // frame_controller di SteamVR misurato coi Touch
    CHECK(AimInRawPlausible(a));
    a.pitch = -95.0f;
    CHECK(!AimInRawPlausible(a));
    a.pitch = -39.4f;
    a.roll = 40.0f;
    CHECK(!AimInRawPlausible(a));
}

// Canna (catena intera: telaio -> angoli esatti -> FRotationMatrix di UE3) nel
// telaio di puntamento A: elevazione e laterale in gradi.
static void BarrelInAim(const float gun[3][4], const float a[3][4], double* elev, double* lat) {
    Euler e = ExactEuler(gun);
    float f[3], r[3], u[3], fa[3], ra[3], ua[3];
    UeAxesFromRotator(e.pitch, e.yaw, e.roll, f, r, u);
    UeAxesFromPose(a, fa, ra, ua);
    const double df = f[0] * fa[0] + f[1] * fa[1] + f[2] * fa[2], du = f[0] * ua[0] + f[1] * ua[1] + f[2] * ua[2],
                 dr = f[0] * ra[0] + f[1] * ra[1] + f[2] * ra[2];
    *elev = std::atan2(du, df) * kDeg;
    *lat = std::atan2(dr, df) * kDeg;
}

// Controller col raggio di puntamento a livello (e a +-30), ruotato attorno al
// raggio di 0/30/60 gradi: la canna sta a gunangle sopra il raggio nel telaio
// del controller (+-0,1 gradi) e niente di lato. Prima (aimpitch 0, yaw
// miscelato): 39,4 gradi sopra e fino a 14 di lato.
static void TestGunFrameLevelAim() {
    double worst = 0.0, worstOld = 0.0, worstOldLat = 0.0;
    for (float aimPitch : {0.0f, 30.0f, -30.0f})
        for (float roll : {0.0f, 30.0f, 60.0f})
            for (float angle : {-10.0f, 0.0f, 5.0f, 15.0f}) {
                float a[3][4], rx[3][4], raw[3][4], gun[3][4];
                PoseFromEuler(aimPitch / float(kDeg), 0.4f, roll / float(kDeg), a);
                RotXDeg(-kAimPitchInRawDeg, rx);
                MulRot3(a, rx, raw);  // raw = A * Rx(+39.4)
                for (int i = 0; i < 3; ++i) raw[i][3] = 0.3f * i - 0.2f;
                GunFrameParams p;
                p.angleDeg = angle;
                GunFrameFromRaw(raw, p, gun);
                double elev = 0.0, lat = 0.0;
                BarrelInAim(gun, a, &elev, &lat);
                worst = std::fmax(worst, std::fmax(std::fabs(elev - angle), std::fabs(lat)));
                if (angle == 0.0f) {
                    // Il codice di prima: raw (aimpitch 0), pitch asin e yaw miscelato.
                    float f[3], r[3], u[3], fa[3], ra[3], ua[3];
                    UeAxesFromRotator(AsinPitch(raw), BlendedYaw(raw), std::atan2(-raw[1][0], raw[1][1]), f, r, u);
                    UeAxesFromPose(a, fa, ra, ua);
                    const double df = f[0] * fa[0] + f[1] * fa[1] + f[2] * fa[2];
                    worstOld = std::fmax(worstOld, std::atan2(f[0] * ua[0] + f[1] * ua[1] + f[2] * ua[2], df) * kDeg);
                    worstOldLat = std::fmax(worstOldLat, std::fabs(std::atan2(f[0] * ra[0] + f[1] * ra[1] + f[2] * ra[2], df) * kDeg));
                }
            }
    std::printf("gun frame: level/+-30 aim x roll 0/30/60 x gunangle -10/0/5/15: worst %.4f deg (before: barrel up to %.1f deg above, "
                "%.1f deg to the side)\n", worst, worstOld, worstOldLat);
    CHECK(worst < 0.1);
    CHECK(worstOld > 39.0 && worstOldLat > 5.0);
    // gunyaw: laterale = atan2(sin y, cos a cos y), elevazione esatta.
    float a[3][4], rx[3][4], raw[3][4], gun[3][4];
    PoseFromEuler(-0.2f, 1.0f, 30.0f / float(kDeg), a);
    RotXDeg(-kAimPitchInRawDeg, rx);
    MulRot3(a, rx, raw);
    GunFrameParams p;
    p.angleDeg = 5.0f;
    p.yawDeg = 8.0f;
    GunFrameFromRaw(raw, p, gun);
    double elev = 0.0, lat = 0.0;
    BarrelInAim(gun, a, &elev, &lat);
    const double wantLat = std::atan2(std::sin(8.0 / kDeg), std::cos(5.0 / kDeg) * std::cos(8.0 / kDeg)) * kDeg;
    CHECK(std::fabs(elev - 5.0) < 0.05 && std::fabs(lat - wantLat) < 0.05 && lat > 7.9);
    // Lo stockangle dello sviluppatore (aimpitch -24 -> 15.4) da' la stessa canna di prima (raw ruotato di -24).
    float rold[3][4], gold[3][4];
    RotXDeg(-24.0f, rold);
    MulRot3(raw, rold, gold);
    p.angleDeg = 15.4f;
    p.yawDeg = 0.0f;
    GunFrameFromRaw(raw, p, gun);
    CHECK(PoseAngleDeg(gun, gold) < 0.01);
}

// Rotazione attorno al palmo: il punto dell'arma sul palmo vero resta sul palmo
// per ogni angolo; con l'angolo nullo sul raw il polso e' l'origine del raw
// (la posizione di prima); "openxrgrip off" (posa = grip) ruota attorno all'origine.
static void TestGunPivot() {
    std::mt19937 rng(426u);
    double worstPalm = 0.0, worstRef = 0.0, worstOff = 0.0, worstLine = 0.0;
    for (int i = 0; i < 2000; ++i) {
        float raw[3][4];
        RandomPose(rng, raw);
        const bool left = (i & 1) != 0;
        float palm[3];
        PalmOf(raw, left, true, palm);
        for (float angle : {-30.0f, 0.0f, 12.0f, 39.4f}) {
            GunFrameParams p;
            p.left = left;
            p.angleDeg = angle;
            p.yawDeg = (i % 3) * 4.0f;
            float g[3][4];
            GunFrameFromRaw(raw, p, g);
            // punto del palmo nel telaio dell'arma = o (origine della grip nel raw) per ogni angolo
            const float o[3] = {left ? kGripOriginX : -kGripOriginX, kGripOriginY, kGripOriginZ};
            float back[3];
            for (int k = 0; k < 3; ++k) back[k] = g[k][3] + g[k][0] * o[0] + g[k][1] * o[1] + g[k][2] * o[2];
            worstPalm = std::fmax(worstPalm, Dist3(back, palm));
            // la linea della canna passa per il palmo (entro la parte laterale/verticale di o: 7 e 1,8 mm)
            float d[3] = {palm[0] - g[0][3], palm[1] - g[1][3], palm[2] - g[2][3]};
            const float t = -(d[0] * g[0][2] + d[1] * g[1][2] + d[2] * g[2][2]);
            float perp[3] = {d[0] + t * g[0][2], d[1] + t * g[1][2], d[2] + t * g[2][2]};
            const float zero[3] = {0, 0, 0};
            worstLine = std::fmax(worstLine, Dist3(perp, zero));
            if (angle == 39.4f && p.yawDeg == 0.0f) {
                const float rt[3] = {raw[0][3], raw[1][3], raw[2][3]}, gt[3] = {g[0][3], g[1][3], g[2][3]};
                worstRef = std::fmax(worstRef, Dist3(rt, gt));
            }
            p.gripFix = false;
            GunFrameFromRaw(raw, p, g);
            const float rt[3] = {raw[0][3], raw[1][3], raw[2][3]}, gt[3] = {g[0][3], g[1][3], g[2][3]};
            worstOff = std::fmax(worstOff, Dist3(rt, gt));
        }
    }
    std::printf("gun pivot: palm kept %.4f mm, barrel line off the palm %.2f mm, angle 0 on the raw = raw origin %.4f mm, "
                "openxrgrip off %.4f mm\n", worstPalm * 1000.0, worstLine * 1000.0, worstRef * 1000.0, worstOff * 1000.0);
    CHECK(worstPalm < 1e-4);
    CHECK(worstLine < 0.0075);
    CHECK(worstRef < 1e-4);
    CHECK(worstOff < 1e-6);
}

// B+Y: la linea di mira nel telaio di puntamento del controller da' l'angolo
// della presa anche col controller ruotato. La formula di prima (beccheggio
// della testa - beccheggio assoluto del raw) sbagliava appena c'era rollio.
static void TestSightLineCalibration() {
    double worst = 0.0, worstOld = 0.0;
    for (float roll : {0.0f, 30.0f, 60.0f})
        for (float angle : {-10.0f, 0.0f, 15.4f, 40.0f}) {
            // Controller qualunque, canna (con l'angolo) che guarda dove guarda la testa.
            float a[3][4], rx[3][4], raw[3][4], gun[3][4];
            PoseFromEuler(-0.1f, 0.7f, roll / float(kDeg), a);
            RotXDeg(-kAimPitchInRawDeg, rx);
            MulRot3(a, rx, raw);
            GunFrameParams p;
            p.angleDeg = angle;
            GunFrameFromRaw(raw, p, gun);
            const float head[3] = {-gun[0][2], -gun[1][2], -gun[2][2]};
            float elev = 0.0f, lat = 0.0f;
            SightLineInAim(raw, AimInRaw(), head, &elev, &lat);
            worst = std::fmax(worst, std::fmax(std::fabs(elev - angle), std::fabs(lat)));
            // Prima: offset aimpitch = pitch della testa - pitch del raw -> stockangle = offset + 39.4.
            const double oldAngle = (std::asin(head[1]) - AsinPitch(raw)) * kDeg + 39.4;
            worstOld = std::fmax(worstOld, std::fabs(oldAngle - angle));
        }
    std::printf("B+Y calibration: roll 0/30/60 x angle -10/0/15.4/40: worst %.4f deg (old formula %.1f deg)\n", worst, worstOld);
    CHECK(worst < 0.01);
    CHECK(worstOld > 5.0);
}

static void TestAimpitchMigration() {
    static const ComputedChange kComputed[] = {{10, "aimpitch", &MigrateAimpitchLine}};
    const size_t nc = sizeof(kAimpitchCommentChanges) / sizeof(kAimpitchCommentChanges[0]);
    std::string done;
    // Ini distribuito fino al 25/10: commento + "aimpitch 0" -> presa normale, gunangle 0, commenti nuovi.
    std::vector<std::string> lines = {"aim hand",
                                      "# Pitch correction of the controller ray (degrees), only for \"aim hand\".",
                                      "# Gunstock: aim the stock at something far away, looking at it through the",
                                      "# sights in the middle of your view, and hold B + Y together for 1 second: the",
                                      "# angle is measured and saved here.",
                                      "aimpitch 0",
                                      "spread on 1 0.1 0.5 1.5",
                                      "configversion 9"};
    CHECK(MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1));
    CHECK(lines.size() == 11);
    CHECK(lines.size() == 11 && lines[1].rfind("# Controller hold:", 0) == 0 && lines[4].find("Basic > Controller hold") != std::string::npos &&
          lines[5] == "holdmode normal" && lines[6] == kGunAngleComment1 && lines[8] == "gunangle 0" && lines[9] == "spread on 1 0.1 0.5 1.5" &&
          lines[10] == "configversion 10");
    // Calibrato dallo sviluppatore: -24 -> calcio 15.4 (anche con la virgola decimale).
    for (const char* v : {"aimpitch -24.0", "aimpitch -24,0", "AimPitch -24  # calcio"}) {
        lines = {v, "configversion 9"};
        CHECK(MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1));
        CHECK(lines.size() == 7 && lines[0] == "holdmode gunstock" && lines[3] == "gunangle 0" && lines[4] == kStockAngleComment1 &&
              lines[5] == "stockangle 15.4" && lines[6] == "configversion 10");
    }
    // Fuori da +-45 (calibrazione sbagliata della 0.9.3, il parser la azzerava) e valore mancante: come 0.
    for (const char* v : {"aimpitch 60", "aimpitch -67", "aimpitch"}) {
        lines = {v, "configversion 9"};
        CHECK(MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1));
        CHECK(lines.size() == 5 && lines[0] == "holdmode normal" && lines[3] == "gunangle 0");
    }
    // Altri valori: stockangle = X + 39.4.
    lines = {"aimpitch 10", "configversion 9"};
    MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1);
    CHECK(lines.size() == 7 && lines[5] == "stockangle 49.4");
    // Regressione finale: i ritocchi dall'Advanced (0,05 per click) sotto 5 gradi non sono un calcio -> presa normale;
    // da 5 in su (e i valori di B+Y, multipli di 0,5) resta il calcio.
    for (const char* v : {"aimpitch 0.05", "aimpitch -0.35", "aimpitch 1.00", "aimpitch -2.50", "aimpitch 4.95", "aimpitch -4.9"}) {
        lines = {v, "configversion 9"};
        CHECK(MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1));
        CHECK(lines.size() == 5 && lines[0] == "holdmode normal" && lines[3] == "gunangle 0");
    }
    for (const char* v : {"aimpitch 5.0", "aimpitch -5", "aimpitch -39.5"}) {
        lines = {v, "configversion 9"};
        CHECK(MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1));
        CHECK(lines.size() == 7 && lines[0] == "holdmode gunstock");
    }
    lines = {"aimpitch -5", "configversion 9"};
    MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1);
    CHECK(lines.size() == 7 && lines[5] == "stockangle 34.4");
    // Chiave assente: nessuna riga in piu'. Gia' alla 10: niente (idempotente).
    lines = {"hud 0.4 100", "configversion 9"};
    CHECK(MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1));
    CHECK(lines.size() == 2 && lines[0] == "hud 0.4 100");
    lines = {"aimpitch -24.0", "configversion 10"};
    CHECK(!MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1));
    CHECK(lines[0] == "aimpitch -24.0");
    // Due passate di fila: la seconda non tocca piu' nulla.
    lines = {"aimpitch -24.0", "configversion 9"};
    MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1);
    std::vector<std::string> once = lines;
    CHECK(!MigrateConfigLines(lines, 10, kAimpitchCommentChanges, nc, &done, kComputed, 1) && lines == once);
}

static void TestNextNumericChoice() {
    const std::vector<std::string> c = {"-15", "-10", "-5", "0", "5", "10", "15"};
    CHECK(NextNumericChoice(c, "2.5", 1) == 4);    // 2.5 -> 5
    CHECK(NextNumericChoice(c, "2.5", -1) == 3);   // 2.5 -> 0
    CHECK(NextNumericChoice(c, "17", 1) == 0);     // oltre il bordo: si riparte
    CHECK(NextNumericChoice(c, "-20", -1) == 6);
    CHECK(NextNumericChoice(c, "-12", -1) == 0);
    CHECK(NextNumericChoice(c, "abc", 1) == 0 && NextNumericChoice(c, "abc", -1) == 6);
    CHECK(MenuStepFor("gunangle", 0.0f).step == 1.0f && MenuStepFor("stockangle", 15.4f).step == 1.0f &&
          MenuStepFor("gunyaw", 0.0f).step == 1.0f);
    const std::vector<std::string>* w = AdvancedWordChoices("holdmode");
    CHECK(w && w->size() == 2 && (*w)[0] == "normal" && (*w)[1] == "gunstock");
}

// --- Review WI-4a (round 1) ---------------------------------------------------------

// Rotazione di deg gradi attorno all'asse ax (Rodrigues), traslazione nulla.
static void AxisAngleRot(const double ax[3], double deg, float m[3][4]) {
    const double n = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    const double x = ax[0] / n, y = ax[1] / n, z = ax[2] / n, c = std::cos(deg / kDeg), s = std::sin(deg / kDeg), t = 1.0 - c;
    const double r[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                            {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                            {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i][j] = static_cast<float>(r[i][j]);
        m[i][3] = 0.0f;
    }
}

// "movedir hand": con il raggio di puntamento quasi verticale (mano rilassata)
// e un tremolio di 2 gradi in ogni direzione, la direzione (yaw miscelato) si
// sposta meno di 3 gradi; lo yaw esatto, usato fino al round 1, gira di decine.
static void TestPointingHeadingNearVertical() {
    const AimInRaw aim;  // Touch: raggio 39,4 gradi sotto il raw -Z
    float rx[3][4], am[3][4];
    RotXDeg(-kAimPitchInRawDeg, rx);
    AimRotInRaw(aim, am);
    std::printf("pointing heading near vertical (2 deg wobble, 26 axes):");
    for (float pitch : {-80.0f, -85.0f, -88.0f}) {
        float a0[3][4];
        PoseFromEuler(pitch / float(kDeg), 30.0f / float(kDeg), 0.0f, a0);
        auto headingOf = [&](const float a[3][4], double* exact) {
            float raw[3][4], la[3][4];
            MulRot3(a, rx, raw);  // raw = A * Rx(+39.4): il raggio di puntamento e' A
            MulRot3(raw, am, la);
            *exact = ExactEuler(la).yaw * kDeg;
            return PointingHeadingYaw(raw, aim) * kDeg;
        };
        double e0 = 0.0;
        const double h0 = headingOf(a0, &e0);
        double swingH = 0.0, swingE = 0.0;
        for (int ix = -1; ix <= 1; ++ix)
            for (int iy = -1; iy <= 1; ++iy)
                for (int iz = -1; iz <= 1; ++iz) {
                    if (!ix && !iy && !iz) continue;
                    const double ax[3] = {double(ix), double(iy), double(iz)};
                    for (double deg : {-2.0, 2.0}) {
                        float w[3][4], a[3][4];
                        AxisAngleRot(ax, deg, w);
                        MulRot3(w, a0, a);
                        double e = 0.0;
                        const double h = headingOf(a, &e);
                        swingH = std::fmax(swingH, std::fabs(std::remainder(h - h0, 360.0)));
                        swingE = std::fmax(swingE, std::fabs(std::remainder(e - e0, 360.0)));
                    }
                }
        std::printf(" ray %.0f: heading %.2f deg (exact yaw %.1f)", pitch, swingH, swingE);
        CHECK(swingH < 3.0);
        if (pitch <= -85.0f) CHECK(swingE > 15.0);  // il problema del round 1
    }
    std::printf("\n");
    // Lontano dalla verticale la direzione e' quella del raggio (a rollio 0 coincide con lo yaw esatto).
    float a[3][4], raw[3][4];
    PoseFromEuler(-20.0f / float(kDeg), 50.0f / float(kDeg), 0.0f, a);
    MulRot3(a, rx, raw);
    CHECK(std::fabs(PointingHeadingYaw(raw, aim) * kDeg - 50.0) < 0.01);
    // Torretta: yaw fermo oltre 70 gradi dall'orizzonte.
    CHECK(!TurretYawHeld(0.0f) && !TurretYawHeld(-25.0f) && !TurretYawHeld(69.9f) && !TurretYawHeld(-69.9f));
    CHECK(TurretYawHeld(70.1f) && TurretYawHeld(-70.1f) && TurretYawHeld(-89.0f));
}

// Misura del raggio di puntamento di OpenXR: ricordata per profilo, rimisura
// ogni 30 s, misure non plausibili scartate.
static void TestAimRayMeasure() {
    const std::string touch = "/interaction_profiles/oculus/touch_controller";
    AimRayMeasure m;
    double t = 1000.0;
    int ev = AimRayMeasure::kNone, n = 0;
    CHECK(m.SetProfile(touch, t) == AimRayMeasure::kNone && !m.valid && m.Due(t));
    const float s1[3] = {-39.4f, 0.0f, 0.0f};
    while (!m.valid && n < 50) {
        if (m.Due(t)) {
            ev = m.Sample(t, s1);
            ++n;
        }
        t += 50.0;  // 20 Hz di chiamate, campioni a 100 ms
    }
    CHECK(m.valid && ev == AimRayMeasure::kMeasured && n == 10 && std::fabs(m.cur.pitch + 39.4f) < 1e-4f);
    CHECK(!m.Due(t + 1000.0));  // misura fatta: niente campioni fino a 30 s
    // Profilo perso e ritrovato (controller spento e riacceso): la misura torna subito.
    CHECK(m.SetProfile("", t) == AimRayMeasure::kNone && !m.valid && !m.Due(t + 500.0));
    CHECK(m.SetProfile(touch, t + 2000.0) == AimRayMeasure::kReused && m.valid && m.cur.pitch == -39.4f);
    t += 2000.0;
    CHECK(!m.Due(t + 29000.0) && m.Due(t + 30000.0));
    // Rimisura: 0,1 gradi = nessun cambio; 1 grado = cambio.
    t += 30000.0;
    const float s2[3] = {-39.3f, 0.0f, 0.0f}, s3[3] = {-38.4f, 0.1f, 0.0f};
    for (int i = 0; i < 10; ++i, t += 100.0) ev = m.Sample(t, s2);
    CHECK(ev == AimRayMeasure::kNone && m.cur.pitch == -39.4f && m.valid);
    t += 30000.0;
    for (int i = 0; i < 10; ++i, t += 100.0) ev = m.Sample(t, s3);
    CHECK(ev == AimRayMeasure::kChanged && std::fabs(m.cur.pitch + 38.4f) < 1e-3f);
    // Con una misura in uso, una non plausibile (posa di pinch) non la sostituisce.
    const float bad[3] = {-85.0f, 40.0f, 0.0f};
    t += 30000.0;
    for (int i = 0; i < 10; ++i, t += 100.0) ev = m.Sample(t, bad);
    CHECK(ev == AimRayMeasure::kNone && m.valid && std::fabs(m.cur.pitch + 38.4f) < 1e-3f);
    // Profilo nuovo: niente da riusare; non plausibile = avviso, resta il modello.
    CHECK(m.SetProfile("/interaction_profiles/htc/vive_controller", t) == AimRayMeasure::kNone && !m.valid);
    for (int i = 0; i < 10; ++i, t += 100.0) ev = m.Sample(t, bad);
    CHECK(ev == AimRayMeasure::kImplausible && !m.valid);
    // Campioni che saltano (mani tracciate in movimento): la serie non si chiude.
    m.SetProfile(touch, t);  // ritorno dei Touch: riusata (-38.4)
    CHECK(m.valid && std::fabs(m.cur.pitch + 38.4f) < 1e-3f);
    m.ResetSpace();  // openxrgrip cambiato: spazio della mano nuovo
    CHECK(!m.valid);
    for (int i = 0; i < 40; ++i, t += 100.0) {
        const float w[3] = {-39.4f + (i % 2 ? 0.5f : 0.0f), 0.0f, 0.0f};
        ev = m.Sample(t, w);
    }
    CHECK(!m.valid && ev == AimRayMeasure::kNone);
    CHECK(m.SetProfile("", t) == AimRayMeasure::kNone && m.SetProfile(touch, t) == AimRayMeasure::kNone && !m.valid);  // cache vuota
    std::printf("OpenXR aim measurement: measured after 10 samples, profile back = re-used at once, re-measure 30 s (0.1 deg kept, "
                "1 deg changed), implausible kept out, new hand space = cache cleared\n");
}

// --- WI-3: corpo, seduti, accucciata ---------------------------------------------

static void TestNeckAnchor() {
    const float d2r = 0.01745329252f;
    // Neck di Walker non spostato (media in piedi) e occhio a fp 15 avanti.
    const float neck[2] = {0.5f, 0.4f};
    const float nf = 4.0f, nu = 6.5f;
    float eye[3] = {15.0f, 0.0f, 32.5f}, s[2];
    // Testa dritta: gli occhi di Walker (neck + 8.2 avanti) finiscono sull'occhio.
    NeckAnchorShift(eye, 0.0f, 0.0f, 0.0f, nf, nu, neck, s);
    CHECK(std::fabs(neck[0] + s[0] + kWalkerEyeFwdOfNeck - eye[0]) < 1e-4f && std::fabs(neck[1] + s[1] - eye[1]) < 1e-4f);
    // Testa che ruota attorno al perno (occhi sulla sfera del modello): lo spostamento non cambia.
    const float pivot[3] = {eye[0] - nf, 0.0f, eye[2] - nu};
    float maxDev = 0.0f;
    for (float pitch : {0.0f, -15.0f, -30.0f, -60.0f, -85.0f, 20.0f})
        for (float yaw : {0.0f, 60.0f, -120.0f, 179.0f})
            for (float roll : {0.0f, 15.0f}) {
                float f[3], r[3], u[3];
                UeAxesFromRotator(pitch * d2r, yaw * d2r, roll * d2r, f, r, u);
                float e[3];
                for (int i = 0; i < 3; ++i) e[i] = pivot[i] + f[i] * nf + u[i] * nu;
                float t[2];
                NeckAnchorShift(e, pitch * d2r, yaw * d2r, roll * d2r, nf, nu, neck, t);
                maxDev = (std::max)(maxDev, std::hypot(t[0] - s[0], t[1] - s[1]));
            }
    CHECK(maxDev < 1e-3f);
    // Il giocatore si sporge (il perno avanza di 10): il corpo lo segue di 10.
    float lean[3] = {eye[0] + 10.0f, eye[1] - 3.0f, eye[2]};
    float t[2];
    NeckAnchorShift(lean, 0.0f, 0.0f, 0.0f, nf, nu, neck, t);
    CHECK(std::fabs(t[0] - s[0] - 10.0f) < 1e-4f && std::fabs(t[1] - s[1] + 3.0f) < 1e-4f);
    // Passo: al massimo 200 u/s, lunghezza al massimo 60.
    float cur[2] = {0.0f, 0.0f};
    const float far[2] = {100.0f, 0.0f};
    NeckAnchorStep(cur, far, 0.01f, 200.0f, 60.0f);
    CHECK(std::fabs(cur[0] - 2.0f) < 1e-4f && cur[1] == 0.0f);
    for (int i = 0; i < 100; ++i) NeckAnchorStep(cur, far, 0.01f, 200.0f, 60.0f);
    CHECK(std::fabs(cur[0] - 60.0f) < 1e-4f);
    const float near[2] = {59.5f, 0.0f};
    NeckAnchorStep(cur, near, 0.01f, 200.0f, 60.0f);
    CHECK(cur[0] == 59.5f);
    // Seduti: occhi all'altezza misurata di Walker; misura assurda -> costante.
    CHECK(std::fabs(SeatedEyeHeight(30.55f) - 36.25f) < 1e-4f && SeatedEyeHeight(-1000.0f) == kWalkerEyeStand);
    // Fix 1: occhio a testa dritta (abbassamento del corpo sul perno del collo):
    // girando o chinando la testa attorno al perno resta l'occhio dritto.
    float maxPivotErr = 0.0f;
    for (float pitch : {0.0f, -15.0f, -30.0f, -60.0f, -85.0f, 20.0f})
        for (float yaw : {0.0f, 60.0f, -120.0f})
            for (float roll : {0.0f, 15.0f}) {
                float f[3], r[3], u[3];
                UeAxesFromRotator(pitch * d2r, yaw * d2r, roll * d2r, f, r, u);
                const float ez = pivot[2] + f[2] * nf + u[2] * nu;
                maxPivotErr = (std::max)(maxPivotErr, std::fabs(NeckPivotEyeZ(ez, pitch * d2r, yaw * d2r, roll * d2r, nf, nu) - eye[2]));
            }
    CHECK(maxPivotErr < 1e-3f);
    {
        float f[3], r[3], u[3];
        UeAxesFromRotator(-85.0f * d2r, 0.0f, 0.0f, f, r, u);
        const float drop = eye[2] - (pivot[2] + f[2] * nf + u[2] * nu);
        CHECK(std::fabs(drop - 9.92f) < 0.02f);  // l'occhio scende di 9,9 unita' a -85: il perno no
        std::printf("neck pivot eye: -85 deg moves the eye %.2f units down, pivot eye error over 36 poses %.5f\n", drop, maxPivotErr);
    }
    // Portata: bersaglio a 30 avanti dalla spalla, braccio 26 (margine 2 -> R 24): avanti di 6.
    {
        const float v[3] = {30.0f, 0.0f, 0.0f};
        CHECK(std::fabs(ReachForwardNeed(v, 24.0f) - 6.0f) < 1e-4f);
        const float v2[3] = {20.0f, 5.0f, -5.0f};  // gia' raggiungibile
        CHECK(ReachForwardNeed(v2, 24.0f) == 0.0f);
        const float v3[3] = {10.0f, 30.0f, 0.0f};  // di lato oltre la portata: avanti fino al punto piu' vicino
        CHECK(std::fabs(ReachForwardNeed(v3, 24.0f) - 10.0f) < 1e-4f);
        const float v4[3] = {28.0f, 8.0f, -10.0f};  // la distanza dopo lo spostamento e' R
        const float n4 = ReachForwardNeed(v4, 24.0f);
        CHECK(std::fabs(std::sqrt((28.0f - n4) * (28.0f - n4) + 64.0f + 100.0f) - 24.0f) < 1e-3f);
        // Fix 2: margine con segno (negativo = spazio per arretrare): spinta voluta = spinta attuale + margine.
        CHECK(std::fabs(ReachForwardMargin(v2, 24.0f) - (20.0f - std::sqrt(576.0f - 50.0f))) < 1e-4f && ReachForwardMargin(v2, 24.0f) < 0.0f);
        CHECK(std::fabs(ReachForwardMargin(v, 24.0f) - 6.0f) < 1e-4f);
        // Ciclo: bersaglio raggiungibile dopo una spinta di 5 -> la spinta voluta torna 0 (prima restava 5).
        {
            float push = 5.0f;
            const float vNear[3] = {20.0f - push, 5.0f, -5.0f};  // misurato con la mesh gia' avanti di 5
            const float want = (std::max)(0.0f, push + ReachForwardMargin(vNear, 24.0f));
            CHECK(want == 0.0f);
            const float vFar[3] = {30.0f - push, 0.0f, 0.0f};  // serve 6 in tutto: con 5 gia' dati, 1 in piu'
            CHECK(std::fabs(push + ReachForwardMargin(vFar, 24.0f) - 6.0f) < 1e-4f);
        }
    }
    // Anello del collo: occhio 10 sopra e 11 davanti al Neck, sguardo a -85 con 50 gradi sotto:
    // il bordo guarda 45 gradi indietro; anello a 4 davanti al Neck, 1 sopra.
    {
        const float neckP[3] = {0.0f, 0.0f, 27.0f};
        const float e1[3] = {11.0f, 0.0f, 37.0f};
        const float c1 = NeckRingClearance(e1, -85.0f * d2r, 0.0f, 50.0f * d2r, neckP, 4.0f, 1.0f, 1.0f);
        // profondita' 9, serve 9 + 1 = 10 dietro; ora 11 - 4 = 7 -> 3 indietro.
        CHECK(std::fabs(c1 - 3.0f) < 1e-3f);
        // Occhio sotto l'anello (seduti, testa dritta a 36,25 e chinata a 26,3): niente.
        const float e2[3] = {11.0f, 0.0f, 26.3f};
        CHECK(NeckRingClearance(e2, -85.0f * d2r, 0.0f, 50.0f * d2r, neckP, 4.0f, 1.0f, 1.0f) == 0.0f);
        // Guardando a -30 il bordo guarda avanti: niente.
        CHECK(NeckRingClearance(e1, -30.0f * d2r, 0.0f, 50.0f * d2r, neckP, 4.0f, 1.0f, 1.0f) == 0.0f);
        // Sguardo girato di 90 gradi: lo spostamento e' lungo lo sguardo (il punto dell'anello e' a destra).
        const float e3[3] = {0.0f, 11.0f, 37.0f};
        CHECK(std::fabs(NeckRingClearance(e3, -85.0f * d2r, 90.0f * d2r, 50.0f * d2r, neckP, 4.0f, 1.0f, 1.0f) - 3.0f) < 1e-3f);
    }
}

static void TestSeatedDetector() {
    auto run = [](float height, float calib, bool present = true, float noise = 0.01f) {
        SeatedDetector d;
        d.Start();
        int r = -1;
        for (int i = 0; i < 400 && r < 0; ++i) r = d.Step(i * 0.011, height + noise * std::sin(i * 0.7f), present, calib);
        return r;
    };
    CHECK(run(1.15f, -1.0f) == 1);   // seduto
    CHECK(run(1.20f, -1.0f) == 1);
    CHECK(run(1.33f, -1.0f) == 1);   // seduto alto (fix 1: soglia 1,35)
    CHECK(run(1.70f, -1.0f) == 0);   // in piedi
    CHECK(run(1.45f, -1.0f) == 0);   // in piedi, basso
    // Fix 1: seduti all'avvio e poi in piedi: dopo 3 s oltre 30 cm sopra il riferimento seduto si
    // rimisura; un'alzata di 2 s o un dondolio di 25 cm non la fanno ripartire.
    {
        SeatedStandCheck sc;
        int fires = 0;
        double at = -1.0;
        for (int i = 0; i < 72 * 20; ++i) {
            const double t = i / 72.0;
            const float y = (t < 5.0 ? 1.15f : 1.72f) + 0.01f * std::sin(i * 0.3f);
            if (sc.Step(t, y, 1.15f, true)) {
                ++fires;
                if (at < 0.0) at = t;
            }
        }
        CHECK(fires >= 1 && at > 7.9 && at < 8.1);
        SeatedStandCheck sc2;
        int fires2 = 0;
        for (int i = 0; i < 72 * 60; ++i) {
            const double t = i / 72.0, ph = std::fmod(t, 10.0);
            const float y = ph > 3.0 && ph < 5.0 ? 1.55f : 1.15f + 0.25f * static_cast<float>(ph > 6.0 && ph < 9.0);
            fires2 += sc2.Step(t, y, 1.15f, true);
        }
        CHECK(fires2 == 0);
        SeatedStandCheck sc3;  // non attivo (seated on/off o gia' in piedi): mai
        int fires3 = 0;
        for (int i = 0; i < 72 * 10; ++i) fires3 += sc3.Step(i / 72.0, 1.75f, 1.15f, false);
        CHECK(fires3 == 0);
        std::printf("seated then standing up: re-measure at %.2f s (head 57 cm higher from 5 s)\n", at);
    }
    CHECK(run(1.45f, 1.85f) == 1);   // 40 cm sotto l'altezza salvata: seduto
    CHECK(run(1.62f, 1.85f) == 0);
    CHECK(run(1.15f, -1.0f, false) == -1);  // visore non indossato: niente decisione
    // Senza cattura non decide mai; dopo una decisione nemmeno (mai a meta' missione).
    SeatedDetector d;
    CHECK(d.Step(0.0, 1.1f, true, -1.0f) == -1);
    d.Start();
    int r = -1;
    double t = 0.0;
    for (; r < 0 && t < 5.0; t += 0.011) r = d.Step(t, 1.1f, true, -1.0f);
    CHECK(r == 1 && t >= SeatedDetector::kWindow && t < SeatedDetector::kWindow + 0.05);
    CHECK(d.Step(t + 1.0, 1.8f, true, -1.0f) == -1);
    // Visore tolto a meta' finestra: la finestra riparte.
    d.Start();
    r = -1;
    for (int i = 0; i < 200; ++i) d.Step(i * 0.011, 1.1f, true, -1.0f);
    d.Step(2.3, 1.1f, false, -1.0f);
    for (t = 2.31; r < 0 && t < 10.0; t += 0.011) r = d.Step(t, 1.1f, true, -1.0f);
    CHECK(r == 1 && t > 2.31 + SeatedDetector::kWindow - 0.02);
    // Mediana: un picco breve (visore in mano per un attimo) non cambia la decisione.
    d.Start();
    r = -1;
    for (int i = 0; r < 0 && i < 400; ++i) r = d.Step(i * 0.011, (i > 100 && i < 130) ? 1.9f : 1.12f, true, -1.0f);
    CHECK(r == 1 && std::fabs(d.lastMedian - 1.12f) < 1e-4f);
}

static void TestCrouchView() {
    const float dt = 1.0f / 72.0f;
    auto base = []() {
        CrouchViewIn in;
        in.mode = 0;
        in.active = true;
        in.standUp = 10.0f;
        in.rampTime = 0.35f;
        in.dt = 1.0f / 72.0f;
        in.buttonCrouch = true;  // m_wantsToCrouch acceso dal tasto (i casi del riparo lo spengono)
        return in;
    };
    // 1) Tasto da in piedi: V 0 -> 20 entro crouchtime + 0,15 s, monotona, al massimo 3 unita' per tick.
    {
        CrouchView cv;
        CrouchViewIn in = base();
        float prev = 0.0f, maxStep = 0.0f;
        bool mono = true;
        int reachedAt = -1;
        for (int i = 0; i < 72; ++i) {
            in.G = 20.0f;
            CrouchViewOut o = cv.Step(in);
            CHECK(o.comp == 20.0f);
            if (o.v < prev - 1e-5f) mono = false;
            maxStep = (std::max)(maxStep, o.v - prev);
            prev = o.v;
            if (reachedAt < 0 && std::fabs(o.v - 20.0f) < 1e-3f) reachedAt = i;
        }
        CHECK(mono && maxStep <= 3.0f && reachedAt >= 0 && reachedAt * dt <= 0.35f + 0.15f && cv.source == CrouchView::kButton);
        // In piedi col tasto: G torna 0, V scende a 0 in modo continuo.
        prev = 20.0f;
        mono = true;
        for (int i = 0; i < 72; ++i) {
            in.G = 0.0f;
            CrouchViewOut o = cv.Step(in);
            if (o.v > prev + 1e-5f) mono = false;
            prev = o.v;
        }
        CHECK(mono && prev == 0.0f && cv.source == CrouchView::kNone);
    }
    // 2) Accucciata vera: V resta 0, comp annulla G (testa 1:1).
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.realDrop = 20.0f;
        in.ownedByReal = true;
        float maxV = 0.0f;
        for (int i = 0; i < 100; ++i) {
            in.G = 20.0f;
            CrouchViewOut o = cv.Step(in);
            maxV = (std::max)(maxV, o.v);
            CHECK(o.comp == 20.0f);
        }
        CHECK(maxV == 0.0f && cv.source == CrouchView::kReal);
    }
    // 3) Mezzo accucciato davvero (10) poi tasto: V = 20 - 10; la testa che si muove dopo non cambia V.
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.realDrop = 10.0f;
        in.G = 20.0f;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        CHECK(std::fabs(cv.v - 10.0f) < 1e-3f);
        for (int i = 0; i < 200; ++i) {
            in.realDrop = 10.0f + 8.0f * std::sin(i * 0.1f);
            cv.Step(in);
        }
        CHECK(std::fabs(cv.v - 10.0f) < 1e-3f);
    }
    // 4) Tasto, poi accucciata vera di 40 cm (20 unita') e in piedi: V resta (esclusiva del tasto),
    //    niente salto rialzandosi (vista = testa + V, V costante).
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.G = 20.0f;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        float view0 = -(in.realDrop + cv.v), maxJump = 0.0f, prevView = view0;
        for (int i = 0; i <= 60; ++i) {  // giu' in 0,4 s e su
            in.realDrop = i <= 30 ? 20.0f * i / 30.0f : 20.0f * (60 - i) / 30.0f;
            CrouchViewOut o = cv.Step(in);
            float view = -(in.realDrop + o.v);
            maxJump = (std::max)(maxJump, std::fabs(view - prevView));
            prevView = view;
        }
        CHECK(std::fabs(cv.v - 20.0f) < 1e-3f && maxJump < 0.7f && cv.source == CrouchView::kButton);
    }
    // 5) Accucciata vera, poi in piedi ma il gioco tiene Walker giu' (soffitto): dopo 0,5 s "held",
    //    V = G in un colpo sotto il battito.
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.G = 20.0f;
        in.ownedByReal = true;
        in.realDrop = 20.0f;
        for (int i = 0; i < 30; ++i) cv.Step(in);
        in.realDrop = 0.0f;
        bool blinked = false;
        int heldAt = -1;
        for (int i = 0; i < 60; ++i) {
            CrouchViewOut o = cv.Step(in);
            if (o.blink) blinked = true;
            if (heldAt < 0 && cv.source == CrouchView::kHeld) heldAt = i;
        }
        CHECK(blinked && heldAt >= 0 && heldAt * dt > 0.45f && heldAt * dt < 0.6f && cv.v == 20.0f);
        // Subito dopo un ricentramento la discesa vera non vale: niente "held".
        CrouchView cv2;
        in.realDrop = 20.0f;
        for (int i = 0; i < 30; ++i) cv2.Step(in);
        in.realDrop = 0.0f;
        in.recentRecenter = true;
        for (int i = 0; i < 30; ++i) cv2.Step(in);
        CHECK(cv2.source == CrouchView::kReal && cv2.v == 0.0f);
    }
    // 6) Copertura: in piedi V 0 (ci si abbassa da soli), seduti V = G.
    for (bool seated : {false, true}) {
        CrouchView cv;
        CrouchViewIn in = base();
        in.buttonCrouch = false;  // il riparo non accende m_wantsToCrouch
        in.G = 20.0f;
        in.inCover = in.coverRecent = true;
        in.seated = seated;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        CHECK(cv.source == CrouchView::kCover && std::fabs(cv.v - (seated ? 20.0f : 0.0f)) < 1e-3f);
        // Fix 1: uscito dal riparo ancora accucciato, V resta (prima dopo ~1,8 s diventava
        // "tasto" e la vista scendeva di 20 col giocatore fermo).
        in.inCover = false;
        float vMin = 1e9f, vMax = -1e9f;
        for (int i = 0; i < 72 * 3; ++i) {
            in.coverRecent = i < 108;  // 1,5 s
            cv.Step(in);
            vMin = (std::min)(vMin, cv.v);
            vMax = (std::max)(vMax, cv.v);
        }
        CHECK(cv.source == CrouchView::kCover && vMax - vMin < 1e-3f && std::fabs(cv.v - (seated ? 20.0f : 0.0f)) < 1e-3f);
        // Poi il tasto (m_wantsToCrouch acceso): accucciata del tasto, V -> G.
        in.buttonCrouch = true;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        CHECK(cv.source == CrouchView::kButton && std::fabs(cv.v - 20.0f) < 1e-3f);
    }
    // 7) Non attiva (mezzo, scena, morto): azzerata subito; modi vecchi = comp di prima, V 0.
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.G = 20.0f;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        in.active = false;
        CrouchViewOut o = cv.Step(in);
        CHECK(o.v == 0.0f && o.comp == 0.0f && cv.source == CrouchView::kNone);
        in.active = true;
        in.mode = 1;
        o = cv.Step(in);
        CHECK(o.comp == 20.0f && o.v == 0.0f);
        in.seated = true;
        o = cv.Step(in);
        CHECK(o.comp == 0.0f && o.v == 0.0f);  // "real" da seduti: come il gioco (prima)
        in.mode = 2;
        in.seated = false;
        o = cv.Step(in);
        CHECK(o.comp == 0.0f && o.v == 0.0f);
    }
    // 8) crouchtime 0: salto sotto il battito.
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.rampTime = 0.0f;
        in.G = 20.0f;
        bool blink = false;
        for (int i = 0; i < 3; ++i) blink |= cv.Step(in).blink;
        CHECK(blink && cv.v == 20.0f);
    }
    // 10) Fix 1: accucciati col tasto e poi A a un riparo basso: resta l'accucciata del tasto,
    //     V tenuto (prima diventava "copertura" e in piedi la vista risaliva di 20 con Walker accucciato).
    for (bool seated : {false, true}) {
        CrouchView cv;
        CrouchViewIn in = base();
        in.G = 20.0f;
        in.seated = seated;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        CHECK(cv.source == CrouchView::kButton && std::fabs(cv.v - 20.0f) < 1e-3f);
        in.inCover = in.coverRecent = true;
        float vMin = 1e9f;
        for (int i = 0; i < 72; ++i) vMin = (std::min)(vMin, cv.Step(in).v);
        CHECK(cv.source == CrouchView::kButton && vMin > 20.0f - 1e-3f);
    }
    // 11) Capsula giu' senza tasto ne' riparo: 0,3 s di attesa (vista ferma), poi come il tasto (script);
    //     se nell'attesa arriva lo stato di copertura e' il riparo (rientro nel riparo dopo aver mirato).
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.buttonCrouch = false;
        in.G = 20.0f;
        int startAt = -1;
        for (int i = 0; i < 72 * 2; ++i) {
            CrouchViewOut o = cv.Step(in);
            if (startAt < 0 && o.v > 0.0f) startAt = i;
        }
        // Fix 2: attesa kScriptHold (0,5 s, era 0,3).
        CHECK(cv.source == CrouchView::kButton && std::fabs(cv.v - 20.0f) < 1e-3f && startAt * dt > CrouchView::kScriptHold - 0.002f &&
              startAt * dt < CrouchView::kScriptHold + 0.06f);
        CrouchView cv2;
        CrouchViewIn in2 = base();
        in2.buttonCrouch = false;
        in2.G = 20.0f;
        for (int i = 0; i < 10; ++i) cv2.Step(in2);
        in2.inCover = true;
        for (int i = 0; i < 72; ++i) cv2.Step(in2);
        CHECK(cv2.source == CrouchView::kCover && cv2.v == 0.0f);
    }
    // 12) Tasto premuto entro 1,5 s dall'uscita da un riparo: subito accucciata del tasto
    //     (prima "copertura" per ~1,8 s, poi la rampa).
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.coverRecent = true;
        in.G = 20.0f;
        for (int i = 0; i < 3; ++i) cv.Step(in);
        CHECK(cv.source == CrouchView::kButton);
    }
    // 9) G sotto 2 (rumore della capsula) non parte; deve durare 2 tick.
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.G = 1.5f;
        for (int i = 0; i < 10; ++i) cv.Step(in);
        CHECK(cv.source == CrouchView::kNone && cv.v == 0.0f);
        in.G = 20.0f;
        cv.Step(in);
        CHECK(cv.source == CrouchView::kNone);
        cv.Step(in);
        CHECK(cv.source == CrouchView::kButton);
    }
    // 13) Fix 2 (review alta): colpo subito (TakeHit 9, PlayHitReactionAnim 58), inciampo (63), scatto (40):
    //     capsula giu' 0,6 s senza tasto ne' riparo -> V 0 per tutto il tempo (vista ferma, comp = G),
    //     anche ripetuto ogni 3 s per 30 s (sotto il fuoco a Gorge02). Prima V saliva a 16-20 dopo 0,3 s.
    for (int move : {9, 58, 63, 40}) {
        CrouchView cv;
        CrouchViewIn in = base();
        in.buttonCrouch = false;
        float maxV = 0.0f;
        bool compOk = true;
        for (int i = 0; i < 72 * 30; ++i) {
            const double t = i * dt, ph = std::fmod(t, 3.0);
            const bool hit = ph < 0.6;
            // la capsula scende col colpo e torna su 0,1 s dopo la fine della mossa
            in.G = ph < 0.7 ? 20.0f : 0.0f;
            in.specialMove = hit ? move : 0;
            CrouchViewOut o = cv.Step(in);
            maxV = (std::max)(maxV, o.v);
            if (o.comp != in.G) compOk = false;
        }
        CHECK(maxV == 0.0f && compOk && cv.source == CrouchView::kNone);
    }
    // 13b) La mossa finisce e la capsula resta giu' (script vero dopo): script solo dopo kSpecialQuiet +
    //      kScriptHold senza mosse; una capsula giu' senza mosse ramp come prima (caso 11).
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.buttonCrouch = false;
        in.G = 20.0f;
        int startAt = -1;
        for (int i = 0; i < 72 * 3; ++i) {
            in.specialMove = i * dt < 0.6f ? 9 : 0;
            CrouchViewOut o = cv.Step(in);
            if (startAt < 0 && o.v > 0.0f) startAt = i;
        }
        CHECK(cv.source == CrouchView::kButton && std::fabs(cv.v - 20.0f) < 1e-3f && startAt * dt > 0.6f + CrouchView::kSpecialQuiet + CrouchView::kScriptHold - 0.03f &&
              startAt * dt < 0.6f + CrouchView::kSpecialQuiet + CrouchView::kScriptHold + 0.06f);
        // Col tasto (m_wantsToCrouch) durante una mossa speciale resta l'accucciata del tasto (fatta dal giocatore).
        CrouchView cv2;
        CrouchViewIn in2 = base();
        in2.G = 20.0f;
        in2.specialMove = 40;
        for (int i = 0; i < 72; ++i) cv2.Step(in2);
        CHECK(cv2.source == CrouchView::kButton && std::fabs(cv2.v - 20.0f) < 1e-3f);
    }
    // 14) Fix 2 (review bassa): tasto, riparo basso, mira fuori (Walker si alza: G -> 0 col riparo), di nuovo
    //     dentro: V resta 20, vista ferma (comp segue G); uscendo dal riparo in piedi V torna a 0.
    {
        CrouchView cv;
        CrouchViewIn in = base();
        in.G = 20.0f;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        in.inCover = in.coverRecent = true;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        float vMin = 1e9f, vMax = -1e9f;
        for (int i = 0; i < 72 * 4; ++i) {
            in.G = (i / 72) % 2 == 0 ? 0.0f : 20.0f;  // 1 s fuori a mirare, 1 s giu', due volte
            CrouchViewOut o = cv.Step(in);
            vMin = (std::min)(vMin, o.v);
            vMax = (std::max)(vMax, o.v);
            CHECK(o.comp == in.G);
        }
        CHECK(cv.source == CrouchView::kButton && vMin > 20.0f - 1e-3f && vMax < 20.0f + 1e-3f);
        in.inCover = in.coverRecent = false;
        in.buttonCrouch = false;
        in.G = 0.0f;
        for (int i = 0; i < 72; ++i) cv.Step(in);
        CHECK(cv.source == CrouchView::kNone && cv.v == 0.0f);
    }
}

// Fix 2 (review media): seduti, in piedi 8 s (rimisura -> in piedi), di nuovo seduti: il ritorno si
// riconosce dopo 3 s; un'accucciata breve o fino a 20 cm sopra il riferimento seduto no.
static void TestSeatedReturn() {
    SeatedReturnCheck rc;
    CHECK(!rc.Step(0.0, 1.15f, true));  // non armato
    rc.Arm(1.15f);
    int fires = 0;
    double at = -1.0;
    for (int i = 0; i < 72 * 20; ++i) {
        const double t = i / 72.0;
        const float y = (t < 8.0 ? 1.72f : 1.16f) + 0.01f * std::sin(i * 0.3f);
        if (rc.Step(t, y, true)) {
            ++fires;
            if (at < 0.0) at = t;
        }
    }
    CHECK(fires == 1 && at > 10.9 && at < 11.1 && !rc.armed);
    rc.Arm(1.15f);
    int fires2 = 0;
    for (int i = 0; i < 72 * 30; ++i) {
        const double t = i / 72.0, ph = std::fmod(t, 6.0);
        const float y = ph < 2.0 ? 1.20f : (ph < 4.0 ? 1.72f : 1.36f);  // accucciate di 2 s, poi 21 cm sopra
        fires2 += rc.Step(t, y, true);
    }
    CHECK(fires2 == 0);
    rc.Arm(1.15f);
    int fires3 = 0;
    for (int i = 0; i < 72 * 10; ++i) fires3 += rc.Step(i / 72.0, 1.15f, false);  // non attivo
    CHECK(fires3 == 0);
    // Detector con soglia dall'ini.
    SeatedDetector d;
    d.seatedBelow = 1.25f;
    d.Start();
    int r = -1;
    for (int i = 0; i < 400 && r < 0; ++i) r = d.Step(i * 0.011, 1.30f, true, -1.0f);
    CHECK(r == 0);
    std::printf("seated, standing 8 s, sitting again: re-measure at %.2f s (sat down at 8 s)\n", at);
}

// Fix 2: rotazione degli FBoneAtom (collo nel rendering), sui valori letti da Walker il 26/10:
// Spine3 (q, T, S 0,999) e LocalAtoms del Neck (11,841 lungo X) -> Neck in SpaceBases.
static void TestBoneAtomRotate() {
    const float q[4] = {-0.582f, 0.440f, 0.392f, -0.560f};
    const float v[3] = {11.841f * 0.999f, 0.0f, 0.0f};
    float r[3];
    QuatRotateVec(q, v, r);
    const float neck[3] = {-2.588f + r[0], -67.258f + r[1], -0.194f + r[2]};
    CHECK(std::fabs(neck[0] - 1.030f) < 0.03f && std::fabs(neck[1] - -78.506f) < 0.03f && std::fabs(neck[2] - 0.244f) < 0.03f);
    const float id[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    QuatRotateVec(id, v, r);
    CHECK(r[0] == v[0] && r[1] == 0.0f && r[2] == 0.0f);
}

static void TestWi3Migration() {
    const size_t n = sizeof(kWi3ConfigChanges) / sizeof(kWi3ConfigChanges[0]);
    std::string done;
    std::vector<std::string> lines = {"height auto", "seated off", "crouchview real", "physicalcrouch on", "fpbody on", "configversion 9"};
    CHECK(MigrateConfigLines(lines, 10, kWi3ConfigChanges, n, &done));
    CHECK(lines[1] == "seated auto" && lines[2] == "crouchview auto" && lines[4] == "fpbody full" && lines[5] == "configversion 10");
    lines = {"fpbody off", "seated on", "crouchview game", "configversion 9"};
    CHECK(MigrateConfigLines(lines, 10, kWi3ConfigChanges, n, &done));
    CHECK(lines[0] == "fpbody arms" && lines[1] == "seated on" && lines[2] == "crouchview game");
    lines = {"seated off", "configversion 10"};
    CHECK(!MigrateConfigLines(lines, 10, kWi3ConfigChanges, n, &done) && lines[0] == "seated off");
    const std::vector<std::string>* w = AdvancedWordChoices("crouchview");
    CHECK(w && w->size() == 3 && (*w)[0] == "auto");
    w = AdvancedWordChoices("seated");
    CHECK(w && w->size() == 3 && (*w)[0] == "auto");
    CHECK(MenuStepFor("crouchtime", 0.35f).step == 0.05f);
}

// Merge copia A + copia B (27/10): la versione 10 e' quella della copia A
// (solidhands), i cambi dei fix del tester (nella copia B anche loro "10") sono
// la 11. Stessa tabella di control.cpp MigrateConfig: un ini alla 9 riceve
// tutti e due, l'ini dell'utente gia' alla 10 riceve solo quelli della copia B
// (e un "solidhands off" scelto dopo la 10 resta off), uno alla 11 niente.
// 0.10.0: ini di una versione precedente rimesso ai predefiniti (control.cpp LoadConfigFile).
static void TestConfigReset() {
    CHECK(vr::pure::kResetConfigVersion == 12 && vr::pure::kResetConfigVersion > kTesterFixesConfigVersion);
    CHECK(vr::pure::NeedsConfigReset({"stereo 1.6 sync", "configversion 11"}));
    CHECK(vr::pure::NeedsConfigReset({"stereo 1.6 sync"}));  // senza configversion: versione 1
    CHECK(!vr::pure::NeedsConfigReset({"stereo 1.6 sync", "configversion 12"}));
    CHECK(!vr::pure::NeedsConfigReset({"configversion 13"}));
    CHECK(!vr::pure::NeedsConfigReset({}));  // niente da salvare
}

static void TestMergedConfigVersions() {
    CHECK(kTesterFixesConfigVersion == 11);
    std::vector<ConfigChange> changes = {{10, "solidhands off", "solidhands held"},
                                         {kTesterFixesConfigVersion, "headcollision on", "headcollision push"}};
    changes.insert(changes.end(), std::begin(kWi3ConfigChanges), std::end(kWi3ConfigChanges));
    changes.insert(changes.end(), std::begin(kAimpitchCommentChanges), std::end(kAimpitchCommentChanges));
    const ComputedChange computed[] = {{kTesterFixesConfigVersion, "aimpitch", &MigrateAimpitchLine},
                                       {kTesterFixesConfigVersion, "twohand", &MigrateTwoHandLine},
                                       {kTesterFixesConfigVersion, "leftgrip", &MigrateTwoHandLine}};
    const size_t nc = sizeof(computed) / sizeof(computed[0]);
    auto has = [](const std::vector<std::string>& l, const char* x) {
        for (const auto& s : l)
            if (s == x) return true;
        return false;
    };
    std::string done;
    // Ini alla 9: entrambi i gruppi.
    std::vector<std::string> v9 = {"solidhands off", "headcollision on", "seated off", "aimpitch -24.0", "fpbody on",
                                   "configversion 9"};
    CHECK(MigrateConfigLines(v9, 11, changes.data(), changes.size(), &done, computed, nc));
    CHECK(has(v9, "solidhands held") && has(v9, "headcollision push") && has(v9, "seated auto") && has(v9, "fpbody full"));
    CHECK(has(v9, "holdmode gunstock") && has(v9, "stockangle 15.4") && !has(v9, "aimpitch -24.0") && has(v9, "configversion 11"));
    // L'ini dell'utente (copia A, gia' alla 10): solo i cambi della copia B.
    std::vector<std::string> v10 = {"solidhands off", "headcollision on", "seated off", "aimpitch -24.0", "fpbody on",
                                    "configversion 10"};
    CHECK(MigrateConfigLines(v10, 11, changes.data(), changes.size(), &done, computed, nc));
    CHECK(has(v10, "solidhands off") && has(v10, "headcollision push") && has(v10, "seated auto") && has(v10, "fpbody full"));
    CHECK(has(v10, "holdmode gunstock") && has(v10, "stockangle 15.4") && has(v10, "configversion 11"));
    CHECK(done.find("solidhands") == std::string::npos && done.find("headcollision") != std::string::npos);
    const std::string doneUser = done;
    // Un ini della copia B gia' alla 10 (i suoi cambi gia' fatti): rifarli non cambia nulla.
    std::vector<std::string> b10 = {"headcollision push", "seated auto", "holdmode gunstock", "stockangle 15.4", "configversion 10"};
    CHECK(MigrateConfigLines(b10, 11, changes.data(), changes.size(), &done, computed, nc));
    CHECK(b10[0] == "headcollision push" && b10[2] == "holdmode gunstock" && b10[3] == "stockangle 15.4" && has(b10, "configversion 11"));
    // Alla 11: niente da fare.
    std::vector<std::string> v11 = {"headcollision on", "configversion 11"};
    CHECK(!MigrateConfigLines(v11, 11, changes.data(), changes.size(), &done, computed, nc) && v11[0] == "headcollision on");
    std::printf("merged configversion: v9 -> 11 both groups, user's v10 -> 11 copy B's only (%zu chars of changes), v11 untouched\n",
                doneUser.size());
}

// --- WI-4b: presa a due mani (twohand_math.h) ---------------------------------------
// Banco: controller destro come posa raw dei Touch costruita dal raggio di
// puntamento A (raw = A * Rx(39,4), come gunframe.py), telaio dell'arma WI-4a
// (gunangle 0, perno nel palmo), astina dalla presa disegnata tipica dei fucili
// (polso sinistro 33,1 cm avanti, 9,6 a sinistra, 4,6 sopra il polso destro).
namespace th = vr::twohand;

struct ThRig {
    float raw[3][4];
    float gun[3][4];
    float palm[3];
};
static ThRig ThRightRig(float pitchDeg, float yawDeg, float rollDeg, const float pos[3], float gunAngle = 0.0f) {
    ThRig r;
    const float k = 0.01745329252f;
    float a[3][4], rx[3][4];
    PoseFromEuler(pitchDeg * k, yawDeg * k, rollDeg * k, a);
    RotXDeg(-kAimPitchInRawDeg, rx);
    MulRot3(a, rx, r.raw);
    for (int i = 0; i < 3; ++i) r.raw[i][3] = pos[i];
    GunFrameParams p;
    p.angleDeg = gunAngle;
    GunFrameFromRaw(r.raw, p, r.gun);
    PalmOf(r.raw, false, true, r.palm);
    return r;
}
static th::Guard ThRifleGuard(const float gunInRaw[3][4]) {
    const float q[3] = {0.331f, -0.096f, 0.046f}, off[3] = {0, 0, 0};
    float t[3];
    th::PalmTargetFromDrawnGrip(q, gunInRaw, false, true, off, t);
    return th::MakeGuard(t[0], t[1], t[2], false);
}
static th::Guard ThDefaultGuard() {
    GunFrameParams p;
    float g[3][4];
    GunRotInRaw(p, g);
    return ThRifleGuard(g);
}
// Punto dell'astina disegnata (palmo sinistro) a t lungo la canna del telaio
// gun: nel telaio dell'arma (inclinazione attuale), come il modello fuori
// dalla presa (review round 1, M3). Con rollio 0 coincide con GuardFrame.
static void ThGuardPoint(const th::Guard& g, const ThRig& r, float t, float out[3], float extraUp = 0.0f, float extraRight = 0.0f) {
    float f[3], rr[3], u[3];
    th::GunAxes(r.gun, f, rr, u);
    for (int i = 0; i < 3; ++i)
        out[i] = r.palm[i] + t * f[i] + (g.right + extraRight) * rr[i] + (g.up + extraUp) * u[i];
}
static float ThBarrelDeg(const float a[3][4], const float b[3][4]) {
    const float fa[3] = {-a[0][2], -a[1][2], -a[2][2]}, fb[3] = {-b[0][2], -b[1][2], -b[2][2]};
    float x[3];
    th::Cross3(fa, fb, x);
    return static_cast<float>(std::atan2(static_cast<double>(th::Norm3(x)), static_cast<double>(th::Dot3(fa, fb))) * 57.29577951308232);
}
static th::Input ThInput(const ThRig& r, const float palmL[3], bool grip, float dt, const th::Guard& g) {
    th::Input in;
    in.dt = dt;
    std::memcpy(in.gun, r.gun, sizeof(in.gun));
    std::memcpy(in.palmR, r.palm, sizeof(in.palmR));
    std::memcpy(in.rightRaw, r.raw, sizeof(in.rightRaw));
    in.leftValid = true;
    std::memcpy(in.palmL, palmL, sizeof(in.palmL));
    in.grip = grip;
    in.guard = g;
    return in;
}

static void TestTwoHandPalmTarget() {
    // Controller sinistro parallelo al destro con l'origine raw sulla presa
    // disegnata: il suo palmo deve stare dove dice PalmTargetFromDrawnGrip (esatto).
    const float pos[3] = {0.1f, 1.2f, -0.3f};
    for (float roll : {0.0f, 35.0f}) {
        for (float ang : {0.0f, 12.0f}) {
            ThRig r = ThRightRig(-10.0f, 25.0f, roll, pos, ang);
            GunFrameParams p;
            p.angleDeg = ang;
            float gir[3][4];
            GunRotInRaw(p, gir);
            const float q[3] = {0.331f, -0.096f, 0.046f}, off[3] = {0.0f, 0.0f, 0.0f};
            float t[3];
            th::PalmTargetFromDrawnGrip(q, gir, false, true, off, t);
            // Polso sinistro disegnato = polso destro + q (coordinate dell'arma).
            float f[3], rr[3], u[3];
            th::GunAxes(r.gun, f, rr, u);
            float lraw[3][4];
            std::memcpy(lraw, r.raw, sizeof(lraw));
            for (int i = 0; i < 3; ++i) lraw[i][3] = r.gun[i][3] + q[0] * f[i] + q[1] * rr[i] + q[2] * u[i];
            float pl[3];
            PalmOf(lraw, true, true, pl);
            float want[3];
            for (int i = 0; i < 3; ++i) want[i] = r.palm[i] + t[0] * f[i] + t[1] * rr[i] + t[2] * u[i];
            float e[3] = {pl[0] - want[0], pl[1] - want[1], pl[2] - want[2]};
            CHECK(th::Norm3(e) < 1e-5f);
        }
    }
    th::Guard g = ThDefaultGuard();
    std::printf("two-hand guard (rifle, gunangle 0): palm target %.1f cm along, %.1f right, %.1f up; segment %.1f..%.1f cm\n",
                g.fwd * 100.0f, g.right * 100.0f, g.up * 100.0f, g.tMin * 100.0f, g.tMax * 100.0f);
    CHECK(g.valid && !g.shortGun && g.tMin < g.fwd && g.tMax > g.fwd);
}

// T3: palmo sull'astina a 3 distanze: presa col grip, rotazione < 0,05 gradi.
static void TestTwoHandOnGuard() {
    const float pos[3] = {0.15f, 1.25f, -0.35f};
    th::Guard g = ThDefaultGuard();
    for (float roll : {0.0f, 30.0f}) {
        for (float t : {g.tMin + 0.005f, g.fwd, g.tMax - 0.005f}) {
            ThRig r = ThRightRig(-12.0f, 40.0f, roll, pos);
            th::Solver s;
            float pl[3];
            ThGuardPoint(g, r, t, pl);
            th::Output o;
            for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, true, 1.0f / 72.0f, g));
            CHECK(o.engaged);
            CHECK(o.dist < 1e-3f);
            CHECK(ThBarrelDeg(o.gun, r.gun) < 0.05f);
            CHECK(o.corrDeg < 0.05f);
        }
    }
}

// T4: palmo 6 cm piu' in alto (rampa 0,2 s, entro twohandgrip): la canna lo
// segue, e a 0,4 s dalla fine del moto l'astina passa per il palmo entro 0,2
// cm. Review round 1: a 10 cm (oltre twohandgrip) la canna lo segue in parte
// (prima tutta: anche una mano che se ne andava trascinava l'arma). Review
// round 2: il peso e' l'allontanamento della sinistra in ANGOLO (soglie
// twohandgrip / twohandrelease all'astina, 35,5 cm): 10 cm a 30 cm sono ~18
// gradi, peso ~0,3 (round 1, in cm al palmo: 0,80).
static void TestTwoHandFollowsLeft() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    th::Guard g = ThDefaultGuard();
    for (float up : {0.06f, 0.10f}) {
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        th::Solver s;
        float pl[3];
        ThGuardPoint(g, r, 0.30f, pl);
        const float dt = 1.0f / 72.0f;
        th::Output o;
        for (int k = 0; k < 30; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
        CHECK(o.engaged && o.corrDeg < 0.05f);
        const float base[3] = {pl[0], pl[1], pl[2]};
        const int ramp = 14;  // 0,2 s
        for (int k = 1; k <= ramp + 29; ++k) {
            const float u = k < ramp ? static_cast<float>(k) / ramp : 1.0f;
            pl[1] = base[1] + up * u;
            o = s.Step(ThInput(r, pl, true, dt, g));
        }
        // L'astina del telaio finale passa per il palmo?
        const float dist = o.distOut;
        float f[3], rr[3], u[3];
        th::GunAxes(o.gun, f, rr, u);
        const float pitch = std::asin(f[1]) * 57.2957795f;
        std::printf("two-hand follow: left palm +%.0f cm at 30 cm -> barrel %.2f deg up from the one-hand frame (distance weight %.2f), "
                    "guard %.3f cm from the palm 0.4 s after\n",
                    up * 100.0f, ThBarrelDeg(o.gun, r.gun), o.weight, dist * 100.0f);
        CHECK(o.engaged);
        if (up < 0.08f) {
            CHECK(pitch > 3.0f);
            CHECK(o.weight > 0.999f);
            CHECK(dist < 0.002f);
        } else {
            const float aIn = std::atan2(0.08f, g.fwd) * 57.2957795f, aOut = std::atan2(0.15f, g.fwd) * 57.2957795f;
            const float want = 1.0f - th::SmoothStep01((o.departDeg - aIn) / (aOut - aIn));
            std::printf("two-hand follow: +10 cm = %.1f deg of departure (full to %.1f, none at %.1f): weight %.3f, expected %.3f\n",
                        o.departDeg, aIn, aOut, o.weight, want);
            CHECK(o.departDeg > aIn && o.departDeg < aOut);
            CHECK(std::fabs(o.weight - want) < 0.01f && o.weight > 0.05f && o.weight < 0.95f);
            CHECK(std::fabs(o.corrDeg - o.weight * o.targetDeg) < 0.05f);
        }
    }
}

// T5 / plan: il polso sinistro che ruota (palmo fermo) non muove la canna;
// 20 gradi di rollio del polso destro attorno al palmo la muovono < 0,3 gradi.
// Review round 1: col peso della distanza la prova si fa col palmo sul
// modello (il rollio di 20 gradi sposta la canna con una mano di 12,7 gradi,
// 7,3 cm all'astina: dentro twohandgrip, peso 1): < 0,3 gradi. Col palmo
// gia' 3-4 cm fuori lo stesso rollio porta la mano oltre twohandgrip
// rispetto alla canna con una mano e l'arma torna in parte alla destra
// (stessa regola della mano che esce): < 1,5 gradi.
static void TestTwoHandWristRotations() {
  for (int offModel : {0, 1}) {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    th::Guard g = ThDefaultGuard();
    ThRig r = ThRightRig(-8.0f, 10.0f, 0.0f, pos);
    th::Solver s;
    float pl[3];
    if (offModel) ThGuardPoint(g, r, 0.33f, pl, 0.03f, -0.02f);  // 3-4 cm fuori dall'astina: la correzione non e' nulla
    else ThGuardPoint(g, r, 0.33f, pl);
    const float dt = 1.0f / 72.0f;
    th::Output o;
    for (int k = 0; k < 60; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
    CHECK(o.engaged && (offModel ? o.corrDeg > 3.0f : o.corrDeg < 0.05f));
    float before[3][4];
    std::memcpy(before, o.gun, sizeof(before));
    // Polso destro: rollio attorno all'asse -Z raw che passa per il palmo (avambraccio).
    float worst = 0.0f;
    for (float rollAdd : {-20.0f, 20.0f}) {
        th::Solver s2 = s;
        float ax[3] = {-r.raw[0][2], -r.raw[1][2], -r.raw[2][2]}, v[3], R[3][3];
        for (int i = 0; i < 3; ++i) v[i] = ax[i] * rollAdd * 0.01745329252f;
        th::RotFromVec(v, R);
        ThRig r2 = r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r2.raw[i][j] = R[i][0] * r.raw[0][j] + R[i][1] * r.raw[1][j] + R[i][2] * r.raw[2][j];
        // origine raw ruotata attorno al palmo
        float op[3] = {r.raw[0][3] - r.palm[0], r.raw[1][3] - r.palm[1], r.raw[2][3] - r.palm[2]};
        for (int i = 0; i < 3; ++i) r2.raw[i][3] = r.palm[i] + R[i][0] * op[0] + R[i][1] * op[1] + R[i][2] * op[2];
        GunFrameParams p;
        GunFrameFromRaw(r2.raw, p, r2.gun);
        PalmOf(r2.raw, false, true, r2.palm);
        th::Output o2;
        for (int k = 0; k < 60; ++k) o2 = s2.Step(ThInput(r2, pl, true, dt, g));
        const float one = ThBarrelDeg(r2.gun, r.gun);
        const float two = ThBarrelDeg(o2.gun, before);
        worst = two > worst ? two : worst;
        std::printf("two-hand right wrist roll %+.0f deg (palm %s): one-hand barrel moves %.2f deg, two-hand barrel %.3f deg "
                    "(palm %.1f cm from the one-hand model, weight %.2f)\n",
                    rollAdd, offModel ? "3.6 cm off the model" : "on the model", one, two, o2.dist * 100.0f, o2.weight);
    }
    CHECK(worst < (offModel ? 1.5f : 0.3f));
    // Polso sinistro: rotazioni con il palmo fermo -> nessun ingresso (solo il palmo entra): canna invariata.
    o = s.Step(ThInput(r, pl, true, dt, g));
    CHECK(ThBarrelDeg(o.gun, before) < 0.1f);
    if (!offModel) continue;
    // Il vecchio test: la linea tra le origini raw (10 cm davanti ai palmi) con la sinistra che ruota di 30 gradi.
    {
        float lraw[3][4];
        std::memcpy(lraw, r.raw, sizeof(lraw));
        const float oL[3] = {kGripOriginX, kGripOriginY, kGripOriginZ};
        float worstOld = 0.0f;
        for (float yaw : {-30.0f, 30.0f}) {
            float a[3][4], rx[3][4];
            PoseFromEuler(-8.0f * 0.01745329252f, (10.0f + yaw) * 0.01745329252f, 0.0f, a);
            RotXDeg(-kAimPitchInRawDeg, rx);
            MulRot3(a, rx, lraw);
            // origine raw tale che il palmo resti pl
            for (int i = 0; i < 3; ++i) lraw[i][3] = pl[i] - (lraw[i][0] * oL[0] + lraw[i][1] * oL[1] + lraw[i][2] * oL[2]);
            float lineNew[3] = {lraw[0][3] - r.raw[0][3], lraw[1][3] - r.raw[1][3], lraw[2][3] - r.raw[2][3]};
            float l0[3];
            std::memcpy(lraw, r.raw, sizeof(lraw));
            for (int i = 0; i < 3; ++i) lraw[i][3] = pl[i] - (r.raw[i][0] * oL[0] + r.raw[i][1] * oL[1] + r.raw[i][2] * oL[2]);
            for (int i = 0; i < 3; ++i) l0[i] = lraw[i][3] - r.raw[i][3];
            th::Normalize3(lineNew);
            th::Normalize3(l0);
            float c = th::Dot3(lineNew, l0);
            c = c > 1.0f ? 1.0f : c;
            float deg = std::acos(c) * 57.2957795f;
            worstOld = deg > worstOld ? deg : worstOld;
        }
        std::printf("two-hand: left wrist yaw +-30 deg, palm fixed: old raw-origin line moves %.1f deg, new barrel 0.000\n", worstOld);
        CHECK(worstOld > 5.0f);
    }
  }
}

// T6: controller destro inclinato di 30 gradi: il rollio dell'arma attorno
// alla sua canna resta entro 0,5 gradi da quello del telaio con una mano.
static void TestTwoHandRollKept() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    th::Guard g = ThDefaultGuard();
    ThRig r = ThRightRig(-5.0f, 0.0f, 30.0f, pos);
    th::Solver s;
    float pl[3];
    ThGuardPoint(g, r, 0.35f, pl, 0.04f, 0.0f);
    th::Output o;
    for (int k = 0; k < 60; ++k) o = s.Step(ThInput(r, pl, true, 1.0f / 72.0f, g));
    CHECK(o.engaged && o.corrDeg > 3.0f);
    const Euler e1 = ExactEuler(r.gun), e2 = ExactEuler(o.gun);
    std::printf("two-hand roll: one-hand %.2f, two-hand %.2f deg (correction %.2f deg)\n", e1.roll * 57.2957795f, e2.roll * 57.2957795f,
                o.corrDeg);
    CHECK(std::fabs(WrapPi(e2.roll - e1.roll)) * 57.2957795f < 0.5f);
}

// T7: soglie, intenzione, isteresi.
static void TestTwoHandHysteresis() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
    auto at = [&](float off, float t, float* out) { ThGuardPoint(g, r, t, out, off, 0.0f); };
    float pl[3];
    {  // 7 cm col grip: subito
        th::Solver s;
        at(0.07f, 0.35f, pl);
        th::Output o = s.Step(ThInput(r, pl, true, dt, g));
        CHECK(o.engaged && o.event == 1 && o.byGrip);
    }
    {  // 7 cm senza grip: dopo 150 ms
        th::Solver s;
        at(0.07f, 0.35f, pl);
        int n = 0;
        th::Output o;
        while (!(o = s.Step(ThInput(r, pl, false, dt, g))).engaged && n < 100) ++n;
        const float ms = (n + 1) * dt * 1000.0f;
        std::printf("two-hand dwell: engaged after %.0f ms\n", ms);
        CHECK(ms >= 150.0f && ms < 150.0f + 2 * dt * 1000.0f);
        CHECK(!o.byGrip);
    }
    {  // 10 cm: mai
        th::Solver s;
        at(0.10f, 0.35f, pl);
        bool any = false;
        for (int k = 0; k < 100; ++k) any = any || s.Step(ThInput(r, pl, true, dt, g)).engaged;
        CHECK(!any);
    }
    {  // tenuta a 14 cm, rilascio a 16 cm
        th::Solver s;
        at(0.0f, 0.35f, pl);
        s.Step(ThInput(r, pl, true, dt, g));
        at(0.14f, 0.35f, pl);
        bool held = true;
        for (int k = 0; k < 20; ++k) held = held && s.Step(ThInput(r, pl, true, dt, g)).engaged;
        CHECK(held);
        at(0.16f, 0.35f, pl);
        th::Output o = s.Step(ThInput(r, pl, true, dt, g));
        CHECK(!o.engaged && o.event == 2);
    }
    {  // fuori dal segmento lungo la canna
        for (float t : {g.tMin - 0.10f, g.tMax + 0.10f}) {
            th::Solver s;
            at(0.0f, t, pl);
            bool any = false;
            for (int k = 0; k < 30; ++k) any = any || s.Step(ThInput(r, pl, true, dt, g)).engaged;
            CHECK(!any);
        }
    }
    {  // grip lasciato: rilascio; poi la sosta non riprende finche' la mano non esce
        th::Solver s;
        at(0.0f, 0.35f, pl);
        s.Step(ThInput(r, pl, true, dt, g));
        th::Output o = s.Step(ThInput(r, pl, false, dt, g));
        CHECK(!o.engaged && o.event == 2);
        bool any = false;
        for (int k = 0; k < 40; ++k) any = any || s.Step(ThInput(r, pl, false, dt, g)).engaged;
        CHECK(!any);
        at(0.12f, 0.35f, pl);
        s.Step(ThInput(r, pl, false, dt, g));
        at(0.0f, 0.35f, pl);
        for (int k = 0; k < 40; ++k) any = any || s.Step(ThInput(r, pl, false, dt, g)).engaged;
        CHECK(any);  // uscita e ritorno: la sosta vale di nuovo
        // grip di nuovo: presa subito
        th::Solver s2;
        s2.Step(ThInput(r, pl, true, dt, g));
        s2.Step(ThInput(r, pl, false, dt, g));
        CHECK(s2.Step(ThInput(r, pl, true, dt, g)).engaged);
    }
}

// T8: continuita'. Presa con la mano 7 cm fuori (correzione ~11 gradi) e 15
// gradi: passo per tick <= 2,5 gradi a 72 e 90 Hz. Rilascio: nessun salto;
// lo scarto cala solo col moto della mano dell'arma.
static void TestTwoHandContinuity() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    th::Guard g = ThDefaultGuard();
    for (float hz : {72.0f, 90.0f}) {
        const float dt = 1.0f / hz;
        for (float off : {0.07f, 0.0935f}) {  // 0.0935 a 35 cm: ~15 gradi
            ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
            th::Solver s;
            s.p.engageM = 0.10f;  // per avere 15 gradi alla presa
            float pl[3];
            ThGuardPoint(g, r, 0.35f, pl, off, 0.0f);
            float prev[3][4];
            std::memcpy(prev, r.gun, sizeof(prev));
            float worst = 0.0f, last = 0.0f;
            th::Output o;
            for (int k = 0; k < 60; ++k) {
                o = s.Step(ThInput(r, pl, true, dt, g));
                const float step = ThBarrelDeg(o.gun, prev);
                worst = step > worst ? step : worst;
                std::memcpy(prev, o.gun, sizeof(prev));
                last = o.corrDeg;
            }
            std::printf("two-hand engage at %.0f Hz, correction %.1f deg: largest step %.2f deg per tick\n", hz, last, worst);
            CHECK(o.engaged && last > 10.0f);
            CHECK(worst <= 2.5f);
            // Rilascio (grip lasciato) con le mani ferme: nessun passo, scarto invariato per 2 s.
            float before[3][4];
            std::memcpy(before, o.gun, sizeof(before));
            o = s.Step(ThInput(r, pl, false, dt, g));
            CHECK(!o.engaged && o.event == 2);
            CHECK(ThBarrelDeg(o.gun, before) < 1e-3f);
            for (int k = 0; k < static_cast<int>(2.0f * hz); ++k) o = s.Step(ThInput(r, pl, false, dt, g));
            CHECK(std::fabs(o.corrDeg - last) < 0.01f);
            // La mano dell'arma gira di 10 gradi: lo scarto cala di 5.
            for (int k = 1; k <= 20; ++k) {
                ThRig r2 = ThRightRig(-5.0f, 0.5f * k, 0.0f, pos);
                o = s.Step(ThInput(r2, pl, false, dt, g));
            }
            std::printf("two-hand release: leftover %.2f -> %.2f deg after 10 deg of gun-hand motion\n", last, o.corrDeg);
            CHECK(std::fabs(o.corrDeg - (last - 5.0f)) < 0.3f);
        }
    }
}

// T9: filtro. Spazzata rigida a 180 gradi/s: ritardo <= 4 gradi. Rumore di 0,5
// gradi rms sulla linea tra i palmi (bianco, 72 Hz): uscita <= 0,15 gradi rms.
static void TestTwoHandFilter() {
    th::Guard g = ThDefaultGuard();
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    for (float hz : {72.0f, 90.0f}) {
        const float dt = 1.0f / hz;
        th::Solver s;
        float worstLag = 0.0f;
        th::Output o;
        for (int k = 0; k < static_cast<int>(1.5f * hz); ++k) {
            const float yaw = k < static_cast<int>(0.3f * hz) ? 0.0f : 180.0f * (k - 0.3f * hz) * dt;
            ThRig r = ThRightRig(-5.0f, yaw, 0.0f, pos);
            float pl[3];
            ThGuardPoint(g, r, 0.35f, pl);
            o = s.Step(ThInput(r, pl, true, dt, g));
            if (k > static_cast<int>(0.6f * hz)) worstLag = ThBarrelDeg(o.gun, r.gun) > worstLag ? ThBarrelDeg(o.gun, r.gun) : worstLag;
        }
        std::printf("two-hand filter at %.0f Hz: 180 deg/s sweep lag %.2f deg\n", hz, worstLag);
        CHECK(worstLag <= 4.0f);
        // Rumore
        th::Solver sn;
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        float pl0[3];
        ThGuardPoint(g, r, 0.35f, pl0);
        std::mt19937 rng(7);
        std::normal_distribution<float> nd(0.0f, 0.35f * std::tan(0.5f * 0.01745329252f) / std::sqrt(2.0f));
        double sumIn = 0.0, sumOut = 0.0;
        int n = 0;
        for (int k = 0; k < static_cast<int>(20.0f * hz); ++k) {
            float pl[3] = {pl0[0] + nd(rng), pl0[1] + nd(rng), pl0[2]};
            o = sn.Step(ThInput(r, pl, true, dt, g));
            if (k > static_cast<int>(1.0f * hz)) {
                float d[3] = {pl[0] - r.palm[0], pl[1] - r.palm[1], pl[2] - r.palm[2]}, d0[3] = {pl0[0] - r.palm[0], pl0[1] - r.palm[1], pl0[2] - r.palm[2]};
                th::Normalize3(d);
                th::Normalize3(d0);
                float c = th::Dot3(d, d0);
                c = c > 1.0f ? 1.0f : c;
                const double ai = std::acos(c) * 57.2957795, ao = o.corrDeg;
                sumIn += ai * ai;
                sumOut += ao * ao;
                ++n;
            }
        }
        const double rin = std::sqrt(sumIn / n), rout = std::sqrt(sumOut / n);
        std::printf("two-hand filter at %.0f Hz: input noise %.3f deg rms -> barrel %.3f deg rms\n", hz, rin, rout);
        CHECK(rin > 0.4 && rin < 0.6);
        CHECK(rout <= 0.15);
    }
}

// T10: GUNSTOCK. La mira resta quella del controller destro (1000 pose a caso
// della sinistra); presa solo entro 6 cm / 25 gradi dalla posa sul calcio.
static void TestTwoHandGunstock() {
    th::Guard g = ThDefaultGuard();
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    ThRig r = ThRightRig(-5.0f, 15.0f, 10.0f, pos);
    // Posa sul calcio: sinistra 30 cm avanti lungo il raw, ruotata di 20 gradi.
    float lraw[3][4], rel[3][4];
    PoseFromEuler(0.1f, 0.35f, -0.2f, rel);
    rel[0][3] = 0.02f;
    rel[1][3] = -0.05f;
    rel[2][3] = -0.30f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) lraw[i][j] = r.raw[i][0] * rel[0][j] + r.raw[i][1] * rel[1][j] + r.raw[i][2] * rel[2][j];
        lraw[i][3] = r.raw[i][3] + r.raw[i][0] * rel[0][3] + r.raw[i][1] * rel[1][3] + r.raw[i][2] * rel[2][3];
    }
    float m6[6], mpos[3], mrot[3][4];
    th::StockMountFromPoses(r.raw, lraw, m6);
    th::StockMountToPose(m6, mpos, mrot);
    CHECK(std::fabs(mpos[2] + 0.30f) < 1e-4f && th::RotAngleDeg(mrot, rel) < 0.01f);
    auto in = [&](const float lr[3][4]) {
        float pl[3];
        PalmOf(lr, true, true, pl);
        th::Input i = ThInput(r, pl, false, 1.0f / 72.0f, g);
        i.gunstock = true;
        i.mountValid = true;
        std::memcpy(i.mountPos, mpos, sizeof(mpos));
        std::memcpy(i.mountRot, mrot, sizeof(mrot));
        std::memcpy(i.leftRaw, lr, sizeof(i.leftRaw));
        return i;
    };
    th::Solver s;
    th::Output o = s.Step(in(lraw));
    CHECK(o.engaged && o.mountCm < 0.01f && o.mountDeg < 0.05f);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);
    float worstAim = 0.0f;
    int engagedFar = 0, engagedNear = 0, nearCount = 0;
    for (int k = 0; k < 1000; ++k) {
        float lr[3][4], rr[3][4];
        const float sc = k < 500 ? 0.03f : 0.5f;  // meta' vicino al calcio, meta' ovunque
        PoseFromEuler(U(rng) * (k < 500 ? 0.15f : 1.5f), U(rng) * (k < 500 ? 0.15f : 3.0f), U(rng) * (k < 500 ? 0.15f : 3.0f), rr);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) lr[i][j] = lraw[i][0] * rr[0][j] + lraw[i][1] * rr[1][j] + lraw[i][2] * rr[2][j];
            lr[i][3] = lraw[i][3] + U(rng) * sc;
        }
        th::Solver s1;
        o = s1.Step(in(lr));
        worstAim = ThBarrelDeg(o.gun, r.gun) > worstAim ? ThBarrelDeg(o.gun, r.gun) : worstAim;
        CHECK(!o.aimChanged);
        const bool inside = o.mountCm < 6.0f && o.mountDeg < 25.0f;
        if (o.engaged && !inside) ++engagedFar;
        if (inside) {
            ++nearCount;
            if (o.engaged) ++engagedNear;
        }
    }
    std::printf("two-hand gunstock: 1000 left poses, barrel vs the right controller %.4f deg; engaged inside 6 cm / 25 deg %d of %d, outside %d\n",
                worstAim, engagedNear, nearCount, engagedFar);
    CHECK(worstAim < 0.01f);
    CHECK(engagedFar == 0 && engagedNear == nearCount && nearCount > 50);
    // Senza posa salvata: prova dell'astina per la presa, mira invariata.
    th::Solver s2;
    float pl[3];
    ThGuardPoint(g, r, 0.35f, pl, 0.03f, 0.0f);
    th::Input i2 = ThInput(r, pl, true, 1.0f / 72.0f, g);
    i2.gunstock = true;
    o = s2.Step(i2);
    CHECK(o.engaged && !o.aimChanged);
}

// T11: veti: mai la presa, mira invariata; i veti duri tolgono subito lo scarto.
static void TestTwoHandVetoes() {
    th::Guard g = ThDefaultGuard();
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
    float pl[3];
    ThGuardPoint(g, r, 0.35f, pl, 0.04f, 0.0f);
    for (int v : {th::kVetoOff, th::kVetoStationary, th::kVetoScene, th::kVetoNoGun, th::kVetoProp, th::kVetoPropGrip,
                  th::kVetoReload, th::kVetoGrenade, th::kVetoNoGrip}) {
        th::Solver s;
        bool any = false, aim = false;
        for (int k = 0; k < 40; ++k) {
            th::Input in = ThInput(r, pl, true, 1.0f / 72.0f, g);
            in.veto = v;
            th::Output o = s.Step(in);
            any = any || o.engaged;
            aim = aim || o.aimChanged;
        }
        CHECK(!any && !aim);
        CHECK(*th::VetoName(v) != 0);
    }
    // Presa, poi veto: rilascio; duro = scarto via subito, morbido = resta.
    for (int v : {th::kVetoStationary, th::kVetoReload}) {
        th::Solver s;
        th::Output o;
        for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, true, 1.0f / 72.0f, g));
        CHECK(o.engaged && o.corrDeg > 3.0f);
        th::Input in = ThInput(r, pl, true, 1.0f / 72.0f, g);
        in.veto = v;
        o = s.Step(in);
        CHECK(!o.engaged && o.event == 2);
        if (v & th::kHardVetoes) CHECK(!o.aimChanged);
        else CHECK(o.aimChanged && o.corrDeg > 3.0f);
    }
}

// T12: controller sinistro perso per un tick: rilascio (scarto tenuto) e
// stato azzerato; al ritorno la presa solo con le regole (grip o sosta).
static void TestTwoHandTrackingLoss() {
    th::Guard g = ThDefaultGuard();
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
    float pl[3];
    ThGuardPoint(g, r, 0.35f, pl, 0.04f, 0.0f);
    th::Solver s;
    th::Output o;
    for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, false, 1.0f / 72.0f, g));
    CHECK(o.engaged && !o.byGrip);
    float before[3][4];
    std::memcpy(before, o.gun, sizeof(before));
    th::Input lost = ThInput(r, pl, false, 1.0f / 72.0f, g);
    lost.leftValid = false;
    o = s.Step(lost);
    CHECK(!o.engaged && o.event == 2 && ThBarrelDeg(o.gun, before) < 1e-3f);
    o = s.Step(ThInput(r, pl, false, 1.0f / 72.0f, g));
    CHECK(!o.engaged);  // non subito: sosta di 150 ms
    int n = 1;
    while (!(o = s.Step(ThInput(r, pl, false, 1.0f / 72.0f, g))).engaged && n < 50) ++n;
    CHECK(n >= 10 && n <= 12);
}

// T13: pistola: direzione invariata, presa con il palmo sulla coppa.
static void TestTwoHandPistol() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
    th::Guard g = th::MakeGuard(0.02f, -0.06f, -0.03f, true);
    CHECK(g.tMin == g.tMax);
    float f[3], rr[3], u[3], gr[3], gu[3];
    th::GunAxes(r.gun, f, rr, u);
    th::GuardFrame(f, u, gr, gu);
    float pl[3];
    for (int i = 0; i < 3; ++i) pl[i] = r.palm[i] + 0.02f * f[i] + g.right * gr[i] + g.up * gu[i] + 0.03f * gu[i];
    th::Solver s;
    th::Output o;
    for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, true, 1.0f / 72.0f, g));
    CHECK(o.engaged && !o.aimChanged && std::strcmp(o.how, "short weapon") == 0);
    for (int i = 0; i < 3; ++i) pl[i] += 0.14f * gu[i];
    o = s.Step(ThInput(r, pl, true, 1.0f / 72.0f, g));
    CHECK(!o.engaged);
}

// --- Review WI-4b round 1 -------------------------------------------------------------
// Tutto lo spazio del tracciamento girato di yaw attorno alla verticale per
// l'origine (vr::pure::SpaceShift: x' = c x - s z, z' = s x + c z).
static void ThYawPoint(float p[3], float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float x = p[0], z = p[2];
    p[0] = c * x - s * z;
    p[2] = s * x + c * z;
}
static void ThYawPose(float m[3][4], float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    for (int j = 0; j < 4; ++j) {
        const float x = m[0][j], z = m[2][j];
        m[0][j] = c * x - s * z;
        m[2][j] = s * x + c * z;
    }
}
static void ThYawRig(ThRig& r, float yaw, float gunAngle = 0.0f) {
    ThYawPose(r.raw, yaw);
    GunFrameParams p;
    p.angleDeg = gunAngle;
    GunFrameFromRaw(r.raw, p, r.gun);
    PalmOf(r.raw, false, true, r.palm);
}
// Polso destro ruotato attorno all'asse -Z raw che passa per il palmo (avambraccio).
static ThRig ThRollWrist(const ThRig& r, float rollAddDeg) {
    float ax[3] = {-r.raw[0][2], -r.raw[1][2], -r.raw[2][2]}, v[3], R[3][3];
    for (int i = 0; i < 3; ++i) v[i] = ax[i] * rollAddDeg * 0.01745329252f;
    th::RotFromVec(v, R);
    ThRig r2 = r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r2.raw[i][j] = R[i][0] * r.raw[0][j] + R[i][1] * r.raw[1][j] + R[i][2] * r.raw[2][j];
    float op[3] = {r.raw[0][3] - r.palm[0], r.raw[1][3] - r.palm[1], r.raw[2][3] - r.palm[2]};
    for (int i = 0; i < 3; ++i) r2.raw[i][3] = r.palm[i] + R[i][0] * op[0] + R[i][1] * op[1] + R[i][2] * op[2];
    GunFrameParams p;
    GunFrameFromRaw(r2.raw, p, r2.gun);
    PalmOf(r2.raw, false, true, r2.palm);
    return r2;
}

// H1 (a): mano che lascia l'astina (presa per sosta) dritta in giu' a
// 0,3/0,6/1,2 m/s: passo della canna <= 2,5 gradi a tick a 72 e 90 Hz, scarto
// ~0 dopo il rilascio. Prima: canna trascinata di 21,9/21,5/20,6 gradi e
// lasciata li' (porting del revisore, departure.py).
static void TestTwoHandDeparture() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    th::Guard g = ThDefaultGuard();
    for (float hz : {72.0f, 90.0f}) {
        const float dt = 1.0f / hz;
        for (float speed : {0.3f, 0.6f, 1.2f}) {
            ThRig r = ThRightRig(0.0f, 0.0f, 0.0f, pos);
            th::Solver s;
            float pl[3];
            ThGuardPoint(g, r, g.fwd, pl);
            th::Output o;
            for (int k = 0; k < 30; ++k) o = s.Step(ThInput(r, pl, false, dt, g));
            CHECK(o.engaged && !o.byGrip);
            float prev[3][4];
            std::memcpy(prev, o.gun, sizeof(prev));
            float worst = 0.0f, peak = 0.0f, atRelease = -1.0f;
            const float y0 = pl[1];
            for (int k = 1; k < static_cast<int>(1.5f * hz); ++k) {
                pl[1] = y0 - speed * dt * k;
                o = s.Step(ThInput(r, pl, false, dt, g));
                const float step = ThBarrelDeg(o.gun, prev);
                worst = step > worst ? step : worst;
                peak = o.corrDeg > peak ? o.corrDeg : peak;
                std::memcpy(prev, o.gun, sizeof(prev));
                if (o.event == 2) atRelease = o.corrDeg;
            }
            std::printf("two-hand departure %.1f m/s at %.0f Hz: barrel dragged at most %.1f deg, largest step %.2f deg per tick, "
                        "%.2f deg at the release, %.3f deg after\n",
                        speed, hz, peak, worst, atRelease, o.corrDeg);
            CHECK(atRelease >= 0.0f);
            CHECK(worst <= 2.5f);
            CHECK(o.corrDeg < 0.01f && !o.engaged);
            CHECK(peak < 14.0f);
        }
    }
}

// H1 (b): grip lasciato con la mano 9,5 cm fuori (scarto rimasto); il palmo
// rimesso sull'astina DISEGNATA (girata dallo scarto) riprende l'arma, col
// grip subito o con la sosta dopo essere uscito, girandola < 1 grado. Prima
// la prova usava solo l'astina con una mano: 12-13 cm, nessuna presa.
static void TestTwoHandRegrab() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    for (int viaDwell : {0, 1}) {
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        th::Solver s;
        float pl[3];
        ThGuardPoint(g, r, g.fwd, pl, 0.06f, 0.0f);
        th::Output o;
        for (int k = 0; k < 60; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
        CHECK(o.engaged && o.corrDeg > 5.0f);
        for (int k = 0; k < 40; ++k) {
            ThGuardPoint(g, r, g.fwd, pl, 0.06f + 0.035f * (k < 20 ? k / 20.0f : 1.0f), 0.0f);
            o = s.Step(ThInput(r, pl, true, dt, g));
        }
        CHECK(o.engaged);
        o = s.Step(ThInput(r, pl, false, dt, g));  // grip lasciato
        CHECK(!o.engaged && o.event == 2 && o.corrDeg > 3.0f);
        const float leftover = o.corrDeg;
        // Astina disegnata (telaio con lo scarto) al punto della presa.
        float f2[3], r2[3], u2[3], drawn[3];
        th::GunAxes(o.gun, f2, r2, u2);
        for (int i = 0; i < 3; ++i) drawn[i] = r.palm[i] + g.fwd * f2[i] + g.right * r2[i] + g.up * u2[i];
        if (viaDwell) {
            // fuori (oltre la presa + 2 cm) e di nuovo sopra: la sosta vale
            float away[3];
            for (int i = 0; i < 3; ++i) away[i] = drawn[i] - 0.20f * r2[i];  // 20 cm a sinistra dell'astina
            for (int k = 0; k < 10; ++k) o = s.Step(ThInput(r, away, false, dt, g));
            CHECK(!o.engaged);
        }
        float before[3][4];
        std::memcpy(before, o.gun, sizeof(before));
        th::Output o1 = s.Step(ThInput(r, drawn, !viaDwell, dt, g));
        const float oneHandCm = o1.dist * 100.0f;
        float worst = 0.0f;
        bool engaged = o1.engaged;
        o = o1;
        for (int k = 0; k < 60; ++k) {
            o = s.Step(ThInput(r, drawn, !viaDwell, dt, g));
            engaged = engaged || o.engaged;
            const float dv = ThBarrelDeg(o.gun, before);
            worst = dv > worst ? dv : worst;
        }
        std::printf("two-hand re-grab (%s) on the drawn grip after a release with %.1f deg left: %.1f cm from the one-hand barrel model, "
                    "engaged %d, gun turned %.3f deg\n",
                    viaDwell ? "rest" : "grip", leftover, oneHandCm, (int)o.engaged, worst);
        CHECK(oneHandCm > 8.0f);  // la prova di prima non la prendeva
        CHECK(engaged && o.engaged);
        CHECK(worst < 1.0f);
    }
}

// H2: ricentramento del runtime assorbito (lo spazio gira di 40 gradi in un
// tick, pose comprese). Con ShiftSpace la canna resta quella di prima girata
// con lo spazio (< 0,1 gradi) e lo scarto rimasto non cambia; senza, il filtro
// inseguiva il salto e lo scarto calava di 20 gradi.
static void TestTwoHandRecenter() {
    const float pos[3] = {0.1f, 1.2f, -0.2f};
    const float dt = 1.0f / 72.0f, yaw = 40.0f * 0.01745329252f;
    th::Guard g = ThDefaultGuard();
    for (int phase : {0, 1}) {
        for (int withShift : {0, 1}) {
            ThRig r = ThRightRig(-8.0f, 15.0f, 0.0f, pos);
            th::Solver s;
            float pl[3];
            ThGuardPoint(g, r, 0.35f, pl, 0.05f, 0.0f);
            th::Output o;
            for (int k = 0; k < 60; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
            CHECK(o.engaged && o.corrDeg > 5.0f);
            bool grip = true;
            if (phase == 1) {
                grip = false;
                for (int k = 0; k < 10; ++k) o = s.Step(ThInput(r, pl, false, dt, g));
                CHECK(!o.engaged && o.corrDeg > 5.0f);
            }
            const float before = o.corrDeg;
            float ref[3][4];
            std::memcpy(ref, o.gun, sizeof(ref));
            ThYawPose(ref, yaw);
            ThYawRig(r, yaw);
            ThYawPoint(pl, yaw);
            if (withShift) s.ShiftSpace(yaw);
            float worst = 0.0f, first = 0.0f;
            for (int k = 0; k < 8; ++k) {
                o = s.Step(ThInput(r, pl, grip, dt, g));
                const float dv = ThBarrelDeg(o.gun, ref);
                if (k == 0) first = dv;
                worst = dv > worst ? dv : worst;
            }
            std::printf("two-hand runtime recenter 40 deg (%s, %s): barrel vs the same barrel in the new space %.3f deg first tick, "
                        "%.3f worst; correction %.2f -> %.2f deg\n",
                        phase ? "released, leftover" : "engaged", withShift ? "state shifted" : "old", first, worst, before, o.corrDeg);
            if (withShift) {
                CHECK(worst < 0.1f);
                CHECK(std::fabs(o.corrDeg - before) < 0.05f);
            } else {
                CHECK(worst > 1.0f);
            }
        }
    }
    // Manubrio (WI-4c): stesso salto, yaw filtrato girato con lo spazio.
    for (int withShift : {0, 1}) {
        Handlebar hb;
        HandlebarIn in;
        in.dt = dt;
        in.allowed = in.rightValid = in.leftValid = true;
        in.gripR = in.gripL = true;
        float pr[3] = {0.25f, 1.1f, -0.4f}, pL[3] = {-0.25f, 1.1f, -0.42f};
        const float fw[3] = {0.0f, -0.2f, -0.98f};
        HandlebarOut o;
        for (int k = 0; k < 60; ++k) {
            std::memcpy(in.palmR, pr, sizeof(pr));
            std::memcpy(in.palmL, pL, sizeof(pL));
            std::memcpy(in.fwdR, fw, sizeof(fw));
            std::memcpy(in.fwdL, fw, sizeof(fw));
            in.oneYaw = 0.05f;
            o = hb.Step(in);
        }
        CHECK(o.engaged && o.weight > 0.99f);
        const float yawBefore = o.yaw;
        ThYawPoint(pr, yaw);
        ThYawPoint(pL, yaw);
        float fw2[3] = {fw[0], fw[1], fw[2]};
        ThYawPoint(fw2, yaw);
        if (withShift) hb.ShiftYaw(yaw);
        float worst = 0.0f;
        for (int k = 0; k < 8; ++k) {
            std::memcpy(in.palmR, pr, sizeof(pr));
            std::memcpy(in.palmL, pL, sizeof(pL));
            std::memcpy(in.fwdR, fw2, sizeof(fw2));
            std::memcpy(in.fwdL, fw2, sizeof(fw2));
            in.oneYaw = WrapPi(0.05f + yaw);
            o = hb.Step(in);
            const float e = std::fabs(WrapPi(o.yaw - (yawBefore + yaw))) * 57.2957795f;
            worst = e > worst ? e : worst;
        }
        std::printf("handlebar runtime recenter 40 deg (%s): turret yaw off the shifted one by %.3f deg at most\n",
                    withShift ? "state shifted" : "old", worst);
        if (withShift) CHECK(worst < 0.1f);
        else CHECK(worst > 1.0f);
    }
}

// M3: controller destro inclinato di 0/15/30/45 gradi, palmo sull'astina
// DISEGNATA (telaio dell'arma): presa senza girare l'arma (< 1 grado; prima,
// col modello sempre in verticale, 4,7/7,0/10,3 gradi a 20/30/45). Dopo la
// presa il polso destro ruota di 20 gradi: canna < 0,3 gradi e la sinistra
// disegnata (presa disegnata + drawShift) resta sul palmo vero (< 0,3 cm).
static void TestTwoHandCant() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    for (float roll : {0.0f, 15.0f, 30.0f, 45.0f}) {
        ThRig r = ThRightRig(-6.0f, 20.0f, roll, pos);
        float pl[3];
        ThGuardPoint(g, r, g.fwd, pl);
        // La vecchia astina in verticale: quanto girava la presa.
        float f1[3], r1[3], u1[3], fOld[3];
        th::GunAxes(r.gun, f1, r1, u1);
        const float d[3] = {pl[0] - r.palm[0], pl[1] - r.palm[1], pl[2] - r.palm[2]};
        float oldTurn = -1.0f;
        if (th::SolveForward(g, d, f1, u1, 0.0f, fOld)) {
            float x[3];
            th::Cross3(f1, fOld, x);
            oldTurn = std::atan2(th::Norm3(x), th::Dot3(f1, fOld)) * 57.2957795f;
        }
        th::Solver s;
        th::Output o;
        for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
        const float turn = ThBarrelDeg(o.gun, r.gun);
        CHECK(o.engaged);
        CHECK(turn < 1.0f);
        float before[3][4];
        std::memcpy(before, o.gun, sizeof(before));
        float worstBarrel = 0.0f, worstHand = 0.0f;
        for (float add : {-20.0f, 20.0f}) {
            th::Solver s2 = s;
            ThRig r2 = ThRollWrist(r, add);
            th::Output o2;
            for (int k = 0; k < 60; ++k) o2 = s2.Step(ThInput(r2, pl, true, dt, g));
            const float b = ThBarrelDeg(o2.gun, before);
            worstBarrel = b > worstBarrel ? b : worstBarrel;
            float f2[3], rr2[3], u2[3], drawn[3];
            th::GunAxes(o2.gun, f2, rr2, u2);
            for (int i = 0; i < 3; ++i)
                drawn[i] = r2.palm[i] + g.fwd * f2[i] + g.right * rr2[i] + g.up * u2[i] + o2.drawShift[0] * f2[i] +
                           o2.drawShift[1] * rr2[i] + o2.drawShift[2] * u2[i];
            const float e[3] = {drawn[0] - pl[0], drawn[1] - pl[1], drawn[2] - pl[2]};
            const float h = th::Norm3(e) * 100.0f;
            worstHand = h > worstHand ? h : worstHand;
        }
        std::printf("two-hand cant %.0f deg: palm on the drawn grip -> gun turned %.3f deg (vertical model: %.2f); right wrist roll "
                    "+-20 after: barrel %.3f deg, drawn left hand %.2f cm from the real palm\n",
                    roll, turn, oldTurn, worstBarrel, worstHand);
        CHECK(worstBarrel < 0.3f);
        CHECK(worstHand < 0.3f);
    }
}

// M4: controller sinistro ruotato (rollio 45/90, beccheggio 30) rispetto al
// destro, polso disegnato (origine raw) sulla presa disegnata: con la
// rotazione attuale nell'astina la presa non gira l'arma (< 1 grado); con
// l'ipotesi del controller parallelo il palmo stava 5-10 cm fuori.
static void TestTwoHandLiveLeft() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f, k = 0.01745329252f;
    const float q[3] = {0.331f, -0.096f, 0.046f}, off[3] = {0.0f, 0.0f, 0.0f};
    ThRig r = ThRightRig(-8.0f, 10.0f, 0.0f, pos);
    th::Guard gPar = ThDefaultGuard();
    // {rollio, beccheggio, yaw} del controller sinistro nel raw destro
    const float cases[][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 30.0f}, {0.0f, 0.0f, 45.0f}, {0.0f, 0.0f, 60.0f},
                              {90.0f, 0.0f, 0.0f}, {0.0f, 30.0f, 0.0f}, {45.0f, 30.0f, 30.0f}};
    float worstOld = 0.0f;
    for (const auto& cs : cases) {
        float rel[3][4], lraw[3][4];
        PoseFromEuler(cs[1] * k, cs[2] * k, cs[0] * k, rel);
        MulRot3(r.raw, rel, lraw);
        float f[3], rr[3], u[3];
        th::GunAxes(r.gun, f, rr, u);
        for (int i = 0; i < 3; ++i) lraw[i][3] = r.gun[i][3] + q[0] * f[i] + q[1] * rr[i] + q[2] * u[i];
        float pl[3];
        PalmOf(lraw, true, true, pl);
        float m[3][3], t[3];
        th::LeftToGun(r.gun, lraw, m);
        th::PalmTargetFromDrawnGripRot(q, m, false, true, off, t);
        th::Guard g = th::MakeGuard(t[0], t[1], t[2], false);
        // Distanza dal modello parallelo (prima).
        float rr0[3], uu0[3];
        th::GunAxes(r.gun, f, rr0, uu0);
        const float d[3] = {pl[0] - r.palm[0], pl[1] - r.palm[1], pl[2] - r.palm[2]};
        const float oldCm = th::GuardDistance(gPar, d, f, rr0, uu0, nullptr) * 100.0f;
        th::Solver s;
        th::Output o;
        for (int n = 0; n < 40; ++n) o = s.Step(ThInput(r, pl, true, dt, g));
        const float turn = ThBarrelDeg(o.gun, r.gun);
        std::printf("two-hand left controller roll %.0f pitch %.0f yaw %.0f vs the right, drawn wrist on the drawn grip: palm %.2f cm "
                    "from the live model (parallel model: %.1f cm), engaged %d, gun turned %.3f deg\n",
                    cs[0], cs[1], cs[2], o.dist * 100.0f, oldCm, (int)o.engaged, turn);
        CHECK(o.engaged && turn < 1.0f && o.dist < 0.002f);
        worstOld = oldCm > worstOld ? oldCm : worstOld;
        // Dopo la presa il polso sinistro ruota (palmo fermo): l'astina e'
        // quella fissata alla presa, la canna non si muove.
        float before[3][4];
        std::memcpy(before, o.gun, sizeof(before));
        th::Input in = ThInput(r, pl, true, dt, gPar);  // astina di adesso diversa: conta quella fissata
        for (int n = 0; n < 30; ++n) o = s.Step(in);
        CHECK(ThBarrelDeg(o.gun, before) < 0.05f);
    }
    CHECK(worstOld > 5.0f);  // il modello parallelo sbagliava di piu' di 5 cm
}

// M-a: "sinistra sull'arma" per i gesti con la geometria della presa: in
// fondo al segmento e 7 cm fuori il palmo e' entro twohandrelease (nearDist).
static void TestTwoHandNear() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    th::Guard g = ThDefaultGuard();
    ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
    struct C {
        float t, up, right;
    } cases[] = {{g.tMax, 0.0f, 0.0f}, {g.tMax + 0.03f, 0.0f, 0.0f}, {g.fwd, 0.0f, -0.07f}, {g.fwd, -0.07f, 0.0f}};
    for (const C& c : cases) {
        float pl[3];
        ThGuardPoint(g, r, c.t, pl, c.up, c.right);
        th::Solver s;
        th::Output o = s.Step(ThInput(r, pl, false, 1.0f / 72.0f, g));
        std::printf("two-hand near the gun: palm %.0f cm along, %.0f up, %.0f right of the grip -> %.1f cm from the barrel model\n",
                    c.t * 100.0f, c.up * 100.0f, c.right * 100.0f, o.nearDist * 100.0f);
        CHECK(o.nearDist < 0.15f);
    }
}

// --- Review WI-4b round 2 -------------------------------------------------------------
// Il peso del round 1 (distanza in cm dal modello con UNA mano, dove sta il
// palmo) faceva entrare nella canna a due mani il rollio dell'avambraccio
// destro, il punto lungo l'astina e il rumore del controller destro. Ora il
// peso guarda l'allontanamento della sinistra (solo il moto del palmo).

// Palmo sul modello a "t" lungo la canna, nel telaio di r ruotato di thetaDeg
// attorno all'asse ax (mondo) per il palmo destro (la linea delle mani fuori
// dalla canna con una mano di theta).
static void ThLinePoint(const th::Guard& g, const ThRig& r, const float ax[3], float thetaDeg, float t, float out[3]) {
    float f0[3], r0[3], u0[3], v[3], R[3][3];
    th::GunAxes(r.gun, f0, r0, u0);
    for (int i = 0; i < 3; ++i) v[i] = ax[i] * thetaDeg * 0.01745329252f;
    th::RotFromVec(v, R);
    float f[3], rr[3], u[3];
    for (int i = 0; i < 3; ++i) {
        f[i] = R[i][0] * f0[0] + R[i][1] * f0[1] + R[i][2] * f0[2];
        rr[i] = R[i][0] * r0[0] + R[i][1] * r0[1] + R[i][2] * r0[2];
        u[i] = R[i][0] * u0[0] + R[i][1] * u0[1] + R[i][2] * u0[2];
    }
    for (int i = 0; i < 3; ++i) out[i] = r.palm[i] + t * f[i] + g.right * rr[i] + g.up * u[i];
}
static float ThBarrelVsDeg(const float a[3][4], const float f[3]) {
    const float fa[3] = {-a[0][2], -a[1][2], -a[2][2]};
    float x[3];
    th::Cross3(fa, f, x);
    return std::atan2(th::Norm3(x), th::Dot3(fa, f)) * 57.2957795f;
}

// Alto (1): rollio dell'avambraccio destro +-20/30 gradi (asse -Z raw per il
// palmo) col palmo sul modello a 36/45/50/60 cm, col grip e appoggiato, e col
// palmo 3,6 cm fuori: canna < 0,3 gradi, nessun rilascio. Round 1: a 45/50/55/60
// cm 2,5/5,3/8,2/10,6 gradi (+-20), a 36 cm con +-30 10,95 gradi.
static void TestTwoHandRollAlong() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    float worstAll = 0.0f;
    int releases = 0;
    for (int grip : {1, 0}) {
        for (float t : {0.36f, 0.45f, 0.50f, 0.60f}) {
            for (float off : {0.0f, 0.036f}) {
                ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
                float pl[3];
                ThGuardPoint(g, r, t, pl, off * 0.6f, -off * 0.8f);
                th::Solver s;
                th::Output o;
                for (int k = 0; k < 72; ++k) o = s.Step(ThInput(r, pl, grip != 0, dt, g));
                CHECK(o.engaged);
                float before[3][4];
                std::memcpy(before, o.gun, sizeof(before));
                for (float roll : {20.0f, 30.0f}) {
                    for (float sg : {-1.0f, 1.0f}) {
                        th::Solver s2 = s;
                        th::Output o2;
                        // a scatto (come la prova del revisore) e poi a rampa di 0,5 s
                        for (int ramp : {0, 1}) {
                            th::Solver s3 = s2;
                            for (int k = 1; k <= 108; ++k) {
                                const float a = ramp ? sg * roll * (k < 36 ? k / 36.0f : 1.0f) : sg * roll;
                                ThRig r2 = ThRollWrist(r, a);
                                o2 = s3.Step(ThInput(r2, pl, grip != 0, dt, g));
                                releases += o2.event == 2;
                                const float dv = ThBarrelDeg(o2.gun, before);
                                worstAll = dv > worstAll ? dv : worstAll;
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("two-hand round 2: forearm roll +-20/30 deg, palm on the model (and 3.6 cm off) at 36/45/50/60 cm, grip and rest: "
                "barrel moved at most %.4f deg, %d releases\n",
                worstAll, releases);
    CHECK(worstAll < 0.3f);
    CHECK(releases == 0);
}

// Alto (2): la mano scorre lungo l'astina da 21,5 a 59,5 cm in 1 s con la
// linea delle mani 10/12 gradi fuori dalla canna con una mano (in su e di
// lato): la canna resta sulla linea delle mani (< 0,3 gradi dopo), presa
// mai persa, peso 1, la sinistra disegnata sul palmo (< 0,3 cm). Round 1:
// 2,65/8,31 gradi fuori e la mano disegnata 2,8/8,6 cm dal palmo.
static void TestTwoHandSlide() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    for (int axis : {0, 1}) {
        for (float theta : {10.0f, 12.0f}) {
            ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
            float f0[3], r0[3], u0[3];
            th::GunAxes(r.gun, f0, r0, u0);
            const float* ax = axis == 0 ? r0 : u0;
            float v[3], R[3][3], fl[3];
            for (int i = 0; i < 3; ++i) v[i] = ax[i] * theta * 0.01745329252f;
            th::RotFromVec(v, R);
            for (int i = 0; i < 3; ++i) fl[i] = R[i][0] * f0[0] + R[i][1] * f0[1] + R[i][2] * f0[2];
            th::Solver s;
            th::Output o;
            float pl[3];
            const float t0 = g.tMin + 0.01f, t1 = g.tMax - 0.01f;
            ThLinePoint(g, r, ax, theta, t0, pl);
            for (int k = 0; k < 72; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
            CHECK(o.engaged);
            bool always = true;
            float minW = 1.0f;
            for (int k = 1; k <= 72; ++k) {
                ThLinePoint(g, r, ax, theta, t0 + (t1 - t0) * k / 72.0f, pl);
                o = s.Step(ThInput(r, pl, true, dt, g));
                always = always && o.engaged;
                minW = o.weight < minW ? o.weight : minW;
            }
            for (int k = 0; k < 72; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
            const float dev = ThBarrelVsDeg(o.gun, fl);
            std::printf("two-hand round 2: slide 21.5 -> 59.5 cm, hands' line %.0f deg off the one-hand barrel (%s): engaged throughout %d, "
                        "lowest weight %.3f, barrel %.3f deg off the hands' line, palm %.2f cm from the drawn model\n",
                        theta, axis == 0 ? "up" : "sideways", (int)always, minW, dev, o.distOut * 100.0f);
            CHECK(always && minW > 0.999f);
            CHECK(dev < 0.3f);
            CHECK(o.distOut < 0.003f);
        }
    }
}

// Alto (3): rumore di rotazione del controller destro (0,2 / 0,4 gradi rms
// per asse, attorno al palmo) col palmo fermo sulla linea a 45 / 50 cm, 10 /
// 12 gradi fuori: nessun rumore in piu' sulla canna (< 0,01 gradi rms). E
// nella fascia (sinistra allontanata, peso ~0,5): la parte della destra e'
// filtrata, < 0,1 gradi rms. Round 1: 0,28-0,46 gradi rms con 0,2 gradi.
static void TestTwoHandJitter() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    auto jitterRig = [&](const ThRig& r, std::mt19937& rng, float sigmaDeg) {
        std::normal_distribution<float> nd(0.0f, sigmaDeg * 0.01745329252f);
        float v[3] = {nd(rng), nd(rng), nd(rng)}, R[3][3];
        th::RotFromVec(v, R);
        ThRig r2 = r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r2.raw[i][j] = R[i][0] * r.raw[0][j] + R[i][1] * r.raw[1][j] + R[i][2] * r.raw[2][j];
        float op[3] = {r.raw[0][3] - r.palm[0], r.raw[1][3] - r.palm[1], r.raw[2][3] - r.palm[2]};
        for (int i = 0; i < 3; ++i) r2.raw[i][3] = r.palm[i] + R[i][0] * op[0] + R[i][1] * op[1] + R[i][2] * op[2];
        GunFrameParams p;
        GunFrameFromRaw(r2.raw, p, r2.gun);
        PalmOf(r2.raw, false, true, r2.palm);
        return r2;
    };
    auto rmsAbout = [](const std::vector<std::array<float, 3>>& fs) {
        float m[3] = {0.0f, 0.0f, 0.0f};
        for (const auto& f : fs)
            for (int i = 0; i < 3; ++i) m[i] += f[i];
        th::Normalize3(m);
        double sq = 0.0;
        for (const auto& f : fs) {
            float x[3];
            th::Cross3(m, f.data(), x);
            const double a = std::atan2(th::Norm3(x), th::Dot3(m, f.data())) * 57.2957795;
            sq += a * a;
        }
        return std::sqrt(sq / fs.size());
    };
    double worst = 0.0;
    for (float jit : {0.2f, 0.4f}) {
        for (float theta : {10.0f, 12.0f}) {
            for (float t : {0.45f, 0.50f}) {
                ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
                float f0[3], r0[3], u0[3], pl[3];
                th::GunAxes(r.gun, f0, r0, u0);
                th::Solver s;
                th::Output o;
                ThLinePoint(g, r, r0, theta, g.tMin + 0.01f, pl);
                for (int k = 0; k < 72; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
                for (int k = 1; k <= 36; ++k) {
                    ThLinePoint(g, r, r0, theta, g.tMin + 0.01f + (t - g.tMin - 0.01f) * k / 36.0f, pl);
                    o = s.Step(ThInput(r, pl, true, dt, g));
                }
                for (int k = 0; k < 144; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
                std::mt19937 rng(11);
                std::vector<std::array<float, 3>> fs;
                for (int k = 0; k < 720; ++k) {
                    o = s.Step(ThInput(jitterRig(r, rng, jit), pl, true, dt, g));
                    fs.push_back({-o.gun[0][2], -o.gun[1][2], -o.gun[2][2]});
                }
                const double n = rmsAbout(fs);
                worst = n > worst ? n : worst;
                CHECK(o.engaged);
            }
        }
    }
    std::printf("two-hand round 2: right-controller rotation jitter 0.2/0.4 deg, palm still on the hands' line at 45/50 cm: barrel noise "
                "%.4f deg rms at most\n",
                worst);
    CHECK(worst < 0.01);
    // Nella fascia: palmo allontanato verso il basso fino a peso ~0,5 e tenuto li'.
    {
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        float pl[3];
        ThGuardPoint(g, r, g.fwd, pl);
        th::Solver s;
        th::Output o;
        for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, false, dt, g));
        CHECK(o.engaged);
        float f0[3], r0[3], u0[3];
        th::GunAxes(r.gun, f0, r0, u0);
        const float y0 = pl[1];
        int k = 0;
        while (o.engaged && o.weight > 0.5f && k++ < 200) {
            pl[1] = y0 - 0.002f * k;
            o = s.Step(ThInput(r, pl, false, dt, g));
        }
        for (int n = 0; n < 72; ++n) o = s.Step(ThInput(r, pl, false, dt, g));
        const float w = o.weight;
        std::mt19937 rng(5);
        std::vector<std::array<float, 3>> fs;
        float wMin = 1.0f, wMax = 0.0f;
        for (int n = 0; n < 720; ++n) {
            o = s.Step(ThInput(jitterRig(r, rng, 0.2f), pl, false, dt, g));
            fs.push_back({-o.gun[0][2], -o.gun[1][2], -o.gun[2][2]});
            wMin = o.weight < wMin ? o.weight : wMin;
            wMax = o.weight > wMax ? o.weight : wMax;
        }
        const double n = rmsAbout(fs);
        std::printf("two-hand round 2: left hand in the band (weight %.2f, %.1f deg away), right jitter 0.2 deg: barrel noise %.4f deg rms, "
                    "weight %.3f..%.3f, engaged %d\n",
                    w, o.departDeg, n, wMin, wMax, (int)o.engaged);
        CHECK(o.engaged && w > 0.3f && w < 0.7f);
        CHECK(n < 0.1);
        CHECK(wMax - wMin < 0.05f);
    }
}

// Medio (1): grip lasciato con uno scarto di 12/16/20 gradi (presa a 21,5 cm
// con la linea delle mani theta fuori), mano via, poi il palmo sull'arma
// DISEGNATA a 59 cm col grip: una sola presa e l'arma gira < 1 grado. Round
// 1: 7,8 gradi (12) e presa / rilascio a ogni tick (16, 20).
// Medio (2): il polso sinistro VISTO messo sulla presa disegnata dell'arma
// disegnata, con l'astina dalla rotazione attuale della sinistra nel telaio
// DISEGNATO (Solver::DrawnRot, come hooks.cpp): < 1 grado (round 1, col
// telaio con una mano: 2,3 / 0,6 / 1,7 gradi).
static void TestTwoHandRegrabAlong() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    for (float theta : {12.0f, 16.0f, 20.0f}) {
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        float f0[3], r0[3], u0[3], pl[3];
        th::GunAxes(r.gun, f0, r0, u0);
        th::Solver s;
        th::Output o;
        ThLinePoint(g, r, r0, theta, g.tMin + 0.01f, pl);
        for (int k = 0; k < 72; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
        o = s.Step(ThInput(r, pl, false, dt, g));
        const float left = o.corrDeg;
        CHECK(!o.engaged && left > theta - 0.5f);
        float away[3];
        for (int i = 0; i < 3; ++i) away[i] = pl[i] - 0.25f * r0[i];
        for (int k = 0; k < 10; ++k) o = s.Step(ThInput(r, away, false, dt, g));
        float before[3][4], fD[3], rD[3], uD[3];
        std::memcpy(before, o.gun, sizeof(before));
        th::GunAxes(o.gun, fD, rD, uD);
        const float tRe = g.tMax - 0.015f;
        for (int i = 0; i < 3; ++i) pl[i] = r.palm[i] + tRe * fD[i] + g.right * rD[i] + g.up * uD[i];
        int events = 0, engagedTicks = 0;
        float worst = 0.0f;
        for (int k = 0; k < 72; ++k) {
            o = s.Step(ThInput(r, pl, true, dt, g));
            events += o.event != 0;
            engagedTicks += o.engaged;
            const float dv = ThBarrelDeg(o.gun, before);
            worst = dv > worst ? dv : worst;
        }
        std::printf("two-hand round 2: leftover %.1f deg, palm on the drawn gun at %.0f cm with the grip: %d event(s), engaged %d/72, "
                    "gun turned %.3f deg\n",
                    left, tRe * 100.0f, events, engagedTicks, worst);
        CHECK(events == 1 && engagedTicks == 72);
        CHECK(worst < 1.0f);
    }
    // Polso sinistro visto sulla presa dell'arma disegnata (controller sinistro parallelo al destro).
    const float q[3] = {0.331f, -0.096f, 0.046f}, off[3] = {0.0f, 0.0f, 0.0f};
    for (float up : {0.06f, 0.095f, 0.12f}) {
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        th::Solver s;
        th::Output o;
        float pl[3];
        ThGuardPoint(g, r, g.fwd, pl, 0.06f, 0.0f);
        for (int k = 0; k < 60; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
        for (int k = 0; k < 40; ++k) {
            ThGuardPoint(g, r, g.fwd, pl, 0.06f + (up - 0.06f) * (k < 20 ? k / 20.0f : 1.0f), 0.0f);
            o = s.Step(ThInput(r, pl, true, dt, g));
        }
        o = s.Step(ThInput(r, pl, false, dt, g));
        const float left = o.corrDeg;
        float f2[3], r2[3], u2[3], away[3];
        th::GunAxes(o.gun, f2, r2, u2);
        for (int i = 0; i < 3; ++i) away[i] = pl[i] - 0.20f * r2[i];
        for (int k = 0; k < 10; ++k) o = s.Step(ThInput(r, away, false, dt, g));
        float before[3][4];
        std::memcpy(before, o.gun, sizeof(before));
        th::GunAxes(o.gun, f2, r2, u2);
        float lraw[3][4];
        std::memcpy(lraw, r.raw, sizeof(lraw));
        for (int i = 0; i < 3; ++i) lraw[i][3] = o.gun[i][3] + q[0] * f2[i] + q[1] * r2[i] + q[2] * u2[i];
        float palmL[3];
        PalmOf(lraw, true, true, palmL);
        // Astina come in hooks.cpp: rotazione attuale della sinistra nel telaio DISEGNATO.
        float drawn[3][4], m[3][3], t[3];
        s.DrawnRot(r.gun, drawn);
        th::LeftToGun(drawn, lraw, m);
        th::PalmTargetFromDrawnGripRot(q, m, false, true, off, t);
        const th::Guard live = th::MakeGuard(t[0], t[1], t[2], false);
        // ... e col telaio con una mano (round 1), per confronto.
        th::LeftToGun(r.gun, lraw, m);
        th::PalmTargetFromDrawnGripRot(q, m, false, true, off, t);
        const float oldCm = std::sqrt((t[0] - live.fwd) * (t[0] - live.fwd) + (t[1] - live.right) * (t[1] - live.right) +
                                      (t[2] - live.up) * (t[2] - live.up)) * 100.0f;
        float worst = 0.0f;
        int eng = 0;
        for (int k = 0; k < 72; ++k) {
            o = s.Step(ThInput(r, palmL, true, dt, live));
            eng += o.engaged;
            const float dv = ThBarrelDeg(o.gun, before);
            worst = dv > worst ? dv : worst;
        }
        std::printf("two-hand round 2: leftover %.1f deg, visible left wrist on the drawn grip of the drawn gun: engaged %d/72, gun turned "
                    "%.3f deg (one-hand-frame model %.2f cm off)\n",
                    left, eng, worst, oldCm);
        CHECK(eng == 72 && worst < 1.0f);
    }
}

// Mirare con due mani: spazzata di 60 gradi a 120 gradi/s con la rotazione
// della destra in ritardo / in anticipo di 3 tick, o che segue solo al 90%:
// nessun allontanamento (peso 1, presa tenuta). Le due parti si annullano
// anche quando arrivano in tick diversi.
static void TestTwoHandSteerLag() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    struct C {
        int lag;
        float follow;
        const char* name;
    } cases[] = {{3, 1.0f, "right hand 3 ticks late"}, {-3, 1.0f, "right hand 3 ticks early"}, {0, 0.9f, "right hand follows 90%"}};
    for (const C& cs : cases) {
        th::Solver s;
        th::Output o;
        auto yawAt = [&](int k) {
            const float t = k * dt;
            return t < 0.5f ? 0.0f : (t < 1.0f ? 120.0f * (t - 0.5f) : 60.0f);
        };
        float minW = 1.0f, maxDep = 0.0f;
        bool always = true;
        for (int k = 0; k < 180; ++k) {
            const float yL = yawAt(k), yR = cs.follow * yawAt(k - cs.lag);
            ThRig rl = ThRightRig(-5.0f, yL, 0.0f, pos), rr = ThRightRig(-5.0f, yR, 0.0f, pos);
            float pl[3];
            ThGuardPoint(g, rl, g.fwd, pl);
            o = s.Step(ThInput(rr, pl, true, dt, g));
            if (k > 40) {
                always = always && o.engaged;
                minW = o.weight < minW ? o.weight : minW;
                maxDep = o.departDeg > maxDep ? o.departDeg : maxDep;
            }
        }
        std::printf("two-hand round 2: two-handed sweep of 60 deg, %s: engaged throughout %d, lowest weight %.3f, departure at most %.2f deg\n",
                    cs.name, (int)always, minW, maxDep);
        CHECK(always && minW > 0.999f);
    }
}

// Rilasci nuovi: la mano dell'arma girata via di 60 gradi con il palmo fermo
// (mani in disaccordo oltre la fascia larga), e il palmo che scorre 25 cm
// oltre la fine dell'astina: rilascio, poi lo scarto torna a 0. Col grip
// stretto dopo un rilascio per distanza la presa non riparte finche' il grip
// non si lascia.
static void TestTwoHandNewReleases() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    {
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        float pl[3];
        ThGuardPoint(g, r, g.fwd, pl);
        th::Solver s;
        th::Output o;
        for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
        const char* why = "";
        float apartAt = -1.0f;
        for (int k = 1; k <= 144; ++k) {
            ThRig r2 = ThRightRig(-5.0f, 60.0f * (k < 72 ? k / 72.0f : 1.0f), 0.0f, pos);
            const th::Output prev = o;
            o = s.Step(ThInput(r2, pl, true, dt, g));
            if (o.event == 2) {
                why = o.why;
                apartAt = prev.apartDeg;
            }
        }
        std::printf("two-hand round 2: gun hand turned 60 deg away, palm still: released (%s) at %.1f deg apart, correction after %.3f deg\n",
                    why, apartAt, o.corrDeg);
        CHECK(!o.engaged && std::strcmp(why, "gun hand turned away from the left hand") == 0);
        CHECK(o.corrDeg < 0.01f);
    }
    {
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        float pl[3];
        ThGuardPoint(g, r, g.fwd, pl);
        th::Solver s;
        th::Output o;
        for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, false, dt, g));
        const char* why = "";
        for (int k = 1; k <= 144; ++k) {
            ThGuardPoint(g, r, g.fwd + (g.tMax + 0.25f - g.fwd) * (k < 72 ? k / 72.0f : 1.0f), pl);
            o = s.Step(ThInput(r, pl, false, dt, g));
            if (o.event == 2) why = o.why;
        }
        std::printf("two-hand round 2: palm slid 25 cm past the end of the handguard: released (%s)\n", why);
        CHECK(!o.engaged && std::strcmp(why, "left hand past the end of the barrel") == 0 && o.corrDeg < 0.01f);
    }
    {
        // Grip stretto, la mano va via (rilascio per distanza), torna sull'arma col grip ancora stretto: niente presa.
        ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
        float pl[3], base[3];
        ThGuardPoint(g, r, g.fwd, pl);
        std::memcpy(base, pl, sizeof(base));
        th::Solver s;
        th::Output o;
        for (int k = 0; k < 40; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
        bool released = false;
        for (int k = 1; k <= 72 && !released; ++k) {
            pl[1] = base[1] - 0.6f * dt * k;
            o = s.Step(ThInput(r, pl, true, dt, g));
            released = o.event == 2;
        }
        CHECK(released && s.needRegrip);
        for (int k = 0; k < 30; ++k) o = s.Step(ThInput(r, pl, true, dt, g));  // la correzione torna a 0
        bool any = false;
        for (int k = 0; k < 40; ++k) any = any || s.Step(ThInput(r, base, true, dt, g)).engaged;
        const bool heldBack = !any;
        s.Step(ThInput(r, base, false, dt, g));
        o = s.Step(ThInput(r, base, true, dt, g));
        std::printf("two-hand round 2: grip held through a distance release, hand back on the gun: engaged %d while still held, %d after a new "
                    "press\n",
                    (int)!heldBack, (int)o.engaged);
        CHECK(heldBack && o.engaged);
    }
}

// Scarto rimasto (grip lasciato) e colpi di polso in un tick (20 gradi: "synthctrl" in gioco): il calo
// (0,5 gradi per grado) arriva tutto, a 150 gradi/s al piu'. Prima il limite per tick lo tagliava: 4 colpi
// (120 gradi di moto) toglievano 8,3 gradi su 15,7 (in gioco: 7,2 rimasti).
static void TestTwoHandLeftoverFlick() {
    const float pos[3] = {0.0f, 1.2f, 0.0f};
    const float dt = 1.0f / 72.0f;
    th::Guard g = ThDefaultGuard();
    ThRig r = ThRightRig(-5.0f, 0.0f, 0.0f, pos);
    float pl[3];
    ThGuardPoint(g, r, g.fwd, pl, 0.06f, 0.0f);
    th::Solver s;
    th::Output o;
    for (int k = 0; k < 60; ++k) o = s.Step(ThInput(r, pl, true, dt, g));
    for (int k = 0; k < 40; ++k) {
        ThGuardPoint(g, r, g.fwd, pl, 0.06f + 0.035f * (k < 20 ? k / 20.0f : 1.0f), 0.0f);
        o = s.Step(ThInput(r, pl, true, dt, g));
    }
    o = s.Step(ThInput(r, pl, false, dt, g));
    const float left = o.corrDeg;
    float away[3];
    for (int i = 0; i < 3; ++i) away[i] = pl[i] - 0.25f * r.gun[i][0];
    float worstC = 0.0f, prevC = o.corrDeg, afterFirst = -1.0f;
    for (float yaw : {20.0f, -20.0f, 20.0f, 0.0f}) {
        ThRig r2 = ThRightRig(-5.0f, yaw, 0.0f, pos);
        for (int k = 0; k < 36; ++k) {
            o = s.Step(ThInput(r2, away, false, dt, g));
            worstC = std::fabs(o.corrDeg - prevC) > worstC ? std::fabs(o.corrDeg - prevC) : worstC;
            prevC = o.corrDeg;
        }
        if (afterFirst < 0.0f) afterFirst = o.corrDeg;
    }
    std::printf("two-hand round 2: leftover %.2f deg, gun hand flicked 20 deg in one tick: %.2f left after the first flick, %.3f after "
                "4 flicks (120 deg), correction step at most %.2f deg per tick\n",
                left, afterFirst, o.corrDeg, worstC);
    CHECK(left > 10.0f && std::fabs(afterFirst - (left - 10.0f)) < 0.05f && o.corrDeg < 0.01f && worstC < 2.1f);
}

// Riga "twohand on <cm> [s]" / "leftgrip on <cm>" scritta a mano: configversion 10 la riduce a on/off.
static void TestTwoHandMigration() {
    std::vector<std::string> lines = {"twohand on 12 0.3", "leftgrip on 20", "configversion 9"};
    const ComputedChange comp[] = {{10, "twohand", &MigrateTwoHandLine}, {10, "leftgrip", &MigrateTwoHandLine}};
    std::string done;
    CHECK(MigrateConfigLines(lines, 10, nullptr, 0, &done, comp, 2));
    CHECK(lines.size() == 3 && lines[0] == "twohand on" && lines[1] == "leftgrip on" && lines[2] == "configversion 10");
    lines = {"twohand off 9", "twohand on", "configversion 9"};
    CHECK(MigrateConfigLines(lines, 10, nullptr, 0, &done, comp, 2));
    CHECK(lines[0] == "twohand off" && lines[1] == "twohand on");
    lines = {"twohand on 12", "configversion 10"};
    CHECK(!MigrateConfigLines(lines, 10, nullptr, 0, &done, comp, 2) && lines[0] == "twohand on 12");
}

// Chiavi nuove nel menu Advanced (passi) e nell'ini distribuito.
static void TestTwoHandKeys() {
    CHECK(MenuStepFor("twohandgrip", 8.0f).step == 1.0f);
    CHECK(MenuStepFor("twohandrelease", 15.0f).step == 1.0f);
    CHECK(MenuStepFor("twohanddwell", 0.15f).step == 0.05f);
    CHECK(MenuStepFor("twohandsmooth", 1.0f).step == 0.25f);
    CHECK(MenuStepFor("stocktol", 6.0f).step == 1.0f);
    CHECK(StepConfigNumber("twohanddwell", "0.15", 1) == "0.20");
    CHECK(StepConfigNumber("stocktol", "6 25", -1) == "5 25");
    // Review round 1: una chiave per numero (il menu passa solo il primo).
    CHECK(MenuStepFor("twohandguardfwd", 0.0f).step == 0.5f);
    CHECK(MenuStepFor("twohandguardright", 0.0f).step == 0.5f);
    CHECK(MenuStepFor("twohandguardup", 0.0f).step == 0.5f);
    CHECK(MenuStepFor("stocktoldeg", 25.0f).step == 1.0f);
    CHECK(StepConfigNumber("twohandguardright", "0", 1) == "0.5");
    CHECK(StepConfigNumber("stocktoldeg", "25", -1) == "24");
}

// --- Follow-up della regressione finale (27/10) ------------------------------------

// Corpo e testa a 72 Hz con turnwithhead (eccesso oltre thr a 180 gradi/s, come hooks.cpp) e, se follow,
// vr::pure::BodyFollow. head(t) in gradi. Ritorna lo scarto testa-corpo alla fine; *maxRate / *maxAcc del corpo
// dovuti al solo BodyFollow (gradi/s, gradi/s^2), *flips = volte in cui il corpo e' passato oltre la testa.
template <class HeadFn>
static double BodyAlignModel(HeadFn head, double seconds, bool follow, double* maxRate, double* maxAcc, int* flips,
                             double* offAt = nullptr, double atT = -1.0, double* movingFollow = nullptr) {
    const double dt = 1.0 / 72.0, thr = 45.0;
    double body = 0.0, prevRate = 0.0;
    BodyFollow bf;
    *maxRate = *maxAcc = 0.0;
    *flips = 0;
    if (movingFollow) *movingFollow = 0.0;
    double prevHead = head(0.0);
    for (int i = 1; i <= static_cast<int>(seconds / dt); ++i) {
        const double t = i * dt, h = head(t);
        double ex = std::remainder(h - body, 360.0);
        if (std::fabs(ex) > thr) {
            double shift = ex - std::copysign(thr, ex);
            const double maxStep = 180.0 * dt;
            if (std::fabs(shift) > maxStep) shift = std::copysign(maxStep, shift);
            body += shift;
            ex -= shift;
        }
        double rate = 0.0;
        if (follow) {
            const double before = ex;
            const double step =
                bf.Step(t, static_cast<float>(dt), static_cast<float>(h / 57.2957795), static_cast<float>(ex / 57.2957795), true) * 57.2957795;
            body += step;
            ex -= step;
            rate = step / dt;
            if (before * ex < -1e-6) ++*flips;
            if (movingFollow && std::fabs(std::remainder(h - prevHead, 360.0)) / dt > 60.0)
                *movingFollow = std::fmax(*movingFollow, std::fabs(rate));
        }
        *maxRate = std::fmax(*maxRate, std::fabs(rate));
        if (i > 1) *maxAcc = std::fmax(*maxAcc, std::fabs(rate - prevRate) / dt);
        prevRate = rate;
        prevHead = h;
        if (offAt && atT >= 0.0 && std::fabs(t - atT) < dt * 0.5) *offAt = ex;
    }
    return std::remainder(head(seconds) - body, 360.0);
}

static void TestBodyAlign() {
    auto smooth = [](double a, double b, double t0, double dur, double t) {
        if (t <= t0) return a;
        if (t >= t0 + dur) return b;
        double u = (t - t0) / dur;
        u = u * u * (3.0 - 2.0 * u);
        return a + (b - a) * u;
    };
    // Lo sguardo della regressione finale: 0 -> 90 in 1 s, fermo 2 s, di nuovo 0 in 1 s, poi fermo.
    auto look = [&](double t) { return t < 1.5 ? smooth(0, 90, 0.5, 1.0, t) : smooth(90, 0, 3.5, 1.0, t); };
    double r0, a0, r1, a1, off3 = 0.0;
    int f0, f1;
    const double before = BodyAlignModel(look, 25.0, false, &r0, &a0, &f0);
    const double after = BodyAlignModel(look, 25.0, true, &r1, &a1, &f1, &off3, 4.5 + 3.0);
    BodyFollow ref;
    std::printf("body align: look 90 and back, head off the body after 20 s: %.2f deg without, %.2f with (%.2f deg 3 s after the return); "
                "body speed max %.1f deg/s, acceleration max %.0f deg/s^2 (limit %.0f), crossings %d\n",
                before, after, off3, r1, a1, ref.maxDegS / ref.rampSec, f1);
    CHECK(std::fabs(std::fabs(before) - 45.0) < 0.5);        // il difetto: 45 gradi per sempre
    CHECK(std::fabs(after) < 0.6 && std::fabs(off3) < 1.0);  // sotto la testa entro 3 s
    CHECK(r1 <= ref.maxDegS + 0.01 && a1 <= ref.maxDegS / ref.rampSec * 1.02 + 1.0 && f1 == 0);
    // Tremore della testa ferma (0,2 gradi a 5 Hz, 30 gradi fuori): si allinea comunque.
    double rt, at;
    int ft;
    const double offTrem =
        BodyAlignModel([](double t) { return t < 0.01 ? 0.0 : 30.0 + 0.2 * std::sin(6.28318530718 * 5.0 * t); }, 6.0, true, &rt, &at, &ft);
    CHECK(std::fabs(offTrem) < 0.8 && ft == 0);
    // Testa che si guarda attorno di continuo (+-35 gradi a 0,5 Hz, fino a 110 gradi/s): niente rotazione mentre si
    // muove veloce (sopra 60 gradi/s), solo la rampa che si spegne.
    double rm, am, mov = 0.0;
    int fm;
    BodyAlignModel([](double t) { return 35.0 * std::sin(6.28318530718 * 0.5 * t); }, 10.0, true, &rm, &am, &fm, nullptr, -1.0, &mov);
    std::printf("body align: tremor 0.2 deg at 5 Hz -> %.2f deg left; looking around +-35 deg at 0.5 Hz: body speed max %.1f deg/s, "
                "%.1f while the head turns faster than 60 deg/s\n",
                offTrem, rm, mov);
    CHECK(mov <= 1.0 && fm == 0);
    // Verifica del follow-up: un tick lungo (209 ms, scatto del gioco) a velocita' piena non da' un passo oltre
    // l'anticipo delle mani (5 gradi): al massimo maxDegS * kMaxStepDt.
    {
        BodyFollow h;
        const float off = 40.0f / 57.2957795f;
        float turned = 0.0f, longStep = 0.0f;
        int i = 0;
        for (; i < 72 * 2 && h.rate < h.maxDegS - 0.01f; ++i) turned += h.Step(i / 72.0, 1.0f / 72.0f, 0.3f, off - turned, true);
        const bool atSpeed = h.rate >= h.maxDegS - 0.01f;
        longStep = h.Step(i / 72.0 + 0.209, 0.209f, 0.3f, off - turned, true) * 57.2957795f;
        std::printf("body align: one 209-ms tick at full speed (%d) -> body step %.2f deg (limit %.2f, the hand lead stops at 5)\n",
                    (int)atSpeed, longStep, h.maxDegS * BodyFollow::kMaxStepDt);
        CHECK(atSpeed && longStep > 0.0f && longStep <= h.maxDegS * BodyFollow::kMaxStepDt + 0.01f && longStep < 5.0f);
    }
    // Spento (bodyalign 0) e non permesso (copertura, torretta): nessun passo.
    BodyFollow off;
    off.holdSec = 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < 300; ++i) sum += off.Step(i / 72.0, 1.0f / 72.0f, 0.5f, 0.5f, true);
    BodyFollow na;
    for (int i = 0; i < 300; ++i) sum += na.Step(i / 72.0, 1.0f / 72.0f, 0.5f, 0.5f, false);
    CHECK(sum == 0.0f);
    // Scarto sotto startDeg: non parte.
    BodyFollow tiny;
    for (int i = 0; i < 300; ++i) sum += tiny.Step(i / 72.0, 1.0f / 72.0f, 0.0f, 2.0f / 57.2957795f, true);
    CHECK(sum == 0.0f);
    // Review del follow-up: arma puntata (busy) = testa in movimento. 40 gradi fuori, testa ferma: con busy non parte;
    // busy a meta' rotazione: si ferma nel primo tick puntato (review 2: prima in rampa, fino a 0,3 s di coda), resta
    // fermo per tutta la mira, poi riparte dopo holdSec (accelerazione della ripartenza entro il limite).
    BodyFollow bz;
    float offb = 40.0f, moved = 0.0f;
    for (int i = 0; i < 216; ++i) {
        const float st = bz.Step(i / 72.0, 1.0f / 72.0f, 0.3f, offb / 57.2957795f, true, true) * 57.2957795f;
        offb -= st;
        moved += std::fabs(st);
    }
    CHECK(moved == 0.0f && !bz.following);
    BodyFollow bm;
    offb = 40.0f;
    float prevRate = 0.0f, maxAccResume = 0.0f, offAtBusy = 0.0f, offAfter = 0.0f, movedBusy = 0.0f;
    const float dtb = 1.0f / 72.0f;
    for (int i = 0; i < 72 * 6; ++i) {
        const double t = i / 72.0;
        const bool busy = t >= 1.2 && t < 2.2;
        const float st = bm.Step(t, dtb, 0.3f, offb / 57.2957795f, true, busy) * 57.2957795f;
        offb -= st;
        const float rate = st / dtb;
        if (t >= 2.2 && i > 0) maxAccResume = std::fmax(maxAccResume, std::fabs(rate - prevRate) / dtb);
        prevRate = rate;
        if (std::fabs(t - 1.2) < 0.5 * dtb) offAtBusy = offb + st;  // prima del primo tick puntato
        if (busy) movedBusy += std::fabs(st);
        offAfter = offb;
    }
    std::printf("body align, gun aimed half-way: %.1f deg left when aimed, %.4f deg turned during the 1-s aim, %.2f deg left 3.8 s "
                "later; acceleration max after the aim %.0f deg/s^2\n",
                offAtBusy, movedBusy, offAfter, maxAccResume);
    CHECK(offAtBusy > 3.0f && movedBusy == 0.0f && std::fabs(offAfter) < 0.6f &&
          maxAccResume <= bm.maxDegS / bm.rampSec * 1.02f + 1.0f);
}

// f3 (27/10): LocalToWorld della mesh prevista dall'attore e ossa rimesse girate attorno all'asse del Pawn.
static void TestUe3BoneMath() {
    using namespace vr::pure::ue3m;
    const float d2r = 0.0174532925f;
    auto frame = [&](float yawDeg, float x, float y, float z) {
        Frame f;
        const float c = std::cos(yawDeg * d2r), s = std::sin(yawDeg * d2r);
        const float ax[3][3] = {{c, s, 0.0f}, {-s, c, 0.0f}, {0.0f, 0.0f, 1.0f}};
        std::memcpy(f.ax, ax, sizeof(ax));
        f.org[0] = x;
        f.org[1] = y;
        f.org[2] = z;
        return f;
    };
    // LocalToWorld indipendente: mesh girata di relYaw e spostata di relT nello spazio dell'attore, attore a yaw e loc.
    auto l2wOf = [&](float relYaw, const float relT[3], const Frame& a, float out[16]) {
        const float c = std::cos(relYaw * d2r), s = std::sin(relYaw * d2r);
        const float rows[3][3] = {{c, s, 0.0f}, {-s, c, 0.0f}, {0.0f, 0.0f, 1.0f}};
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 3; ++i) out[4 * r + i] = rows[r][0] * a.ax[0][i] + rows[r][1] * a.ax[1][i] + rows[r][2] * a.ax[2][i];
            out[4 * r + 3] = 0.0f;
        }
        for (int i = 0; i < 3; ++i) out[12 + i] = a.org[i] + relT[0] * a.ax[0][i] + relT[1] * a.ax[1][i] + relT[2] * a.ax[2][i];
        out[15] = 1.0f;
    };
    // 1) Quaternione <-> matrice (anche w piccolo: traccia negativa).
    float worstQ = 0.0f;
    const float qs[][4] = {{0, 0, 0, 1}, {0.3f, -0.2f, 0.5f, 0.78f}, {0.9f, 0.1f, -0.3f, 0.05f}, {-0.1f, 0.95f, 0.2f, -0.1f}, {0.2f, 0.1f, -0.97f, 0.02f}};
    for (const auto& q0 : qs) {
        float q[4], n = std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
        for (int i = 0; i < 4; ++i) q[i] = q0[i] / n;
        float m[3][3], r[4];
        QuatToRows(q, m);
        RowsToQuat(m, r);
        const float sg = (q[0] * r[0] + q[1] * r[1] + q[2] * r[2] + q[3] * r[3]) < 0.0f ? -1.0f : 1.0f;
        for (int i = 0; i < 4; ++i) worstQ = std::fmax(worstQ, std::fabs(q[i] - sg * r[i]));
    }
    // 2) LocalToWorld prevista: relativa presa con l'attore a 30 gradi, attore ora a 39 gradi e spostato,
    // Translation cambiata di (1, -2, 0.5).
    const float relT[3] = {12.0f, -3.0f, -44.0f}, relT2[3] = {13.0f, -5.0f, -43.5f};
    const Frame aD = frame(30.0f, 100.0f, 200.0f, 50.0f), aN = frame(39.0f, 103.0f, 198.0f, 50.0f);
    float l2wD[16], l2wN[16], pred[16], rel[4][3];
    l2wOf(-90.0f, relT, aD, l2wD);
    l2wOf(-90.0f, relT2, aN, l2wN);
    L2WToActor(l2wD, aD, rel);
    const float dOrg[3] = {relT2[0] - relT[0], relT2[1] - relT[1], relT2[2] - relT[2]};
    std::memcpy(pred, l2wD, sizeof(pred));
    L2WFromActor(rel, aN, dOrg, pred);
    float worstL = 0.0f;
    for (int i = 0; i < 16; ++i) worstL = std::fmax(worstL, std::fabs(pred[i] - l2wN[i]));
    // 3) f4: ossa buone composte con l2wD e il riferimento delle mani fg, rimesse quando la mesh e' a l2wN (attore
    // girato di 9 gradi e spostato, Translation cambiata): con RestoreCorrection l'osso torna nel mondo dove stava
    // rispetto al riferimento delle mani. (a) riferimento fermo nella stanza (corpo girato sotto la testa, passo
    // della stanza): stessa posa nel mondo; (b) riferimento girato di 7 gradi e spostato (stick): posa portata con
    // lui. Anche con una scala uguale nelle due LocalToWorld (DrawScale).
    auto toWorld = [&](const float qq[4], const float tt[3], const float l[16], float pos[3], float axes[3][3]) {
        float bm[3][3];
        QuatToRows(qq, bm);
        for (int i = 0; i < 3; ++i) pos[i] = l[12 + i] + tt[0] * l[i] + tt[1] * l[4 + i] + tt[2] * l[8 + i];
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < 3; ++i) axes[r][i] = bm[r][0] * l[i] + bm[r][1] * l[4 + i] + bm[r][2] * l[8 + i];
    };
    auto unitAxes = [](float ax[3][3]) {
        for (int r = 0; r < 3; ++r) {
            const float n = std::sqrt(ax[r][0] * ax[r][0] + ax[r][1] * ax[r][1] + ax[r][2] * ax[r][2]);
            for (int i = 0; i < 3; ++i) ax[r][i] /= n;
        }
    };
    float worstB = 0.0f, qn = 1.0f, yawA = 0.0f;
    bool okRot = true;
    for (int sc = 0; sc < 2; ++sc) {
        float lD[16], lN[16];
        std::memcpy(lD, l2wD, sizeof(lD));
        std::memcpy(lN, l2wN, sizeof(lN));
        const float k = sc == 0 ? 1.0f : 1.15f;
        for (int i = 0; i < 12; ++i)
            if (i % 4 != 3) {
                lD[i] *= k;
                lN[i] *= k;
            }
        for (int cs = 0; cs < 2; ++cs) {
            float q[4] = {0.3f, -0.2f, 0.5f, 0.78f}, t[3] = {10.0f, 20.0f, 30.0f};
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (float& v : q) v /= n;
            const HandFrameW fg = {{120.0f, 230.0f, 110.0f}, 0.3f};
            const HandFrameW fn = cs == 0 ? fg : HandFrameW{{123.0f, 228.0f, 110.5f}, 0.3f + 7.0f * d2r};
            float p0[3], a0[3][3], p1[3], a1[3][3];
            toWorld(q, t, lD, p0, a0);
            Xf m;
            okRot = okRot && RestoreCorrection(lD, fg, fn, lN, &m);
            if (sc == 0 && cs == 0) yawA = XfAngle(m) / d2r;
            TransformBoneAtom(q, t, m);
            toWorld(q, t, lN, p1, a1);
            unitAxes(a0);
            unitAxes(a1);
            const float dy = fn.yaw - fg.yaw, c = std::cos(dy), s1 = std::sin(dy);
            auto rz = [&](const float v[3], float o[3]) {
                o[0] = v[0] * c - v[1] * s1;
                o[1] = v[0] * s1 + v[1] * c;
                o[2] = v[2];
            };
            float e[3];
            const float rp[3] = {p0[0] - fg.org[0], p0[1] - fg.org[1], p0[2] - fg.org[2]};
            rz(rp, e);
            for (int i = 0; i < 3; ++i) worstB = std::fmax(worstB, std::fabs(p1[i] - (fn.org[i] + e[i])));
            for (int r = 0; r < 3; ++r) {
                rz(a0[r], e);
                for (int i = 0; i < 3; ++i) worstB = std::fmax(worstB, std::fabs(a1[r][i] - e[i]) * 100.0f);
            }
            qn = std::fmin(qn, 1.0f - std::fabs(1.0f - std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])));
        }
    }
    std::printf("ue3 bone math: quat round trip %.2e, predicted LocalToWorld %.2e, restored bone back in the hand frame %.2e "
                "(axes x100; frame still / turned 7 deg and moved; scale 1 / 1.15), correction angle with the frame still %.2f deg "
                "(mesh turned 9), |q| %.6f\n",
                worstQ, worstL, worstB, yawA, qn);
    CHECK(worstQ < 1e-5f && worstL < 1e-3f && okRot && worstB < 2e-3f && std::fabs(qn - 1.0f) < 1e-5f &&
          std::fabs(yawA - 9.0f) < 0.01f);
}

// Spinta della testa: compagno che passa attraverso la testa (valori della regressione finale, ph_pass 30 unita'),
// muro che si avvicina, spigolo, rilascio.
static void TestHeadPushStep() {
    const float dt = 1.0f / 72.0f;
    float s[3] = {-1.98f, 2.31f, 0.30f};
    const float c[3] = {-1.74f, -4.62f, 1.48f};
    bool ch = false;
    // Prima: spinta piu' grande = subito.
    const float oldStep = std::sqrt((c[0] - s[0]) * (c[0] - s[0]) + (c[1] - s[1]) * (c[1] - s[1]) + (c[2] - s[2]) * (c[2] - s[2]));
    float maxStep = 0.0f, t = 0.0f;
    int sideTicks = 0;
    for (int i = 0; i < 72; ++i) {
        const float p[3] = {s[0], s[1], s[2]};
        if (HeadPushStep(s, c, dt, &ch) == 2) ++sideTicks;
        const float d = std::sqrt((s[0] - p[0]) * (s[0] - p[0]) + (s[1] - p[1]) * (s[1] - p[1]) + (s[2] - p[2]) * (s[2] - p[2]));
        maxStep = std::fmax(maxStep, d);
        const float r = std::sqrt((s[0] - c[0]) * (s[0] - c[0]) + (s[1] - c[1]) * (s[1] - c[1]) + (s[2] - c[2]) * (s[2] - c[2]));
        if (r > 0.1f) t = (i + 1) * dt;
    }
    std::printf("head push side flip (squadmate through the head): eye step %.2f units in one tick before, now max %.3f units per tick, "
                "within 0.1 units after %.2f s (%d limited ticks)\n",
                oldStep, maxStep, t, sideTicks);
    CHECK(oldStep > 6.9f && maxStep <= 40.0f * dt + 1e-4f && t < 0.5f && sideTicks > 0 && !ch);
    // Capsula che attraversa la linea (normale che ruota di 180 gradi in 6 tick, spinta 5 unita'): passo <= 0,56.
    float q[3] = {-1.0f, 5.0f, 0.0f};
    float qmax = 0.0f;
    for (int i = 0; i < 72; ++i) {
        const float a = 1.5707963f + 3.14159265f * std::fmin(1.0f, i / 6.0f);
        const float cc[3] = {-1.0f, 5.0f * std::sin(a), 0.0f};
        const float p[3] = {q[0], q[1], q[2]};
        HeadPushStep(q, cc, dt, &ch);
        qmax = std::fmax(qmax, std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2])));
    }
    std::printf("head push: normal turning 180 deg in 6 ticks, push 5 units: max eye step %.3f units per tick\n", qmax);
    // Review del follow-up: con l'occhio mai piu' di 6 unita' dietro la spinta nuova, la normale che si gira di 30
    // gradi per tick supera il limite: passo massimo ~1,6 unita' (0,556 senza quel limite; 10 di colpo prima del
    // follow-up). Il caso misurato in gioco (sopra) resta a 0,556.
    CHECK(qmax <= 2.0f);
    // Review del follow-up: residuo di un contatto lasciato (0,01 unita' da un lato), poi un muro dall'altro lato che
    // cresce a 70 unita'/s fino a 20, o che arriva di colpo a 10: prima l'occhio restava 9,5 / 9,4 unita' dietro la
    // spinta (oltre il margine di 8, attraverso la superficie); ora 0 (residuo = nessuna spinta), sia come Pawn sia come muro.
    for (int pawn = 0; pawn < 2; ++pawn) {
        for (int kind = 0; kind < 2; ++kind) {
            float rs[3] = {0.0f, 0.01f, 0.0f};
            bool rch = false;
            float worst = 0.0f;
            for (int i = 0; i < 144; ++i) {
                const float push = kind == 0 ? std::fmin(20.0f, 70.0f * (i + 1) * dt) : 10.0f;
                const float rc[3] = {-push, 0.0f, 0.0f};
                HeadPushStep(rs, rc, dt, &rch, HeadPushParams(), pawn != 0);
                worst = std::fmax(worst, std::fabs(rc[0] - rs[0]));
            }
            std::printf("head push: residual 0.01 units, then a %s on the other side (%s): eye at most %.2f units behind the push\n",
                        kind == 0 ? "push growing at 70 units/s" : "sudden 10-unit push", pawn ? "pawn" : "wall", worst);
            CHECK(worst < 0.01f);
        }
    }
    // Muro (non Pawn) dall'altro lato con una spinta vera applicata (3 unita'): subito, come prima del follow-up.
    float wa[3] = {0.0f, 3.0f, 0.0f};
    const float wb[3] = {-6.0f, 0.0f, 0.0f};
    bool wch = false;
    CHECK(HeadPushStep(wa, wb, dt, &wch, HeadPushParams(), false) == 0 && wa[0] == -6.0f && wa[1] == 0.0f && !wch);
    // Pawn con una spinta vera da un lato (3 unita') e una nuova di colpo di 10 dall'altro: dolce, ma l'occhio mai
    // piu' di 6 unita' (margine 8 - 2) dietro la spinta nuova.
    float pa[3] = {0.0f, 3.0f, 0.0f};
    bool pch = false;
    float pworst = 0.0f;
    int ptick = 0;
    for (int i = 0; i < 72; ++i) {
        const float pc[3] = {-10.0f, 0.0f, 0.0f};
        if (HeadPushStep(pa, pc, dt, &pch, HeadPushParams(), true) == 2) ++ptick;
        pworst = std::fmax(pworst, 10.0f + pa[0]);  // spinta nuova lungo -x: dietro = 10 - (-pa[0])
    }
    std::printf("head push: pawn, 3 units on one side then 10 at once on the other: eye at most %.2f units behind the push (limit 6), "
                "%d eased ticks\n",
                pworst, ptick);
    CHECK(pworst <= 6.0f + 1e-3f && ptick > 0);
    // Muro che si avvicina (stessa normale, spinta che cresce): subito, come prima.
    ch = false;
    float w[3] = {0.0f, 0.0f, 2.0f};
    const float w2[3] = {0.0f, 0.0f, 5.0f};
    CHECK(HeadPushStep(w, w2, dt, &ch) == 0 && w[2] == 5.0f);
    // Primo contatto: subito.
    float z[3] = {0.0f, 0.0f, 0.0f};
    const float z2[3] = {3.0f, 0.0f, 0.0f};
    CHECK(HeadPushStep(z, z2, dt, &ch) == 0 && z[0] == 3.0f);
    // Spigolo: arriva il secondo piano (spinta che cresce entro 60 gradi): subito.
    float k[3] = {2.0f, 0.0f, 0.0f};
    const float k2[3] = {2.0f, 3.0f, 0.0f};
    CHECK(HeadPushStep(k, k2, dt, &ch) == 0 && k[1] == 3.0f);
    // Rilascio: tau 0,1 s come prima (nessun limite di velocita').
    float r[3] = {0.0f, 8.0f, 0.0f};
    const float r2[3] = {0.0f, 0.0f, 0.0f};
    CHECK(HeadPushStep(r, r2, dt, &ch) == 1 && std::fabs(r[1] - 8.0f * std::exp(-dt / 0.1f)) < 1e-4f);
    // Tick lungo: subito.
    float l[3] = {0.0f, 3.0f, 0.0f};
    CHECK(HeadPushStep(l, c, 0.8f, &ch) == 0 && l[1] == c[1] && !ch);
}

// Blocco al limite della torretta: rinculo della M2E2 a 22 gradi dentro i limiti (righe TURRET della regressione
// finale) e limite vero del gioco con il tremolio della mano.
static void TestTurretLatchHysteresis() {
    const float u = 65536.0f / 360.0f;
    // (passo, ripreso dal gioco, errore) in gradi: le sequenze dei 3 blocchi della regressione finale.
    const float recoil[][3] = {{0.110f, 0.236f, 0.30f}, {0.159f, 0.181f, 0.43f}, {0.165f, 0.126f, 0.45f}, {0.000f, -0.137f, 0.41f},
                               {0.104f, 0.242f, 0.29f}, {0.159f, 0.187f, 0.43f}, {0.088f, 0.143f, 0.24f}, {0.110f, 0.110f, 0.30f},
                               {0.110f, 0.08f, 0.30f},  {-0.044f, -0.302f, -0.13f}, {-0.176f, -0.121f, -0.38f}};
    TurretLimitLatch lt;
    int on = 0;
    for (int rep = 0; rep < 20; ++rep)
        for (const auto& r : recoil) {
            const int ev = lt.Count(static_cast<int32_t>(r[0] * u), static_cast<int32_t>(r[1] * u), static_cast<int32_t>(r[2] * u));
            if (ev == 1 || ev == -1) ++on;
        }
    std::printf("turret latch: M2E2 recoil sequence x20 -> %d latches (final regression: 3 in 1.2 s)\n", on);
    CHECK(on == 0);
    // Limite vero a +40 nel gioco, mano 2 gradi oltre con tremolio +-0,3: un blocco, mai sciolto per 8 s.
    TurretLimitLatch l2;
    int32_t pc = static_cast<int32_t>(39.0f * u), step = 0, err0 = 0;
    const int32_t lim = static_cast<int32_t>(40.0f * u);
    int ons = 0, offs = 0, push = 0;
    bool pending = false;
    for (int i = 0; i < 72 * 8; ++i) {
        if (pending) {
            const int32_t refused = step > 0 && pc > lim ? pc - lim : 0;
            pc -= refused;
            const int ev = l2.Count(step, refused, err0);
            if (ev == 1 || ev == -1) ++ons;
            if (ev == -2) ++offs;
        }
        const float hand = 42.0f + 0.3f * std::sin(i * 0.7f);
        const int32_t err = static_cast<int32_t>(hand * u) - pc;
        bool rel = false;
        const int32_t dy = l2.Filter(TurretYawStep(err, 1.0f / 72.0f), err, i / 72.0, &rel);
        if (rel) ++offs;
        if (dy > 0 && pc >= lim) ++push;
        pc += dy;
        step = dy;
        err0 = err;
        pending = true;
    }
    std::printf("turret latch: game limit +40, hand +42 +-0.3: latch on %d, off %d, push ticks at the limit %d of %d (probes every 0.5 s)\n", ons,
                offs, push, 72 * 8);
    CHECK(ons == 1 && offs == 0 && push < 30);
    // Isteresi dello scioglimento: errore tornato di 0,11 gradi -> resta; di 0,3 -> libero (il passo indietro passa sempre).
    TurretLimitLatch l3;
    for (int i = 0; i < 3; ++i) l3.Count(300, 290, 900);
    CHECK(l3.dir == 1 && l3.Filter(-8, -20, 1.0) == -8 && l3.dir == 1);
    bool rel3 = false;
    CHECK(l3.Filter(-20, -55, 1.1, &rel3) == -20 && rel3 && l3.dir == 0);
    // Review del follow-up: ritorno lento (mano da +42 a +30 a 3-15 gradi/s, ferma 1 s, poi a destra a 2-10 gradi/s
    // fino a +38, dentro il limite del gioco a +40): il blocco si scioglie sul ritorno e la torretta segue la mano
    // (prima, col solo kReleaseErr: 2,5-5 gradi dietro).
    float worstLag = 0.0f;
    int stuck = 0;
    for (float rs : {3.0f, 6.0f, 15.0f}) {
        for (float ps : {2.0f, 5.0f, 10.0f}) {
            TurretLimitLatch l4;
            int32_t pc4 = static_cast<int32_t>(39.0f * u), step4 = 0, err4 = 0;
            bool pend4 = false;
            float hand = 42.0f;
            int phase = 0;
            double tRet = 0.0;
            float lag = 0.0f;
            for (int i = 0; i < 72 * 12; ++i) {
                const double t = i / 72.0;
                if (t < 2.0) {
                    hand = 42.0f;
                } else if (phase <= 1 && hand > 30.0f) {
                    phase = 1;
                    hand = std::fmax(30.0f, hand - rs / 72.0f);
                    tRet = t;
                } else if (phase <= 2 && t < tRet + 1.0) {
                    phase = 2;
                } else {
                    phase = 3;
                    hand = std::fmin(38.0f, hand + ps / 72.0f);
                }
                if (pend4) {
                    const int32_t refused = step4 > 0 && pc4 > lim ? pc4 - lim : 0;
                    pc4 -= refused;
                    l4.Count(step4, refused, err4);
                }
                const int32_t err = static_cast<int32_t>(hand * u) - pc4;
                const int32_t dy = l4.Filter(TurretYawStep(err, 1.0f / 72.0f), err, t);
                pc4 += dy;
                step4 = dy;
                err4 = err;
                pend4 = true;
                if (phase == 3 && hand < 38.0f) lag = std::fmax(lag, (hand * u - pc4) / u);
            }
            worstLag = std::fmax(worstLag, lag);
            if (l4.dir != 0) ++stuck;
        }
    }
    std::printf("turret latch: slow return then a push inside the limit (9 speed pairs): turret at most %.2f deg behind the hand, "
                "%d runs still latched at the end\n",
                worstLag, stuck);
    CHECK(worstLag < 0.5f && stuck == 0);
}

static void TestMenuStepRanges() {
    CHECK(StepConfigNumber("necksize", "1", +1) == "1.00");
    CHECK(StepConfigNumber("necksize", "0.9", +1) == "1.00");
    CHECK(StepConfigNumber("necksize", "0", -1) == "0.00");
    CHECK(StepConfigNumber("necksize", "1.10", +1) == "1.00");  // ini gia' oltre (scritto dal menu di prima)
    CHECK(StepConfigNumber("necksize", "1", -1) == "0.90");
    CHECK(StepConfigNumber("bodyalign", "0.4", +1) == "0.5" && StepConfigNumber("bodyalign", "0", -1) == "0.0");
    CHECK(StepConfigNumber("bodyalignspeed", "40", +1) == "45" && StepConfigNumber("bodyalignspeed", "90", +1) == "90");
    CHECK(StepConfigNumber("turnwithhead", "0", -1) == "0" && StepConfigNumber("neckring", "10", +1) == "10.0");
    CHECK(StepConfigNumber("seatedstand", "0.80 3", +1) == "0.80 3");
    CHECK(StepConfigNumber("gunangle", "15", +1) == "16.0");  // dentro i limiti del comando (-45..45)
    // Review del follow-up: ogni chiave col limite del suo comando (control.cpp).
    CHECK(StepConfigNumber("vignette", "1.0", +1) == "1.0" && StepConfigNumber("vignette", "0", -1) == "0.0");
    CHECK(StepConfigNumber("eyesmooth", "1.00", +1) == "1.00" && StepConfigNumber("armstretch", "1.6", +1) == "1.60");
    CHECK(StepConfigNumber("armstretch", "1.0", -1) == "1.00" && StepConfigNumber("outputscale", "0.25", -1) == "0.25");
    CHECK(StepConfigNumber("turnspeed", "0", -1) == "0" && StepConfigNumber("turnspeed", "720", +1) == "720");
    CHECK(StepConfigNumber("snapturn", "0", -1) == "0" && StepConfigNumber("snapturn", "30", +1) == "45");
    CHECK(StepConfigNumber("gunangle", "45", +1) == "45.0" && StepConfigNumber("twohanddwell", "1.00", +1) == "1.00");
    CHECK(StepConfigNumber("predictmax", "250", +1) == "250" && StepConfigNumber("turretgroundup", "89", +1) == "89");
}

int main() {
    TestCorpseDrag();
    TestMeleeHit();
    TestDisarm();
    TestShield();
    TestPropNoise();
    TestGrenadeBack();
    TestThrowHit();
    TestWorldHit();
    TestAnalogGrip();
    TestSolidShape();
    TestPeakThrow();
    TestHapticMix();
    TestImpactPulse();
    TestImpactEdge();
    TestImpactEdgeRates();
    TestTwoHandPalmTarget();
    TestTwoHandOnGuard();
    TestTwoHandFollowsLeft();
    TestTwoHandWristRotations();
    TestTwoHandRollKept();
    TestTwoHandHysteresis();
    TestTwoHandContinuity();
    TestTwoHandFilter();
    TestTwoHandGunstock();
    TestTwoHandVetoes();
    TestTwoHandTrackingLoss();
    TestTwoHandPistol();
    TestTwoHandKeys();
    TestTwoHandDeparture();
    TestTwoHandRegrab();
    TestTwoHandRecenter();
    TestTwoHandCant();
    TestTwoHandLiveLeft();
    TestTwoHandNear();
    TestTwoHandRollAlong();
    TestTwoHandSlide();
    TestTwoHandJitter();
    TestTwoHandRegrabAlong();
    TestTwoHandSteerLag();
    TestTwoHandNewReleases();
    TestTwoHandLeftoverFlick();
    TestTwoHandMigration();
    TestNeckAnchor();
    TestSeatedDetector();
    TestCrouchView();
    TestSeatedReturn();
    TestBoneAtomRotate();
    TestWi3Migration();
    TestMergedConfigVersions();
    TestConfigReset();
    TestStableEyeHeight();
    TestWallPullStep();
    TestLowProbeLatch();
    TestClampIntoPlane();
    TestSpaceShift();
    TestHeadJumpDetector();
    TestRecenterHands();
    TestViewHeightRecapture();
    TestClampIntoPlanes();
    TestModeBlend();
    TestEulerRoundTrip();
    TestBlendedYawBaseline();
    TestHeadViewUnits();
    TestViewJump();
    TestTurretMath();
    TestHandlebar();
    TestHandlebarGap();
    TestTurretHandles();
    TestTurretLimitBuzz();
    TestUiTextFits();
    TestHintSequence();
    TestObjHints();
    TestGripStripLatch();
    TestTurretYawHold();
    TestTurretClamp();
    TestTurretLimitsModel();
    TestTurretBookkeeping();
    TestDynRes();
    TestPredictFrameAuto();
    TestRelativeRotationVector();
    TestPalmOf();
    TestAimInRawModel();
    TestGunFrameLevelAim();
    TestGunPivot();
    TestSightLineCalibration();
    TestAimpitchMigration();
    TestNextNumericChoice();
    TestPointingHeadingNearVertical();
    TestAimRayMeasure();
    TestQuatFromPose();
    TestWrapPi();
    TestOneEuro();
    TestComputedMigration();
    TestMenuSteps();
    TestAdvancedWordChoices();
    TestLockWaitStats();
    TestParseVersion();
    TestJson();
    TestRevoked();
    TestCompanions();
    TestDll32();
    TestNormalize();
    TestMigrate();
    TestFitRecommendedSize();
    TestDefaultLinesMissing();
    TestBodyAlign();
    TestHeadPushStep();
    TestUe3BoneMath();
    TestTurretLatchHysteresis();
    TestMenuStepRanges();
    std::printf("unit tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
