#pragma once
// Funzioni pure (niente Windows, niente gioco, niente log): versioni delle
// release, manifesto updates.json, JSON minimale di GitHub, controllo della
// dll a 32 bit, normalizzazione delle righe dell'ini e migrazione dei
// predefiniti. Usate da updater.cpp e control.cpp e coperte da
// tests/unit_tests.cpp (R-T2).
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
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
inline bool MigrateConfigLines(std::vector<std::string>& lines, int targetVersion, const ConfigChange* changes,
                               size_t changeCount, std::string* done) {
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

}  // namespace vr::pure
