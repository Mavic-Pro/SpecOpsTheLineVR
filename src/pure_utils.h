#pragma once
// Funzioni pure (niente Windows, niente gioco, niente log): versioni delle
// release, manifesto updates.json, JSON minimale di GitHub, controllo della
// dll a 32 bit, normalizzazione delle righe dell'ini e migrazione dei
// predefiniti; dal 25/10 anche angoli delle pose (ExactEuler), palmo dei
// Touch (PalmOf), filtro 1-euro e passi del menu. Usate da updater.cpp,
// control.cpp, hooks.cpp, vr_openxr.cpp e vr_menu.cpp e coperte da
// tests/unit_tests.cpp (R-T2).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vr::pure {

// "v1.2.3" / "1.2.3-beta1" -> {1, 2, 3, pre ? 0 : 1}: la pre-release vale meno
// della finale con gli stessi numeri (chi aveva la beta non passava mai alla
// 0.9.9 vera, audit 20/10).
inline std::vector<int> ParseVersion(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    std::vector<int> out;
    size_t i = 0;
    bool pre = false;
    while (i < v.size() && out.size() < 3) {
        size_t j = i;
        while (j < v.size() && isdigit(static_cast<unsigned char>(v[j]))) ++j;
        if (j == i) break;
        out.push_back(atoi(v.substr(i, j - i).c_str()));
        if (j < v.size() && v[j] == '-') pre = true;
        if (j >= v.size() || v[j] != '.') break;
        i = j + 1;
    }
    while (out.size() < 3) out.push_back(0);
    out.push_back(pre ? 0 : 1);
    return out;
}

// Valore stringa di una chiave JSON a partire da "from" (niente parser: la
// risposta di GitHub ha forma fissa). at = indice della virgoletta finale.
inline std::string JsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* at = nullptr) {
    std::string k = "\"" + key + "\"";
    size_t p = json.find(k, from);
    if (p == std::string::npos) return {};
    size_t colon = json.find(':', p + k.size());
    if (colon == std::string::npos) return {};
    p = json.find('"', colon + 1);
    if (p == std::string::npos) return {};
    size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return {};
    if (at) *at = e;
    return json.substr(p + 1, e - p - 1);
}

// Numero intero di una chiave JSON nell'intervallo [from, to) (l'ultima occorrenza prima di "to").
inline long long JsonNumberBefore(const std::string& json, const std::string& key, size_t from, size_t to) {
    std::string k = "\"" + key + "\":";
    size_t p = json.rfind(k, to);
    if (p == std::string::npos || p < from) return -1;
    return atoll(json.c_str() + p + k.size());
}

// updates.json: {"revoked": ["0.9.1"], "recommended": "v0.9.2"}.
inline bool IsRevoked(const std::string& manifest, const std::string& version) {
    size_t r = manifest.find("\"revoked\"");
    if (r == std::string::npos) return false;
    size_t open = manifest.find('[', r), close = manifest.find(']', r);
    if (open == std::string::npos || close == std::string::npos) return false;
    std::string list = manifest.substr(open, close - open);
    return list.find("\"" + version + "\"") != std::string::npos ||
           list.find("\"v" + version + "\"") != std::string::npos;
}

// companions.json di una release: {"files": [{"name": "...", "size": N,
// "sha256": "..."}]}. Esclude i nomi con percorsi, specopsvr.ini e d3d9.dll
// e le voci senza dimensione o impronta.
struct Companion {
    std::string name;
    long long size;
    std::string sha256;
};
inline std::vector<Companion> ParseCompanions(const std::string& manifest) {
    std::vector<Companion> out;
    size_t files = manifest.find("\"files\"");
    if (files == std::string::npos) return out;
    size_t p = manifest.find('[', files);
    if (p == std::string::npos) return out;
    for (;;) {
        // Un oggetto {...} per file, campi in qualsiasi ordine.
        size_t open = manifest.find('{', p), close = manifest.find('}', open);
        if (open == std::string::npos || close == std::string::npos) break;
        std::string obj = manifest.substr(open, close - open + 1);
        p = close + 1;
        std::string name = JsonString(obj, "name");
        long long size = JsonNumberBefore(obj, "size", 0, obj.size());
        std::string sha = JsonString(obj, "sha256");
        if (name.empty()) continue;
        bool safe = name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
                    name.find("..") == std::string::npos;
        std::string lower;
        for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!safe || lower == "specopsvr.ini" || lower == "d3d9.dll" || sha.empty() || size <= 0) continue;
        out.push_back({name, size, sha});
    }
    return out;
}

// PE a 32 bit (IMAGE_FILE_MACHINE_I386).
inline bool IsDll32(const std::string& data) {
    if (data.size() < 0x200 || data[0] != 'M' || data[1] != 'Z') return false;
    uint32_t pe = *reinterpret_cast<const uint32_t*>(data.data() + 0x3C);
    if (pe + 6 > data.size() || data.compare(pe, 4, std::string("PE\0\0", 4)) != 0) return false;
    uint16_t machine = *reinterpret_cast<const uint16_t*>(data.data() + pe + 4);
    return machine == 0x014C;
}

// Riga dell'ini / del file dei comandi normalizzata: tabulazioni -> spazi,
// commento "#" tolto, minuscole, virgola decimale -> punto, true/yes -> on,
// false/no -> off. I comandi "verbatim" (nomi di oggetti, percorsi) restano
// come sono.
inline std::string NormalizeConfigLine(const std::string& raw) {
    std::string line = raw;
    for (char& c : line)
        if (c == '\t') c = ' ';
    std::istringstream head(line);
    std::string cmd;
    head >> cmd;
    static const char* kVerbatim[] = {"ucall", "ce", "console", "giveweapon", "gunoffsetclass", "updaterepo", "shot",
                                      "mirrorshot", "gesturesim", "armcontrols", "pad", "toast"};
    bool verbatim = false;
    for (const char* v : kVerbatim)
        if (cmd == v) verbatim = true;
    if (verbatim || cmd.empty() || cmd[0] == '#') return line;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    for (size_t i = 0; i < line.size(); ++i) {
        line[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
        if (line[i] == ',' && i > 0 && i + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[i - 1])) &&
            std::isdigit(static_cast<unsigned char>(line[i + 1])))
            line[i] = '.';
    }
    std::istringstream words(line);
    std::string w, out;
    while (words >> w) {
        if (w == "true" || w == "yes") w = "on";
        if (w == "false" || w == "no") w = "off";
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

// Chiave di una riga dell'ini: il comando, piu' il sottocomando per quelli
// che ne hanno ("gesture grenade on", "dynres min 65").
inline std::string ConfigKey(const std::string& line) {
    std::istringstream in(line);
    std::string cmd, sub;
    in >> cmd >> sub;
    if (cmd == "gesture" || (cmd == "dynres" && (sub == "min" || sub == "budget"))) return cmd + " " + sub;
    return cmd;
}

// Migrazione dei predefiniti cambiati tra versioni dell'ini: ogni cambio vale
// solo per gli ini piu' vecchi della sua versione. Ritorna false se l'ini era
// gia' aggiornato (niente da scrivere); "done" elenca i cambi applicati.
struct ConfigChange {
    int version;
    const char* from;
    const char* to;
};
// Versione dei cambi dei fix del tester (copia B, WI-2..WI-4): nella copia B
// erano la 10, come quelli della copia A (solidhands) ma con cambi diversi; al
// merge (27/10) sono diventati la 11, cosi' un ini gia' alla 10 li riceve.
constexpr int kTesterFixesConfigVersion = 11;
// 0.10.0 (27/10, richiesta dell'utente: "resettando da capo l'ini dell'utente
// riportando alle impostazioni di default"): un ini di una versione precedente
// si mette da parte (specopsvr.ini.bak-<data>, come reset_settings.bat) e si
// riparte dall'ini distribuito. Dalla 0.9.18 cambiano decine di opzioni e di
// predefiniti (fisica, oggetti, fix del tester): migrarli riga per riga lasciava
// combinazioni mai provate.
constexpr int kResetConfigVersion = 12;
inline int ConfigVersionOf(const std::vector<std::string>& lines) {
    int version = 1;
    for (const auto& l : lines)
        if (l.rfind("configversion ", 0) == 0) version = std::atoi(l.c_str() + 14);
    return version;
}
// Ini da rimettere ai predefiniti (vedi kResetConfigVersion): anche uno senza
// "configversion" (versione 1). Un ini vuoto (nessuna riga) no: niente da salvare.
inline bool NeedsConfigReset(const std::vector<std::string>& lines) {
    return !lines.empty() && ConfigVersionOf(lines) < kResetConfigVersion;
}
// Toglie le righe delle chiavi ritirate e i commenti contigui sopra di esse
// (non le intestazioni "# ----"). Ritorna quante chiavi ha tolto.
inline int RemoveRetiredKeys(std::vector<std::string>& lines, const char* const* keys, size_t keyCount) {
    std::vector<std::string> out;
    int removed = 0;
    for (const auto& l : lines) {
        std::string key = ConfigKey(l);
        size_t sp = key.find(' ');
        if (sp != std::string::npos) key = key.substr(0, sp);
        bool retired = false;
        if (!l.empty() && l[0] != '#')
            for (size_t i = 0; i < keyCount && !retired; ++i) retired = key == keys[i];
        if (!retired) {
            out.push_back(l);
            continue;
        }
        ++removed;
        while (!out.empty() && !out.back().empty() && out.back()[0] == '#' && out.back().rfind("# ----", 0) != 0) out.pop_back();
    }
    if (removed) lines.swap(out);
    return removed;
}
// Cambio calcolato (25/10): il nuovo valore dipende da quello vecchio (es.
// "aimpitch -24" -> "holdmode gunstock" + "stockangle 15.4"). fn riceve la
// riga (normalizzata come nell'ini) e, se va cambiata, mette in *out le righe
// che la sostituiscono (anche piu' di una, o nessuna per toglierla) e ritorna
// true. Vale come i cambi fissi: solo per gli ini piu' vecchi di "version" e
// solo per le righe della chiave "key" (ConfigKey).
struct ComputedChange {
    int version;
    const char* key;
    bool (*fn)(const std::string& line, std::vector<std::string>* out);
};

// Numero per una riga dell'ini: al piu' "decimals" cifre dopo la virgola,
// senza zeri finali ("15.40" -> "15.4", "-0.0" -> "0").
inline std::string FormatConfigNumber(double v, int decimals) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", decimals < 0 ? 0 : (decimals > 6 ? 6 : decimals), v);
    std::string s(b);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

inline bool MigrateConfigLines(std::vector<std::string>& lines, int targetVersion, const ConfigChange* changes,
                               size_t changeCount, std::string* done, const ComputedChange* computed = nullptr,
                               size_t computedCount = 0) {
    int version = 1;
    for (const auto& l : lines)
        if (l.rfind("configversion ", 0) == 0) version = std::atoi(l.c_str() + 14);
    if (version >= targetVersion) return false;
    if (done) done->clear();
    for (auto& l : lines)
        for (size_t i = 0; i < changeCount; ++i)
            if (version < changes[i].version && l == changes[i].from) {
                l = changes[i].to;
                if (done) *done += std::string(" \"") + changes[i].from + "\" -> \"" + changes[i].to + "\";";
            }
    if (computed && computedCount) {
        std::vector<std::string> out;
        out.reserve(lines.size());
        for (const auto& l : lines) {
            bool replaced = false;
            if (!l.empty() && l[0] != '#') {
                const std::string key = ConfigKey(NormalizeConfigLine(l));  // "AimPitch -24  # nota" (WI-4a)
                for (size_t i = 0; i < computedCount && !replaced; ++i) {
                    std::vector<std::string> repl;
                    if (version < computed[i].version && key == computed[i].key && computed[i].fn && computed[i].fn(l, &repl)) {
                        replaced = true;
                        std::string to;
                        for (const auto& r : repl) to += (to.empty() ? "" : " + ") + r;
                        if (done) *done += " \"" + l + "\" -> \"" + (to.empty() ? std::string("(removed)") : to) + "\";";
                        out.insert(out.end(), repl.begin(), repl.end());
                    }
                }
            }
            if (!replaced) out.push_back(l);
        }
        lines.swap(out);
    }
    bool hasVersion = false;
    for (auto& l : lines)
        if (l.rfind("configversion", 0) == 0) {
            l = "configversion " + std::to_string(targetVersion);
            hasVersion = true;
        }
    if (!hasVersion) {
        lines.push_back("# Settings format version (updated automatically).");
        lines.push_back("configversion " + std::to_string(targetVersion));
    }
    return true;
}

// Righe dei predefiniti distribuiti (testo dell'ini incorporato nella dll) le
// cui chiavi (ConfigKey) mancano dall'ini dell'utente, ognuna con i commenti
// subito sopra e preceduta da una riga vuota: il menu VR le aggiunge in
// Advanced dopo quelle dell'utente. install.bat e l'aggiornamento conservano
// l'ini vecchio, e un'opzione nuova (masskg, 24/10) valeva gia'
// (ApplyDefaultsMissingFrom) ma nel menu non compariva. configversion no.
inline std::vector<std::string> DefaultLinesMissingFrom(const std::vector<std::string>& user, const std::string& defaults) {
    std::vector<std::string> have, out, comments;
    for (const auto& l : user) {
        size_t a = l.find_first_not_of(" \t\r");
        if (a != std::string::npos && l[a] != '#') have.push_back(ConfigKey(l));
    }
    std::istringstream in(defaults);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos) {
            comments.clear();
            continue;
        }
        if (l[a] == '#') {
            comments.push_back(l);
            continue;
        }
        const std::string k = ConfigKey(l);
        bool found = k == "configversion";
        for (const auto& h : have) found = found || h == k;
        if (!found) {
            out.push_back("");
            out.insert(out.end(), comments.begin(), comments.end());
            out.push_back(l);
        }
        comments.clear();  // i commenti valgono solo per la riga subito sotto (come nel menu)
    }
    return out;
}

// Dimensione consigliata da SteamVR per un occhio -> back buffer: ridotta in
// proporzione finche' sta in maxSide (non lato per lato), multipla di 8. Prima
// ogni lato era limitato da solo a 4096: con il cursore della risoluzione di
// SteamVR alto (Quest 3 a 120 Hz, tester 22/10) diventava 4096x4096 invece di
// ~0,92:1, e la finestra con i bordi non ci stava (4067x4067).
inline void FitRecommendedSize(uint32_t& w, uint32_t& h, uint32_t maxSide) {
    if (!w || !h) return;
    uint32_t big = w > h ? w : h;
    if (big > maxSide) {
        w = static_cast<uint32_t>(static_cast<uint64_t>(w) * maxSide / big);
        h = static_cast<uint32_t>(static_cast<uint64_t>(h) * maxSide / big);
    }
    w &= ~7u;
    h &= ~7u;
}

// --- Vibrazione degli urti (voce 3 del piano Boneworks, 25/10) --------------
// Impulso per una mano: ampiezza 0..1, durata in ms, frequenza in Hz.
struct HapticPulseSpec {
    float amp = 0.0f;
    unsigned ms = 0;
    float freq = 0.0f;
};

// Impulso d'urto da J (N s, impulso sentito dalla mano) e dalla massa (kg).
// Formula del piano [senza fonte]: ampiezza 0,2 + J / jFull (5 N s: oltre
// ~4 N s e' piena), al piu' 1; durata 12 + 8 x min(J, 4) ms (12-44); 90 Hz
// sopra i 5 kg (tonfo), 225 Hz fino a 5 (tic). gain: 1 = full, 0,5 = low.
// J lo decide chi chiama (revisione 26/10: con kg x dv ogni oggetto da 5 kg
// in su era gia' pieno alla soglia, radio e schiaffi sempre 1,00): oggetto
// tenuto, colpo e oggetto urtato con la massa ridotta oggetto-mano
// (ReducedMass), schiaffo con la massa della mano che colpisce (hapslapkg).
inline HapticPulseSpec ImpactPulse(float J, float kg, float gain, float jFull = 5.0f) {
    if (!(J > 0.0f)) J = 0.0f;
    float a = 0.2f + J / (jFull > 0.1f ? jFull : 0.1f);
    a = a > 1.0f ? 1.0f : a;
    HapticPulseSpec s;
    s.amp = a * (gain > 0.0f ? gain : 0.0f);
    s.ms = static_cast<unsigned>(12.0f + 8.0f * (J < 4.0f ? J : 4.0f) + 0.5f);
    s.freq = kg > 5.0f ? 90.0f : 225.0f;
    return s;
}

// Massa ridotta di due corpi (kg): l'impulso di un urto anelastico tra la mano
// (handKg, la sfera: 5 kg) e un oggetto a velocita' relativa v e' mu x v.
inline float ReducedMass(float kg, float handKg) {
    if (!(kg > 0.0f) || !(handKg > 0.0f)) return 0.0f;
    return kg * handKg / (kg + handKg);
}

// Impulso della presa (prima: 50 ms a 0,9 per tutto): conferma che cresce con
// la massa e, presa al volo (velocita' relativa mano-oggetto oltre 0,5 m/s,
// guanti compresi), almeno l'impulso d'urto di mu x v (mano da handKg).
// Revisione (26/10): la conferma era 0,4 + 0,5 x min(1, kg / 30) per il gain,
// quindi una bottiglia 0,41 (0,21 con low) contro lo 0,9 di prima: poco su
// motori deboli o sull'API legacy. Ora 0,6 + 0,3 x min(1, kg / 30) (0,6-0,9,
// 30-50 ms) e non dimezzata da low (e' una conferma, come prima); il gain
// vale solo per la parte d'urto della presa al volo.
inline HapticPulseSpec GrabPulse(float kg, float relSpeedMs, float gain, float jFull = 5.0f, float handKg = 5.0f) {
    const float f = kg > 0.0f ? (kg < 30.0f ? kg / 30.0f : 1.0f) : 0.0f;
    HapticPulseSpec s;
    s.amp = 0.6f + 0.3f * f;
    s.ms = static_cast<unsigned>(30.0f + 20.0f * f + 0.5f);
    s.freq = kg > 5.0f ? 90.0f : 225.0f;
    if (relSpeedMs > 0.5f) {
        HapticPulseSpec i = ImpactPulse(ReducedMass(kg, handKg) * relSpeedMs, kg, gain, jFull);
        if (i.amp > s.amp) s.amp = i.amp;
        if (i.ms > s.ms) s.ms = i.ms;
    }
    return s;
}

// Uscita di un frame per una mano (UpdateHaptics, chiamata a ogni lettura del
// pad). Parte di prima, identica: motore del gioco solo sulla mano dell'arma
// (600 + motore / 20 us, 150 Hz, 20 ms), un impulso "legacy" della mod
// (PulseHand senza ampiezza) la sostituisce con 3500 us. Poi l'impulso d'urto
// in corso (remainSec > 0) se non e' piu' debole: la sua ampiezza, la sua
// frequenza (0 = 150 Hz) e la durata che gli resta (al piu' 20 ms: ripetuto a
// ogni frame finisce quando deve). us = ampiezza x 3900 per l'API legacy.
struct HapticFrameOut {
    bool on = false;
    bool impact = false;  // l'impulso d'urto ha vinto
    float amp = 0.0f, freq = 0.0f, seconds = 0.0f;
    unsigned short us = 0;
};
inline HapticFrameOut HapticMix(unsigned gameMotor, bool gunHand, bool legacyPulse, double remainSec, float impactAmp, float impactFreq) {
    HapticFrameOut o;
    unsigned us = 0;
    if (gameMotor && gunHand) us = 600 + gameMotor / 20;
    if (legacyPulse) us = 3500;
    if (us) {
        o.on = true;
        o.us = static_cast<unsigned short>(us > 3900 ? 3900 : us);
        o.amp = us > 3900 ? 1.0f : us / 3900.0f;
        o.freq = 150.0f;
        o.seconds = 0.02f;
    }
    if (remainSec > 0.0) {
        const float a = impactAmp < 0.0f ? 0.0f : (impactAmp > 1.0f ? 1.0f : impactAmp);
        if (a > 0.0f && a >= o.amp) {
            o.on = true;
            o.impact = true;
            o.amp = a;
            o.us = static_cast<unsigned short>(a * 3900.0f + 0.5f);
            o.freq = impactFreq > 0.0f ? impactFreq : 150.0f;
            o.seconds = remainSec < 0.02 ? static_cast<float>(remainSec) : 0.02f;
        }
    }
    return o;
}

// Contatto (un impulso per contatto). dvv = velocita' tolta dal contatto in
// questo tick (m/s, vettore: comando del tick prima - gravita' di un passo -
// velocita' misurata), vel = velocita' misurata ora (m/s, vettore). Armato, un
// contatto comincia di colpo: al primo tick con |dvv| oltre meta' soglia e
// oltre il doppio del tick prima (il fronte di salita del piano: trascinata
// sulla sabbia o su un'altra radio, l'attrito toglie 0,18-0,48 m/s per tick in
// crescita lenta mentre la mano accelera, e sommato sembrava un urto); per
// window s si somma dvv: uno spigolo che tocca la sabbia in pendenza e fa
// ruotare la radio le toglie 1,5 m/s in 8 tick da 0,4-0,75 (il confronto per
// tick del piano, "J oltre il doppio del tick prima e dv oltre 0,8", non la
// vedeva: 1 contatto su 3). La velocita' tolta conta al piu' quanto l'oggetto
// si avvicinava all'ostacolo prima del contatto: la minima, nei 3 tick prima,
// della velocita' lungo la direzione tolta. Premuto contro il tavolo da fermo
// il comando che spinge dentro non e' un urto; e con objectweight off la
// guida di prima fa saltare su e giu' l'oggetto bloccato (6 cm, comando fino a
// 9 m/s): l'ultimo tick in giu' a 6,8 m/s dopo tre in su non e' una mano che
// colpisce. Scatta al primo superamento di dvMin (lost = velocita' tolta,
// m/s); poi si riarma solo a contatto finito (|dvv| sotto meta' soglia per
// rearm s): premuto contro un muro, o vibrando contro un ostacolo, non ripete.
// quiet (finestre mute): il contatto consuma lo scatto senza impulso (muted).
//
// Revisione (26/10): le soglie per tick erano tarate solo a 144 Hz.
//  - Sopra 144 Hz una perdita continua (spigolo che fa ruotare, premere)
//    toglie a x dt per tick: a 225 Hz lo spigolo sulla sabbia scendeva sotto
//    la soglia d'inizio. Inizio e riarmo guardano dvN = dv x max(1, dtRef /
//    dt) (dtRef = 1/144 s: la stessa decelerazione da' lo stesso numero).
//  - Sotto 144 Hz niente riduzione: un urto secco toglie la stessa velocita'
//    in un tick a ogni frequenza. Riportata a 1/144 s (proposta della
//    revisione) la perdita dei contatti veri col terreno a 45 Hz (0,78-0,93
//    m/s per tick da 22 ms) scendeva a 0,25-0,29: rigiocando le righe
//    GRABLOG (p3h/replay.py) 3 contatti su 5 persi; senza, 5 su 5 e nessun
//    impulso trascinando la radio a 45 Hz (p3h/drag.py).
//  - Inizio anche senza il raddoppio se nei tick degli ultimi calm s (17 ms,
//    almeno 2 tick) la perdita era sotto un quarto di soglia: un contatto
//    morbido (0,36 poi 0,59 m/s per tick da calmo, la radio a 120 Hz in
//    B_drag144) non raddoppiava mai e non vibrava; l'attrito che cresce
//    trascinando (0,18-0,48) non e' mai calmo prima.
//  - La storia dell'avvicinamento e' a tempo (approach s, almeno 2 tick:
//    25 ms = i 3 tick di prima a 144 Hz, 2 a 90 e a 45, 5 a 225), non 3 tick
//    (67 ms a 45 Hz). Con un tick solo a 45 Hz l'oggetto rimbalzato in giu'
//    per un tick dopo essere salito contava come avvicinamento.
struct ImpactEdgeTune {
    float dvMin = 0.8f;          // m/s: velocita' tolta oltre cui c'e' un urto (hapdv)
    double rearm = 0.04;         // s: contatto finito (dvN sotto meta' soglia) per tanto prima del prossimo (haprearm)
    double window = 0.05;        // s: il contatto somma la velocita' tolta per tanto (hapwindow)
    double approach = 0.025;     // s: velocita' d'avvicinamento dai tick di tanto prima, almeno 2 (hapapproach)
    double calm = 0.017;         // s: calmo prima = perdita sotto un quarto di soglia nei tick di tanto prima (almeno 2)
    double dtRef = 1.0 / 144.0;  // s: tick a cui le soglie per tick valgono come scritte (sopra 144 Hz la perdita e' riportata qui)
};
struct ImpactEdge {
    static constexpr int kHist = 16;
    bool armed = true;
    double lowSince = -1.0;
    double start = -1.0;  // contatto in corso dal (somma), -1 = nessuno
    float sum[3] = {0.0f, 0.0f, 0.0f};
    float hist[kHist][3] = {};   // velocita' misurate dei tick prima (m/s), hist[0] la piu' recente
    double histT[kHist] = {};    // e quando
    float histDvN[kHist] = {};   // e la loro perdita riportata (dvN)
    int histN = 0;
    float pre[kHist][3] = {};    // quelle entro approach s all'inizio del contatto
    int preN = 0;
    float prevDv = 0.0f;     // |dvv| del tick prima (m/s)
    float prevDvN = 0.0f;    // lo stesso riportato al tick di riferimento
    bool fired = false;
    bool Step(const float dvv[3], const float vel[3], double now, double dt, const ImpactEdgeTune& tn, bool quiet, bool* muted = nullptr,
              float* lost = nullptr) {
        if (muted) *muted = false;
        const float dv = std::sqrt(dvv[0] * dvv[0] + dvv[1] * dvv[1] + dvv[2] * dvv[2]);
        const double dtc = dt < 0.001 ? 0.001 : (dt > 0.1 ? 0.1 : dt);
        const float dvN = dtc < tn.dtRef ? dv * static_cast<float>(tn.dtRef / dtc) : dv;
        const float dvMin = tn.dvMin;
        const float lo = 0.5f * dvMin;
        bool fire = false;
        float before = 0.0f;  // la perdita piu' bassa nei tick degli ultimi calm s (0 senza storia)
        for (int k = 0; k < histN; ++k) {
            if (k >= 2 && now - histT[k] > tn.calm + 1e-4) break;  // almeno i 2 tick prima
            before = k == 0 || histDvN[k] < before ? histDvN[k] : before;
        }
        if (start < 0.0 && armed && dvN > lo && (dvN > 2.0f * prevDvN || before < 0.25f * lo)) {
            start = now;
            sum[0] = sum[1] = sum[2] = 0.0f;
            preN = 0;
            for (int k = 0; k < histN; ++k) {
                if (k >= 2 && now - histT[k] > tn.approach + 1e-4) break;  // almeno i 2 tick prima
                for (int c = 0; c < 3; ++c) pre[preN][c] = hist[k][c];
                ++preN;
            }
            fired = false;
        }
        if (start >= 0.0) {
            for (int c = 0; c < 3; ++c) sum[c] += dvv[c];
            const float s = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
            float approach = 0.0f;  // la minima lungo la direzione tolta (0 senza storia)
            if (s > 1e-6f && preN > 0) {
                approach = 1e30f;
                for (int k = 0; k < preN; ++k) {
                    const float a = (pre[k][0] * sum[0] + pre[k][1] * sum[1] + pre[k][2] * sum[2]) / s;
                    approach = a < approach ? a : approach;
                }
                approach = approach > 0.0f ? approach : 0.0f;
            }
            const float eff = s < approach ? s : approach;
            if (!fired && eff > dvMin) {
                fired = true;
                armed = false;
                fire = !quiet;
                if (muted) *muted = quiet;
                if (lost) *lost = eff;
            }
            if (now - start >= tn.window) {
                start = -1.0;
                armed = false;
                lowSince = -1.0;
            }
        }
        if (start < 0.0) {
            if (dvN < lo) {
                if (lowSince < 0.0) lowSince = now;
                if (!armed && now - lowSince >= tn.rearm) armed = true;
            } else {
                lowSince = -1.0;
            }
        }
        for (int k = kHist - 1; k > 0; --k) {
            for (int c = 0; c < 3; ++c) hist[k][c] = hist[k - 1][c];
            histT[k] = histT[k - 1];
            histDvN[k] = histDvN[k - 1];
        }
        for (int c = 0; c < 3; ++c) hist[0][c] = vel[c];
        histT[0] = now;
        histDvN[0] = dvN;
        if (histN < kHist) ++histN;
        prevDv = dv;
        prevDvN = dvN;
        return fire;
    }
};

// --- Lancio: velocita' di picco (voce 4 del piano Boneworks, 25/10) ---------
// Campione della storia della mano: posizione (unita'), assi (righe X, Y, Z
// nel mondo, come LocalToWorld normalizzata) e ora (s).
struct ThrowSample {
    float p[3];
    float ax[3][3];
    double t;
};
struct PeakThrowOut {
    bool ok = false;          // almeno un campione valido nella finestra
    bool okW = false;         // velocita' angolare misurata attorno al picco
    float v[3] = {0, 0, 0};   // velocita' della mano al picco (unita'/s), media 1:2:1 con i vicini (campioni ripuliti)
    float w[3] = {0, 0, 0};   // velocita' angolare della mano (rad/s), media su 5 campioni attorno al picco
    int peak = -1;            // indice del picco (0 = il campione piu' recente)
    int nV = 0, nW = 0;       // campioni nelle due medie
    int replaced = 0;         // campioni della finestra sostituiti dalla mediana dei loro 3 (isolati sbagliati)
    int rejected = 0;         // campioni scartati come salti del tracciamento
    int rejectedW = 0;        // velocita' angolari scartate come salti (polso girato di colpo: posa finta, controller ritrovato)
    float peakSpeed = 0.0f;   // modulo di v rispetto a ref (unita'/s): il picco che conta
    float rawSpeed = 0.0f;    // il campione grezzo piu' veloce della finestra (unita'/s, rispetto a ref): diagnostica
    double peakAge = 0.0;     // s tra il picco e il campione piu' recente
    // Al rilascio, come throwmode classic: dal campione piu' recente al primo
    // ad almeno avgWin s prima (al piu' gli 8 piu' recenti, come HistVelocity).
    bool okAvg = false, okWAvg = false;
    float vAvg[3] = {0, 0, 0};  // velocita' media della mano (unita'/s)
    float wAvg[3] = {0, 0, 0};  // velocita' angolare media del polso (rad/s)
    float speed[32] = {};     // modulo di ogni campione rispetto a ref (unita'/s; -1 senza velocita', -2 scartato): diagnostica
};

// Asse-angolo di una rotazione R (asse unitario, angolo 0..pi); false se nulla.
inline bool AxisAngle(const float R[3][3], float axis[3], float* angle) {
    float tr = R[0][0] + R[1][1] + R[2][2];
    float c = (tr - 1.0f) * 0.5f;
    c = c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);
    const float ang = std::acos(c);
    if (!(ang > 1e-4f)) return false;
    float ax[3] = {R[2][1] - R[1][2], R[0][2] - R[2][0], R[1][0] - R[0][1]};
    float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (l < 1e-5f) {
        // 180 gradi: asse dalla colonna dominante di R + I.
        const int k = R[0][0] > R[1][1] ? (R[0][0] > R[2][2] ? 0 : 2) : (R[1][1] > R[2][2] ? 1 : 2);
        for (int i = 0; i < 3; ++i) ax[i] = R[i][k] + (i == k ? 1.0f : 0.0f);
        l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    }
    if (!(l > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) axis[i] = ax[i] / l;
    *angle = ang;
    return true;
}

// Stima del lancio dalla storia della mano (s[0] il campione piu' recente, n
// al piu' 32), come Half-Life: Alyx e GetEstimatedPeakVelocities di SteamVR:
// - velocita' del campione k: da s[k] e dal primo campione piu' vecchio ad
//   almeno minDt s (senza visore la mano finta puo' restare ferma per qualche
//   tick e poi saltare: tra due tick vicini la velocita' sarebbe a scatti);
// - salti del tracciamento: oltre jumpSpeed (unita'/s, rispetto a ref, la
//   velocita' di Walker: correndo la mano va gia' veloce) il campione e' scartato;
// - campioni isolati sbagliati (revisione 26/10): un campione che dista
//   dalla mediana dei suoi 3 (k-1, k, k+1, per componente) piu' di
//   max(outAbs, outRel x la velocita' della mediana) e' sostituito dalla
//   mediana. Prima un solo campione sbagliato sotto jumpSpeed (un controller
//   Quest che torna in vista con 5 cm di scatto: ~4,5 m/s per un frame a
//   90 Hz) diventava il picco e la media 1:2:1 ne teneva meta'. Anche uno
//   che va e torna (due velocita' opposte di fila) sparisce. Solo gli
//   isolati: la mediana da sola su un picco a punta perdeva fino a 0,13 m/s
//   (rampa del piano a 90 Hz: 3,70 invece di 3,75), qui un picco pulito
//   resta identico. outAbs <= 0: nessuna sostituzione (come prima);
// - picco: il campione (ripulito) piu' veloce (rispetto a ref) entro window
//   s dal piu' recente, mediato con i vicini k-1 e k+1 (i 3 frame attorno al
//   picco, come Alyx), il picco con peso 2: con la media semplice una rampa a
//   punta (su a 4 m/s in 150 ms, giu' a 2 in 60) perdeva ~0,2 m/s a 120 Hz
//   (1:2:1 ~0,12), il rumore dei tick quasi uguale;
// - velocita' angolare: rotazione tra gli assi degli stessi due campioni
//   (riferimento del mondo, come in Drive), media su k-2..k+2; oltre spinJump
//   rad/s e' un salto (un polso vero non arriva a 50 rad/s; synthleftrot gira
//   la mano finta di 80 gradi in un tick: 167 rad/s, e il lancio partiva a 55);
// - al rilascio (per posare un oggetto, ThrowPeakBlend): la velocita' media
//   e la rotazione media dal campione piu' recente al primo ad almeno avgWin s
//   prima, fra gli 8 piu' recenti (la stessa finestra di throwmode classic),
//   dai campioni gia' controllati (senza salti, isolati sostituiti).
inline PeakThrowOut PeakThrow(const ThrowSample* s, int n, double window, double minDt, float jumpSpeed, const float ref[3],
                              float spinJump = 50.0f, double avgWin = 0.06, float outAbs = 25.0f, float outRel = 0.2f) {
    PeakThrowOut o;
    constexpr int kMax = 32;
    n = n < kMax ? n : kMax;
    if (n < 2) return o;
    float v[kMax][3] = {}, w[kMax][3] = {}, dtk[kMax] = {};
    bool okV[kMax] = {}, okW[kMax] = {};
    for (int k = 0; k < n; ++k) {
        o.speed[k] = -1.0f;
        int j = -1;
        for (int m = k + 1; m < n; ++m)
            if (s[k].t - s[m].t >= minDt) {
                j = m;
                break;
            }
        if (j < 0) continue;
        const double dt = s[k].t - s[j].t;
        if (!(dt > 1e-4) || dt > 0.1) continue;  // storia interrotta: niente velocita' da qui
        dtk[k] = static_cast<float>(dt);
        float rel2 = 0.0f;
        for (int c = 0; c < 3; ++c) {
            v[k][c] = static_cast<float>((s[k].p[c] - s[j].p[c]) / dt);
            rel2 += (v[k][c] - ref[c]) * (v[k][c] - ref[c]);
        }
        if (!std::isfinite(rel2)) continue;
        if (rel2 > jumpSpeed * jumpSpeed) {
            ++o.rejected;
            o.speed[k] = -2.0f;
            continue;
        }
        okV[k] = true;
        o.speed[k] = std::sqrt(rel2);
        float R[3][3] = {}, axis[3], ang = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) R[r][c] += s[k].ax[i][r] * s[j].ax[i][c];
        if (AxisAngle(R, axis, &ang)) {
            if (ang < 1.5f && ang / static_cast<float>(dt) <= spinJump) {
                for (int c = 0; c < 3; ++c) w[k][c] = axis[c] * ang / static_cast<float>(dt);
                okW[k] = true;
            } else {
                ++o.rejectedW;
            }
        } else {
            okW[k] = true;  // nessuna rotazione: w = 0
        }
    }
    // Campioni isolati sbagliati: mediana di 3 per componente (k-1, k, k+1
    // tra quelli validi; con due soli, per componente il piu' vicino a ref);
    // chi ne dista troppo prende la mediana (u), gli altri restano (u = v).
    float u[kMax][3] = {};
    for (int k = 0; k < n; ++k) {
        if (!okV[k]) continue;
        for (int c = 0; c < 3; ++c) u[k][c] = v[k][c];
        if (!(outAbs > 0.0f)) continue;
        int idx[3], m = 0;
        for (int q = k - 1; q <= k + 1; ++q)
            if (q >= 0 && q < n && okV[q]) idx[m++] = q;
        if (m < 2) continue;
        float med[3];
        for (int c = 0; c < 3; ++c) {
            if (m == 3) {
                float x = v[idx[0]][c], y = v[idx[1]][c], z = v[idx[2]][c];
                if (x > y) std::swap(x, y);
                if (y > z) std::swap(y, z);
                if (x > y) std::swap(x, y);
                med[c] = y;
            } else {
                const float x = v[idx[0]][c], y = v[idx[1]][c];
                med[c] = std::fabs(x - ref[c]) <= std::fabs(y - ref[c]) ? x : y;
            }
        }
        float d2 = 0.0f, m2 = 0.0f;
        for (int c = 0; c < 3; ++c) {
            d2 += (v[k][c] - med[c]) * (v[k][c] - med[c]);
            m2 += (med[c] - ref[c]) * (med[c] - ref[c]);
        }
        const float thr = outAbs > outRel * std::sqrt(m2) ? outAbs : outRel * std::sqrt(m2);
        if (d2 > thr * thr) {
            for (int c = 0; c < 3; ++c) u[k][c] = med[c];
            if (s[0].t - s[k].t <= window) ++o.replaced;
        }
    }
    float best = -1.0f;
    for (int k = 0; k < n && s[0].t - s[k].t <= window; ++k) {
        if (!okV[k]) continue;
        float r2 = 0.0f;
        for (int c = 0; c < 3; ++c) r2 += (u[k][c] - ref[c]) * (u[k][c] - ref[c]);
        if (r2 > best) {
            best = r2;
            o.peak = k;
        }
        if (o.speed[k] > o.rawSpeed) o.rawSpeed = o.speed[k];
    }
    {
        // Al rilascio: la finestra di classic (dal piu' recente al primo ad
        // almeno avgWin s prima, 8 campioni al piu'), come media pesata (dt)
        // delle velocita' dei campioni gia' controllati: senza salti (oltre
        // jumpSpeed / spinJump) e con gli isolati sbagliati sostituiti. Su una
        // storia pulita e' la media di classic (le velocita' di un tick si
        // sommano allo spostamento); prima, come differenza tra gli estremi,
        // un polso girato di 80 gradi in un tick (regrab: synthleftrot) sui
        // 50 ms della finestra leggeva 24 rad/s, sotto spinJump, e la leva
        // lanciava la radio posata a 5 m/s.
        const int na = n < 8 ? n : 8;
        int j = -1;
        for (int k = 1; k < na; ++k) {
            j = k;
            if (s[0].t - s[k].t >= avgWin) break;
        }
        float sv[3] = {0, 0, 0}, sw[3] = {0, 0, 0}, tv = 0.0f, tw = 0.0f;
        for (int k = 0; k < j; ++k) {
            if (!okV[k] || !(dtk[k] > 0.0f)) continue;
            for (int c = 0; c < 3; ++c) sv[c] += u[k][c] * dtk[k];
            tv += dtk[k];
            if (!okW[k]) continue;
            for (int c = 0; c < 3; ++c) sw[c] += w[k][c] * dtk[k];
            tw += dtk[k];
        }
        if (tv > 1e-3f && std::isfinite(sv[0] + sv[1] + sv[2])) {
            o.okAvg = true;
            for (int c = 0; c < 3; ++c) o.vAvg[c] = sv[c] / tv;
        }
        if (tw > 1e-3f && std::isfinite(sw[0] + sw[1] + sw[2])) {
            o.okWAvg = true;
            for (int c = 0; c < 3; ++c) o.wAvg[c] = sw[c] / tw;
        }
    }
    if (o.peak < 0) return o;
    o.ok = true;
    o.peakAge = s[0].t - s[o.peak].t;
    float wsum = 0.0f;
    for (int k = o.peak - 1; k <= o.peak + 1; ++k) {
        if (k < 0 || k >= n || !okV[k]) continue;
        const float wk = k == o.peak ? 2.0f : 1.0f;
        for (int c = 0; c < 3; ++c) o.v[c] += wk * u[k][c];
        wsum += wk;
        ++o.nV;
    }
    float p2 = 0.0f;
    for (int c = 0; c < 3; ++c) {
        o.v[c] /= wsum;
        p2 += (o.v[c] - ref[c]) * (o.v[c] - ref[c]);
    }
    o.peakSpeed = std::sqrt(p2);
    for (int k = o.peak - 2; k <= o.peak + 2; ++k) {
        if (k < 0 || k >= n || !okV[k] || !okW[k]) continue;
        for (int c = 0; c < 3; ++c) o.w[c] += w[k][c];
        ++o.nW;
    }
    if (o.nW > 0) {
        o.okW = true;
        for (int c = 0; c < 3; ++c) o.w[c] /= static_cast<float>(o.nW);
    }
    return o;
}

// Limite di energia del lancio: velocita' massima (m/s) per kg con energia
// cinetica al piu' eMax J: sqrt(2 eMax / kg). 0 kg o eMax <= 0: nessun limite (1e9).
inline float ThrowEnergyCap(float kg, float eMax) {
    if (!(kg > 0.0f) || !(eMax > 0.0f)) return 1e9f;
    return std::sqrt(2.0f * eMax / kg);
}

// Lancio o oggetto posato (revisione 26/10)? Peso 0..1 del picco rispetto
// alla media al rilascio, dalla velocita' del picco (m/s, rispetto a Walker):
// 0 fino a lo, 1 da hi in su, smoothstep in mezzo (nessuno scalino tra un
// lancio piano e uno appena piu' forte). Chi posa un oggetto rallenta la mano
// (0,6 -> 0,1 m/s): col solo picco partiva a 0,6 x throwboost. hi <= lo:
// gradino a lo.
inline float ThrowPeakBlend(float speed, float lo, float hi) {
    if (!(speed > lo)) return 0.0f;
    if (!(hi > lo) || speed >= hi) return 1.0f;
    const float x = (speed - lo) / (hi - lo);
    return x * x * (3.0f - 2.0f * x);
}

// Mano che segue la fisica (voce 5 del piano, "solidhands"): ampiezza
// disegnata di uno scostamento m (cm o gradi). Zona morta morbida: sotto dead
// m x smoothstep(m / dead) (1 mm su 2,5 cm resta 0,005 mm: il tremolio di
// PhysX non si vede), da dead in su m intero (stessa pendenza: nessuno
// scalino), poi al piu' cap. dead <= 0: nessuna zona morta.
inline float SolidShape(float m, float dead, float cap) {
    if (!(m > 0.0f)) return 0.0f;
    float out = m;
    if (dead > 0.0f && m < dead) {
        const float x = m / dead;
        out = m * x * x * (3.0f - 2.0f * x);
    }
    return cap > 0.0f && out > cap ? cap : out;
}

// Filtro della mano disegnata: frazione verso il valore voluto in dt secondi,
// costante di tempo tin quando lo scostamento cresce e tout quando cala
// (entra in fretta, esce piano: niente scatti rilasciando). dt <= 0: 0.
inline float SolidFilterAlpha(float dt, bool growing, float tinMs, float toutMs) {
    if (!(dt > 0.0f)) return 0.0f;
    const float tau = (growing ? tinMs : toutMs) * 0.001f;
    if (!(tau > 1e-4f)) return 1.0f;
    return 1.0f - std::exp(-dt / tau);
}

// Portata del braccio della mano disegnata (voce 5): il polso p non va piu'
// lontano dalla spalla di lim = max(reach, |bersaglio - spalla|). Oltre, p
// torna sulla sfera di raggio lim attorno alla spalla lungo la sua direzione:
// si toglie solo la parte che allunga il braccio, quella di lato resta, e il
// risultato e' continuo (proiezione su una palla: mai piu' lontano di prima).
// Ritorna di quanto l'ha accorciato (0 = dentro). reach <= 0: niente.
inline float ReachClamp(const float shoulder[3], const float target[3], float reach, float p[3]) {
    if (!(reach > 0.0f)) return 0.0f;
    float q2 = 0.0f, r2 = 0.0f, rr[3];
    for (int c = 0; c < 3; ++c) {
        const float q = target[c] - shoulder[c];
        rr[c] = p[c] - shoulder[c];
        q2 += q * q;
        r2 += rr[c] * rr[c];
    }
    const float lim = (std::max)(reach, std::sqrt(q2));
    const float rl = std::sqrt(r2);
    if (!(rl > lim) || !(rl > 1e-4f)) return 0.0f;
    const float k = lim / rl;
    for (int c = 0; c < 3; ++c) p[c] = shoulder[c] + rr[c] * k;
    return rl - lim;
}

// --- Forza della presa (voce 6 del piano "Boneworks", "analoggrip") ---------
// Funzioni pure di vr_input.cpp GripAxis e di props.cpp GripValueNow /
// GripFactor / GripStep (revisione 26/10: prima tre righe in props.cpp senza
// collaudo; il percorso del controller vero non era mai passato).

// Valore analogico del grip dall'asse del controller (le due componenti di
// rAxis[2]: i binding legacy dei Touch lo mettono in y, OpenXR in x), senza
// l'1 che il tasto premuto da' a GripValue (sotto OpenXR il tasto scatta gia'
// a 0,5). Componenti NaN o negative = 0, oltre 1 = 1. Un grip solo a tasto
// (asse fermo sotto 0,05 col tasto premuto) da' 1: presa stretta, nessun cambio.
inline float GripAxisOf(float x, float y, bool button) {
    if (!(x >= 0.0f)) x = 0.0f;
    if (!(y >= 0.0f)) y = 0.0f;
    float v = x > y ? x : y;
    if (v > 1.0f) v = 1.0f;
    return v < 0.05f && button ? 1.0f : v;
}

// Valore del grip per la forza della presa: con il grip premuto (g_gripRaw)
// quello dell'asse (sopra 0,05; sotto o NaN: 1, grip solo a tasto); senza
// grip premuto (presa finta di grabsim, grabtest) 1, presa stretta.
inline float GripHeldValue(bool pressed, float axis) {
    if (!pressed) return 1.0f;
    return axis > 0.05f ? (axis < 1.0f ? axis : 1.0f) : 1.0f;
}

// Frazione di forza e coppia per un valore del grip v: mn fino a lo (la soglia
// della presa: sotto OpenXR il tasto scatta a 0,5, e 0,5-0,6 resta la presa
// piu' lenta), 1 da hi in su, in linea retta in mezzo. mn in 0,05..1; NaN: 1
// (un valore rotto non indebolisce la presa).
inline float GripFactorOf(float v, float mn, float lo, float hi) {
    mn = mn > 0.05f ? (mn < 1.0f ? mn : 1.0f) : 0.05f;
    if (!(v == v)) return 1.0f;
    if (!(hi > lo + 0.01f)) hi = lo + 0.01f;
    float t = (v - lo) / (hi - lo);
    t = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
    return mn + (1.0f - mn) * t;
}

// Frazione verso il valore voluto in dt secondi con costante di tempo tau
// (esponenziale: la stessa risposta a 90 e a 250 Hz). tau ~0: 1; dt <= 0: 0.
inline float GripEaseAlpha(float dt, float tau) {
    if (!(dt > 0.0f)) return 0.0f;
    if (!(tau > 1e-4f)) return 1.0f;
    return 1.0f - std::exp(-dt / tau);
}

// Filtro della forza della presa (GripStep, un passo per tick di Drive).
// env: inviluppo superiore della frazione voluta (piu' stretta subito; piu'
// lenta solo dopo hold s dall'ultimo massimo, poi con costante ease s: il
// tremolio del grip non entra). f: il valore in uso, che sale verso env con
// costante rise s e scende con env: continuo nei due versi (niente scalini di
// forza, piega e guadagno; revisione 26/10: prima stringere valeva in un
// tick). slots: massimo di f a fette di win/6 s, per il lancio (GripPeakOf).
// Si parte dalla presa stretta (1).
struct GripFilterState {
    float env = 1.0f;
    float f = 1.0f;
    double holdAt = -1.0;
    float slots[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    double slotAt = -1.0;
    int slot = 0;
};

inline void GripFilterStep(GripFilterState& s, float target, double now, float dt, float hold, float ease, float rise, float win) {
    if (!(target == target)) target = 1.0f;
    if (s.holdAt < 0.0) s.holdAt = now;
    if (target >= s.env) {
        s.env = target;
        s.holdAt = now;
    } else if (now - s.holdAt >= hold) {
        s.env += (target - s.env) * GripEaseAlpha(dt, ease);
        if (s.env - target < 0.002f) s.env = target;
    }
    if (s.env > s.f) {
        s.f += (s.env - s.f) * GripEaseAlpha(dt, rise);
        if (s.env - s.f < 0.002f) s.f = s.env;
    } else {
        s.f = s.env;
    }
    // Massimo degli ultimi win s: 6 fette da win/6, la corrente e le 5 prima
    // (copre da 5/6 a tutta la finestra). Primo passo, finestra nulla o un
    // buco piu' lungo della finestra: tutte le fette ripartono da f.
    const double w = win > 0.0f ? win / 6.0 : 0.0;
    if (s.slotAt < 0.0 || !(w > 0.0) || now - s.slotAt >= win) {
        for (float& x : s.slots) x = s.f;
        s.slotAt = now;
        s.slot = 0;
        return;
    }
    while (now - s.slotAt >= w) {
        s.slot = (s.slot + 1) % 6;
        s.slots[s.slot] = s.f;
        s.slotAt += w;
    }
    if (s.f > s.slots[s.slot]) s.slots[s.slot] = s.f;
}

// La forza della presa piu' alta degli ultimi win s (almeno quella in uso):
// il budget del lancio (props.cpp Release, ThrowSpinLimit). Aprire la mano
// mentre si lancia, anche piano (150-250 ms), non toglie forza al lancio.
inline float GripPeakOf(const GripFilterState& s) {
    float m = s.f;
    for (float x : s.slots) m = x > m ? x : m;
    return m;
}

// Lancio che ferisce (voce 1 del piano dei combattimenti, 26/10): livello
// del colpo di un oggetto lanciato su un nemico, dall'energia cinetica
// rispetto al nemico (0,5 kg v^2) e dall'osso. Come Source (tabella a gradini
// dell'energia, soglie contro i rifiuti volanti) ma a tre livelli, perche' in
// Spec Ops la mischia e' un atterramento, non un danno:
// - 0 niente: sotto minSpeed m/s o minJ J (rimbalza e basta);
// - 1 leggero: TakeDamage piccolo (lightMin + lightPerJ x J, al piu'
//   lightMax; in testa x headMul) con YDamageType_Melee e la reazione fisica
//   del gioco;
// - 2 pesante: da heavyJ J con almeno heavyKg kg, o in testa da headJ J con
//   almeno headKg kg: la reazione della mischia di Walker (HandleMeleeAttack:
//   barcolla o cade), danno heavyMin + heavyPerJ x (J - heavyJ), al piu'
//   heavyMax (la mischia di Walker, m_meleeDamagePawns 40).
// suspect (salto del tracciamento al rilascio): al piu' leggero.
struct ThrowHitParams {
    float minSpeed = 3.0f, minJ = 4.0f;
    float heavyJ = 30.0f, heavyKg = 3.0f;
    float headJ = 15.0f, headKg = 1.0f;
    float lightMin = 3.0f, lightPerJ = 0.15f, lightMax = 15.0f, headMul = 1.5f;
    float heavyMin = 20.0f, heavyPerJ = 0.4f, heavyMax = 40.0f;
};
struct ThrowHitOut {
    int tier = 0;         // 0 niente, 1 leggero, 2 pesante
    int damage = 0;
    float joules = 0.0f;
    bool demoted = false;  // pesante ridotto a leggero (suspect)
};
inline ThrowHitOut ThrowHitTier(float kg, float speed, bool head, bool suspect, const ThrowHitParams& p) {
    ThrowHitOut o;
    if (!(kg > 0.0f) || !(speed > 0.0f) || !std::isfinite(kg * speed)) return o;
    o.joules = 0.5f * kg * speed * speed;
    if (speed < p.minSpeed || o.joules < p.minJ) return o;
    const bool heavy = (o.joules >= p.heavyJ && kg >= p.heavyKg) || (head && o.joules >= p.headJ && kg >= p.headKg);
    if (heavy && !suspect) {
        o.tier = 2;
        const float d = p.heavyMin + p.heavyPerJ * (o.joules - (head ? p.headJ : p.heavyJ));
        o.damage = static_cast<int>(std::lround(d < p.heavyMin ? p.heavyMin : (d > p.heavyMax ? p.heavyMax : d)));
        return o;
    }
    o.demoted = heavy;
    o.tier = 1;
    float d = (p.lightMin + p.lightPerJ * o.joules) * (head ? p.headMul : 1.0f);
    d = d < p.lightMin ? p.lightMin : (d > p.lightMax ? p.lightMax : d);
    o.damage = static_cast<int>(std::lround(d));
    return o;
}

// Filtro dei salti del tracciamento per il danno del lancio (voce 1): la
// velocita' che conta al colpo e' quella d'impatto rispetto al nemico, ma al
// piu' quella che il lancio pulito puo' dare: la velocita' affidabile del
// rilascio (rispetto a Walker, campioni ripuliti) piu' la caduta di dz metri
// (g in m/s^2), piu' margin m/s. Senza rilascio noto (trusted < 0) al piu'
// cap. Un oggetto spinto oltre da altro (un salto della posa, PhysX che lo
// espelle da una compenetrazione) non colpisce piu' forte del lancio.
inline float ThrowDamageSpeed(float impact, float trusted, float dz, float g, float margin, float cap) {
    if (!(impact > 0.0f) || !std::isfinite(impact)) return 0.0f;
    float bound = cap;
    if (trusted >= 0.0f && std::isfinite(trusted)) {
        const float fall = dz > 0.0f && std::isfinite(dz) ? 2.0f * g * dz : 0.0f;
        bound = std::sqrt(trusted * trusted + fall) + margin;
        if (cap > 0.0f && bound > cap) bound = cap;
    }
    return impact < bound ? impact : bound;
}

// Vetri, finestre di sabbia e casse (voce 2 del piano, 26/10): danno di un
// oggetto che colpisce un bersaglio del mondo, dall'energia cinetica
// (J = 0,5 kg v^2). Il resto lo decide il gioco con le sue soglie (la salute
// del pezzo, le modifiche "Cracked" / "Destroyed" con la loro soglia, il
// Kismet della valanga): qui solo quanto danno porta l'urto.
// - niente sotto minSpeed m/s o minJ J;
// - perJ x J, al piu' lightMax per un oggetto leggero (sotto heavyKg kg o
//   sotto heavyJ J: la bottiglia incrina il vetro del pullman, salute 60 e
//   "Cracked" a 30, ma non lo sfonda da sola), al piu' heavyMax per uno
//   pesante (la radio lanciata lo sfonda);
// - tenuto in mano (held): conta anche il braccio dietro all'oggetto, armKg
//   kg in piu' nell'energia (un colpo in mano a 4 m/s sarebbe meno della meta'
//   di un lancio), ma non nella prova del pesante, che guarda la massa
//   dell'oggetto: col braccio ogni oggetto sopra 1 kg era pesante, e una
//   bottiglia in mano a 6 m/s (3,6 kg, 65 J) faceva 81 e sfondava il vetro del
//   pullman da sola (revisione della voce 2, 26/10).
// suspect (salto del tracciamento al rilascio): al piu' leggero.
struct WorldHitParams {
    float minSpeed = 2.0f, minJ = 3.0f;
    float perJ = 1.25f;
    float heavyJ = 40.0f, heavyKg = 3.0f;
    float lightMax = 40.0f, heavyMax = 250.0f;
    float armKg = 2.0f;
};
struct WorldHitOut {
    int damage = 0;
    float joules = 0.0f;
    float kg = 0.0f;      // massa dell'energia (col braccio se in mano)
    bool heavy = false;   // l'oggetto da solo pesa almeno heavyKg (e l'energia basta)
    bool capped = false;  // leggero: danno tagliato a lightMax
};
inline WorldHitOut WorldHitDamage(float kg, float speed, bool held, bool suspect, const WorldHitParams& p) {
    WorldHitOut o;
    if (!(kg > 0.0f) || !(speed > 0.0f) || !std::isfinite(kg * speed)) return o;
    o.kg = kg + (held && p.armKg > 0.0f ? p.armKg : 0.0f);
    o.joules = 0.5f * o.kg * speed * speed;
    if (speed < p.minSpeed || o.joules < p.minJ) return o;
    o.heavy = !suspect && o.joules >= p.heavyJ && kg >= p.heavyKg;
    float d = p.perJ * o.joules;
    const float cap = o.heavy ? p.heavyMax : p.lightMax;
    if (d > cap) {
        d = cap;
        o.capped = !o.heavy;
    }
    o.damage = static_cast<int>(std::lround(d < 1.0f ? 1.0f : d));
    return o;
}

// Mischia con oggetti tenuti (voce 4 del piano dei combattimenti, 26/10):
// colpo di un oggetto tenuto in mano su un nemico. speed = velocita' del punto
// che colpisce (punta o centro: mano piu' polso per la leva, come in Drive)
// nel mondo, in m/s; kg = massa dell'oggetto.
// - 0 niente sotto minSpeed (2,5 m/s, il piano: 2,5-3): un tocco, una spinta;
// - 1 leggero: TakeDamage piccolo con YDamageType_Melee (non uccide mai), al
//   piu' lightMax; anche per gli oggetti sotto staggerKg (lattine, carta);
// - 2 la reazione della mischia di Walker (HandleMeleeAttack: barcolla o
//   cade) da staggerSpeed (3 m/s) con almeno staggerKg, o in testa.
// Danno: base + perJ x J, con J = 0,5 (kg + armKg) v^2 (il braccio dietro
// all'oggetto, come WorldHitDamage), al piu' bodyMax (meta' della mischia di
// Walker); in testa x headMul (2, il piano), al piu' headMax (la mischia di
// Walker, m_meleeDamagePawns 40: chi chiama lo legge da Walker). HL2 non da'
// danno agli oggetti tenuti (massa ridotta a 1 kg "to prevent the player from
// adding crazy amounts of energy"): qui il tetto fa la stessa cosa.
struct MeleeHitParams {
    float minSpeed = 2.5f, staggerSpeed = 3.0f, staggerKg = 0.5f, armKg = 2.0f;
    float base = 5.0f, perJ = 0.4f, bodyMax = 20.0f, headMul = 2.0f, headMax = 40.0f, lightMax = 15.0f;
};
struct MeleeHitOut {
    int tier = 0;          // 0 niente, 1 leggero, 2 mischia (barcolla / cade)
    int damage = 0;
    float joules = 0.0f;   // col braccio
    bool capped = false;   // tagliato al tetto
};
// swing (revisione della voce 4, 26/10): velocita' dello stesso punto rispetto
// a Walker (meno FrameVelocity, il moto con cui porta le mani). Soglia e
// livello vogliono un colpo vero: la minore tra swing e speed; il danno resta
// dall'energia nel mondo (speed). Camminando contro un nemico con l'oggetto
// fermo in mano (3,7-3,8 m/s nel mondo, swing quasi 0) non e' un colpo.
inline MeleeHitOut MeleeHit(float kg, float speed, float swing, bool head, const MeleeHitParams& p) {
    MeleeHitOut o;
    if (!(kg > 0.0f) || !(speed > 0.0f) || !std::isfinite(kg * speed) || !std::isfinite(swing)) return o;
    o.joules = 0.5f * (kg + (p.armKg > 0.0f ? p.armKg : 0.0f)) * speed * speed;
    const float eff = swing < speed ? swing : speed;
    if (eff < p.minSpeed) return o;
    o.tier = kg >= p.staggerKg && (eff >= p.staggerSpeed || head) ? 2 : 1;
    float d = p.base + p.perJ * o.joules;
    float cap = p.bodyMax;
    if (head) {
        d *= p.headMul;
        cap = p.headMax;
    }
    if (o.tier == 1 && cap > p.lightMax) cap = p.lightMax;
    if (d > cap) {
        d = cap;
        o.capped = true;
    }
    o.damage = static_cast<int>(std::lround(d < 1.0f ? 1.0f : d));
    return o;
}
// Walker fermo: swing = speed.
inline MeleeHitOut MeleeHit(float kg, float speed, bool head, const MeleeHitParams& p) { return MeleeHit(kg, speed, speed, head, p); }

// Disarmo (voce 10 del piano dei combattimenti, 27/10): colpo sull'arma di un
// nemico con un oggetto tenuto, lanciato o con la mano vuota. Conta la
// quantita' di moto (kg x m/s) del colpo:
// - oggetto tenuto: kg + armKg (il braccio dietro, come MeleeHit) alla minore
//   tra la velocita' nel mondo e quella rispetto a Walker (swing: camminare
//   contro il nemico con l'oggetto fermo non e' un colpo);
// - mano vuota: handKg (mano e avambraccio, come hapslapkg), stessa regola;
// - oggetto lanciato: kg alla velocita' rispetto al nemico (speed; swing non conta).
// Livelli: 0 niente (sotto la velocita' minima o sotto knockP); 1 colpo
// all'arma che non gliela toglie: la reazione della mischia di Walker
// (barcolla, fuoco e ricarica interrotti); 2 l'arma gli cade (da dropP).
// Esempi: mano a 3 m/s 6 (barcolla), a 4 m/s 8 (cade); bottiglia (1,6 kg)
// tenuta a 2,5 m/s 9 (cade); lattina (0,4 kg) tenuta a 3 m/s 7,2 (barcolla);
// bottiglia lanciata a 5 m/s 8 (cade), a 3 m/s 4,8 (barcolla); lattina
// lanciata a 8 m/s 3,2 (niente).
enum class DisarmHow : uint8_t { kHeld, kThrown, kHand };
struct DisarmParams {
    float minHeld = 2.5f, minHand = 3.0f, minThrown = 3.0f;  // m/s
    float armKg = 2.0f, handKg = 2.0f;
    float knockP = 4.0f, dropP = 8.0f;  // kg m/s
};
struct DisarmOut {
    int tier = 0;          // 0 niente, 1 barcolla (l'arma resta), 2 l'arma cade
    float speed = 0.0f;    // velocita' che conta (m/s)
    float impulse = 0.0f;  // kg m/s
    bool slow = false;     // sotto la velocita' minima
};
inline DisarmOut DisarmHit(DisarmHow how, float kg, float speed, float swing, const DisarmParams& p) {
    DisarmOut o;
    if (!std::isfinite(speed) || !(speed > 0.0f) || !std::isfinite(kg) || kg < 0.0f) return o;
    float m = 0.0f, eff = speed, minV = 0.0f;
    switch (how) {
        case DisarmHow::kHeld:
            m = kg + (p.armKg > 0.0f ? p.armKg : 0.0f);
            if (!std::isfinite(swing)) return o;
            eff = swing < speed ? swing : speed;
            minV = p.minHeld;
            break;
        case DisarmHow::kHand:
            m = p.handKg > 0.0f ? p.handKg : 0.0f;
            if (!std::isfinite(swing)) return o;
            eff = swing < speed ? swing : speed;
            minV = p.minHand;
            break;
        case DisarmHow::kThrown:
            m = kg;
            minV = p.minThrown;
            break;
    }
    if (eff < 0.0f) eff = 0.0f;
    o.speed = eff;
    o.impulse = m * eff;
    if (eff < minV) {
        o.slow = true;
        return o;
    }
    o.tier = o.impulse >= p.dropP ? 2 : (o.impulse >= p.knockP ? 1 : 0);
    return o;
}

// Segmento a-b contro un box orientato (voce 10: l'arma del nemico): centro lc
// e mezze estensioni le nello spazio locale di m (FMatrix di UE3: righe X, Y,
// Z e origine, come LocalToWorld; la scala sta nelle righe), gonfiato di
// margin unita' del mondo su ogni lato. true se lo tocca; t = frazione del
// segmento dove entra (0 se a e' gia' dentro).
inline bool SegmentHitsObb(const float* a, const float* b, const float m[4][4], const float* lc, const float* le, float margin, float* t) {
    float tmin = 0.0f, tmax = 1.0f;
    for (int i = 0; i < 3; ++i) {
        const float s2 = m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2];
        if (!(s2 > 1e-8f) || !std::isfinite(s2)) return false;
        const float la = ((a[0] - m[3][0]) * m[i][0] + (a[1] - m[3][1]) * m[i][1] + (a[2] - m[3][2]) * m[i][2]) / s2;
        const float lb = ((b[0] - m[3][0]) * m[i][0] + (b[1] - m[3][1]) * m[i][1] + (b[2] - m[3][2]) * m[i][2]) / s2;
        if (!std::isfinite(la) || !std::isfinite(lb) || !std::isfinite(lc[i]) || !std::isfinite(le[i])) return false;
        const float mg = (margin > 0.0f ? margin : 0.0f) / std::sqrt(s2);
        const float lo = lc[i] - std::fabs(le[i]) - mg, hi = lc[i] + std::fabs(le[i]) + mg;
        const float d = lb - la;
        if (std::fabs(d) < 1e-9f) {
            if (la < lo || la > hi) return false;
            continue;
        }
        float t0 = (lo - la) / d, t1 = (hi - la) / d;
        if (t0 > t1) {
            const float k = t0;
            t0 = t1;
            t1 = k;
        }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
        if (tmin > tmax) return false;
    }
    if (t) *t = tmin;
    return true;
}

// Scudi con oggetti tenuti (voce 3 del piano dei combattimenti, 26/10).
// Materiale dell'oggetto dal nome della mesh (minuscolo): fragile (piatti,
// libri, carta, cartone, vetro, cuscini, stoffa), metallo o sabbia (casse
// metalliche, munizioni, cassette degli attrezzi, barili, taniche, elmetti,
// sacchi di sabbia), il resto normale. Il metallo vince sul fragile
// ("metal_plate" e' una lastra di metallo).
enum class ShieldMaterial : uint8_t { kNormal, kFragile, kMetal };
inline ShieldMaterial ShieldMaterialOf(const std::string& lowerName) {
    static const char* const kMetal[] = {"metal", "steel", "iron", "ammo", "toolbox", "tool_box", "safe", "barrel", "drum", "tank",
                                         "canister", "jerry", "gascan", "gas_can", "bucket", "helmet", "locker", "sandbag",
                                         "extinguisher", "armor", "armour", "fuel", "pelican", "ammobox"};
    static const char* const kFragile[] = {"plate", "book", "magazine", "paper", "card", "carton", "glass", "bottle", "cup",
                                           "mug", "bowl", "vase", "pillow", "cushion", "sleeping", "cloth", "shirt", "hat", "toy",
                                           "soda", "fruit", "food", "laptop", "phone", "frame_photo", "photo"};
    for (const char* k : kMetal)
        if (lowerName.find(k) != std::string::npos) return ShieldMaterial::kMetal;
    for (const char* k : kFragile)
        if (lowerName.find(k) != std::string::npos) return ShieldMaterial::kFragile;
    return ShieldMaterial::kNormal;
}

// Colpi che l'oggetto regge prima di volare via dalla mano (il piano: piatti
// e libri ~2, valigie e radio ~6, casse metalliche ~15). Fragile 2, metallo
// 15; il resto dalla massa: sotto 3 kg 3, sotto 6 kg 5, sotto 25 kg 6 (radio
// 5-8 kg, valigia 16,8 kg), oltre 10. La durabilita' impedisce che lo scudo
// renda invincibili.
inline int ShieldDurability(ShieldMaterial m, float kg) {
    if (m == ShieldMaterial::kFragile) return 2;
    if (m == ShieldMaterial::kMetal) return 15;
    if (!(kg > 0.0f) || kg < 3.0f) return 3;
    if (kg < 6.0f) return 5;
    if (kg < 25.0f) return 6;
    return 10;
}

// Usura di un colpo (raffica di un'arma nello stesso istante: i pallini di un
// fucile a pompa contano una volta) dal danno dell'arma (YWeapon.m_damage
// +0x704, letto il 26/10: M4 30, AK47 50, HK417 79, M2 135, Mossberg 365,
// Barrett 1400): 1 sotto 100, 2 sotto 300, 3 oltre.
inline int ShieldWear(float damage) {
    if (!(damage >= 100.0f)) return 1;
    return damage < 300.0f ? 2 : 3;
}

// Impulso del proiettile sentito dalla mano (N s, per la vibrazione e la
// spinta): 0,8 + danno / 40, tra 1 e 6 (M4 1,55; AK47 2,05; HK417 2,8;
// Barrett 6). Non e' la quantita' di moto vera: una scala per sentire la
// differenza tra una pistola e un fucile di precisione.
inline float ShieldShotImpulse(float damage) {
    float j = 0.8f + (damage > 0.0f && std::isfinite(damage) ? damage : 0.0f) / 40.0f;
    return j < 1.0f ? 1.0f : (j > 6.0f ? 6.0f : j);
}

// Velocita' data all'oggetto dal colpo (m/s): J / massa (almeno 0,5 kg), tra
// 0,2 e 3. E l'ampiezza della spinta che torna (cm): 2,5 x dv, tra 0,8 e 5.
inline float ShieldDv(float J, float kg) {
    float dv = J / (kg > 0.5f ? kg : 0.5f);
    return dv < 0.2f ? 0.2f : (dv > 3.0f ? 3.0f : dv);
}
inline float ShieldKickCm(float dv) {
    float a = 2.5f * dv;
    return a < 0.8f ? 0.8f : (a > 5.0f ? 5.0f : a);
}

// Forma della spinta del colpo sull'oggetto tenuto (0..1): sale in tau e torna,
// (t / tau) e^(1 - t / tau); 0 prima del colpo e dopo 8 tau.
inline float ShieldKickShape(float t, float tau) {
    if (!(t > 0.0f) || !(tau > 0.0f) || t > 8.0f * tau) return 0.0f;
    const float x = t / tau;
    return x * std::exp(1.0f - x);
}

// Oggetto fatto volare via dalla mano dal colpo che lo consuma (revisione
// della voce 3): lo scudo si tiene davanti al viso, e lungo il colpo (3,5 m/s
// fissi, per ogni massa) tornava indietro attraverso la testa e la camera.
// Velocita' (m/s) dalla spinta del colpo (ShieldDv): 0,8 + 1,2 dv, al piu'
// 3,5 (radio da 5 kg con l'AK47 ~1,3; valigia ~1,0; bottiglia 3,5).
inline float ShieldKnockSpeed(float dv) {
    const float v = 0.8f + 1.2f * (dv > 0.0f && std::isfinite(dv) ? dv : 0.0f);
    return v > 3.5f ? 3.5f : v;
}
// Direzione (unitaria in out): quella del colpo d senza la parte orizzontale
// verso la testa, piu' 0,8 di lato dalla parte della mano (side > 0 destra,
// < 0 sinistra, rispetto alla direzione testa -> oggetto; assi di Unreal: X
// avanti, Y destra, Z su) e 0,5 in basso, o in alto se l'oggetto e' sopra gli
// occhi (cadendo passerebbe sulla testa). Non ha mai una parte orizzontale
// verso la testa.
inline void ShieldKnockDir(const float d[3], const float obj[3], const float head[3], float side, float out[3]) {
    float f[2] = {obj[0] - head[0], obj[1] - head[1]};  // testa -> oggetto, in orizzontale
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1]);
    if (!(fl > 1.0f) || !std::isfinite(fl)) {
        // Oggetto proprio sopra o sotto la testa: "avanti" = contro il colpo.
        f[0] = -d[0];
        f[1] = -d[1];
        fl = std::sqrt(f[0] * f[0] + f[1] * f[1]);
        if (!(fl > 1e-4f) || !std::isfinite(fl)) {
            f[0] = 1.0f;
            f[1] = 0.0f;
            fl = 1.0f;
        }
    }
    f[0] /= fl;
    f[1] /= fl;
    float v[3] = {std::isfinite(d[0]) ? d[0] : 0.0f, std::isfinite(d[1]) ? d[1] : 0.0f, std::isfinite(d[2]) ? d[2] : 0.0f};
    const float back = -(v[0] * f[0] + v[1] * f[1]);  // parte orizzontale verso la testa
    if (back > 0.0f) {
        v[0] += back * f[0];
        v[1] += back * f[1];
    }
    const float s = side < 0.0f ? -1.0f : 1.0f;
    v[0] += 0.8f * s * -f[1];  // destra di f = (-f.y, f.x)
    v[1] += 0.8f * s * f[0];
    v[2] += obj[2] > head[2] ? 0.5f : -0.5f;
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; ++i) out[i] = l > 1e-6f ? v[i] / l : 0.0f;
}

// Granate nemiche rilanciate (voce 8 del piano, 27/10). Le granate dell'IA
// seguono Actor.Velocity (unita'/s): in mano ogni tick la velocita' che la
// porta sul palmo nel tick dopo, err / dt (err = palmo - Location) piu' la
// velocita' della mano (il palmo intanto si sposta), al piu' cap. dt fuori
// da 1-100 ms: 11 ms (come il tick degli oggetti). Valori non finiti: 0.
inline float GrenadeHoldVel(const float err[3], float dt, const float hv[3], float cap, float out[3]) {
    if (!(dt >= 0.001f && dt <= 0.1f)) dt = 0.011f;
    float s2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float e = std::isfinite(err[i]) ? err[i] : 0.0f, h = std::isfinite(hv[i]) ? hv[i] : 0.0f;
        out[i] = e / dt + h;
        s2 += out[i] * out[i];
    }
    float s = std::sqrt(s2);
    if (cap > 0.0f && s > cap) {
        for (int i = 0; i < 3; ++i) out[i] *= cap / s;
        s = cap;
    }
    return s;
}
// Rilancio (revisione della voce 8, 27/10: il modello del lancio degli
// oggetti): hand e' la parte della mano rispetto a Walker (picco di
// PeakThrowVel, o la media di 60 ms meno FrameVelocity con throwmode
// classic), base la velocita' di Walker (FrameVelocity). throwboost solo
// sulla mano, al piu' cap rispetto a Walker (1,3 volte la velocita' di lancio
// dell'IA: 1000 unita'/s = 20 m/s per la frag, quindi 26 m/s), poi base:
// correndo la corsa non diventa il 130%. Prima la media di 60 ms nel mondo,
// tutta per throwboost: una mano che frena prima di aprirsi la lasciava
// cadere vicino a Walker. Ritorna la velocita' data nel mondo.
inline float GrenadeThrowVel(const float hand[3], const float base[3], float boost, float cap, float out[3]) {
    if (!(boost > 0.0f) || !std::isfinite(boost)) boost = 1.0f;
    float s2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        out[i] = (std::isfinite(hand[i]) ? hand[i] : 0.0f) * boost;
        s2 += out[i] * out[i];
    }
    const float s = std::sqrt(s2);
    if (cap > 0.0f && s > cap)
        for (int i = 0; i < 3; ++i) out[i] *= cap / s;
    float w2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        out[i] += std::isfinite(base[i]) ? base[i] : 0.0f;
        w2 += out[i] * out[i];
    }
    return std::sqrt(w2);
}
// Granata da segnare e da prendere (revisione della voce 8): almeno minFuse s
// di miccia (fosforo 0,7 s, stun 1,1 e frag della squadra 1,7 a terra ne
// hanno quasi niente: esploderebbe in mano). Miccia non letta (-1: timer
// 'OnExplode' non trovato) come prima: si'.
inline bool GrenadeFuseGrabbable(float fuse, float minFuse) {
    if (!std::isfinite(fuse)) return false;
    return fuse < 0.0f || fuse >= minFuse;
}
// A terra: ha gia' rimbalzato (m_numBounces) o scivola (m_isSliding) e si
// muove piano (velocita' dagli spostamenti, unita'/s: la Velocity di una
// granata ferma cresce con la gravita' senza muoverla).
inline bool GrenadeOnGround(int bounces, bool sliding, float posSpeed, float maxSpeed) {
    return (bounces >= 1 || sliding) && std::isfinite(posSpeed) && posSpeed < maxSpeed;
}
// Tempo della previsione dei rimbalzi (m_currentPredictionTime) per farla
// ripartire dal rilancio: il tempo passato dal lancio (m_startTime), tra 0 e
// la miccia (DoPrediction si ferma oltre la miccia). Il tempo all'impatto che
// la previsione manda all'IA (m_startTime + tempo - adesso) torna giusto.
inline float GrenadePredTime(float now, float startTime, float life) {
    if (!std::isfinite(now) || !std::isfinite(startTime) || !(life > 0.0f)) return 0.0f;
    const float t = now - startTime;
    return t < 0.0f ? 0.0f : (t > life ? life : t);
}

// Rumore di un oggetto che sbatte (voce 9 del piano, 27/10): fattore del
// raggio d'udito dell'IA rispetto a quello del corpo a corpo del gioco
// (m_hearEventMeleeAttack, 300 unita' = 6 m). Dall'energia dell'urto,
// 1/2 kg dv^2 (dv = cambio di velocita' in m/s in 50 ms, rimbalzo compreso:
// una bottiglia lanciata a 6,4 m/s cambia di ~11 m/s): sqrt(E / 40 J), tra
// 0,5 e 2; un vetro che si rompe x1,5, al piu' 2,5. Misurati sul manichino
// (27/10): bottiglia da 0,73 kg a 11 m/s ~1,05 (6,3 m), a 8,7 m/s 0,83
// (5 m); radio da 5,1 kg a 11 m/s 2 (12 m); lattina da 0,3 kg a 4 m/s 0,5
// (3 m). Valori non validi: 1 (come il gioco).
inline float PropNoiseLoudness(float kg, float dv, bool glassBreaks) {
    if (!(kg > 0.0f) || !std::isfinite(kg) || !(dv > 0.0f) || !std::isfinite(dv)) return glassBreaks ? 1.5f : 1.0f;
    float l = std::sqrt(0.5f * kg * dv * dv / 40.0f);
    l = l < 0.5f ? 0.5f : (l > 2.0f ? 2.0f : l);
    if (glassBreaks) l = l * 1.5f > 2.5f ? 2.5f : l * 1.5f;
    return l;
}

// Segmento a -> b contro una scatola orientata (centro c, assi ax unitari,
// mezze estensioni e): frazione del segmento in cui entra (0..1) e normale
// della faccia d'entrata (verso a). false se non la attraversa, se a e' gia'
// dentro o se l'entrata e' oltre b.
inline bool SegmentObbEntry(const float* a, const float* b, const float* c, const float ax[3][3], const float* e, float* tEnter,
                            float n[3]) {
    float t0 = -1e30f, t1 = 1e30f;
    int face = -1;
    float faceSign = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float pa = (a[0] - c[0]) * ax[i][0] + (a[1] - c[1]) * ax[i][1] + (a[2] - c[2]) * ax[i][2];
        const float d = (b[0] - a[0]) * ax[i][0] + (b[1] - a[1]) * ax[i][1] + (b[2] - a[2]) * ax[i][2];
        if (std::fabs(d) < 1e-9f) {
            if (pa < -e[i] || pa > e[i]) return false;
            continue;
        }
        float ta = (-e[i] - pa) / d, tb = (e[i] - pa) / d;
        float s = -1.0f;  // entra dalla faccia -e (normale -ax)
        if (ta > tb) {
            std::swap(ta, tb);
            s = 1.0f;
        }
        if (ta > t0) {
            t0 = ta;
            face = i;
            faceSign = s;
        }
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    if (face < 0 || t0 < 0.0f || t0 > 1.0f) return false;
    *tEnter = t0;
    for (int k = 0; k < 3; ++k) n[k] = faceSign * ax[face][k];
    return true;
}

// --- Angoli delle pose (25/10, bug del tester) --------------------------------
//
// Matrici di posa OpenVR 3x4 (m[riga][colonna]): colonna 0 = destra, 1 = su,
// 2 = indietro (avanti = -colonna 2), colonna 3 = posizione; righe = x destra,
// y su, z indietro dello spazio del tracciamento. Angoli in radianti con le
// convenzioni della mod e di FRotator di UE3: yaw positivo verso destra,
// pitch positivo in su, roll come atan2(-destra.y, su.y). Con questi tre
// angoli FRotationMatrix rifa' esattamente la posa (test "Euler round trip").
constexpr float kPi = 3.14159265358979f;

// Angolo in [-pi, pi].
inline float WrapPi(float a) { return std::remainder(a, 2.0f * kPi); }

struct Euler {
    float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
};

// Angoli esatti (yaw-pitch-roll come FRotator): pitch = asin(avanti.y),
// yaw = atan2(-m02, m22), roll = atan2(-m10, m11). Nessuna miscela con l'asse
// "su": vale con la testa inclinata di lato.
inline Euler ExactEuler(const float m[3][4]) {
    float fy = -m[1][2];
    fy = fy > 1.0f ? 1.0f : (fy < -1.0f ? -1.0f : fy);
    Euler e;
    e.pitch = std::asin(fy);
    e.yaw = std::atan2(-m[0][2], m[2][2]);
    e.roll = std::atan2(-m[1][0], m[1][1]);
    return e;
}

// Yaw "di direzione" (YawFromOpenVrPose di hooks.cpp fino al 25/10): avanti
// orizzontale piu' l'asse "su" pesato con |avanti.y|. Stabile guardando quasi
// in verticale (audit 19/10), ma non e' lo yaw esatto: con la testa inclinata
// di lato sbaglia di 4,5 gradi a pitch -30 / roll 10 e di 9,3 a -20 / 30
// (bug 2 e 6 del tester). Va bene per una direzione (ricentramento, corpo),
// non per la vista o la mira.
inline float BlendedYaw(const float m[3][4]) {
    float forwardX = -m[0][2];
    float forwardZ = -m[2][2];
    float forwardY = -m[1][2];
    float k = forwardY < 0.0f ? -forwardY : forwardY;
    float sgn = forwardY < 0.0f ? 1.0f : -1.0f;
    forwardX += sgn * k * m[0][1];
    forwardZ += sgn * k * m[2][1];
    return std::atan2(forwardX, -forwardZ);
}

// Pitch = asin(avanti.y) (PitchFromOpenVrPose di hooks.cpp).
inline float AsinPitch(const float m[3][4]) {
    float forwardY = -m[1][2];
    if (forwardY > 1.0f) forwardY = 1.0f;
    if (forwardY < -1.0f) forwardY = -1.0f;
    return std::asin(forwardY);
}

// Rotazione (traslazione nulla) con ExactEuler(m) = (pitch, yaw, roll):
// Ry(-yaw) * Rx(pitch) * Rz(-roll) nello spazio di OpenVR.
inline void PoseFromEuler(float pitch, float yaw, float roll, float m[3][4]) {
    const float cy = std::cos(-yaw), sy = std::sin(-yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(-roll), sr = std::sin(-roll);
    const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const float rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const float rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    float t[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i][j] = t[i][0] * rz[0][j] + t[i][1] * rz[1][j] + t[i][2] * rz[2][j];
        m[i][3] = 0.0f;
    }
}

// Assi (avanti, destra, su) di FRotationMatrix di UE3 per un FRotator in
// radianti (come RotatorToAxes di hooks.cpp).
inline void UeAxesFromRotator(float pitch, float yaw, float roll, float f[3], float r[3], float u[3]) {
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw), cr = std::cos(roll),
                sr = std::sin(roll);
    f[0] = cp * cy; f[1] = cp * sy; f[2] = sp;
    r[0] = sr * sp * cy - cr * sy; r[1] = sr * sp * sy + cr * cy; r[2] = -sr * cp;
    u[0] = -(cr * sp * cy + sr * sy); u[1] = cy * sr - cr * sp * sy; u[2] = cr * cp;
}

// Assi della posa OpenVR nel riferimento di UE3 (X avanti = -z, Y destra = x, Z su = y).
inline void UeAxesFromPose(const float m[3][4], float f[3], float r[3], float u[3]) {
    f[0] = m[2][2]; f[1] = -m[0][2]; f[2] = -m[1][2];
    r[0] = -m[2][0]; r[1] = m[0][0]; r[2] = m[1][0];
    u[0] = -m[2][1]; u[1] = m[0][1]; u[2] = m[1][1];
}

// Quaternione (w, x, y, z) della rotazione di una posa OpenVR (per i log: viewlog).
inline void QuatFromPose(const float m[3][4], float q[4]) {
    const float t = m[0][0] + m[1][1] + m[2][2];
    if (t > 0.0f) {
        const float s = std::sqrt(t + 1.0f) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (m[2][1] - m[1][2]) / s;
        q[2] = (m[0][2] - m[2][0]) / s;
        q[3] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[0][1] + m[1][0]) / s;
        q[3] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q[0] = (m[0][2] - m[2][0]) / s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = 0.25f * s;
        q[3] = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q[0] = (m[1][0] - m[0][1]) / s;
        q[1] = (m[0][2] + m[2][0]) / s;
        q[2] = (m[1][2] + m[2][1]) / s;
        q[3] = 0.25f * s;
    }
}

// Angolo (gradi) tra due terne di assi ortonormali (righe a[0..2], b[0..2]),
// in doppia precisione e robusto anche sotto 0,01 gradi (atan2 di seno e coseno).
inline double AxesAngleDeg(const float a[3][3], const float b[3][3]) {
    double r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            r[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) r[i][j] += static_cast<double>(a[k][i]) * b[k][j];
        }
    const double c = (r[0][0] + r[1][1] + r[2][2] - 1.0) * 0.5;
    const double sx = r[2][1] - r[1][2], sy = r[0][2] - r[2][0], sz = r[1][0] - r[0][1];
    const double s = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
    return std::atan2(s, c) * 57.29577951308232;
}

// Angolo (gradi) tra le rotazioni di due pose OpenVR (solo la parte 3x3).
inline double PoseAngleDeg(const float a[3][4], const float b[3][4]) {
    float ra[3][3], rb[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            ra[j][i] = a[i][j];  // colonne -> righe (terna di assi)
            rb[j][i] = b[i][j];
        }
    return AxesAngleDeg(ra, rb);
}

// Vettore di rotazione (radianti, spazio del tracciamento) di A * B^T: la
// rotazione che porta la posa B nella posa A (solo la parte 3x3). Con la
// velocita' angolare w (rad/s, stesso spazio) dot(v, w) / |w|^2 e' di quanti
// secondi A e' "piu' avanti" di B lungo il moto (riga POSEAGE, WI-1).
inline void RelativeRotationVector(const float a[3][4], const float b[3][4], float out[3]) {
    double d[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            d[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) d[i][j] += static_cast<double>(a[i][k]) * b[j][k];
        }
    const double vx = 0.5 * (d[2][1] - d[1][2]), vy = 0.5 * (d[0][2] - d[2][0]), vz = 0.5 * (d[1][0] - d[0][1]);
    const double s = std::sqrt(vx * vx + vy * vy + vz * vz), c = 0.5 * (d[0][0] + d[1][1] + d[2][2] - 1.0);
    const double th = std::atan2(s, c), k = s > 1e-9 ? th / s : 1.0;
    out[0] = static_cast<float>(vx * k);
    out[1] = static_cast<float>(vy * k);
    out[2] = static_cast<float>(vz * k);
}

// "predictframe auto" (review WI-1): il frame in piu' di OpenVR si accende (o
// si spegne) solo se la riga POSEAGE lo conferma per due finestre di fila.
// Ogni finestra: mediana di quanti ms la posa di disegno e' predetta piu'
// avanti di quella del frame data dal runtime (WaitGetPoses), sui frame con la
// testa che gira. Spento e mediana sotto -0,5 frame = manca un frame; acceso e
// sopra +0,5 frame = un frame di troppo. Dopo un cambio la finestra seguente
// si salta (la media del ritardo misurato si assesta in ~150 frame).
struct PredictFrameAuto {
    bool on = false;
    int votes = 0;     // finestre di fila che chiedono il cambio
    bool skip = false; // finestra subito dopo un cambio
    static constexpr int kMinTurning = 30;  // frame con la testa che gira per finestra
    // true se lo stato cambia.
    bool Window(float medianMs, int turning, float frameMs) {
        if (skip) {
            skip = false;
            votes = 0;
            return false;
        }
        if (turning < kMinTurning || !(frameMs > 0.0f) || !std::isfinite(medianMs)) return false;  // finestra muta
        const bool wantChange = on ? medianMs > 0.5f * frameMs : medianMs < -0.5f * frameMs;
        votes = wantChange ? votes + 1 : 0;
        if (votes < 2) return false;
        on = !on;
        votes = 0;
        skip = true;
        return true;
    }
};

// --- Palmo dei Touch -------------------------------------------------------
//
// Posa "grip" di OpenXR nel riferimento "raw" dei Touch di SteamVR:
// componente openxr_grip dei modelli oculus_quest2_controller_* e
// oculus_quest_plus_controller_* (resources/rendermodels di SteamVR):
// origine (+-0.007, -0.00182941, 0.1019482) m (x positivo per la sinistra),
// rotate_xyz (20.6, 0, 0). L'origine e' il centro del palmo. Il backend
// OpenXR (vr_openxr.cpp, GripToRaw) ne usa l'inversa per riportare la posa
// grip al riferimento raw.
constexpr float kGripOriginX = 0.007f, kGripOriginY = -0.00182941f, kGripOriginZ = 0.1019482f;
constexpr float kGripPitchDeg = 20.6f;

// Posa grip nel riferimento raw: rotazione attorno a x di pitchDeg (openxrgrip) e origine del palmo.
inline void GripInRaw(bool left, float pitchDeg, float m[3][4]) {
    const float c = std::cos(pitchDeg * 0.01745329252f), s = std::sin(pitchDeg * 0.01745329252f);
    const float t[3][4] = {{1, 0, 0, left ? kGripOriginX : -kGripOriginX}, {0, c, -s, kGripOriginY}, {0, s, c, kGripOriginZ}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = t[i][j];
}

// Centro del palmo (metri, spazio del tracciamento) da una posa raw del
// controller. gripFix false ("openxrgrip off" col backend OpenXR): la posa e'
// gia' quella grip, il palmo e' la sua origine.
inline void PalmOf(const float raw[3][4], bool left, bool gripFix, float out[3]) {
    const float o[3] = {gripFix ? (left ? kGripOriginX : -kGripOriginX) : 0.0f, gripFix ? kGripOriginY : 0.0f,
                        gripFix ? kGripOriginZ : 0.0f};
    for (int i = 0; i < 3; ++i) out[i] = raw[i][3] + raw[i][0] * o[0] + raw[i][1] * o[1] + raw[i][2] * o[2];
}

// --- Telaio dell'arma (WI-4a, bug 6 del tester: "gun angle is completely off") ---
//
// Raggio di puntamento dei Touch nel riferimento raw: componente openxr_aim
// degli stessi modelli di SteamVR (quest2 e quest_plus, sinistra e destra):
// rotate_xyz (-39.4, 0, 0), origine (+-0.007, -0.03894766, 0.00949694) m. E' la
// posa aim di OpenXR (VDXR: la posa OVR, la "forward" dei giochi Touch nativi)
// e il laser del menu. Il raw -Z punta 39,4 gradi SOPRA questo raggio: con
// "aimpitch 0" (predefinito fino al 25/10) la canna andava 39,4 gradi sopra
// dove il giocatore punta (lo sviluppatore giocava con "aimpitch -24").
constexpr float kAimPitchInRawDeg = -39.4f;
constexpr float kAimOriginY = -0.03894766f, kAimOriginZ = 0.00949694f;
// Posa grip di VDXR nella posa aim (= OVR): RotX(60 gradi), (0, -3, 9,5) cm
// (calibrazione di VDXR, action.cpp rebindControllerActions; e' anche
// openxr_grip nella openxr_aim dei modelli di SteamVR).
constexpr float kGripInAimPitchDeg = 60.0f;

// Raggio di puntamento nel riferimento della posa che la mod riceve (angoli in
// gradi come ExactEuler: pitch < 0 = il raggio sta sotto il -Z della posa).
struct AimInRaw {
    float pitch = kAimPitchInRawDeg, yaw = 0.0f, roll = 0.0f;
};

// Senza misura: OpenVR (SteamVR, Steam Link, runtime finto) = costante dei
// Touch (i controller non Touch sono gia' allineati ai Touch dalla punta,
// vr_input UpdatePoseFix: la costante vale dopo la correzione). OpenXR: la posa
// arriva da grip * inverse(GripInRaw(openxrgrip)), quindi il raggio sta a
// openxrgrip - 60 gradi (-39,4 col predefinito 20,6; -60 con "openxrgrip off").
inline AimInRaw DefaultAimInRaw(bool openxr, bool gripFix, float gripPitchDeg) {
    AimInRaw a;
    if (openxr) a.pitch = (gripFix ? gripPitchDeg : 0.0f) - kGripInAimPitchDeg;
    return a;
}

// Misura del backend OpenXR (posa aim nello spazio della mano) plausibile per
// un controller: raggio 20 gradi sopra .. 80 sotto il -Z della posa, yaw e
// rollio entro 25 gradi. Un frame_controller di SteamVR misurato coi Touch da'
// ~-22 (dentro), una posa di pinch delle mani o una posa rotta no.
inline bool AimInRawPlausible(const AimInRaw& a) {
    return a.pitch >= -80.0f && a.pitch <= 20.0f && std::fabs(a.yaw) <= 25.0f && std::fabs(a.roll) <= 25.0f;
}

// Logica della misura del raggio di puntamento di OpenXR (vr_openxr.cpp
// MeasureAimInRaw, sotto il lock del backend; i valori pubblicati restano
// atomici li'). Review WI-4a (round 1):
// - l'ultima misura valida si ricorda per profilo (nello stesso spazio della
//   mano): quando lo stesso profilo torna (controller spento e riacceso,
//   cambio di profilo e ritorno) vale subito, senza il secondo e piu' col
//   modello openxrgrip - 60 e il doppio scatto dell'arma;
// - ogni 30 s si rimisura (10 campioni): una misura diversa di oltre 0,3 gradi
//   sostituisce quella in uso (riga nel log solo se cambia). Le mani tracciate
//   che emulano i Touch col profilo touch_controller non restano cosi'
//   bloccate su una posa di pinch tenuta ferma 1 s.
struct AimRayMeasure {
    static constexpr int kSamples = 10;
    static constexpr double kSampleMs = 100.0, kRemeasureMs = 30000.0;
    static constexpr float kStableDeg = 0.2f, kChangeDeg = 0.3f;
    enum Event { kNone = 0, kMeasured = 1, kReused = 2, kChanged = 3, kImplausible = 4 };
    std::map<std::string, AimInRaw> cache;
    std::string profile;  // vuoto = nessun controller (o mani tracciate)
    AimInRaw cur;
    bool valid = false, warned = false;
    int n = 0;
    float s[kSamples][3] = {};
    double lastSampleMs = -1e18, doneMs = -1e18;
    // Tutto da capo (sessione chiusa).
    void Reset() {
        cache.clear();
        profile.clear();
        valid = warned = false;
        n = 0;
        lastSampleMs = doneMs = -1e18;
    }
    // Spazio della mano nuovo (openxrgrip): le misure vecchie non valgono piu'.
    void ResetSpace() {
        cache.clear();
        valid = false;
        n = 0;
    }
    // Profilo attuale ("" per nessuno o le mani tracciate).
    Event SetProfile(const std::string& p, double now) {
        if (p == profile) return kNone;
        profile = p;
        n = 0;
        warned = false;
        valid = false;
        auto it = p.empty() ? cache.end() : cache.find(p);
        if (it == cache.end()) return kNone;
        cur = it->second;
        valid = true;
        doneMs = now;
        return kReused;
    }
    // Serve un campione adesso? (misura in corso, nessuna misura, o 30 s dall'ultima)
    bool Due(double now) const {
        return !profile.empty() && now - lastSampleMs >= kSampleMs && (!valid || n > 0 || now - doneMs >= kRemeasureMs);
    }
    // Campione non disponibile (posa non valida): la serie riparte.
    void Drop(double now) {
        lastSampleMs = now;
        n = 0;
    }
    // Un campione (pitch, yaw, roll in gradi); alla decima, la media.
    Event Sample(double now, const float a[3], AimInRaw* outMean = nullptr) {
        lastSampleMs = now;
        if (n > 0 && (std::fabs(a[0] - s[0][0]) > kStableDeg || std::fabs(a[1] - s[0][1]) > kStableDeg ||
                      std::fabs(a[2] - s[0][2]) > kStableDeg))
            n = 0;  // non ferma: si ricomincia
        for (int k = 0; k < 3; ++k) s[n][k] = a[k];
        if (++n < kSamples) return kNone;
        AimInRaw r;
        r.pitch = r.yaw = r.roll = 0.0f;
        for (int i = 0; i < kSamples; ++i) {
            r.pitch += s[i][0] / kSamples;
            r.yaw += s[i][1] / kSamples;
            r.roll += s[i][2] / kSamples;
        }
        n = 0;
        doneMs = now;
        if (outMean) *outMean = r;
        if (!AimInRawPlausible(r)) return valid ? kNone : kImplausible;  // con una misura buona in uso la si tiene
        const bool changed = valid && (std::fabs(r.pitch - cur.pitch) > kChangeDeg || std::fabs(r.yaw - cur.yaw) > kChangeDeg ||
                                       std::fabs(r.roll - cur.roll) > kChangeDeg);
        if (valid && !changed) return kNone;
        const bool first = !valid;
        cur = r;
        valid = true;
        cache[profile] = r;
        return first ? kMeasured : kChanged;
    }
};

// Prodotto delle parti 3x3 (traslazione nulla) di due pose OpenVR.
inline void MulRot3(const float a[3][4], const float b[3][4], float out[3][4]) {
    float t[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i][j] = t[i][j];
        out[i][3] = 0.0f;
    }
}

// Rotazione attorno all'asse x (destra) / y (su) della posa, gradi: Rx > 0
// alza il -Z verso +Y; Ry > 0 gira il -Z verso -X (a sinistra).
inline void RotXDeg(float deg, float m[3][4]) {
    const float c = std::cos(deg * 0.01745329252f), s = std::sin(deg * 0.01745329252f);
    const float t[3][4] = {{1, 0, 0, 0}, {0, c, -s, 0}, {0, s, c, 0}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = t[i][j];
}
inline void RotYDeg(float deg, float m[3][4]) {
    const float c = std::cos(deg * 0.01745329252f), s = std::sin(deg * 0.01745329252f);
    const float t[3][4] = {{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = t[i][j];
}

// Parametri del telaio: raggio di puntamento nel raw, angolo della presa
// (gunangle con "holdmode normal", stockangle con "gunstock": gradi della canna
// sopra il raggio, attorno all'asse destro del raggio), gunyaw gia' col segno
// della mano fisica (+ = canna verso destra), mano fisica e palmo (PalmOf).
struct GunFrameParams {
    AimInRaw aim;
    float angleDeg = 0.0f;
    float yawDeg = 0.0f;
    bool left = false;
    bool gripFix = true;  // false = la posa e' gia' la grip ("openxrgrip off"): palmo nell'origine
};

// Raggio di puntamento nel raw (rotazione).
inline void AimRotInRaw(const AimInRaw& a, float m[3][4]) {
    PoseFromEuler(a.pitch * 0.01745329252f, a.yaw * 0.01745329252f, a.roll * 0.01745329252f, m);
}

// Rotazione dell'arma nel raw: raggio * Rx(angolo) * Ry(-gunyaw). L'elevazione
// della canna sul raggio e' esattamente l'angolo (la calibrazione B+Y lo misura
// cosi'); gunyaw gira l'arma attorno al proprio asse "su".
inline void GunRotInRaw(const GunFrameParams& p, float m[3][4]) {
    float aim[3][4], rx[3][4], ry[3][4], t[3][4];
    AimRotInRaw(p.aim, aim);
    RotXDeg(p.angleDeg, rx);
    RotYDeg(-p.yawDeg, ry);
    MulRot3(aim, rx, t);
    MulRot3(t, ry, m);
}

// Telaio dell'arma nel tracciamento dalla posa raw del controller: rotazione
// raw * GunRotInRaw e origine ruotata attorno al PALMO. Prima l'angolo ruotava
// attorno all'origine raw (10,2 cm avanti lungo il -Z dal palmo): cambiarlo
// spostava l'arma rispetto alla mano vera. Ora il punto del polso (dove la mod
// mette Hand_Right) resta dov'era col telaio raw (angolo 0 nel raw = la
// posizione di prima, invariata) e ruota col telaio attorno al palmo: il
// punto dell'arma che stava sul palmo vero ci resta per ogni angolo, e la
// linea della canna passa sempre per il palmo.
inline void GunFrameFromRaw(const float raw[3][4], const GunFrameParams& p, float out[3][4]) {
    float g[3][4];
    GunRotInRaw(p, g);
    MulRot3(raw, g, out);
    float palm[3];
    PalmOf(raw, p.left, p.gripFix, palm);
    // origine = palmo + R_arma * (origine raw - palmo, nel raw) = palmo - R_arma * o
    const float o[3] = {p.gripFix ? (p.left ? kGripOriginX : -kGripOriginX) : 0.0f, p.gripFix ? kGripOriginY : 0.0f,
                        p.gripFix ? kGripOriginZ : 0.0f};
    for (int i = 0; i < 3; ++i) out[i][3] = palm[i] - (out[i][0] * o[0] + out[i][1] * o[1] + out[i][2] * o[2]);
}

// Calibrazione B+Y (WI-4a): direzione della linea di mira (avanti della testa,
// spazio del tracciamento) nel telaio di puntamento del controller.
// elevazione = atan2(h . su, h . avanti), di lato = atan2(h . destra, h .
// avanti), gradi. Prima: beccheggio della testa - beccheggio assoluto del raw,
// applicato pero' come rotazione attorno all'asse destro del controller:
// giusto solo a rollio 0 (col calcio inclinato la canna andava di lato).
inline void SightLineInAim(const float raw[3][4], const AimInRaw& a, const float headFwd[3], float* elevDeg, float* latDeg) {
    float aim[3][4], m[3][4];
    AimRotInRaw(a, aim);
    MulRot3(raw, aim, m);
    float f = 0.0f, u = 0.0f, r = 0.0f;
    for (int i = 0; i < 3; ++i) {
        f -= headFwd[i] * m[i][2];
        u += headFwd[i] * m[i][1];
        r += headFwd[i] * m[i][0];
    }
    *elevDeg = std::atan2(u, f) * 57.2957795f;
    *latDeg = std::atan2(r, f) * 57.2957795f;
}

// Direzione (heading) del raggio di puntamento per gli usi "di direzione"
// (review WI-4a, round 1): "movedir hand". Lo yaw esatto del raggio e' mal
// condizionato con il raggio quasi verticale (mano rilassata: raw -Z 40-50
// gradi in giu' -> raggio a 80-90 gradi in giu'): 2 gradi di tremolio lo
// spostano di ~11 gradi a -80, ~22 a -85, 45+ a -88, e passa per la
// verticale girando di 180. BlendedYaw (avanti + |avanti.y| x su) resta
// stabile li', come lo yaw di direzione della testa (regola del WI-1: lo yaw
// esatto solo per la vista e la mira, quello miscelato per le direzioni).
inline float PointingHeadingYaw(const float raw[3][4], const AimInRaw& a) {
    float aim[3][4], m[3][4];
    AimRotInRaw(a, aim);
    MulRot3(raw, aim, m);
    return BlendedYaw(m);
}

// Torretta (review WI-4a, round 1): con la canna oltre 70 gradi dall'orizzonte
// lo yaw esatto del telaio dell'arma non dice piu' dove si mira (tremolio di
// pochi gradi = decine di gradi di yaw): la torretta tiene il suo yaw (nessun
// passo) finche' la canna non torna sotto il limite. Nessuna torretta del
// gioco arriva a quei beccheggi (M2 +21/-23, elicottero +18/-25).
constexpr float kTurretYawHoldPitchDeg = 70.0f;
inline bool TurretYawHeld(float barrelPitchDeg) { return std::fabs(barrelPitchDeg) > kTurretYawHoldPitchDeg; }

// Limiti delle chiavi (gradi): si limita l'angolo della presa, non l'aimpitch
// di prima (+-45 sul raw: un -10 sul raggio era -49,4 e tornava a 0, canna 39
// gradi in su). B+Y rifiuta fuori da questi limiti.
constexpr float kGunAngleMin = -45.0f, kGunAngleMax = 45.0f;
constexpr float kStockAngleMin = -60.0f, kStockAngleMax = 90.0f;
constexpr float kGunYawMax = 30.0f;
inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Migrazione di "aimpitch" (configversion 11, WI-4a; era la 10 nella copia B). aimpitch era l'angolo
// della canna sul raw -Z dei Touch. 0 (mai calibrato) o fuori da +-45 (la
// calibrazione sbagliata della 0.9.3, il parser la azzerava) -> "holdmode
// normal" + "gunangle 0" (canna lungo il raggio: la correzione del bug 6); X
// diverso da 0 (B+Y col calcio) -> "holdmode gunstock" + "stockangle X + 39.4"
// (stessa canna di prima: -24 -> 15.4). Con i commenti delle chiavi nuove.
// Le 4 righe del commento di aimpitch nell'ini distribuito fino al 25/10
// diventano il commento di holdmode (stessa posizione, la riga sotto e'
// "holdmode ..." dopo la migrazione).
constexpr ConfigChange kAimpitchCommentChanges[] = {
    {kTesterFixesConfigVersion, "# Pitch correction of the controller ray (degrees), only for \"aim hand\".",
     "# Controller hold: \"normal\" = bare controllers, the gun points where the"},
    {kTesterFixesConfigVersion, "# Gunstock: aim the stock at something far away, looking at it through the",
     "# controller points (like the menu laser), tilted by gunangle; \"gunstock\" ="},
    {kTesterFixesConfigVersion, "# sights in the middle of your view, and hold B + Y together for 1 second: the",
     "# controllers on a stock, tilted by stockangle (B + Y held 1 s measures it)."},
    {kTesterFixesConfigVersion, "# angle is measured and saved here.", "# Menu: Basic > Controller hold; Basic > Gun angle follows the hold."}};
constexpr const char* kGunAngleComment1 = "# Gun angle (normal hold): tilt of the gun in degrees above where the controller";
constexpr const char* kGunAngleComment2 = "# points (0 = along the menu laser, + = barrel up). Menu: Basic > Gun angle.";
constexpr const char* kStockAngleComment1 = "# Stock angle (gunstock hold), same meaning; B + Y held 1 s measures it.";
// Regressione finale (review WI-4a round 2): sotto 5 gradi non e' una
// calibrazione B+Y col calcio ma un ritocco dall'Advanced (0,05 gradi per
// click, passo fisso finche' il menu resta aperto: 5 gradi = 100 click). B+Y
// misura l'angolo tra la linea di mira sul calcio e il raw -Z dei Touch:
// -39,4 con la presa da pistola, -24 sul calcio dello sviluppatore; sotto 5
// l'anello del controller punterebbe lungo la canna, presa che nessun calcio
// usa. Un ritocco tenuto come calcio lasciava la canna 39 gradi in su (bug 6
// non corretto) e la presa gunstock: diventa la presa normale, come 0.
constexpr double kAimpitchStockMinDeg = 5.0;
inline bool MigrateAimpitchLine(const std::string& line, std::vector<std::string>* out) {
    std::istringstream in(NormalizeConfigLine(line));
    std::string k;
    double v = 0.0;
    in >> k;
    if (k != "aimpitch") return false;
    if (!(in >> v) || !std::isfinite(v) || v < -45.0 || v > 45.0) v = 0.0;
    if (std::fabs(v) < kAimpitchStockMinDeg) {
        out->push_back("holdmode normal");
        out->push_back(kGunAngleComment1);
        out->push_back(kGunAngleComment2);
        out->push_back("gunangle 0");
        return true;
    }
    out->push_back("holdmode gunstock");
    out->push_back(kGunAngleComment1);
    out->push_back(kGunAngleComment2);
    out->push_back("gunangle 0");
    out->push_back(kStockAngleComment1);
    out->push_back("stockangle " + FormatConfigNumber(ClampF(static_cast<float>(v - kAimPitchInRawDeg), kStockAngleMin, kStockAngleMax), 1));
    return true;
}

// Review WI-4b round 1: "twohand on <cm> [s]" e "leftgrip on <cm>" scritte a
// mano (la vecchia riga AIM diceva "two-handed grip needs N (twohand on <cm>)")
// restavano negli ini dalla versione 8 in poi (RemoveRetiredKeys solo sotto la
// 8) e con la presa del WI-4b cambiavano significato in silenzio: 12-16 cm di
// raggio dal palmo (il doppio dell'area), un vecchio 0,2-0,5 s letto come
// 0,2-0,5 Hz di taglio (ritardo forte). Configversion 11 (10 nella copia B): resta solo on/off.
inline bool MigrateTwoHandLine(const std::string& line, std::vector<std::string>* out) {
    std::istringstream in(NormalizeConfigLine(line));
    std::string k, v, extra;
    in >> k >> v;
    if ((k != "twohand" && k != "leftgrip") || v.empty() || !(in >> extra)) return false;
    out->push_back(k + " " + v);
    return true;
}

// --- Filtro 1-euro (Casiez et al. 2012) ---------------------------------------
//
// Passa-basso con taglio che cresce con la velocita': fermo filtra molto il
// tremore, in movimento quasi niente ritardo. minCutoff in Hz, beta in
// (unita' del segnale)/s^-1, dCutoff in Hz per la derivata. Step con dt in
// secondi; dt <= 0 o il primo campione azzerano lo stato.
struct OneEuro {
    float minCutoff = 1.0f, beta = 0.0f, dCutoff = 1.0f;
    bool init = false;
    float x = 0.0f, dx = 0.0f;
    static float Alpha(float cutoff, float dt) {
        const float tau = 1.0f / (2.0f * kPi * (cutoff > 1e-4f ? cutoff : 1e-4f));
        return 1.0f / (1.0f + tau / dt);
    }
    void Reset() { init = false; }
    float Step(float v, float dt) {
        if (!init || !(dt > 0.0f)) {
            init = true;
            x = v;
            dx = 0.0f;
            return x;
        }
        const float rawDx = (v - x) / dt;
        dx += (rawDx - dx) * Alpha(dCutoff, dt);
        const float cutoff = minCutoff + beta * std::fabs(dx);
        x += (v - x) * Alpha(cutoff, dt);
        return x;
    }
};

// --- Testa: angoli in unita' Unreal (WI-1, 25/10) -----------------------------
//
// Radianti -> unita' Unreal (65536 = giro) arrotondati, non troncati: la
// troncatura dava fino a 0,0055 gradi per angolo, insieme quasi 0,01 gradi
// tra vista disegnata e testa (criterio < 0,01 gradi).
inline int32_t RadiansToUnrealRounded(float rad) {
    return static_cast<int32_t>(std::lround(static_cast<double>(rad) * (65536.0 / 6.283185307179586)));
}

// --- Salto della vista (dissolvenza dal nero, WI-1) ------------------------------
//
// Prima: salto = piu' di 80 unita' in un tick e oltre 3 volte il tick prima
// + 40, senza il tempo. In elicottero (3000 unita'/s) ogni tick lungo (20-83
// ms) sembrava un teletrasporto: nero di 0,3 s fino a 7 volte al secondo
// (log del tester 23/10). Ora si confrontano le velocita' (unita'/s) e su un
// mezzo conta solo il teletrasporto vero (oltre 2000 unita' in un tick).
struct ViewJumpDetector {
    float prevSpeed = 0.0f;  // unita'/s del tick prima
    bool have = false;
    enum Result { kNone = 0, kJump = 1, kTeleport = 2 };
    // dj: spostamento della vista in questo tick (unita'), dt: durata del tick
    // (s), mount: Walker su un mezzo (base che si muove).
    int Step(float dj, float dt, bool mount, float* speedOut = nullptr) {
        const float d = dt < 0.002f ? 0.002f : (dt > 0.25f ? 0.25f : dt);
        const float speed = dj / d;
        if (speedOut) *speedOut = speed;
        const float prev = prevSpeed;
        const bool had = have;
        prevSpeed = speed;
        have = true;
        if (dj > 2000.0f) return kTeleport;
        if (mount || !had || dj <= 80.0f) return kNone;
        // A piedi: oltre lo spostamento possibile camminando o correndo nel
        // tempo del tick (1200 unita'/s + 40, sul tempo vero, non limitato a
        // 0,25 s), o 1,5 volte la velocita' di prima se gia' piu' alta (moto
        // veloce continuo: caduta, scivolata scriptata). Review WI-1: con la
        // sola regola delle velocita' (3 volte + 3600 unita'/s) un salto di
        // 150-800 unita' in un tick lungo (riposizione scriptata o checkpoint
        // con uno scatto) passava senza nero.
        const float walk = 1.5f * prev > kWalkSpeed ? 1.5f * prev : kWalkSpeed;
        if (dj > walk * (dt > 0.0f ? dt : 0.0f) + 40.0f) return kJump;
        // 3 volte la velocita' di prima piu' 3600 unita'/s (le 40 unita' per
        // tick di prima a 90 Hz).
        return speed > 3.0f * prev + 3600.0f ? kJump : kNone;
    }
    static constexpr float kWalkSpeed = 1200.0f;  // unita'/s, oltre la corsa di Walker
};

// --- Torretta: passo di yaw e parte rifiutata (WI-1) ------------------------------
//
// Passo verso il bersaglio: errore avvolto a +-180 gradi (prima no: con
// riferimento e controller ai due lati dei 180 gradi del tracciamento il
// passo era di ~105 gradi nel verso sbagliato), guadagno dal tempo (costante
// 30 ms) ma al massimo 0,5 per tick (0,94 in un tick di 83 ms).
inline float TurretGain(float dt) {
    const float g = 1.0f - std::exp(-(dt > 0.0f ? dt : 0.0f) / 0.03f);
    return g > 0.5f ? 0.5f : g;
}
inline int32_t TurretYawStep(int32_t errUnits, float dt) {
    const int16_t wrapped = static_cast<int16_t>(static_cast<uint16_t>(errUnits & 0xFFFF));
    return static_cast<int32_t>(std::lround(wrapped * static_cast<double>(TurretGain(dt))));
}
// Quanto del passo scritto il gioco ha rifiutato (unita'), al netto della
// base: il gioco porta lo yaw del PlayerController con la base (elicottero)
// di baseFactor (1 misurato in volo, 25/10), quindi
//   pcNow - pcAfterWrite = baseFactor * baseDelta - rifiutato.
inline int32_t TurretRefused(int32_t pcAfterWrite, int32_t pcNow, int32_t baseDelta, float baseFactor = 1.0f) {
    const int16_t moved = static_cast<int16_t>(static_cast<uint16_t>((pcNow - pcAfterWrite) & 0xFFFF));
    return static_cast<int32_t>(std::lround(baseFactor * static_cast<int16_t>(static_cast<uint16_t>(baseDelta & 0xFFFF)))) - moved;
}

// Parte del passo eseguita (da togliere dalla vista spostando il riferimento
// della testa): passo meno rifiutato, al massimo il passo e nel suo verso
// (quello che il gioco fa in piu', come un riporto oltre il limite, non e' del
// giocatore).
inline int32_t TurretExecuted(int32_t step, int32_t refused) {
    int32_t ex = step - refused;
    if (step >= 0) return ex < 0 ? 0 : (ex > step ? step : ex);
    return ex > 0 ? 0 : (ex < step ? step : ex);
}

// Termine della base nel conto (review WI-1): 1 = il gioco porta il
// PlayerController con la base (elicottero del prologo, misurato), 0 quando la
// rotazione della base segue il passo scritto (stesso verso, entro il 25% o 2
// unita'): la base e' la torretta stessa e con 1 tutto il passo eseguito
// risulterebbe rifiutato (vista che gira col passo). Se la base portasse
// davvero il PlayerController e girasse per caso come il passo, con 0 la parte
// eseguita viene comunque il passo (passo + base, limitato al passo): nessun
// errore in entrambi i casi, lontano dal limite del gioco.
inline float TurretBaseFactor(int32_t step, int32_t baseDelta) {
    const int16_t b = static_cast<int16_t>(static_cast<uint16_t>(baseDelta & 0xFFFF));
    if (step == 0 || b == 0 || (b > 0) != (step > 0)) return 1.0f;
    const int32_t tol = std::abs(step) / 4 > 2 ? std::abs(step) / 4 : 2;
    return std::abs(b - step) <= tol ? 0.0f : 1.0f;
}

// Termine della base con la regola del gioco (review 2 WI-1): UpdateTurretAiming
// (0x00f5f8e0) aggiunge il modificatore della base solo se Pawn.Base (+0xAC) ha
// Physics (+0x94) == PHYS_Interpolating (7), come l'InterpActor dell'elicottero
// del prologo. Altrimenti (torretta a terra, base = la torretta stessa o il
// pavimento) la base non porta il PlayerController: 0, anche con una base che
// segue il passo in ritardo o a velocita' limitata (TurretBaseFactor vede solo
// la base che lo segue nello stesso tick: col modello in ritardo la vista
// girava di 18,8 / 35,8 gradi). TurretBaseFactor resta come guardia in piu' per
// le basi mobili. basePhysics < 0 = non letto: la regola di prima.
// baseIsTurret: la base e' l'attore della torretta stessa (Pawn.m_turretInteraction
// +0x1184 -> m_turretActor +0x224): 0 anche se ha Physics 7 (misurato 26/10:
// lo YTurretActor della M2E2 ha Physics 7; li' la base del Pawn e' Terrain).
constexpr int kPhysInterpolating = 7;
inline float TurretBaseTerm(int basePhysics, int32_t step, int32_t baseDelta, bool baseIsTurret = false) {
    if (baseIsTurret) return 0.0f;
    if (basePhysics >= 0 && basePhysics != kPhysInterpolating) return 0.0f;
    return TurretBaseFactor(step, baseDelta);
}

// --- Torretta: modi della presa, manubrio, limiti (WI-4c, bug 1 del tester) ---
//
// "1st scene with turret would feel more natural if both hands controlled the
// turret" e (utente) "per le torrette a due mani crea una modalita' GUNSTOCK e
// una NORMAL, perche' i sistemi di mira sono diversi".

// Yaw fermo con la canna quasi verticale, con isteresi (review WI-4a, round 2):
// si ferma oltre 70 gradi dall'orizzonte e riparte solo sotto 65. Senza
// isteresi, con la canna sul limite il tremolio della mano alternava fermo e
// libero a ogni tick (la torretta poteva vibrare).
constexpr float kTurretYawFreePitchDeg = 65.0f;
struct TurretYawHold {
    bool held = false;
    // true = yaw fermo in questo tick; changed = cambio di stato (riga TURRET).
    bool Step(float barrelPitchDeg, bool* changed = nullptr) {
        const float a = std::fabs(barrelPitchDeg);
        const bool was = held;
        if (!held && a > kTurretYawHoldPitchDeg) held = true;
        else if (held && a < kTurretYawFreePitchDeg) held = false;
        if (changed) *changed = held != was;
        return held;
    }
    void Reset() { held = false; }
};

// Modo della presa sulla torretta: "turretgrip auto|normal|gunstock" (0 auto,
// 1 normal, 2 gunstock). auto segue "holdmode" (0 normal, 1 gunstock).
// Ritorna 0 = NORMAL (manubrio, maniglie), 1 = GUNSTOCK (calcio).
inline int ResolveTurretGrip(int turretGrip, int holdMode) {
    if (turretGrip == 1) return 0;
    if (turretGrip == 2) return 1;
    return holdMode == 1 ? 1 : 0;
}

// Yaw (radianti, convenzione di ExactEuler: atan2(avanti.x, -avanti.z), +
// verso destra) della direzione perpendicolare alla linea orizzontale fra i due
// palmi, dalla parte dell'avanti: avanti = su x (destra - sinistra). Solo
// posizioni: vale qualunque sia il perno (torretta davanti, busto dietro) e
// non dipende dall'orientamento dei controller (VDXR / Steam Link).
inline float HandlebarYawFromPalms(const float left[3], const float right[3]) {
    const float dx = right[0] - left[0], dz = right[2] - left[2];
    return std::atan2(dz, dx);
}

// Presa a manubrio (NORMAL): le due mani sulle maniglie, yaw dalla linea fra i
// palmi, beccheggio = media dei beccheggi esatti dei due telai (raggio di
// puntamento + gunangle, nessun angolo del calcio). Si aggancia con i due grip
// stretti, i palmi a 25-90 cm, la linea quasi laterale rispetto all'avanti dei
// controller (la sinistra a sinistra) e poco inclinata; si tiene con margini
// piu' larghi (isteresi). Filtro 1-euro su yaw e beccheggio; passaggio da e
// verso la mira con una mano in 100 ms (niente scatti del bersaglio).
struct HandlebarParams {
    float engageMinM = 0.25f, engageMaxM = 0.90f;  // distanza dei palmi per agganciare
    float holdMinM = 0.20f, holdMaxM = 1.00f;      // ... per restare agganciati
    float engageAlignDeg = 35.0f, holdAlignDeg = 50.0f;  // avanti della linea vs avanti dei controller
    float engageSlopeDeg = 40.0f, holdSlopeDeg = 50.0f;  // inclinazione della linea fra i palmi
    float blendSec = 0.10f;                        // una mano <-> manubrio
    float minCutoffHz = 1.0f, beta = 4.0f, dCutoffHz = 1.0f;  // 1-euro (radianti)
    // dt massimo di un passo (review WI-4c round 1): hooks limitava dt a 0,1 s
    // = blendSec, e dopo un buco (seconda torretta coi grip gia' stretti,
    // controller perso >= 100 ms, scatto del gioco, cambio di "Turret grip")
    // il peso andava 0 -> 1 in UN tick: la mira saltava di tutto lo scarto
    // manubrio / polso (9,1 gradi in un tick nella corsa "normal_again").
    float maxDtSec = 1.0f / 30.0f;
};
// Parametri dalle chiavi Avanzate (review WI-4c round 1): aggancio a
// minCm-maxCm, tenuta 5 cm sotto e 10 sopra; allineamento e pendenza +15 / +10
// per la tenuta (i valori di prima con i predefiniti 25 / 90 / 35 / 40).
inline HandlebarParams HandlebarParamsFor(float minCm, float maxCm, float alignDeg, float slopeDeg, float blendSec, float smoothHz,
                                          float beta) {
    HandlebarParams p;
    p.engageMinM = minCm * 0.01f;
    p.engageMaxM = (maxCm > minCm + 5.0f ? maxCm : minCm + 5.0f) * 0.01f;
    p.holdMinM = (minCm - 5.0f > 5.0f ? minCm - 5.0f : 5.0f) * 0.01f;
    p.holdMaxM = p.engageMaxM + 0.10f;
    p.engageAlignDeg = alignDeg;
    p.holdAlignDeg = alignDeg + 15.0f < 89.0f ? alignDeg + 15.0f : 89.0f;
    p.engageSlopeDeg = slopeDeg;
    p.holdSlopeDeg = slopeDeg + 10.0f < 89.0f ? slopeDeg + 10.0f : 89.0f;
    p.blendSec = blendSec;
    p.minCutoffHz = smoothHz;
    p.beta = beta;
    return p;
}
struct HandlebarIn {
    float dt = 0.0f;
    bool allowed = false;          // torretta mirata col controller, presa NORMAL
    bool rightValid = false, leftValid = false;  // mani FISICHE (destra, sinistra)
    float palmR[3] = {}, palmL[3] = {};          // palmi, metri nello spazio del tracciamento
    float fwdR[3] = {}, fwdL[3] = {};            // avanti dei due telai (raggio + gunangle)
    float pitchR = 0.0f, pitchL = 0.0f;          // beccheggi esatti dei due telai (radianti)
    bool gripR = false, gripL = false;
    float oneYaw = 0.0f, onePitch = 0.0f;        // mira con una mano (radianti, tracciamento)
};
struct HandlebarOut {
    bool engaged = false;           // posa del manubrio valida in questo tick
    float weight = 0.0f;            // 0 = una mano, 1 = manubrio
    float yaw = 0.0f, pitch = 0.0f; // mira risultante (radianti, tracciamento)
    float barYaw = 0.0f, barPitch = 0.0f;  // manubrio filtrato
    float sepM = 0.0f, alignDeg = 0.0f, slopeDeg = 0.0f;
    const char* why = "";           // perche' non agganciato (o "ok")
    int event = 0;                  // 1 agganciato, 2 rilasciato
};
struct Handlebar {
    HandlebarParams p;
    bool engaged = false;
    float weight = 0.0f;
    bool haveBar = false;
    float barYaw = 0.0f, barPitch = 0.0f;  // ultimo manubrio valido (filtrato)
    OneEuro fYaw, fPitch;
    void Reset() {
        engaged = false;
        weight = 0.0f;
        haveBar = false;
        fYaw.Reset();
        fPitch.Reset();
    }
    // Ricentramento del runtime assorbito (hooks.cpp, WI-2): lo yaw del
    // tracciamento cresce di "yaw" e quello filtrato gira con lui (review
    // WI-4b round 1, H2: prima il filtro inseguiva il salto e la torretta lo seguiva).
    void ShiftYaw(float yaw) {
        if (!haveBar) return;
        barYaw = WrapPi(barYaw + yaw);
        fYaw.x = barYaw;  // stato del filtro nello stesso giro dello yaw avvolto
    }
    // Prova della posa (senza stato): distanza, allineamento, inclinazione.
    static bool PoseOk(const HandlebarIn& in, const HandlebarParams& p, bool hold, float* sep, float* alignDeg, float* slopeDeg,
                       const char** why) {
        const float d[3] = {in.palmR[0] - in.palmL[0], in.palmR[1] - in.palmL[1], in.palmR[2] - in.palmL[2]};
        const float horiz = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        *sep = std::sqrt(horiz * horiz + d[1] * d[1]);
        *slopeDeg = std::atan2(std::fabs(d[1]), horiz) * 57.2957795f;
        // Avanti dei controller: media dei vettori unitari orizzontali (non
        // degli yaw: +170 e -170 farebbero 0).
        float fx = 0.0f, fz = 0.0f;
        for (const float* f : {in.fwdR, in.fwdL}) {
            const float h = std::sqrt(f[0] * f[0] + f[2] * f[2]);
            if (h > 1e-4f) {
                fx += f[0] / h;
                fz += f[2] / h;
            }
        }
        const float fh = std::sqrt(fx * fx + fz * fz);
        float align = 180.0f;
        if (horiz > 1e-4f && fh > 1e-4f) {
            // Avanti della linea = su x d = (dz, 0, -dx).
            const float lx = d[2] / horiz, lz = -d[0] / horiz;
            float c = (lx * fx + lz * fz) / fh;
            c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
            align = std::acos(c) * 57.2957795f;
        }
        *alignDeg = align;
        const float mn = hold ? p.holdMinM : p.engageMinM, mx = hold ? p.holdMaxM : p.engageMaxM;
        const float al = hold ? p.holdAlignDeg : p.engageAlignDeg, sl = hold ? p.holdSlopeDeg : p.engageSlopeDeg;
        if (*sep < mn) *why = "hands too close";
        else if (*sep > mx) *why = "hands too far apart";
        else if (align > 90.0f) *why = "hands crossed (left hand on the right)";
        else if (align > al) *why = "hands not side by side";
        else if (*slopeDeg > sl) *why = "one hand much higher";
        else return true;
        return false;
    }
    HandlebarOut Step(const HandlebarIn& inArg) {
        HandlebarIn in = inArg;
        in.dt = in.dt > p.maxDtSec ? p.maxDtSec : (in.dt > 0.0f ? in.dt : 0.0f);  // review WI-4c: niente peso 0 -> 1 in un passo
        HandlebarOut o;
        const bool tracked = in.allowed && in.rightValid && in.leftValid;
        bool pose = false;
        if (tracked) pose = PoseOk(in, p, engaged, &o.sepM, &o.alignDeg, &o.slopeDeg, &o.why);
        else o.why = !in.allowed ? "not a NORMAL turret grip" : "a controller is not tracked";
        const bool grips = in.gripR && in.gripL;
        if (tracked && pose && !grips) o.why = "both grips not held";
        const bool now = tracked && pose && grips;
        if (now) {
            // Linea fra i palmi -> yaw, media dei beccheggi; filtro sullo yaw
            // srotolato attorno all'ultimo valore (nessun salto a +-180).
            const float raw = HandlebarYawFromPalms(in.palmL, in.palmR);
            const float ref = haveBar ? barYaw : raw;
            const float unwrapped = ref + WrapPi(raw - ref);
            if (!haveBar) {
                fYaw.Reset();
                fPitch.Reset();
            }
            fYaw.minCutoff = fPitch.minCutoff = p.minCutoffHz;
            fYaw.beta = fPitch.beta = p.beta;
            fYaw.dCutoff = fPitch.dCutoff = p.dCutoffHz;
            const float y = fYaw.Step(unwrapped, in.dt);
            barYaw = WrapPi(y);
            fYaw.x -= y - barYaw;  // stato del filtro nello stesso giro dello yaw avvolto
            barPitch = fPitch.Step(0.5f * (in.pitchR + in.pitchL), in.dt);
            haveBar = true;
            o.why = "ok";
        }
        if (now && !engaged) o.event = 1;
        if (!now && engaged) o.event = 2;
        engaged = now;
        // Peso: sale e scende in blendSec (smoothstep sull'uscita).
        const float rate = p.blendSec > 1e-3f ? (in.dt > 0.0f ? in.dt : 0.0f) / p.blendSec : 1.0f;
        weight += now ? rate : -rate;
        weight = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
        if (!in.allowed) weight = 0.0f;
        if (weight <= 0.0f && !now) {
            haveBar = false;  // ripresa da capo: il filtro riparte dal manubrio nuovo
        }
        const float w = weight * weight * (3.0f - 2.0f * weight);
        o.engaged = now;
        o.weight = w;
        o.barYaw = barYaw;
        o.barPitch = barPitch;
        if (haveBar && w > 0.0f) {
            o.yaw = WrapPi(in.oneYaw + w * WrapPi(barYaw - in.oneYaw));
            o.pitch = in.onePitch + w * (barPitch - in.onePitch);
        } else {
            o.yaw = in.oneYaw;
            o.pitch = in.onePitch;
        }
        return o;
    }
};

// Torretta a terra tenuta per le maniglie con le mani vicine (visore 27/10).
// L'utente tiene i controller quasi verticali a 8-25 cm, senza grip: il
// manubrio qui sopra non si aggancia e la mira era il raggio del solo controller
// dell'arma (col controller verticale girava di decine di gradi col polso; "devo
// fare una X tra i due controller per andare a destra"). Poi, mirando con
// l'orientamento delle due mani: "gli assi sono invertiti" - l'utente si
// aspetta una mitragliatrice vera, con le maniglie DIETRO il perno: mani a
// sinistra -> canna a destra, mani giu' -> canna su.
// Modello a perno: all'aggancio il perno P sta pivotM metri davanti al punto
// medio dei palmi lungo la mira di adesso (niente salto); poi la mira e' la
// direzione dal punto medio dei palmi a P. Conta solo dove stanno le mani, non
// come girano i polsi. Passaggio da e verso la mira con una mano in blendSec;
// Reanchor (ricentramento del runtime, cambio di torretta) rimette il perno
// davanti alle mani lungo la mira del tick (niente salto).
// Spazio OpenVR: x destra, y su, -z avanti; yaw = atan2(x, -z), pitch = asin(y)
// come ExactEuler.
struct TurretHandlesOut {
    bool engaged = false;
    float weight = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;           // mira risultante (radianti, tracciamento)
    float pivotYaw = 0.0f, pivotPitch = 0.0f;  // mira dal perno (ultima valida)
    float lever = 0.0f;                        // distanza palmi-perno (m)
    int event = 0;                             // 1 agganciato, 2 rilasciato
};
struct TurretHandles {
    bool engaged = false, haveP = false, haveOut = false;
    float weight = 0.0f;
    float P[3] = {0.0f, 0.0f, 0.0f};
    float pivYaw = 0.0f, pivPitch = 0.0f;  // ultima mira valida dal perno
    float outYaw = 0.0f, outPitch = 0.0f;  // ultima mira data
    void Reset() {
        engaged = haveP = haveOut = false;
        weight = 0.0f;
    }
    // Ricentramento del runtime assorbito (hooks.cpp, WI-2): il riferimento dello
    // yaw cresce di yawShift; la mira tenuta gira con lui (come Handlebar::ShiftYaw)
    // e il perno torna davanti alle mani nel nuovo spazio.
    void Reanchor(float yawShift = 0.0f) {
        if (haveOut) outYaw = WrapPi(outYaw + yawShift);
        haveP = false;
    }
    TurretHandlesOut Step(bool want, const float palmA[3], const float palmB[3], float oneYaw, float onePitch, float dt,
                          float pivotM = 0.35f, float blendSec = 0.15f) {
        TurretHandlesOut o;
        float mid[3];
        for (int i = 0; i < 3; ++i) mid[i] = 0.5f * (palmA[i] + palmB[i]);
        if (!haveOut) {
            outYaw = oneYaw;
            outPitch = onePitch;
            haveOut = true;
        }
        if (want && (!engaged || !haveP)) {
            // Perno davanti alle mani lungo la mira data adesso.
            const float cy = std::cos(outYaw), sy = std::sin(outYaw), cp = std::cos(outPitch), sp = std::sin(outPitch);
            const float d[3] = {sy * cp, sp, -cy * cp};
            const float L = pivotM > 0.05f ? pivotM : 0.05f;
            for (int i = 0; i < 3; ++i) P[i] = mid[i] + L * d[i];
            haveP = true;
            pivYaw = outYaw;
            pivPitch = outPitch;
        }
        if (haveP) {
            const float v[3] = {P[0] - mid[0], P[1] - mid[1], P[2] - mid[2]};
            const float h = std::sqrt(v[0] * v[0] + v[2] * v[2]);
            o.lever = std::sqrt(h * h + v[1] * v[1]);
            if (o.lever > 0.05f) {  // mani sul perno (5 cm): resta l'ultima mira
                pivYaw = std::atan2(v[0], -v[2]);
                pivPitch = std::atan2(v[1], h);
            }
        }
        if (!want) haveP = false;  // riagganciando il perno torna davanti alle mani
        const bool eng = want && haveP;
        if (eng != engaged) o.event = eng ? 1 : 2;
        engaged = eng;
        const float rate = blendSec > 0.0f ? dt / blendSec : 1.0f;
        weight = eng ? (std::min)(1.0f, weight + rate) : (std::max)(0.0f, weight - rate);
        o.engaged = eng;
        o.weight = weight;
        o.pivotYaw = pivYaw;
        o.pivotPitch = pivPitch;
        o.yaw = WrapPi(oneYaw + weight * WrapPi(pivYaw - oneYaw));
        o.pitch = onePitch + weight * (pivPitch - onePitch);
        outYaw = o.yaw;
        outPitch = o.pitch;
        return o;
    }
};

// Limiti della torretta (gradi) nel telaio dell'attore della torretta
// (YTurretActor: Pawn.m_turretInteraction +0x1184 -> m_turretActor +0x224; la
// sua Rotation e' il supporto, non gira con la mira). La mod non spinge oltre.
// Il gioco limita le rotazioni dello stick ma non riporta indietro le scritture
// assolute del PlayerController, o solo da un lato (WI-1: elicottero a +120 ~27
// gradi ripresi per tick, a -120 la torretta arrivava a -175 dall'elicottero;
// M2E2 a +-60: 109 gradi). Misurato con lo stick (WI-4c, 26/10, minigun del
// prologo, "turretaim stick"): yaw +50,0 / -65,0 e beccheggio in su +18,0 nel
// telaio dell'attore, rollio dell'elicottero compreso (i valori di
// CMN_HeliGunCam). In giu' lo stick arrivava a -32,3 ancora in moto (zona
// morbida), ma con le scritture assolute la mira del gioco (Pawn +0x14A8) si
// ferma a -25,0 nel telaio dell'attore e il gioco riprende il resto a ogni tick
// (prima corsa del WI-4c, limite messo a 32: ~2 gradi rifiutati per tick).
struct TurretLimits {
    float yawLeftDeg = 55.0f, yawRightDeg = 45.0f;  // + = destra
    float pitchDownDeg = 23.0f, pitchUpDeg = 21.0f;
};
constexpr TurretLimits kTurretLimitsFlight = {65.0f, 50.0f, 25.0f, 18.0f};  // minigun dell'elicottero (misurati)
constexpr TurretLimits kTurretLimitsGround = {55.0f, 45.0f, 23.0f, 21.0f};  // M2 e torrette a terra (CMN_TurretCam)

// Direzione (pitch, yaw in unita' Unreal) limitata nel telaio del supporto
// (FRotator del supporto in unita'). Ritorna true se e' stata limitata; out*
// = direzione limitata (yaw vicino a quello dato, stesso giro).
inline bool ClampAimInMount(int32_t pitch, int32_t yaw, const int32_t mount[3], const TurretLimits& l, int32_t* outPitch,
                            int32_t* outYaw, float* localYawDeg = nullptr, float* localPitchDeg = nullptr) {
    const double u = 2.0 * 3.14159265358979 / 65536.0;
    const float p = static_cast<float>(pitch * u), y = static_cast<float>(yaw * u);
    const float d[3] = {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
    float f[3], r[3], up[3];
    UeAxesFromRotator(static_cast<float>(mount[0] * u), static_cast<float>(mount[1] * u), static_cast<float>(mount[2] * u), f, r, up);
    const float lx = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    const float ly = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
    const float lz = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    const float k = 57.2957795f;
    float ay = std::atan2(ly, lx) * k, ap = std::atan2(lz, std::sqrt(lx * lx + ly * ly)) * k;
    if (localYawDeg) *localYawDeg = ay;
    if (localPitchDeg) *localPitchDeg = ap;
    const float cy = ClampF(ay, -l.yawLeftDeg, l.yawRightDeg), cp = ClampF(ap, -l.pitchDownDeg, l.pitchUpDeg);
    *outPitch = pitch;
    *outYaw = yaw;
    if (cy == ay && cp == ap) return false;
    const float a = cy / k, b = cp / k;
    const float ll[3] = {std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b)};
    float w[3];
    for (int i = 0; i < 3; ++i) w[i] = ll[0] * f[i] + ll[1] * r[i] + ll[2] * up[i];
    const double wy = std::atan2(w[1], w[0]), wp = std::atan2(w[2], std::sqrt(w[0] * w[0] + w[1] * w[1]));
    const int32_t ny = static_cast<int32_t>(std::lround(wy / u));
    // Stesso giro dello yaw dato (il PlayerController ha giri interi accumulati).
    *outYaw = yaw + static_cast<int16_t>(static_cast<uint16_t>((ny - yaw) & 0xFFFF));
    *outPitch = static_cast<int32_t>(std::lround(wp / u));
    return true;
}

// Blocco al limite del gioco (WI-4c): se il gioco riprende almeno il 70% del
// passo scritto per kTicks tick di seguito nello stesso verso, la mod smette di
// spingere da quel lato (vibrazione breve sulle due mani al blocco). Riprova un
// passo ogni probeSec (il limite puo' essersi spostato) e si sblocca quando
// l'errore torna dall'altra parte o la prova passa.
// Isteresi (follow-up della regressione finale, 27/10): sulla M2E2 a terra il
// rinculo delle raffiche spostava la mira di 0,1-0,3 gradi e il gioco
// "riprendeva" passi di 0,11-0,16 gradi: blocco acceso e spento 3 volte in
// 1,2 s con la mira 22 gradi dentro i limiti. Ora il blocco scatta solo con la
// mira ad almeno kMinErr oltre il PlayerController (un limite vero: la mano
// continua a spingere oltre) per kTicks tick, e l'errore tornato indietro lo
// scioglie solo da kReleaseErr in la' (prima: ogni cambio di segno).
struct TurretLimitLatch {
    int dir = 0;          // lato bloccato: +1 / -1, 0 = libero
    int count = 0;        // tick consecutivi rifiutati nello stesso verso
    int lastSign = 0;
    double lastProbe = 0.0;
    bool probing = false;
    static constexpr int32_t kMinStep = 18;     // ~0,1 gradi: passi piu' piccoli non contano
    static constexpr int32_t kMinErr = 182;     // 1 grado: errore minimo per contare un rifiuto
    static constexpr int32_t kReleaseErr = 46;  // 0,25 gradi: errore tornato indietro che scioglie
    static constexpr int kTicks = 3;
    float probeSec = 0.5f;
    int backTicks = 0;  // tick di fila con la mira non oltre il PlayerController dal lato bloccato
    void Reset() {
        dir = count = lastSign = backTicks = 0;
        probing = false;
    }
    // Conteggio del passo del tick prima (step scritto, refused dal gioco, err =
    // errore di yaw da cui era stato calcolato, stesso verso del passo).
    // Ritorna +1/-1 quando il blocco scatta, -2 quando una prova lo scioglie, 0 altrimenti.
    int Count(int32_t step, int32_t refused, int32_t err) {
        const int s = step > 0 ? 1 : (step < 0 ? -1 : 0);
        const bool counts = s != 0 && std::abs(step) >= kMinStep;
        const bool beyond = static_cast<int64_t>(err) * s >= kMinErr;
        const bool refusedMostly = counts && static_cast<double>(refused) * s >= 0.7 * std::abs(step);
        const bool mostly = refusedMostly && beyond;
        if (dir != 0) {
            if (probing && counts && s == dir) {
                probing = false;
                if (!refusedMostly) {
                    dir = 0;
                    count = 0;
                    return -2;
                }
            }
            return 0;
        }
        if (mostly) count = s == lastSign ? count + 1 : 1;
        else count = 0;
        lastSign = s;
        if (count >= kTicks) {
            dir = s;
            count = 0;
            return s;
        }
        return 0;
    }
    // Passo voluto (dy) con l'errore (err) e l'ora (s): 0 verso il lato
    // bloccato, salvo una prova ogni probeSec. Libero se l'errore torna
    // dall'altra parte di almeno kReleaseErr (isteresi; i passi indietro
    // passano comunque: si blocca solo la spinta verso il limite), o se resta
    // dalla parte di qua (err * dir <= 0) per kTicks tick di fila. Review del
    // follow-up: una mano che torna piano (sotto ~8 gradi/s) tiene l'errore a
    // circa -v * 0,03 s, mai oltre kReleaseErr, e il blocco restava acceso; la
    // spinta seguente verso quel lato, anche dentro il limite, si fermava
    // (torretta 2,5-5 gradi dietro la mano).
    int32_t Filter(int32_t dy, int32_t err, double now, bool* released = nullptr) {
        if (released) *released = false;
        if (dir == 0) {
            backTicks = 0;
            return dy;
        }
        backTicks = static_cast<int64_t>(err) * dir <= 0 ? backTicks + 1 : 0;
        if (static_cast<int64_t>(err) * dir <= -kReleaseErr || backTicks >= kTicks) {
            dir = 0;
            probing = false;
            backTicks = 0;
            if (released) *released = true;
            return dy;
        }
        if ((dy > 0 ? 1 : (dy < 0 ? -1 : 0)) != dir) return dy;
        if (now - lastProbe >= probeSec) {
            lastProbe = now;
            probing = true;
            return dy;
        }
        return 0;
    }
};

// Colpetto al limite (review WI-4c round 1, basso): prima a ogni ingresso nel
// limite con 0,6 s di pausa e nessuna isteresi; sull'elicottero il supporto
// si inclina con la virata (beccheggio del supporto +21,4 -> +25,1 in 1,5 s),
// e una mano ferma vicino al limite lo attraversava di continuo: le due mani
// vibravano fino a ogni 0,6 s. Ora un colpetto solo, riarmato dopo che la mira
// e' rimasta rearmSec ad almeno rearmDeg dentro i limiti.
inline float TurretLimitMarginDeg(float localYawDeg, float localPitchDeg, const TurretLimits& l) {
    float m = l.yawRightDeg - localYawDeg;
    m = std::fmin(m, l.yawLeftDeg + localYawDeg);
    m = std::fmin(m, l.pitchUpDeg - localPitchDeg);
    return std::fmin(m, l.pitchDownDeg + localPitchDeg);
}
struct TurretLimitBuzz {
    bool armed = true;
    double insideSince = -1.0;
    float rearmDeg = 2.0f, rearmSec = 0.5f;
    void Reset() {
        armed = true;
        insideSince = -1.0;
    }
    // clamped: mira limitata in questo tick; marginDeg: distanza dal limite
    // piu' vicino (negativa fuori). true = colpetto ora.
    bool Step(bool clamped, float marginDeg, double nowS) {
        if (clamped) {
            insideSince = -1.0;
            if (!armed) return false;
            armed = false;
            return true;
        }
        if (marginDeg >= rearmDeg) {
            if (insideSince < 0.0) insideSince = nowS;
            if (!armed && nowS - insideSince >= rearmSec) armed = true;
        } else {
            insideSince = -1.0;
        }
        return false;
    }
};

// Grip come maniglie della torretta NORMAL (vr_input ApplyGestures): RB / LB
// tolti al gioco si decidono alla pressione e restano tolti fino al rilascio,
// come la granata (m_lbAllowed). Review WI-4c round 2 (medio): prima la prova
// valeva a ogni lettura; a fine torretta (TurretLeave e' una mossa animata,
// g_gameLocked), in pausa, nella dashboard, in una scena, con i controller persi
// o cambiando presa, i grip ancora stretti arrivavano al gioco come una pressione
// nuova (RB = ordine alla squadra, LB = granata).
//  - pressione con le maniglie attive (o la presa d'uso di un'interazione, che
//    vale A): tolta fino al rilascio, qualunque cosa cambi;
//  - pressione che il gioco non ha ancora visto (tolta da un'altra regola) e
//    maniglie che si attivano: tolta da li' al rilascio;
//  - pressione che il gioco ha gia' visto (es. cominciata in pausa: va al menu del
//    gioco): resta com'e', toglierla a meta' sarebbe un rilascio (granata lanciata).
struct GripStripLatch {
    bool down = false, strip = false, sent = false;
    bool byHandles = false;  // tolto per le maniglie (non solo per la presa d'uso)
    // pressed: tasto premuto (pad + gesti); handles: maniglie attive adesso (presa
    // NORMAL, controller tracciato, non in pausa ne' nella dashboard); useGrip:
    // grip della presa d'uso di un'interazione in questo tick. true = toglierlo.
    bool Step(bool pressed, bool handles, bool useGrip) {
        if (!pressed) {
            down = strip = sent = byHandles = false;
            return false;
        }
        if (!down) {
            strip = handles || useGrip;
            byHandles = handles;
        } else if (!sent && handles && !byHandles) {
            strip = byHandles = true;
        }
        down = true;
        return strip;
    }
    // Dopo tutte le regole: il tasto e' arrivato al gioco in questo tick.
    void Delivered(bool toGame) {
        if (down && toGame) sent = true;
    }
};

// --- Posizione della vista, altezza degli occhi, ricentramenti (WI-2) ------------
//
// Bug 3 del tester ("la scala del mondo cambia di continuo"): la vista si
// spostava senza che la testa si muovesse (muro davanti, trace basse,
// collisione della testa che accorciava tutto il vettore degli occhi, altezza
// legata al massimo della testa, ricentramenti del runtime).

// Altezza degli occhi sopra il centro del Pawn ("height auto", in piedi) con
// un riferimento verticale stabile: ref = altezza in piedi fissata al
// ricentramento voluto, alla calibrazione o all'inizio della scena (metri),
// trk = riferimento che insegue il massimo della testa (soglie di accucciata e
// sporgenza, sempre come prima). La vista e' fp + extra + (pos - trk) * s:
// col valore restituito come extra + fp vale clamp(ref * s - 45) + (pos -
// ref) * s, cioe' non dipende piu' da trk. Si limita solo il riferimento,
// mai lo spostamento della testa del frame (accucciarsi abbassa sempre).
inline float StableEyeHeight(float refM, float trkM, float scale, float lo, float hi) {
    float e = refM * scale - 45.0f;
    e = e < lo ? lo : (e > hi ? hi : e);
    return e + (trkM - refM) * scale;
}

// Arretramento della vista dal muro davanti, per tick: verso il muro subito
// fino a "hard" (gli occhi mai a meno di 4 unita' dalla superficie), il
// resto fino a "want" (12 unita' dal muro, 20 da un bordo basso) a 150
// unita'/s; via dal muro con tau 0,2 s. Prima: tutto a 150 unita'/s, quindi
// girandosi verso un muro per qualche tick gli occhi ci stavano dentro.
inline float WallPullStep(float current, float want, float hard, float dt) {
    if (want < 0.0f) want = 0.0f;
    if (hard > want) hard = want;
    if (!(dt > 0.0f && dt < 0.5f)) return want;
    float out;
    if (want < current) out = current + (want - current) * (1.0f - std::exp(-dt / 0.2f));
    else out = (std::min)(want, current + 150.0f * dt);
    return out < hard ? hard : out;
}

// Trace basse del muro davanti (bordo di una cassa all'altezza del mento,
// cap. 1 cp01): seguono l'altezza della testa, quindi con la testa che
// ondeggia di pochi cm il bordo entrava e usciva e la vista andava avanti e
// indietro di 20 unita'. Isteresi: agganciate, il controllo d'uscita si fa
// kHyst unita' piu' in basso (bisogna alzarsi di 4 unita' oltre il punto
// d'ingresso) e l'arretramento resta per kHold s dopo l'ultimo colpo.
struct LowProbeLatch {
    static constexpr float kHyst = 4.0f, kHold = 0.4f;
    bool engaged = false;
    float since = 0.0f;  // s dall'ultimo colpo
    float want = 0.0f;   // arretramento tenuto
    // Abbassamento delle trace per il prossimo controllo (0 o kHyst).
    float Drop() const { return engaged ? kHyst : 0.0f; }
    // hit: le trace basse colpiscono, wantNow: arretramento chiesto da loro.
    float Step(bool hit, float wantNow, float dt) {
        if (hit) {
            engaged = true;
            since = 0.0f;
            want = wantNow;
        } else if (engaged) {
            since += dt > 0.0f ? dt : 0.0f;
            if (since >= kHold) {
                engaged = false;
                want = 0.0f;
            }
        } else {
            want = 0.0f;
        }
        return want;
    }
};

// Collisione della testa (WI-2): si toglie solo la parte dello spostamento
// che entra nell'ostacolo. Piano colpito: punto hit, normale n (unitaria,
// verso il punto libero "base", in gioco il centro di Walker da cui parte la
// trace). target (occhio) portato ad almeno "margin" dal piano, ma non oltre
// quel punto lungo n (punto gia' a meno di margin: si resta alla sua distanza).
// Prima la collisione scalava tutto il vettore dal centro di Walker agli
// occhi (15 unita' avanti comprese): la vista arretrava e i movimenti laterali
// della testa vicino al muro si accorciavano.
inline bool ClampIntoPlane(const float base[3], float target[3], const float hit[3], const float n[3], float margin) {
    float db = 0.0f, dt = 0.0f;
    for (int i = 0; i < 3; ++i) {
        db += (base[i] - hit[i]) * n[i];
        dt += (target[i] - hit[i]) * n[i];
    }
    float lim = (std::min)(margin, (std::max)(db, 0.0f));
    if (dt >= lim) return false;
    for (int i = 0; i < 3; ++i) target[i] += n[i] * (lim - dt);
    return true;
}

// Piu' piani insieme (spigolo, angolo della stanza; review WI-2 26/10): prima
// col secondo ostacolo si tornava a t (tutto il vettore scalato: guadagno di
// lato 0,79 nell'angolo). Proiezioni alternate sui piani finche' nessuno
// sposta piu' il punto (2 piani ad angolo retto: un giro).
inline bool ClampIntoPlanes(const float base[3], float target[3], const float (*hit)[3], const float (*n)[3], int count,
                            float margin) {
    bool any = false;
    for (int pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (int k = 0; k < count; ++k) moved = ClampIntoPlane(base, target, hit[k], n[k], margin) || moved;
        any = any || moved;
        if (!moved) break;
    }
    return any;
}

// Spinta della collisione della testa nel tempo (follow-up della regressione
// finale, 27/10). Prima: una spinta che cresceva si applicava subito (per non
// vedere attraverso i muri), una che calava si rilasciava con tau 0,1 s. Un
// compagno che passa attraverso la testa (capsula che incrocia la linea dal
// centro di Walker all'occhio) gira la normale del piano da un lato all'altro
// in un tick: la nuova spinta era "piu' grande" e si applicava subito, l'occhio
// saltava di 7 unita' di lato (ph_pass, 30 unita' davanti). Ora si applica
// subito solo una spinta che cresce DALLO STESSO LATO (entro 60 gradi da quella
// applicata: muro, spigolo, angolo della stanza come prima). Un cambio di lato
// apre un passaggio (*changing) che va verso la spinta nuova con tau 0,1 s e
// al massimo sideSpeed unita'/s finche' non la raggiunge (entro doneUnits);
// senza, dopo qualche tick la spinta a meta' strada era "dallo stesso lato" e
// il resto arrivava di colpo (4 unita'). Il rilascio come prima.
// Review del follow-up (27/10): il passaggio dolce valeva per ogni superficie,
// anche col residuo di un contatto appena lasciato (0,001 unita' per ~0,8 s):
// un muro nuovo che arrivava a 70 unita'/s lasciava l'occhio 9,5 unita' dietro
// la spinta, oltre il margine di 8 (attraverso la superficie). Ora: (1) il
// cambio di lato e' dolce solo con easeSides (hooks.cpp: colpo su un Pawn, un
// piano solo); muri, spigoli e oggetti come prima (spinta che cresce: subito);
// (2) una spinta applicata sotto minPush unita' conta come nessuna spinta
// (subito); (3) in ogni caso l'occhio resta al piu' maxInside unita' dietro la
// spinta nuova, lungo la sua direzione (margine 8 - 2: almeno 2 unita' fuori
// dalla superficie).
struct HeadPushParams {
    float releaseTau = 0.1f;   // secondi
    float sideSpeed = 40.0f;   // unita'/s durante il cambio di lato
    float sameSideCos = 0.5f;  // coseno: entro 60 gradi = stesso lato
    float doneUnits = 0.25f;   // cambio di lato finito
    float minPush = 0.5f;      // spinta applicata sotto: come nessuna
    float maxInside = 6.0f;    // massimo che l'occhio resta dietro la spinta nuova (unita')
};
// smooth: spinta applicata (aggiornata); corr: spinta di questo tick; dt in s
// (oltre 0,5 s o non valido: si prende corr); changing: stato del cambio di
// lato tra i tick; easeSides: il cambio di lato puo' essere dolce (Pawn).
// Ritorna 0 subito, 1 rilascio, 2 cambio di lato (limitato).
inline int HeadPushStep(float smooth[3], const float corr[3], float dt, bool* changing, const HeadPushParams& p = HeadPushParams(),
                        bool easeSides = true) {
    const float cn = corr[0] * corr[0] + corr[1] * corr[1] + corr[2] * corr[2];
    const float sn = smooth[0] * smooth[0] + smooth[1] * smooth[1] + smooth[2] * smooth[2];
    const float dot = corr[0] * smooth[0] + corr[1] * smooth[1] + corr[2] * smooth[2];
    const float minSq = p.minPush * p.minPush > 1e-6f ? p.minPush * p.minPush : 1e-6f;
    // Spinta applicata quasi nulla (residuo del rilascio) e una nuova piu' grande: subito.
    const bool residual = sn < minSq && cn >= sn;
    if (!(dt > 0.0f) || dt > 0.5f || sn < 1e-6f || residual) {
        for (int i = 0; i < 3; ++i) smooth[i] = corr[i];
        *changing = false;
        return 0;
    }
    const bool sameSide = cn > 1e-6f && dot >= p.sameSideCos * std::sqrt(cn * sn);
    if (!easeSides) *changing = false;
    if (cn > 1e-6f && !sameSide && easeSides) *changing = true;
    if (*changing) {
        const float r[3] = {corr[0] - smooth[0], corr[1] - smooth[1], corr[2] - smooth[2]};
        if (r[0] * r[0] + r[1] * r[1] + r[2] * r[2] < p.doneUnits * p.doneUnits) *changing = false;
    }
    // Senza passaggio dolce (non un Pawn) una spinta che cresce si applica
    // subito da qualunque lato, come prima del follow-up.
    if (!*changing && cn >= sn && (sameSide || !easeSides)) {
        for (int i = 0; i < 3; ++i) smooth[i] = corr[i];
        return 0;
    }
    const float k = 1.0f - std::exp(-dt / (p.releaseTau > 1e-3f ? p.releaseTau : 1e-3f));
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = (corr[i] - smooth[i]) * k;
    if (*changing) {
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), cap = p.sideSpeed * dt;
        if (dl > cap && dl > 0.0f)
            for (int i = 0; i < 3; ++i) d[i] *= cap / dl;
    }
    for (int i = 0; i < 3; ++i) smooth[i] += d[i];
    // Mai piu' di maxInside dietro la spinta nuova (lungo la sua direzione).
    if (cn > 1e-6f) {
        const float cl = std::sqrt(cn);
        const float along = (smooth[0] * corr[0] + smooth[1] * corr[1] + smooth[2] * corr[2]) / cl;
        const float need = cl - p.maxInside;
        if (along < need)
            for (int i = 0; i < 3; ++i) smooth[i] += corr[i] / cl * (need - along);
    }
    return *changing ? 2 : 1;
}

// Trasformazione dello spazio di tracciamento (ricentramento del runtime):
// p_nuovo = R(yaw) * p_vecchio + t sul piano orizzontale (x, z dello spazio
// OpenVR/OpenXR), con R che aumenta di "yaw" lo yaw di ogni direzione
// (yaw = atan2(-m02, m22), avanti = (sin, -cos)). L'altezza non cambia.
struct SpaceShift {
    float yaw = 0.0f, tx = 0.0f, tz = 0.0f;
    void Apply(float x, float z, float* nx, float* nz) const {
        const float c = std::cos(yaw), s = std::sin(yaw);
        *nx = x * c - z * s + tx;
        *nz = x * s + z * c + tz;
    }
    SpaceShift Inverse() const {
        SpaceShift r;
        r.yaw = -yaw;
        const float c = std::cos(yaw), s = std::sin(yaw);
        // p = R^-1 (q - t)
        r.tx = -(tx * c + tz * s);
        r.tz = -(-tx * s + tz * c);
        return r;
    }
    // Prima a, poi b.
    static SpaceShift Then(const SpaceShift& a, const SpaceShift& b) {
        SpaceShift r;
        r.yaw = WrapPi(a.yaw + b.yaw);
        b.Apply(a.tx, a.tz, &r.tx, &r.tz);
        return r;
    }
};
// Dalla stessa testa vista nei due spazi (prima e dopo il salto).
inline SpaceShift ShiftFromPoses(float yawOld, float xOld, float zOld, float yawNew, float xNew, float zNew) {
    SpaceShift s;
    s.yaw = WrapPi(yawNew - yawOld);
    float rx, rz;
    SpaceShift r0;
    r0.yaw = s.yaw;
    r0.Apply(xOld, zOld, &rx, &rz);
    s.tx = xNew - rx;
    s.tz = zNew - rz;
    return s;
}
// OpenXR XrEventDataReferenceSpaceChangePending.poseInPreviousSpace: origine
// del nuovo spazio nel vecchio (rotazione m, posizione p): p_vecchio = m *
// p_nuovo + p, quindi p_nuovo = m^-1 (p_vecchio - p).
inline SpaceShift ShiftFromPreviousSpacePose(const float m[3][4]) {
    SpaceShift toOld;
    toOld.yaw = ExactEuler(m).yaw;
    toOld.tx = m[0][3];
    toOld.tz = m[2][3];
    return toOld.Inverse();
}
// Riferimenti della testa (yaw del ricentramento, posizione x, z) portati
// nel nuovo spazio: spostamento e yaw della testa rispetto a loro invariati.
inline void RebaseReferences(const SpaceShift& s, float* refYaw, float* posRefX, float* posRefZ) {
    *refYaw = WrapPi(*refYaw + s.yaw);
    float nx, nz;
    s.Apply(*posRefX, *posRefZ, &nx, &nz);
    *posRefX = nx;
    *posRefZ = nz;
}

// Rotazioni 3x3 in doppia precisione (spazio del tracciamento) per il
// rilevatore dei salti: prodotto, A * B^T, vettore di rotazione e ritorno.
namespace rot3 {
inline void Mul(const double a[3][3], const double b[3][3], double o[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}
inline void MulT(const double a[3][3], const double b[3][3], double o[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[j][0] + a[i][1] * b[j][1] + a[i][2] * b[j][2];
}
inline void Log(const double d[3][3], double v[3]) {
    const double vx = 0.5 * (d[2][1] - d[1][2]), vy = 0.5 * (d[0][2] - d[2][0]), vz = 0.5 * (d[1][0] - d[0][1]);
    const double s = std::sqrt(vx * vx + vy * vy + vz * vz), c = 0.5 * (d[0][0] + d[1][1] + d[2][2] - 1.0);
    const double th = std::atan2(s, c), k = s > 1e-12 ? th / s : 1.0;
    v[0] = vx * k;
    v[1] = vy * k;
    v[2] = vz * k;
}
inline void Exp(const double v[3], double r[3][3]) {
    const double th = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const double a = th > 1e-12 ? std::sin(th) / th : 1.0, b = th > 1e-6 ? (1.0 - std::cos(th)) / (th * th) : 0.5;
    const double k[3][3] = {{0, -v[2], v[1]}, {v[2], 0, -v[0]}, {-v[1], v[0], 0}};
    double k2[3][3];
    Mul(k, k, k2);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = (i == j ? 1.0 : 0.0) + a * k[i][j] + b * k2[i][j];
}
// Angolo (radianti) della rotazione A * B^T.
inline double Angle(const double a[3][3], const double b[3][3]) {
    double d[3][3], v[3];
    MulT(a, b, d);
    Log(d, v);
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}
// Rotazione dello spazio di SpaceShift (yaw attorno all'asse verticale y).
inline void YawMatrix(float yaw, double q[3][3]) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double m[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) q[i][j] = m[i][j];
}
}  // namespace rot3

// Posa OpenVR (3x4) portata da uno spazio all'altro: rotazione e x, z.
inline void ShiftPose(const SpaceShift& s, float m[3][4]) {
    double q[3][3], r[3][3], o[3][3];
    rot3::YawMatrix(s.yaw, q);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = m[i][j];
    rot3::Mul(q, r, o);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = static_cast<float>(o[i][j]);
    float nx, nz;
    s.Apply(m[0][3], m[2][3], &nx, &nz);
    m[0][3] = nx;
    m[2][3] = nz;
}

// Scarto tra due pose (posizione e rotazione) in unita' della soglia del
// salto (5 cm, 5 gradi): il maggiore dei due.
inline float PoseStepErr(const float a[3][4], const float b[3][4]) {
    double ra[3][3], rb[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            ra[i][j] = a[i][j];
            rb[i][j] = b[i][j];
        }
    const float dx = a[0][3] - b[0][3], dy = a[1][3] - b[1][3], dz = a[2][3] - b[2][3];
    const float pos = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float ang = static_cast<float>(rot3::Angle(ra, rb) * 57.29577951308232);
    return (std::max)(pos / 0.05f, ang / 5.0f);
}
// Controller nel tick tenuto di un salto del visore (review WI-2 round 2): se
// lo spazio e' cambiato davvero (ricentramento del runtime) anche i controller
// sono saltati con lui, e riportati indietro (s^-1) tornano vicini alla posa
// del tick prima; se il salto era solo del visore (campione sbagliato) no.
// true = riportarlo indietro (posa riportata piu' vicina di quella grezza).
inline bool PoseFollowsShift(const float raw[3][4], const float prev[3][4], const SpaceShift& s, float* errRaw = nullptr,
                             float* errBack = nullptr) {
    float back[3][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) back[i][j] = raw[i][j];
    ShiftPose(s.Inverse(), back);
    const float er = PoseStepErr(raw, prev), eb = PoseStepErr(back, prev);
    if (errRaw) *errRaw = er;
    if (errBack) *errBack = eb;
    return eb < er;
}

// Salto della posa del visore in un tick (ricentramento del runtime):
// scarto dall'estrapolazione a velocita' costante dei due campioni
// precedenti oltre 5 cm (orizzontale) o 5 gradi (rotazione completa), piu'
// l'errore dell'estrapolazione per una testa vera che accelera nel tick (alfa
// * dt^2 con 30 m/s^2 e 3000 gradi/s^2). Tick oltre 150 ms: si riparte.
// Review WI-2 (26/10): prima bastava un campione fuori posto e il salto si
// toglieva subito dai riferimenti, rimesso di scatto dopo 1 s senza evento:
//  - un campione sbagliato (tracciamento) raddoppiava (la storia ripartiva da
//    lui e il tick dopo sembrava un secondo salto): vista spostata di 10, poi
//    20 gradi per 1 s, poi lo scatto indietro;
//  - un campione ripetuto girando la testa, o una posa predetta 80 ms (VDXR)
//    con un tick lungo, sembravano salti di 8-17 gradi;
//  - guardando dritto in basso lo yaw esatto si gira di 180 gradi in un tick.
// Ora:
//  - pose senza predizione (il chiamante), rotazione completa (niente yaw:
//    nessun salto passando per la verticale);
//  - un ricentramento gira lo spazio solo attorno alla verticale: uno scarto
//    con inclinazione non e' un ricentramento;
//  - il campione fuori posto non entra nella storia: kHold (la vista di
//    questo tick e' tenuta, riferimenti intatti) e il tick dopo decide tra
//    (a) spazio nuovo (posa e velocita' continue dal campione saltato:
//    kConfirm, il salto va tolto dai riferimenti), (b) spazio di prima
//    (campione sbagliato o ripetuto) e (c) testa vera (moto continuo dal
//    campione): kRejected, nulla da rimettere.
// Review WI-2 (round 2, 26/10): se il tick dopo il salto arriva oltre 150 ms
// (scatto del gioco: il menu di sistema del Quest compare tenendo premuto il
// tasto del ricentramento) le tre ipotesi non si distinguono piu'. Prima era
// kRejected: riferimenti intatti e tutto il ricentramento (40 gradi) si vedeva
// di scatto. Ora kUnsure con lo spostamento del candidato in *shift: il
// chiamante lo toglie come un kConfirm, lo tiene se arriva l'evento del
// runtime e altrimenti lo rimette sotto un battito di ciglia.
struct HeadJumpDetector {
    enum Result { kNone = 0, kHold = 1, kConfirm = 2, kRejected = 3, kUnsure = 4 };
    enum Why { kWhyNone = 0, kWhyGlitch = 1, kWhyMotion = 2, kWhyUnclear = 3, kWhyTilted = 4, kWhyLate = 5 };
    struct Sample {
        double t = 0.0;
        double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        float x = 0.0f, z = 0.0f;
    };
    Sample h[2];  // storia: [1] il piu' recente
    int n = 0;
    bool haveCand = false, candValid = false;
    Sample cand;
    SpaceShift candShift;
    // Diagnostica dell'ultimo campione: scarto (m, gradi), inclinazione dello
    // scarto (gradi), errori normalizzati delle tre ipotesi (a, b, c), motivo.
    float lastPos = 0.0f, lastAngDeg = 0.0f, lastTiltDeg = 0.0f, lastErr[3] = {0, 0, 0};
    float lastGapMs = 0.0f;  // kUnsure: distanza tra il campione saltato e quello dopo
    int lastWhy = kWhyNone;
    void Reset() {
        n = 0;
        haveCand = false;
    }
    static float PosThr(double d) { return 0.05f + 30.0f * static_cast<float>(d * d); }
    static float AngThrDeg(double d) { return 5.0f + 3000.0f * static_cast<float>(d * d); }
    // Estrapolazione da a (prima) e b (dopo) all'istante t.
    static void Extrap(const Sample& a, const Sample& b, double t, double r[3][3], float* x, float* z) {
        const double k = (t - b.t) / (b.t - a.t);
        double d[3][3], v[3], e[3][3];
        rot3::MulT(b.r, a.r, d);
        rot3::Log(d, v);
        for (double& c : v) c *= k;
        rot3::Exp(v, e);
        rot3::Mul(e, b.r, r);
        *x = b.x + static_cast<float>((b.x - a.x) * k);
        *z = b.z + static_cast<float>((b.z - a.z) * k);
    }
    // Errore normalizzato (1 = soglia) di s rispetto all'estrapolazione di (a, b).
    static float Err(const Sample& a, const Sample& b, const Sample& s, float* posOut = nullptr, float* angOut = nullptr) {
        double r[3][3];
        float px, pz;
        Extrap(a, b, s.t, r, &px, &pz);
        const float pos = std::sqrt((s.x - px) * (s.x - px) + (s.z - pz) * (s.z - pz));
        const float ang = static_cast<float>(rot3::Angle(s.r, r) * 57.29577951308232);
        if (posOut) *posOut = pos;
        if (angOut) *angOut = ang;
        const double d = s.t - b.t;
        return (std::max)(pos / PosThr(d), ang / AngThrDeg(d));
    }
    static Sample Shifted(const Sample& s, const SpaceShift& sh) {
        Sample o = s;
        double q[3][3];
        rot3::YawMatrix(sh.yaw, q);
        rot3::Mul(q, s.r, o.r);
        sh.Apply(s.x, s.z, &o.x, &o.z);
        return o;
    }
    void Push(const Sample& s) {
        if (n == 0) {
            h[1] = s;
            n = 1;
        } else {
            h[0] = h[1];
            h[1] = s;
            n = 2;
        }
    }
    // r: rotazione della posa (colonne = assi del visore nello spazio), x, z
    // orizzontali (m). kHold e kConfirm scrivono *shift (spazio di prima ->
    // spazio di adesso).
    int Step(double now, const float r[3][3], float xNow, float zNow, SpaceShift* shift) {
        Sample s;
        s.t = now;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) s.r[i][j] = r[i][j];
        s.x = xNow;
        s.z = zNow;
        lastPos = lastAngDeg = lastTiltDeg = 0.0f;
        lastWhy = kWhyNone;
        if (haveCand) {
            haveCand = false;
            const double d1 = now - cand.t;
            if (!(d1 > 0.0 && d1 <= 0.15) || n < 2) {
                // Tick lungo dopo un salto tenuto: kUnsure (vedi sopra).
                const bool late = candValid && n >= 2 && d1 > 0.15;
                lastWhy = late ? kWhyLate : kWhyUnclear;
                lastGapMs = static_cast<float>(d1 * 1000.0);
                if (late && shift) *shift = candShift;
                n = 0;
                Push(s);
                return late ? kUnsure : kRejected;
            }
            const float eb = Err(h[0], h[1], s);
            const float ec = Err(h[1], cand, s);
            const float ea = candValid ? Err(Shifted(h[1], candShift), cand, s) : 1e9f;
            lastErr[0] = ea;
            lastErr[1] = eb;
            lastErr[2] = ec;
            if (candValid && ea <= 1.5f && ea <= eb && ea <= ec) {
                if (shift) *shift = candShift;
                h[0] = cand;
                h[1] = s;
                n = 2;
                return kConfirm;
            }
            if (eb <= ec && eb <= 1.5f) {
                lastWhy = kWhyGlitch;  // il campione era sbagliato: storia senza di lui
                Push(s);
            } else if (ec <= 1.5f) {
                lastWhy = kWhyMotion;  // testa vera: si riparte dal campione
                h[0] = cand;
                h[1] = s;
                n = 2;
            } else {
                lastWhy = kWhyUnclear;
                n = 0;
                Push(s);
            }
            return kRejected;
        }
        if (n >= 2) {
            const double dt1 = now - h[1].t, dt0 = h[1].t - h[0].t;
            if (dt1 > 0.0 && dt1 <= 0.15 && dt0 > 0.0 && dt0 <= 0.15) {
                double pr[3][3];
                float px, pz;
                Extrap(h[0], h[1], now, pr, &px, &pz);
                lastPos = std::sqrt((xNow - px) * (xNow - px) + (zNow - pz) * (zNow - pz));
                lastAngDeg = static_cast<float>(rot3::Angle(s.r, pr) * 57.29577951308232);
                if (lastPos > PosThr(dt1) || lastAngDeg > AngThrDeg(dt1)) {
                    // Spazio: Q = R_adesso * R_previsto^T, solo yaw se e' un ricentramento.
                    double q[3][3];
                    rot3::MulT(s.r, pr, q);
                    const double qy = q[1][1] > 1.0 ? 1.0 : (q[1][1] < -1.0 ? -1.0 : q[1][1]);
                    lastTiltDeg = static_cast<float>(std::acos(qy) * 57.29577951308232);
                    SpaceShift sh;
                    sh.yaw = static_cast<float>(std::atan2(q[2][0] - q[0][2], q[0][0] + q[2][2]));
                    SpaceShift r0;
                    r0.yaw = sh.yaw;
                    float rx, rz;
                    r0.Apply(px, pz, &rx, &rz);
                    sh.tx = xNow - rx;
                    sh.tz = zNow - rz;
                    cand = s;
                    candShift = sh;
                    haveCand = true;
                    candValid = lastTiltDeg <= 0.5f * AngThrDeg(dt1);
                    if (candValid) {
                        if (shift) *shift = sh;
                        return kHold;
                    }
                    lastWhy = kWhyTilted;  // si vede com'e'; il tick dopo decide
                    return kNone;
                }
            } else {
                n = 0;  // tick lungo: si riparte
            }
        }
        Push(s);
        return kNone;
    }
};

// Riferimento verticale della vista preso troppo in basso (visore indossato
// da seduti o chinati, poi in piedi): review WI-2 (26/10). Prima il
// riferimento saliva di meta' di ogni aumento dell'altezza per tick, mai in
// giu': col rumore del tracciamento e l'ondeggiare saliva da solo (6 unita'
// in 30 s con 0,2 mm di rumore) e i seduti che si alzavano un attimo
// perdevano altezza per sempre. Ora una sola ripresa: testa (filtrata, 0,25
// s) oltre 15 cm sopra il riferimento per 3 s di fila (il conto riparte solo
// sotto 13 cm: banda morta), poi la mediana degli ultimi 1,5 s. Il chiamante
// la applica sotto un battito di ciglia; mai seduti o con l'altezza fissa.
struct ViewHeightRecapture {
    static constexpr float kAbove = 0.15f, kDeadband = 0.02f, kDwell = 3.0f, kWindow = 1.5f, kTau = 0.25f;
    static constexpr int kN = 512;
    double ts[kN] = {};
    float ys[kN] = {};
    int head = 0, count = 0;
    float filt = 0.0f;
    double lastT = -1.0, aboveSince = -1.0;
    void Reset() {
        count = 0;
        lastT = -1.0;
        aboveSince = -1.0;
    }
    // Ritorna il nuovo riferimento (m) da applicare, o 0.
    float Step(double now, float y, float ref) {
        ts[head] = now;
        ys[head] = y;
        head = (head + 1) % kN;
        if (count < kN) ++count;
        const double dt = lastT >= 0.0 ? now - lastT : -1.0;
        lastT = now;
        if (!(dt > 0.0 && dt < 0.5)) filt = y;
        else filt += (y - filt) * (1.0f - static_cast<float>(std::exp(-dt / kTau)));
        if (filt - ref > kAbove) {
            if (aboveSince < 0.0) aboveSince = now;
        } else if (filt - ref < kAbove - kDeadband) {
            aboveSince = -1.0;
        }
        if (aboveSince < 0.0 || now - aboveSince < kDwell) return 0.0f;
        aboveSince = -1.0;
        float tmp[kN];
        int m = 0;
        for (int i = 0; i < count; ++i) {
            const int idx = (head - 1 - i + kN) % kN;
            if (now - ts[idx] > kWindow) break;
            tmp[m++] = ys[idx];
        }
        if (m < 3) return 0.0f;
        std::nth_element(tmp, tmp + m / 2, tmp + m);
        const float med = tmp[m / 2];
        return med - ref > kAbove - kDeadband ? med : 0.0f;
    }
};

// Passaggio tra terreno fermo e mezzo (WI-2): la differenza tra la vista di
// prima e quella del nuovo modo (fp avanti 15 -> 0, base del collo, limite
// della testa, smorzamento) si toglie in kTime s invece che in un tick.
// Offset nel riferimento del corpo (avanti, destra, su), lineare nel tempo.
struct ModeBlend {
    static constexpr float kTime = 0.3f;
    float off[3] = {0, 0, 0};
    float left = 0.0f;  // s ancora da smorzare
    void Start(const float delta[3]) {
        for (int i = 0; i < 3; ++i) off[i] = delta[i];
        left = kTime;
    }
    // Offset da aggiungere in questo tick (dopo dt s).
    void Step(float dt, float out[3]) {
        if (left <= 0.0f || !(dt > 0.0f)) {
            if (left <= 0.0f) off[0] = off[1] = off[2] = 0.0f;
            for (int i = 0; i < 3; ++i) out[i] = off[i];
            return;
        }
        const float k = dt >= left ? 0.0f : (left - dt) / left;
        left -= dt;
        for (int i = 0; i < 3; ++i) {
            off[i] *= k;
            out[i] = off[i];
        }
        if (left <= 0.0f) off[0] = off[1] = off[2] = 0.0f;
    }
};

// Base (elicottero, piattaforma) in movimento = mezzo solo sopra 20
// unita'/s (WI-2). Prima bastava 0,5 unita' tra due tick (anche un oggetto
// fisico che si assestava sotto Walker), e la vista cambiava modo di colpo.
inline bool BaseMovingFast(float dist, float dt) { return dt > 0.0f && dt < 0.5f && dist > 20.0f * dt && dist > 0.05f; }

// --- Risoluzione dinamica (WI-1) ---------------------------------------------------
//
// Prima: ogni 0,7 s, banda morta 0,70-0,92 del budget sulla media mobile, +5 /
// -10, nessuna memoria. Nel log del tester (VDXR 23/10) 80% <-> 85% ogni
// 0,7-1,4 s (1500 cambi in 37 min): la misura della GPU (tempo tra due
// timestamp D3D9, attese della CPU comprese) saltava da 5 a 8,5 ms per il 13%
// di pixel in piu', e ogni "scale set" costa un tick lungo. Ora:
//  - campioni del livello in corso soltanto, e niente per 1 s dopo un cambio;
//  - p90 degli ultimi 2 s;
//  - giu' sopra 1,0 volte il budget (dopo 2 s al livello) o sopra 0,92 per 2 s;
//  - su dopo 5 s sotto 0,65 (di 5 punti);
//  - un livello lasciato perche' troppo pesante e' bloccato per 20 s, poi 40,
//    80... (fino a 320 s) a ogni nuovo fallimento; 60 s tenuto = perdonato;
//  - almeno 4 s per livello (2 s se si sfora il budget);
//  - una prova fallita (salita poi sforamento entro 30 s) torna al livello di
//    prima, non piu' giu' (con i picchi della GPU condivisa scendeva di due).
struct DynResController {
    // Soglie (rapporti col frame del visore) e tempi: ini "dynresdown",
    // "dynresslow", "dynresup", "dynresdwell", "dynresbackoff" (review WI-1).
    struct Params {
        float down = 1.0f, slow = 0.92f, up = 0.65f;
        double dwell = 4.0, backoff = 20.0;
    };
    Params p;
    int cur = 100, minPct = 65;
    double lastChange = -1.0e9, overSince = -1.0, underSince = -1.0;
    double upAt = -1.0e9;  // ultima salita e livello da cui veniva
    int upFrom = 0;
    struct Fail {
        int count = 0;
        double until = 0.0, since = 0.0;
    };
    Fail fail[21];  // per livello: 0..100 a passi di 5
    struct Sample {
        double t;
        float ms;
    };
    Sample ring[512];
    int head = 0, n = 0;
    float lastP90 = 0.0f;
    static int Slot(int pct) { return (pct < 0 ? 0 : (pct > 100 ? 100 : pct)) / 5; }
    void Reset(int pct, double t) {
        cur = pct;
        lastChange = t;
        overSince = underSince = -1.0;
        n = 0;
    }
    // Un campione GPU (ms per tick stereo) misurato al livello pct.
    void AddSample(double t, float ms, int pct) {
        if (pct != cur || t - lastChange < 1.0 || !(ms > 0.0f)) return;
        ring[head] = {t, ms};
        head = (head + 1) % 512;
        if (n < 512) ++n;
    }
    // p90 degli ultimi 2 s; false se ci sono meno di 8 campioni o meno di 1 s.
    bool P90(double t, float* out) {
        float v[512];
        int k = 0;
        double oldest = t;
        for (int i = 0; i < n; ++i) {
            const Sample& s = ring[(head - 1 - i + 512) % 512];
            if (t - s.t > 2.0) break;
            v[k++] = s.ms;
            oldest = s.t;
        }
        if (k < 8 || t - oldest < 1.0) return false;
        std::sort(v, v + k);
        *out = v[static_cast<int>(0.9 * (k - 1) + 0.5)];
        return true;
    }
    // Ritorna il livello voluto (== cur se niente cambio). budgetMs: frame del visore.
    int Update(double t, float budgetMs) {
        float p90 = 0.0f;
        if (!(budgetMs > 0.0f) || !P90(t, &p90)) return cur;
        lastP90 = p90;
        const double dwell = t - lastChange;
        const float r = p90 / budgetMs;
        overSince = r > p.slow ? (overSince < 0.0 ? t : overSince) : -1.0;
        underSince = r < p.up ? (underSince < 0.0 ? t : underSince) : -1.0;
        Fail& f = fail[Slot(cur)];
        if (f.count && dwell > 60.0 && t - f.since > 60.0) f.count = 0;  // livello tenuto: perdonato
        const double dwellOver = p.dwell * 0.5;  // sforando il budget: meta' (2 s)
        const bool down = (r > p.down && dwell >= dwellOver) || (overSince >= 0.0 && t - overSince >= 2.0 && dwell >= p.dwell);
        if (down && cur > minPct) {
            float next = cur * std::sqrt(0.85f * budgetMs / p90);
            if (next > cur - 5.0f) next = cur - 5.0f;
            if (next < cur - 10.0f) next = cur - 10.0f;
            // Arrotondato verso il livello attuale: una prova fallita (80 -> 85)
            // torna all'80, non al 75 (la misura contaminata esagera il passo).
            int want = static_cast<int>(std::ceil(next / 5.0f - 1e-3f)) * 5;
            if (t - upAt < 30.0 && upFrom < cur && want < upFrom) want = upFrom;  // prova fallita
            if (want < minPct) want = minPct;
            Fail& ff = fail[Slot(cur)];
            ++ff.count;
            double back = p.backoff;
            for (int i = 1; i < ff.count && back < 16.0 * p.backoff; ++i) back *= 2.0;
            ff.until = t + back;
            ff.since = t;
            return want;
        }
        if (underSince >= 0.0 && t - underSince >= 5.0 && dwell >= p.dwell && cur < 100) {
            const int want = cur + 5 > 100 ? 100 : cur + 5;
            if (t >= fail[Slot(want)].until) {
                fail[Slot(want)].since = t;
                upAt = t;
                upFrom = cur;
                return want;
            }
        }
        return cur;
    }
};

// --- Passi del menu VR (Advanced) ------------------------------------------------
//
// Passo di un clic su -/+ per le chiavi numeriche. Prima il passo veniva solo
// dal valore (sotto 1 -> 0,05): "aimpitch 0" si muoveva di 0,05 gradi a clic
// e "openxrgrip 20.6" di 1 grado, arrotondato a 21 (25/10). Le chiavi non in
// tabella tengono la regola di prima: passo dal valore quando il menu viene
// costruito (Item::step), fisso per tutti i clic, cosi' 10 -> 9 -> 10 torna
// indietro (dal valore di ogni clic dava 10 -> 9 -> 9,1; review 25/10).
// lo / hi: limiti del valore, gli stessi del comando in control.cpp
// (follow-up della regressione finale, 27/10: "necksize 1" + un clic scriveva
// 1.10 nell'ini mentre il comando restava a 1; ora il clic si ferma al limite).
struct MenuStep {
    const char* key;
    float step;
    int decimals;
    float lo = -1e30f;
    float hi = 1e30f;
};
inline MenuStep MenuStepFor(const std::string& key, float value) {
    static const MenuStep kSteps[] = {
        {"aimpitch", 1.0f, 1},     {"stockangle", 1.0f, 1, kStockAngleMin, kStockAngleMax},  {"gunangle", 1.0f, 1, kGunAngleMin, kGunAngleMax},
        {"gunyaw", 1.0f, 1, -kGunYawMax, kGunYawMax},
        {"openxrgrip", 0.5f, 1},   {"turnwithhead", 5.0f, 0, 0.0f, 170.0f}, {"turnspeed", 10.0f, 0, 0.0f, 720.0f},  {"snapturn", 15.0f, 0, 0.0f},
        {"vignette", 0.1f, 1, 0.0f, 1.0f},     {"throwboost", 0.1f, 1, 0.5f, 3.0f},  {"grabzone", 1.0f, 0, 2.0f, 30.0f},
        {"masskg", 5.0f, 0, 5.0f, 500.0f},
        {"armstretch", 0.05f, 2, 1.0f, 1.6f},  {"outputscale", 0.05f, 2, 0.25f, 1.0f}, {"weightforce", 50.0f, 0}, {"weighttorque", 2.5f, 1},
        {"weightturn", 0.5f, 1},   {"weightsag", 250.0f, 0}, {"weightdroop", 5.0f, 0},  {"weightbreak", 5.0f, 0},
        {"eyesmooth", 0.02f, 2, 0.0f, 1.0f},   {"crouchtime", 0.05f, 2, 0.0f, 1.5f}, {"height", 0.01f, 2},      {"monitor", 1.0f, 0},
        {"armrot", 1.0f, 0},       {"armoffset", 0.5f, 1},   {"gunoffset", 0.5f, 1},    {"magwell", 0.5f, 1},
        {"xroverscan", 0.25f, 2, 0.0f, 5.0f},  {"predictmax", 10.0f, 0, 20.0f, 250.0f}, {"dynresdown", 0.05f, 2, 0.5f, 2.0f},
        {"dynresslow", 0.02f, 2, 0.5f, 1.5f},
        {"dynresup", 0.05f, 2, 0.2f, 0.95f},    {"dynresdwell", 1.0f, 0, 1.0f, 30.0f}, {"dynresbackoff", 10.0f, 0, 0.0f, 600.0f},
        {"neckmodel", 1.0f, 0},
        {"necksize", 0.1f, 2, 0.0f, 1.0f}, {"neckring", 0.5f, 1, 0.0f, 10.0f}, {"reachslack", 0.5f, 1, 0.0f, 10.0f},
        {"seatedbelow", 0.05f, 2, 0.8f, 1.7f}, {"seatedstand", 0.05f, 2, 0.1f, 0.8f},
        {"bodyalign", 0.1f, 1, 0.0f, 5.0f}, {"bodyalignspeed", 5.0f, 0, 5.0f, 90.0f},
        {"twohandgrip", 1.0f, 0, 3.0f, 20.0f},  {"twohandrelease", 1.0f, 0, 5.0f, 40.0f}, {"twohanddwell", 0.05f, 2, 0.0f, 1.0f},
        {"twohandsmooth", 0.25f, 2, 0.2f, 10.0f},
        {"twohandguard", 0.5f, 1}, {"twohandguardfwd", 0.5f, 1, -30.0f, 30.0f}, {"twohandguardright", 0.5f, 1, -20.0f, 20.0f},
        {"twohandguardup", 0.5f, 1, -20.0f, 20.0f},
        {"stocktol", 1.0f, 0, 1.0f, 20.0f}, {"stocktoldeg", 1.0f, 0, 5.0f, 60.0f},
        {"turretpivot", 1.0f, 0, 10.0f, 80.0f}, {"turretbarmin", 1.0f, 0, 10.0f, 60.0f}, {"turretbarmax", 1.0f, 0, 40.0f, 150.0f}, {"turretbaralign", 1.0f, 0, 10.0f, 75.0f},
        {"turretbarslope", 1.0f, 0, 10.0f, 75.0f},
        {"turretbarblend", 0.02f, 2, 0.0f, 0.5f}, {"turretbarsmooth", 0.25f, 2, 0.1f, 10.0f}, {"turretbarbeta", 0.5f, 1, 0.0f, 20.0f},
        {"turretgroundright", 1.0f, 0, 0.0f, 180.0f}, {"turretgroundleft", 1.0f, 0, 0.0f, 180.0f}, {"turretgroundup", 1.0f, 0, 0.0f, 89.0f},
        {"turretgrounddown", 1.0f, 0, 0.0f, 89.0f},
        {"turretflightright", 1.0f, 0, 0.0f, 180.0f}, {"turretflightleft", 1.0f, 0, 0.0f, 180.0f}, {"turretflightup", 1.0f, 0, 0.0f, 89.0f},
        {"turretflightdown", 1.0f, 0, 0.0f, 89.0f},
    };
    for (const MenuStep& s : kSteps)
        if (key == s.key) return s;
    const float a = std::fabs(value);
    if (a >= 10.0f) return {"", 1.0f, 0};
    if (a >= 1.0f) return {"", 0.1f, 2};
    return {"", 0.05f, 2};
}

// Valore dopo un clic (dir = -1 / +1) per il testo dell'ini "<numero> [resto]":
// il resto ("100" di "hud 0.4 100") resta com'e'. fixedStep > 0: passo delle
// chiavi fuori tabella fissato alla costruzione del menu (regola di prima:
// 0 decimali da 1 in su, altrimenti 2).
inline std::string StepConfigNumber(const std::string& key, const std::string& value, int dir, float fixedStep = 0.0f) {
    std::istringstream in(value);
    float v = 0.0f;
    in >> v;
    std::string rest;
    std::getline(in, rest);
    MenuStep s = MenuStepFor(key, v);
    if (!*s.key && fixedStep > 0.0f) s = {"", fixedStep, fixedStep >= 1.0f ? 0 : 2};
    v += static_cast<float>(dir) * s.step;
    if (v < s.lo) v = s.lo;
    if (v > s.hi) v = s.hi;
    // Somme di passi non esatti in binario (0,1): si riporta il valore al passo.
    const double snapped = std::round(static_cast<double>(v) * 1000.0) / 1000.0;
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", s.decimals, snapped);
    std::string out(b);
    if (out == "-0" || out == "-0.0" || out == "-0.00") out = out.substr(1);
    return out + rest;
}

// Voce Choice di Basic con un valore che non e' tra le scelte (WI-4a: "gunangle
// 2.5" scritto da B+Y): indice della scelta successiva nel verso del clic
// (dir > 0: la prima maggiore, dir < 0: l'ultima minore; oltre i bordi si
// riparte dall'altro capo). Prima si partiva dalla prima scelta: un clic su +
// da 2.5 dava -10. Valori non numerici: la prima (dir > 0) o l'ultima scelta.
inline int NextNumericChoice(const std::vector<std::string>& choices, const std::string& value, int dir) {
    const int n = static_cast<int>(choices.size());
    if (n == 0) return 0;
    char* e = nullptr;
    const double v = std::strtod(value.c_str(), &e);
    if (e == value.c_str()) return dir > 0 ? 0 : n - 1;
    int best = -1;
    for (int i = 0; i < n; ++i) {
        char* ce = nullptr;
        const double c = std::strtod(choices[i].c_str(), &ce);
        if (ce == choices[i].c_str()) continue;
        if (dir > 0 && c > v + 1e-6 && (best < 0 || c < std::strtod(choices[best].c_str(), nullptr))) best = i;
        if (dir < 0 && c < v - 1e-6 && (best < 0 || c > std::strtod(choices[best].c_str(), nullptr))) best = i;
    }
    if (best >= 0) return best;
    return dir > 0 ? 0 : n - 1;
}

// Voci Advanced con valori a parole (review 2 WI-1): l'ini da' al menu solo
// on/off, numeri o testo, e il testo non si cambiava ("predictframe auto" si
// vedeva ma restava fisso; per l'A/B nel visore serviva modificare l'ini e
// riavviare). Per queste chiavi il menu scorre i valori come le voci Choice di
// Basic, nell'ordine della tabella (il predefinito per primo). Solo valori che
// control.cpp capisce; "predict <numero>" resta una voce numerica. Non in
// tabella: chiavi con percorsi o piu' parole (openxrjson, spread, gesture,
// dynres), auto|numero (fov, ipd, height, resolution) e il cambio dell'API di
// input o del modello dei controller a gioco avviato (vrinput, controllerpose).
inline const std::vector<std::string>* AdvancedWordChoices(const std::string& key) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> kTable = {
        {"predictframe", {"auto", "on", "off"}},
        {"predict", {"auto", "off"}},
        {"headcollision", {"push", "off"}},
        {"holdmode", {"normal", "gunstock"}},
        {"crouchview", {"auto", "real", "game"}},
        {"seated", {"auto", "on", "off"}},
        {"fpbody", {"full", "arms"}},
        {"aim", {"hand", "body", "view"}},
        {"movedir", {"head", "body", "hand"}},
        {"turretaim", {"hand", "stick"}},
        {"turretgrip", {"auto", "normal", "gunstock"}},
        {"turretlimits", {"on", "off"}},
        {"hand", {"right", "left"}},
        {"reload", {"simple", "physical"}},
        {"objectweight", {"light", "full", "off"}},
        {"hints", {"auto", "on", "off"}},
        {"runtime", {"auto", "steamvr", "openxr"}},
    };
    for (const auto& e : kTable)
        if (e.first == key) return &e.second;
    return nullptr;
}

// Istogramma fisso dei tempi di attesa in ms (review 2 WI-1, attesa del mutex
// OpenXR): niente vettore con un tetto (4096 campioni si riempivano in 1-2 s
// con ~30 chiamate per tick, e max e "oltre 1 ms" vedevano solo l'inizio della
// finestra). Bin logaritmici, 8 per ottava da 5 us (risoluzione ~9%); quantili
// = bordo superiore del bin; max, somma e conteggi oltre 0,5 / 1 ms esatti.
struct WaitHistogram {
    static constexpr int kBins = 136;  // 5 us .. ~600 ms, poi l'ultimo bin
    static constexpr double kFirstMs = 0.005;
    uint32_t bins[kBins] = {};
    uint32_t n = 0, overHalf = 0, overOne = 0;
    double maxMs = 0.0, sumMs = 0.0;
    static int BinOf(double ms) {
        if (!(ms > kFirstMs)) return 0;
        const int b = 1 + static_cast<int>(std::floor(std::log2(ms / kFirstMs) * 8.0));
        return b >= kBins ? kBins - 1 : b;
    }
    static double UpperEdge(int bin) { return kFirstMs * std::exp2(bin / 8.0); }
    void Add(double ms) {
        if (!(ms >= 0.0)) ms = 0.0;  // anche NaN
        ++bins[BinOf(ms)];
        ++n;
        if (ms > 0.5) ++overHalf;
        if (ms > 1.0) ++overOne;
        if (ms > maxMs) maxMs = ms;
        sumMs += ms;
    }
    // Quantile q (0..1): bordo superiore del bin che lo contiene (al piu' max).
    double Quantile(double q) const {
        if (!n) return 0.0;
        const double target = q * static_cast<double>(n);
        uint32_t acc = 0;
        for (int b = 0; b < kBins; ++b) {
            acc += bins[b];
            if (acc > 0 && static_cast<double>(acc) >= target) return (std::min)(UpperEdge(b), maxMs);
        }
        return maxMs;
    }
    void Reset() { *this = WaitHistogram(); }
};

// --- Corpo, seduti, accucciata (WI-3, bug 4 e 5 del tester) ------------------------
//
// Misure di Walker (26/10, copia B, bodylog con la testa visibile, in piedi):
// occhi (FACE_LEYE / FACE_REYE) 36,25 unita' sopra il centro del Pawn (81 sopra
// il pavimento: un visore a 1,62 m), 8,2 avanti e 5,7 sopra l'osso Neck; Neck
// 30,5 sopra il centro, spalle 26,2. BaseEyeHeight (30) e' solo la camera logica
// del gioco, non gli occhi del modello. Accucciato col gioco gli occhi scendono
// di 29 unita', il centro del Pawn (capsula 45 -> 25) di 20.
constexpr float kWalkerEyeFwdOfNeck = 8.2f, kWalkerEyeAboveNeck = 5.7f;
constexpr float kWalkerEyeStand = 36.25f;  // sopra il centro del Pawn in piedi
constexpr float kWalkerNeckStand = 30.5f;

// Ancora del collo (WI-3): il corpo di Walker segue il collo del giocatore, non
// la posizione del visore. Perno del collo del giocatore P = occhio E meno la
// testa ruotata applicata a (nf, 0, nu) (modello del collo: occhi nf avanti e nu
// sopra il perno). Il punto corrispondente di Walker sta (nf, nu) dietro e sotto
// i suoi occhi, cioe' kWalkerEyeFwdOfNeck - nf avanti al suo Neck. Ritorna lo
// spostamento XY della mesh (riferimento del Pawn: avanti, destra) che porta quel
// punto su P: a testa dritta gli occhi di Walker coincidono con quelli del
// giocatore; girando o chinando la testa il corpo resta fermo (prima seguiva il
// visore 1:1 e guardando in basso il petto avanzava sotto gli occhi).
//   eye: occhio nel riferimento del Pawn; pitch/yaw/roll: testa rispetto al Pawn
//   (radianti, convenzione FRotator); neck: Neck di Walker nel riferimento del
//   Pawn con la mesh non spostata (media).
inline void NeckAnchorShift(const float eye[3], float pitch, float yaw, float roll, float nf, float nu, const float neck[2],
                            float out[2]) {
    float f[3], r[3], u[3];
    UeAxesFromRotator(pitch, yaw, roll, f, r, u);
    const float px = eye[0] - (f[0] * nf + u[0] * nu), py = eye[1] - (f[1] * nf + u[1] * nu);
    out[0] = px - (neck[0] + (kWalkerEyeFwdOfNeck - nf));
    out[1] = py - neck[1];
}

// Occhio "a testa dritta" (WI-3 fix 1): l'altezza che avrebbe l'occhio con la
// stessa testa (stesso perno del collo) ma dritta. Guardando in basso il modello
// del collo porta l'occhio avanti e giu' (a -85 gradi con 8/13 cm: 9,9 unita'
// piu' in basso), ma il collo del giocatore resta dov'e': l'abbassamento del
// corpo deve seguire il perno, non l'occhio. Prima a -60/-85 il corpo scendeva
// di 5-9 unita' e il colletto (con l'apertura del collo nascosto) entrava nella
// meta' bassa della vista (review WI-3, alta 1).
inline float NeckPivotEyeZ(float eyeZ, float pitch, float yaw, float roll, float nf, float nu) {
    float f[3], r[3], u[3];
    UeAxesFromRotator(pitch, yaw, roll, f, r, u);
    return eyeZ - (f[2] * nf + u[2] * nu) + nu;
}

// Portata del braccio dell'arma (WI-3 fix 1, review alta 2): quanto la mesh deve
// andare avanti (unita', >= 0, lungo l'avanti del Pawn) perche' il bersaglio
// della mano, a v dalla spalla (riferimento del Pawn), stia entro R (lunghezza
// del braccio meno il margine). Se nemmeno avanzando ci arriva, la distanza
// minima (v[0]). Sostituisce il limite fisso di prima (mai piu' di 4 unita'
// dietro +10), che legava l'ancora in 81 pose su 90 anche col fucile.
// Fix 2 (review media): con segno (ReachForwardMargin): negativo = di quanto la
// mesh potrebbe arretrare e il bersaglio resterebbe entro R. La spinta del corpo
// e' "spinta di adesso + margine": prima, col solo bisogno >= 0, a bersaglio
// raggiungibile la spinta voluta era uguale a quella attuale e non scendeva mai
// (pistola: 5,4 unita' rimaste col braccio vicino, C3e 5,4).
inline float ReachForwardMargin(const float v[3], float R) {
    if (!(R > 0.0f)) return 0.0f;
    const float side2 = v[1] * v[1] + v[2] * v[2];
    return R * R >= side2 ? v[0] - std::sqrt(R * R - side2) : v[0];
}
inline float ReachForwardNeed(const float v[3], float R) {
    const float need = ReachForwardMargin(v, R);
    return need > 0.0f ? need : 0.0f;
}

// Anello del collo fuori dalla vista (WI-3 fix 1, review alta 1). Con testa e
// Neck nascosti resta l'apertura del colletto attorno al Neck di Walker:
// approssimata da un anello di raggio ringR a ringH sopra il Neck. Guardando in
// basso oltre il bordo inferiore del campo visivo (pitch - alphaDown < -90
// gradi) il bordo inferiore guarda indietro di beta: un punto profondo d sotto
// l'occhio resta fuori solo se sta piu' di d*tan(beta) dietro. Ritorna di quanto
// spostare il corpo all'indietro lungo lo sguardo orizzontale (unita', >= 0)
// perche' il punto dell'anello verso lo sguardo resti fuori di margin. Serve ai
// giocatori alti (occhi 10-20 unita' sopra il collo di Walker); per gli altri il
// perno del collo (NeckAnchorShift) basta.
//   eye, neck: riferimento del Pawn (neck = Neck di Walker come disegnato);
//   pitch/yaw: sguardo rispetto al Pawn (radianti); alphaDown: mezzo campo verso il basso.
inline float NeckRingClearance(const float eye[3], float pitch, float yaw, float alphaDown, const float neck[3], float ringR,
                               float ringH, float margin) {
    const float edge = pitch - alphaDown;  // angolo del bordo inferiore sotto l'orizzonte
    constexpr float kHalfPi = 1.57079632679f;
    if (edge > -kHalfPi) return 0.0f;
    float beta = -edge - kHalfPi;
    if (beta > 1.4f) beta = 1.4f;
    const float gx = std::cos(yaw), gy = std::sin(yaw);
    const float q[3] = {neck[0] + ringR * gx, neck[1] + ringR * gy, neck[2] + ringH};
    const float depth = eye[2] - q[2];
    if (depth <= 0.0f) return 0.0f;
    const float back = -((q[0] - eye[0]) * gx + (q[1] - eye[1]) * gy);
    const float need = depth * std::tan(beta) + margin - back;
    return need > 0.0f ? need : 0.0f;
}

// Ruota v col quaternione (x, y, z, w) di un FBoneAtom (WI-3 fix 2, collo nel
// rendering). Convenzione verificata su Walker (26/10): SpaceBases[Neck].T =
// SpaceBases[Spine3].T + QuatRotateVec(Spine3.q, Spine3.S * LocalAtoms[Neck].T).
inline void QuatRotateVec(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]), ty = 2.0f * (q[2] * v[0] - q[0] * v[2]), tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// Passo dell'ancora: verso il bersaglio al massimo maxSpeed unita'/s, lunghezza
// massima cap (sui tick lunghi o assurdi si usa un tick da 11 ms).
inline void NeckAnchorStep(float cur[2], const float target[2], float dt, float maxSpeed, float cap) {
    float t[2] = {target[0], target[1]};
    const float tl = std::sqrt(t[0] * t[0] + t[1] * t[1]);
    if (tl > cap && tl > 0.0f) {
        t[0] *= cap / tl;
        t[1] *= cap / tl;
    }
    const float step = maxSpeed * (dt > 0.0f && dt < 0.1f ? dt : 0.011f);
    const float d0 = t[0] - cur[0], d1 = t[1] - cur[1], dl = std::sqrt(d0 * d0 + d1 * d1);
    if (dl <= step || dl <= 0.0f) {
        cur[0] = t[0];
        cur[1] = t[1];
    } else {
        cur[0] += d0 * step / dl;
        cur[1] += d1 * step / dl;
    }
}

// Il corpo torna sotto la testa (follow-up della regressione finale, 27/10;
// punto aperto del WI-3). "turnwithhead" gira il corpo solo per la parte oltre
// la soglia (45 gradi) e niente lo riportava indietro: dopo uno sguardo di lato
// e ritorno, senza lo stick, la testa restava 45 gradi fuori dal corpo per
// sempre (seduti e in piedi), e guardando in basso l'occhio stava nello
// spallaccio di Walker (gilet "strappato" da 40 gradi a -85, da 60 a -60).
// Ora, con lo yaw della testa fermo (velocita' filtrata sotto stillDegS) per
// holdSec, il corpo ruota verso la testa: parte da startDeg di scarto, si
// ferma sotto doneDeg; velocita' in rampa (accelerazione maxDegS / rampSec:
// nessuno scatto), al massimo maxDegS, scarto / tauSec vicino all'arrivo (tau
// >= rampa: frenata entro la stessa accelerazione), mai oltre la testa. Testa
// di nuovo in movimento: la rotazione rallenta in rampa. La vista non cambia
// (hooks.cpp sposta il riferimento della testa della stessa rotazione, come
// per turnwithhead); mani, arma e mira restano nello spazio del tracciamento.
// Review del follow-up (27/10): "busy" (arma puntata: due mani, ADS, grilletto
// nell'ultimo secondo): non parte e, se gira, si ferma (review 2: subito, prima
// rallentava in rampa); riparte dopo holdSec di testa ferma senza arma puntata.
struct BodyFollow {
    float holdSec = 0.4f;     // testa ferma per tanto prima di partire (0 = spento)
    float maxDegS = 40.0f;    // velocita' massima del corpo
    float stillDegS = 25.0f;  // sotto: yaw della testa fermo
    float rampSec = 0.3f;     // da 0 alla velocita' massima (e ritorno)
    float tauSec = 0.35f;     // vicino all'arrivo: velocita' = scarto / tau
    float startDeg = 3.0f;    // scarto minimo per partire
    float doneDeg = 0.5f;     // scarto a cui si ferma
    static constexpr float kMaxStepDt = 0.05f;  // tick piu' lunghi: il passo usa questo (40 gradi/s -> al massimo 2 gradi)
    float speedF = 0.0f;      // velocita' dello yaw della testa, filtrata (gradi/s)
    float lastHead = 0.0f;
    bool haveHead = false;
    double stillSince = -1.0;
    float rate = 0.0f;  // velocita' del corpo (gradi/s, col segno dello scarto)
    bool following = false;
    void Reset() {
        speedF = 0.0f;
        haveHead = false;
        stillSince = -1.0;
        rate = 0.0f;
        following = false;
    }
    // now, dt in secondi; headYawRad: yaw della testa nel tracciamento; offRad:
    // testa meno corpo (avvolto); allowed: fuori da copertura, torrette, scene,
    // e solo con la mira del gioco non legata al corpo; busy: arma puntata.
    // Ritorna la rotazione del corpo di questo tick (radianti, verso la testa).
    float Step(double now, float dt, float headYawRad, float offRad, bool allowed, bool busy = false) {
        const float k = 57.2957795f;
        if (!allowed || !(holdSec > 0.0f) || !(maxDegS > 0.0f)) {
            Reset();
            return 0.0f;
        }
        if (!(dt > 0.0f) || dt > 0.25f) {  // tick lungo o assurdo: si riparte
            Reset();
            lastHead = headYawRad;
            haveHead = true;
            return 0.0f;
        }
        const float spd = haveHead ? std::fabs(std::remainder(headYawRad - lastHead, 6.28318530718f)) * k / dt : 0.0f;
        lastHead = headYawRad;
        haveHead = true;
        speedF += (spd - speedF) * (1.0f - std::exp(-dt / 0.08f));
        const float off0 = offRad * k;
        if (busy) {
            // Review 2 del follow-up: arma puntata = fermo subito, non in rampa (la rampa
            // girava ancora fino a 0,3 s dopo grilletto / ADS / due mani). Il corpo non
            // muove la vista (perno sugli occhi), quindi fermarlo di colpo non si vede.
            stillSince = -1.0;
            rate = 0.0f;
            if (following && std::fabs(off0) < doneDeg) following = false;
            return 0.0f;
        }
        if (speedF < stillDegS) {
            if (stillSince < 0.0) stillSince = now;
        } else {
            stillSince = -1.0;
        }
        const bool held = stillSince >= 0.0 && now - stillSince >= holdSec;
        const float off = offRad * k, aoff = std::fabs(off);
        if (following && aoff < doneDeg) following = false;
        if (!following && held && aoff >= startDeg) following = true;
        float target = 0.0f;
        if (following && held) target = std::copysign(std::fmin(maxDegS, aoff / (tauSec > 1e-3f ? tauSec : 1e-3f)), off);
        if (rate * off < 0.0f) rate = 0.0f;  // la testa e' passata dall'altra parte: mai via dalla testa
        // Verifica del follow-up (27/10): un tick lungo (scatto del gioco, 209 ms misurati) dava 8,8 gradi di
        // corpo in un tick, oltre l'anticipo delle mani di UpdateArms (al massimo 5 gradi): canna, colpi e mirino
        // 9,5 gradi fuori per 3 tick. La rampa e il passo usano al massimo kMaxStepDt: un tick lungo gira di meno
        // (la vista non dipende da questo passo).
        const float dts = std::fmin(dt, kMaxStepDt);
        const float acc = maxDegS / (rampSec > 1e-3f ? rampSec : 1e-3f) * dts;
        rate += std::fmax(-acc, std::fmin(acc, target - rate));
        float step = rate * dts;
        if (std::fabs(step) > aoff) step = off;
        return step / k;
    }
};

// Occhi di Walker dal suo Neck (diagnostica: neckstand della riga BODY). I
// seduti usano la costante kWalkerEyeStand (hooks.cpp ViewHeightExtra): prima
// "seated on" teneva fp (40), 3,75 unita' sopra gli occhi di Walker, e "seated
// off" (predefinito) con la testa seduta a 1,2 m la limitava a 30: dentro il
// collo di Walker (Neck 30,5), col busto sotto il mento (tester: "torso
// constantly visible").
inline float SeatedEyeHeight(float walkerNeckZ) {
    const float e = walkerNeckZ + kWalkerEyeAboveNeck;
    return e > 20.0f && e < 60.0f ? e : kWalkerEyeStand;
}

// "seated auto" (WI-3): mediana dell'altezza della testa (m sul pavimento) per
// kWindow s col visore indossato, dopo l'inizio della scena o un ricentramento
// voluto. Seduti sotto kSeatedBelow m, o kBelowCalib m sotto l'altezza in piedi
// salvata (height <m>). Decide solo a una cattura (mai da solo a meta' missione).
// Soglia 1,35 m (fix 1, era 1,30): seduti su una sedia gli occhi stanno a
// ~1,15-1,35 m; in piedi a 1,35 m o meno c'e' solo chi e' alto meno di ~1,47 m.
// Il margine (mediana - soglia) va nella riga SEATED per tararla nel visore.
// Fix 2 (review bassa): la soglia si regola dall'ini ("seatedbelow", Avanzate),
// kSeatedBelow e' il predefinito.
struct SeatedDetector {
    static constexpr float kWindow = 3.0f, kSeatedBelow = 1.35f, kBelowCalib = 0.30f;
    static constexpr int kN = 1024;
    float ys[kN] = {};
    int n = 0;
    double since = -1.0;  // inizio della finestra (-1 = nessuna cattura in corso)
    bool armed = false;
    float lastMedian = 0.0f;
    float seatedBelow = kSeatedBelow;
    void Start() {
        armed = true;
        since = -1.0;
        n = 0;
    }
    // -1 = niente da decidere, 0 = in piedi, 1 = seduti.
    int Step(double now, float y, bool present, float calibratedM) {
        if (!armed) return -1;
        if (!present || !(y > 0.2f && y < 2.6f)) {  // visore tolto o posa assurda: la finestra riparte
            since = -1.0;
            n = 0;
            return -1;
        }
        if (since < 0.0) since = now;
        if (n < kN) ys[n++] = y;
        if (now - since < kWindow || n < 10) return -1;
        float tmp[kN];
        for (int i = 0; i < n; ++i) tmp[i] = ys[i];
        std::nth_element(tmp, tmp + n / 2, tmp + n);
        lastMedian = tmp[n / 2];
        armed = false;
        const bool seated = lastMedian < seatedBelow || (calibratedM > 0.5f && lastMedian < calibratedM - kBelowCalib);
        return seated ? 1 : 0;
    }
};

// "seated auto" riconosciuto seduti e poi in piedi (fix 1, review media): chi
// avvia il gioco seduto alla scrivania e poi si alza restava seduto (occhi 27
// unita' sopra quelli di Walker, accucciata vera spenta) fino a un
// ricentramento. Testa oltre kRise m sopra il riferimento seduto per kHold s di
// fila: si rifa' la misura (SeatedDetector), che decide come all'avvio. Solo in
// su: accucciarsi davvero a lungo non deve mai passare ai seduti.
// Fix 2: kRise / kHold sono i predefiniti, regolabili dall'ini ("seatedstand
// <m> <s>", Avanzate).
struct SeatedStandCheck {
    static constexpr float kRise = 0.30f, kHold = 3.0f;
    float rise = kRise, hold = kHold;
    double since = -1.0;
    void Reset() { since = -1.0; }
    // true una volta quando va rifatta la misura.
    bool Step(double now, float y, float seatedRef, bool active) {
        if (!active || !(y > seatedRef + rise)) {
            since = -1.0;
            return false;
        }
        if (since < 0.0) since = now;
        if (now - since < hold) return false;
        since = -1.0;
        return true;
    }
};

// Ritorno a sedere (fix 2, review media): passati da seduti a in piedi per il
// controllo qui sopra (alzati 6-10 s), il riferimento diventa la testa in piedi
// e nulla rilevava il contrario: tornati a sedere gli occhi finivano ~28 unita'
// sotto quelli di Walker (nel petto) e l'accucciata vera si riarmava (Walker
// accucciato per il resto della partita) fino a un ricentramento. Dopo quel
// passaggio resta armato questo controllo: testa entro kBack m dal vecchio
// riferimento seduto (o sotto) per hold s di fila -> si rifa' la misura, che
// decide come all'avvio. Si disarma a ogni nuova misura (ricentramento, visore
// rimesso, menu) e quando scatta.
struct SeatedReturnCheck {
    static constexpr float kBack = 0.15f;
    bool armed = false;
    float seatedRef = 0.0f;
    float hold = SeatedStandCheck::kHold;
    double since = -1.0;
    void Arm(float ref) {
        armed = ref > 0.2f;
        seatedRef = ref;
        since = -1.0;
    }
    void Disarm() {
        armed = false;
        since = -1.0;
    }
    // true una volta quando va rifatta la misura (e si disarma).
    bool Step(double now, float y, bool present) {
        if (!armed || !present || !(y > 0.2f && y < seatedRef + kBack)) {
            since = -1.0;
            return false;
        }
        if (since < 0.0) since = now;
        if (now - since < hold) return false;
        Disarm();
        return true;
    }
};

// Vista dell'accucciata (WI-3, bug 4: "crouch technically works but there is no
// noticeable change in camera height"). La discesa del Pawn fatta dal gioco (G,
// capsula 45 -> 25) si toglie sempre nello stesso frame (comp = G); un'accucciata
// che il giocatore non ha fatto col corpo (tasto, script, copertura da seduti)
// abbassa la vista di V, che cambia solo ai cambi di stato e mai con la testa,
// in crouchtime s (smoothstep). La testa vera vale sempre 1:1 sopra. Un'accucciata
// vera (physicalcrouch) ha V = 0: mai abbassata due volte.
//   mode: 0 auto, 1 real (prima: comp = G in piedi, niente da seduti), 2 game (comp 0).
struct CrouchViewIn {
    int mode = 0;
    bool active = false;          // vivo, prima persona, a terra, niente scena
    bool seated = false;
    float G = 0.0f;               // abbassamento del Pawn dovuto all'accucciata del gioco (unita')
    bool ownedByReal = false;     // accucciata del gioco chiesta dall'accucciata vera (UpdatePhysicalCrouch)
    float realDrop = 0.0f;        // testa vera sotto il riferimento in piedi (unita', >= 0)
    float standUp = 10.0f;        // sotto questa discesa si e' in piedi davvero (unita')
    bool inCover = false;         // copertura (anche le mosse 19/20)
    bool coverRecent = false;     // copertura vista da poco (1,5 s): solo da riparo a riparo
    bool buttonCrouch = false;    // m_wantsToCrouch del PlayerController acceso, non dall'accucciata vera
    bool coverEdge = false;       // stato di copertura cambiato da meno di 0,3 s: un m_wantsToCrouch che cambia ora e' del gioco
    bool recentRecenter = false;  // ricentramento da meno di 0,5 s: realDrop non vale
    int specialMove = 0;          // YGamePawn.m_specialMove (colpo subito 9/58, scatti 38-42, inciampi 63/64...; 0 = nessuna)
    float rampTime = 0.35f;       // crouchtime (0 = salto sotto un battito di ciglia)
    float dt = 0.0f;
};
struct CrouchViewOut {
    float comp = 0.0f;     // da aggiungere alla base (annulla G)
    float v = 0.0f;        // da togliere dalla base
    bool blink = false;    // salto da coprire col nero (crouchtime 0, "held")
    int source = 0;        // per i log
    bool changed = false;  // sorgente o bersaglio cambiati in questo tick
};
struct CrouchView {
    enum Source { kNone = 0, kButton = 1, kReal = 2, kCover = 3, kHeld = 4 };
    // Fix 2 (review alta): capsula giu' senza tasto ne' riparo = script solo
    // dopo kScriptHold s senza mosse speciali, e mai durante una mossa speciale o
    // nei kSpecialQuiet s dopo (colpo subito, inciampo, scatto: il gioco abbassa
    // la capsula per l'animazione, il giocatore non ha fatto nulla).
    static constexpr float kSpecialQuiet = 0.3f, kScriptHold = 0.5f;
    int source = kNone;
    float v = 0.0f, target = 0.0f, from = 0.0f, rampT = 0.0f, rampDur = 0.0f;
    float realAtStart = 0.0f, heldT = 0.0f, pendingT = 0.0f;
    float specialQuietT = 10.0f;  // s dall'ultima mossa speciale vista
    int startTicks = 0;
    bool prevButton = false, latched = false;  // latched: V tenuto (riparo lasciato ancora accucciati)
    bool popup = false;                        // tasto + riparo, Walker alzato per mirare fuori: V tenuto
    static const char* Name(int s) {
        return s == kButton ? "button/script"
               : s == kReal ? "real crouch"
               : s == kCover ? "cover"
               : s == kHeld ? "held by the game"
                            : "none";
    }
    void Reset() { *this = CrouchView(); }
    CrouchViewOut Step(const CrouchViewIn& in) {
        CrouchViewOut o;
        if (in.mode == 2) o.comp = 0.0f;
        else if (in.mode == 1) o.comp = in.active && !in.seated ? in.G : 0.0f;
        else o.comp = in.active ? in.G : 0.0f;
        if (in.mode != 0 || !in.active) {
            o.changed = source != kNone || v != 0.0f;
            Reset();
            return o;
        }
        const float real = in.recentRecenter ? 0.0f : (in.realDrop > 0.0f ? in.realDrop : 0.0f);
        const int prevSource = source;
        // Un m_wantsToCrouch che si accende insieme a un cambio di copertura
        // (entrata o uscita) e' il gioco che tiene Walker accucciato, non il tasto.
        const bool buttonRise = in.buttonCrouch && !prevButton && !in.coverEdge;
        prevButton = in.buttonCrouch;
        const bool buttonNow = in.buttonCrouch && !in.coverEdge;
        const float dtc = in.dt > 0.0f && in.dt < 0.25f ? in.dt : 0.0f;
        if (in.specialMove != 0) specialQuietT = 0.0f;
        else specialQuietT += dtc;
        bool step = false;
        // Fix 1 (review media): la sorgente viene da m_wantsToCrouch (il tasto e
        // gli script lo accendono, la copertura no), non da finestre di tempo.
        // Prima un'accucciata del tasto diventava "copertura" premendo A a un
        // riparo basso (vista su di 20 con Walker accucciato) e un tasto entro
        // 1,5 s dall'uscita dal riparo restava "copertura" per ~1,8 s.
        if (in.G > 2.0f) {
            popup = false;
            if (source == kNone && ++startTicks >= 2) {
                if (in.ownedByReal) source = kReal;
                else if (buttonNow) source = kButton;
                else if (in.inCover || in.coverRecent) source = kCover;  // da riparo a riparo: lo stato arriva dopo la capsula
                else if (in.specialMove != 0 || specialQuietT < kSpecialQuiet) {
                    // Fix 2 (review alta): colpo subito (9/58), inciampo (63/64),
                    // scatto/scivolata (38-42) abbassano la capsula per l'animazione:
                    // vista ferma (comp = G), mai "script". Prima dopo 0,3 s diventava
                    // un'accucciata e sotto il fuoco la vista scendeva di 20 unita' con
                    // la vignettatura a ogni colpo (Gorge02: ogni ~3 s).
                    pendingT = 0.0f;
                } else {
                    // Capsula giu' senza tasto, riparo ne' mosse: script, o riparo il
                    // cui stato arriva tra poco. Si aspetta kScriptHold s (vista ferma).
                    pendingT += dtc;
                    if (pendingT > kScriptHold) source = kButton;
                }
                if (source != kNone) {
                    realAtStart = real;
                    heldT = pendingT = 0.0f;
                    latched = false;
                }
            }
        } else if (source == kButton && in.inCover) {
            // Fix 2 (review bassa): accucciati col tasto in un riparo basso, mirando
            // fuori (grilletto sinistro o sporgendosi) Walker si alza e la capsula
            // torna alta: e' lo stesso riparo, la vista resta dov'e' (V tenuto, comp
            // segue G). Prima V tornava a 0 (vista su di 20 in 0,35 s) e rientrando
            // nel riparo col tasto ancora acceso scendeva di nuovo di 20; sporgendosi
            // col corpo si sommava al movimento vero della testa.
            popup = true;
            startTicks = 0;
            pendingT = 0.0f;
        } else {
            source = kNone;
            startTicks = 0;
            pendingT = 0.0f;
            latched = false;
            popup = false;
        }
        if (source == kReal) {
            // In piedi davvero ma Walker resta accucciato (soffitto basso, script):
            // la vista raggiunge Walker, sotto un battito (va contro la testa).
            if (!in.ownedByReal || (!in.recentRecenter && real < in.standUp)) heldT += in.dt;
            else heldT = 0.0f;
            if (heldT > 0.5f) {
                source = kHeld;
                step = true;
            }
        }
        // Tasto premuto mentre Walker e' gia' accucciato dal riparo (anche dopo
        // esserne usciti): da qui e' un'accucciata del tasto.
        if (source == kCover && buttonRise) {
            source = kButton;
            realAtStart = real;
            latched = false;
        }
        // Riparo lasciato ancora accucciati: V resta quello che era (in piedi 0,
        // seduti G) finche' Walker non si alza o si preme il tasto. Prima dopo
        // 0,3 s (+1,5 s di attesa) diventava "tasto" e la vista scendeva di 20
        // senza che il giocatore si muovesse. Un'accucciata del tasto che entra in
        // un riparo tiene il suo V (prima diventava "copertura": vista su di 20).
        if (source == kCover && !in.inCover && !in.coverRecent) latched = true;
        float want = 0.0f;
        if (source == kButton) want = popup ? target : in.G - realAtStart;
        else if (source == kHeld) want = in.G;
        else if (source == kCover) want = latched ? target : (in.seated ? in.G : 0.0f);
        if (want < 0.0f) want = 0.0f;
        if (std::fabs(want - target) > 0.01f) {
            const float span = in.G > 20.0f ? in.G : 20.0f;
            float k = std::fabs(want - v) / span;
            k = k < 0.4f ? 0.4f : (k > 1.0f ? 1.0f : k);
            from = v;
            target = want;
            rampT = 0.0f;
            rampDur = in.rampTime * k;
            if (in.rampTime <= 0.0f || step) {
                v = target;
                rampDur = 0.0f;
                o.blink = true;
            }
            o.changed = true;
        }
        if (rampDur > 0.0f && v != target) {
            rampT += in.dt > 0.0f && in.dt < 0.25f ? in.dt : 0.0f;
            const float x = rampT >= rampDur ? 1.0f : rampT / rampDur;
            v = from + (target - from) * x * x * (3.0f - 2.0f * x);
            if (x >= 1.0f) v = target;
        }
        if (source != prevSource) o.changed = true;
        o.v = v;
        o.source = source;
        return o;
    }
};

// --- Cadaveri spinti e trascinati (voce 11, props.cpp CorpseDrive) ----------
// Forma di un corpo del PhysicsAsset negli assi del corpo (RB_BodySetup
// AggGeom): kind 0 sfera (a = raggio), 1 box (a, b, c = mezze misure), 2
// capsula (a = raggio, b = mezza lunghezza del cilindro, lungo l'asse z).
// ax: righe = assi x, y, z della forma; o = origine (unita', scala 1).
struct BodyShape {
    int kind = 0;
    float ax[3][3] = {};
    float o[3] = {};
    float a = 0.0f, b = 0.0f, c = 0.0f;
};

// Distanza (unita', negativa dentro) del punto q (assi del corpo) dalla
// superficie della forma x la scala sc, e il punto della superficie piu'
// vicino (assi del corpo). Dentro un box: la faccia piu' vicina.
inline float BodyShapeDist(const BodyShape& s, float sc, const float q[3], float nearOut[3]) {
    float v[3], x[3];
    for (int c = 0; c < 3; ++c) v[c] = q[c] - s.o[c] * sc;
    for (int k = 0; k < 3; ++k) x[k] = v[0] * s.ax[k][0] + v[1] * s.ax[k][1] + v[2] * s.ax[k][2];
    float n[3] = {x[0], x[1], x[2]};
    float d = 0.0f;
    if (s.kind == 1) {
        const float h[3] = {s.a * sc, s.b * sc, s.c * sc};
        float out2 = 0.0f, in = 1e30f;
        int face = 0;
        for (int k = 0; k < 3; ++k) {
            const float cl = x[k] < -h[k] ? -h[k] : (x[k] > h[k] ? h[k] : x[k]);
            out2 += (x[k] - cl) * (x[k] - cl);
            n[k] = cl;
            const float m = h[k] - std::fabs(x[k]);
            if (m < in) {
                in = m;
                face = k;
            }
        }
        if (out2 > 0.0f) {
            d = std::sqrt(out2);
        } else {
            d = -in;
            n[face] = x[face] >= 0.0f ? h[face] : -h[face];
        }
    } else {
        float c[3] = {0.0f, 0.0f, 0.0f};
        if (s.kind == 2) c[2] = x[2] < -s.b * sc ? -s.b * sc : (x[2] > s.b * sc ? s.b * sc : x[2]);
        const float w[3] = {x[0] - c[0], x[1] - c[1], x[2] - c[2]};
        const float wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), r = s.a * sc;
        d = wl - r;
        for (int k = 0; k < 3; ++k) n[k] = wl > 1e-4f ? c[k] + w[k] * r / wl : c[k];
    }
    for (int c = 0; c < 3; ++c) nearOut[c] = s.o[c] * sc + n[0] * s.ax[0][c] + n[1] * s.ax[1][c] + n[2] * s.ax[2][c];
    return d;
}

// Impulsi di un tick della presa (moduli, unita' di AddImpulse): il corpo
// intero vorrebbe massa totale x dv, al piu' jmax (forza della mano x dt);
// il corpo preso ne ha la sua parte (massa del corpo x dv), il resto va alla
// radice (orizzontale, in props.cpp).
struct CorpseImpulse {
    float limb = 0.0f, root = 0.0f;
};
inline CorpseImpulse CorpseImpulseSplit(float limbMass, float totalMass, float dvl, float jmax) {
    CorpseImpulse r;
    if (!(dvl > 0.0f) || !(jmax > 0.0f) || !(limbMass > 0.0f) || !std::isfinite(dvl + jmax + limbMass)) return r;
    const float whole = (totalMass > limbMass && std::isfinite(totalMass) ? totalMass : limbMass) * dvl;
    const float tot = whole < jmax ? whole : jmax;
    r.limb = limbMass * dvl < tot ? limbMass * dvl : tot;
    r.root = tot - r.limb;
    return r;
}

// Forza della presa che cresce (regola del piano Boneworks: pesante, mai
// immobile): con il punto preso oltre 10 cm dal palmo e senza avvicinarsi
// (errore el non sotto quello del tick prima - 0,2 cm) il moltiplicatore
// raddoppia ogni 0,4 s fino a x4; entro 6 cm torna 1. s = unita' per metro.
struct CorpsePull {
    float mul = 1.0f;
    double growAt = -1.0;
};
inline void CorpsePullStep(CorpsePull& p, float el, float lastEl, float s, double now) {
    if (el < 0.06f * s) {
        p.mul = 1.0f;
        p.growAt = -1.0;
    } else if (el > 0.10f * s && el >= lastEl - 0.002f * s) {
        if (p.growAt < 0.0) {
            p.growAt = now + 0.4;
        } else if (now >= p.growAt && p.mul < 4.0f) {
            p.mul = p.mul * 2.0f > 4.0f ? 4.0f : p.mul * 2.0f;
            p.growAt = now + 0.4;
        }
    } else {
        p.growAt = -1.0;
    }
}

// Migrazione WI-3 (configversion 11, 10 nella copia B; solo righe ai vecchi predefiniti): seduti
// riconosciuti da soli, vista dell'accucciata col tasto, corpo con nomi nuovi
// (on -> full, off -> arms: "off" voleva dire "solo mani e arma").
constexpr ConfigChange kWi3ConfigChanges[] = {
    {kTesterFixesConfigVersion, "seated off", "seated auto"},
    {kTesterFixesConfigVersion, "crouchview real", "crouchview auto"},
    {kTesterFixesConfigVersion, "fpbody on", "fpbody full"},
    {kTesterFixesConfigVersion, "fpbody off", "fpbody arms"},
};

// f3 (27/10): trasformazioni per l'aggiornamento delle ossa di Walker (hooks.cpp, UpdateSkelPoseThunk e
// KeepGoodBones). Matrici UE3 a righe (punto * matrice): righe 0-2 assi X, Y, Z, origine a parte.
// Quaternioni (x, y, z, w) come FQuat / FBoneAtom.
namespace ue3m {
// Riferimento d'attore: assi (righe, ortonormali: RotatorToAxes) e origine (Location).
struct Frame {
    float ax[3][3];
    float org[3];
};
inline void ToLocalDir(const Frame& f, const float w[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = w[0] * f.ax[i][0] + w[1] * f.ax[i][1] + w[2] * f.ax[i][2];
}
inline void ToWorldDir(const Frame& f, const float l[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = l[0] * f.ax[0][i] + l[1] * f.ax[1][i] + l[2] * f.ax[2][i];
}
// LocalToWorld (16 float, righe X, Y, Z, origine a 12..14) nel riferimento dell'attore: righe 0-2 direzioni,
// riga 3 punto.
inline void L2WToActor(const float l2w[16], const Frame& a, float rel[4][3]) {
    for (int r = 0; r < 3; ++r) ToLocalDir(a, l2w + 4 * r, rel[r]);
    const float d[3] = {l2w[12] - a.org[0], l2w[13] - a.org[1], l2w[14] - a.org[2]};
    ToLocalDir(a, d, rel[3]);
}
// Al contrario, con l'origine spostata di dOrg nel riferimento dell'attore (Translation cambiata dallo scatto).
inline void L2WFromActor(const float rel[4][3], const Frame& a, const float dOrg[3], float l2w[16]) {
    for (int r = 0; r < 3; ++r) ToWorldDir(a, rel[r], l2w + 4 * r);
    const float o[3] = {rel[3][0] + dOrg[0], rel[3][1] + dOrg[1], rel[3][2] + dOrg[2]};
    float w[3];
    ToWorldDir(a, o, w);
    for (int i = 0; i < 3; ++i) l2w[12 + i] = a.org[i] + w[i];
}
// Matrice di rotazione (righe) da un quaternione (FQuatRotationTranslationMatrix).
inline void QuatToRows(const float q[4], float m[3][3]) {
    const float x2 = q[0] + q[0], y2 = q[1] + q[1], z2 = q[2] + q[2];
    const float xx = q[0] * x2, xy = q[0] * y2, xz = q[0] * z2, yy = q[1] * y2, yz = q[1] * z2, zz = q[2] * z2;
    const float wx = q[3] * x2, wy = q[3] * y2, wz = q[3] * z2;
    m[0][0] = 1.0f - (yy + zz);
    m[0][1] = xy + wz;
    m[0][2] = xz - wy;
    m[1][0] = xy - wz;
    m[1][1] = 1.0f - (xx + zz);
    m[1][2] = yz + wx;
    m[2][0] = xz + wy;
    m[2][1] = yz - wx;
    m[2][2] = 1.0f - (xx + yy);
}
// Quaternione da una matrice di rotazione a righe (FQuat(const FMatrix&)).
inline void RowsToQuat(const float m[3][3], float q[4]) {
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        const float invS = 1.0f / std::sqrt(tr + 1.0f), s = 0.5f * invS;
        q[3] = 0.5f / invS;
        q[0] = (m[1][2] - m[2][1]) * s;
        q[1] = (m[2][0] - m[0][2]) * s;
        q[2] = (m[0][1] - m[1][0]) * s;
        return;
    }
    int i = 0;
    if (m[1][1] > m[0][0]) i = 1;
    if (m[2][2] > m[i][i]) i = 2;
    static const int nxt[3] = {1, 2, 0};
    const int j = nxt[i], k = nxt[j];
    float s = m[i][i] - m[j][j] - m[k][k] + 1.0f;
    const float invS = 1.0f / std::sqrt(s > 1e-12f ? s : 1e-12f);
    float qt[4];
    qt[i] = 0.5f / invS;
    s = 0.5f * invS;
    qt[3] = (m[j][k] - m[k][j]) * s;
    qt[j] = (m[i][j] + m[j][i]) * s;
    qt[k] = (m[i][k] + m[k][i]) * s;
    for (int n = 0; n < 4; ++n) q[n] = qt[n];
}
inline bool Inverse3(const float m[3][3], float o[3][3]) {
    const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2],
                c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float k = 1.0f / det;
    o[0][0] = c00 * k;
    o[1][0] = c01 * k;
    o[2][0] = c02 * k;
    o[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    o[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    o[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    o[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    o[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    o[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    return true;
}
inline void Mul3(const float a[3][3], const float b[3][3], float o[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}
// f4 (27/10): trasformazione affine a righe, p' = p * r + t (r: righe come una LocalToWorld).
struct Xf {
    float r[3][3];
    float t[3];
};
inline Xf XfFromL2W(const float l2w[16]) {
    Xf x;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; i < 3; ++i) x.r[r][i] = l2w[4 * r + i];
    for (int i = 0; i < 3; ++i) x.t[i] = l2w[12 + i];
    return x;
}
// Prima a, poi b.
inline Xf XfMul(const Xf& a, const Xf& b) {
    Xf o;
    Mul3(a.r, b.r, o.r);
    for (int i = 0; i < 3; ++i) o.t[i] = a.t[0] * b.r[0][i] + a.t[1] * b.r[1][i] + a.t[2] * b.r[2][i] + b.t[i];
    return o;
}
inline bool XfInverse(const Xf& a, Xf* o) {
    if (!Inverse3(a.r, o->r)) return false;
    for (int i = 0; i < 3; ++i) o->t[i] = -(a.t[0] * o->r[0][i] + a.t[1] * o->r[1][i] + a.t[2] * o->r[2][i]);
    return true;
}
inline void XfPoint(const Xf& x, const float p[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = p[0] * x.r[0][i] + p[1] * x.r[1][i] + p[2] * x.r[2][i] + x.t[i];
}
// Riferimento delle mani nel mondo (origine del tracciamento e yaw in radianti, attorno a Z da +X verso +Y): un
// punto fermo nella stanza sta in org + Rz(yaw) x posizione.
struct HandFrameW {
    float org[3];
    float yaw;
};
// Correzione delle ossa rimesse da KeepGoodBones (spazio della componente): le ossa buone sono state composte
// con la LocalToWorld l2wGood e il riferimento delle mani fg; ora la mesh si disegna con l2wNow e il
// riferimento e' fn. Un punto fermo rispetto al riferimento delle mani resta tale:
// M = l2wGood * D * l2wNow^-1, con D = punto del mondo relativo a fg -> stesso punto relativo a fn
// (rotazione di fn.yaw - fg.yaw, origine da fg.org a fn.org). Copre la rotazione e lo spostamento del Pawn,
// la Translation della mesh (ancora del collo) e i passi della stanza. false se una matrice e' singolare.
inline bool RestoreCorrection(const float l2wGood[16], const HandFrameW& fg, const HandFrameW& fn, const float l2wNow[16], Xf* m) {
    Xf d;
    const float dy = fn.yaw - fg.yaw, cs = std::cos(dy), sn = std::sin(dy);
    const float rz[3][3] = {{cs, sn, 0.0f}, {-sn, cs, 0.0f}, {0.0f, 0.0f, 1.0f}};
    std::memcpy(d.r, rz, sizeof(rz));
    for (int i = 0; i < 3; ++i) d.t[i] = fn.org[i] - (fg.org[0] * rz[0][i] + fg.org[1] * rz[1][i] + fg.org[2] * rz[2][i]);
    Xf nowInv;
    if (!XfInverse(XfFromL2W(l2wNow), &nowInv)) return false;
    *m = XfMul(XfMul(XfFromL2W(l2wGood), d), nowInv);
    return true;
}
// Atomo d'osso nello spazio della componente (quaternione q, traslazione t) portato da m: rotazione composta
// (prima l'osso, poi m; m e' una rotazione pura quando le due LocalToWorld hanno la stessa scala) e
// posizione trasformata. Il segno del quaternione resta quello di prima (nessun salto nelle interpolazioni).
inline void TransformBoneAtom(float q[4], float t[3], const Xf& m) {
    float qm[3][3], r[3][3], nq[4];
    QuatToRows(q, qm);
    Mul3(qm, m.r, r);
    RowsToQuat(r, nq);
    const float dot = nq[0] * q[0] + nq[1] * q[1] + nq[2] * q[2] + nq[3] * q[3];
    const float n = std::sqrt(nq[0] * nq[0] + nq[1] * nq[1] + nq[2] * nq[2] + nq[3] * nq[3]);
    const float k = (dot < 0.0f ? -1.0f : 1.0f) / (n > 1e-12f ? n : 1.0f);
    for (int i = 0; i < 4; ++i) q[i] = nq[i] * k;
    float p[3];
    XfPoint(m, t, p);
    for (int i = 0; i < 3; ++i) t[i] = p[i];
}
// Angolo di rotazione (radianti, senza segno, attorno a qualunque asse) e spostamento (unita') del punto p dati
// da m. Lo spazio della mesh di Walker ha -Y in alto: un angolo attorno a Z non sarebbe lo yaw.
inline float XfAngle(const Xf& m) {
    const float c = 0.5f * (m.r[0][0] + m.r[1][1] + m.r[2][2] - 1.0f);
    return std::acos(c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c));
}
inline float XfMoveAt(const Xf& m, const float p[3]) {
    float o[3];
    XfPoint(m, p, o);
    return std::sqrt((o[0] - p[0]) * (o[0] - p[0]) + (o[1] - p[1]) * (o[1] - p[1]) + (o[2] - p[2]) * (o[2] - p[2]));
}
}  // namespace ue3m

}  // namespace vr::pure
