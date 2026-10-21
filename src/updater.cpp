#include "updater.h"
#include "control.h"
#include "logger.h"
#include "version.h"
#include "pure_utils.h"
#include "vr_toast.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <string>
#include <thread>
#include <vector>

// Aggiornamento automatico dalle release di GitHub.
//
// All'avvio (thread a parte, dopo la lettura dell'ini) chiede
// api.github.com/repos/<repo>/releases/latest; se il tag ("v1.2.3") e' piu'
// recente di SPECOPSVR_VERSION scarica l'allegato "d3d9.dll" della release,
// controlla che sia una dll a 32 bit e la mette al posto di quella in uso:
// Windows permette di rinominare una dll caricata, quindi la vecchia diventa
// d3d9.dll.old (cancellata all'avvio seguente) e la nuova vale dal prossimo
// avvio del gioco. "autoupdate off" nell'ini lo disattiva.
//
// Sicurezza (R-R4):
//  - sentinella di avvio: la versione appena installata lascia
//    specopsvr_update.pending; ogni avvio incrementa specopsvr_boot.count, che
//    viene azzerato quando il gioco esce normalmente o resta vivo 90 s dopo la
//    lettura dell'ini. Due avvii morti prima -> al terzo la d3d9.dll.prev
//    torna al suo posto e la versione finisce in specopsvr_skip.txt (non viene
//    reinstallata finche' il file esiste);
//  - file compagni: la release puo' avere un allegato companions.json (nome,
//    dimensione, SHA-256 di openvr_api.dll, script .bat/.ps1, README.txt):
//    quelli diversi o mancanti vengono scaricati, mai specopsvr.ini.
namespace vr::updater {

namespace {

const char* kPendingFile = "\\specopsvr_update.pending";
const char* kBootCountFile = "\\specopsvr_boot.count";
const char* kSkipFile = "\\specopsvr_skip.txt";
constexpr int kFailedBootsBeforeRollback = 2;
constexpr DWORD kHealthyAfterConfigMs = 90000;

std::string ReadTextFile(const std::string& path) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    char buf[256];
    DWORD n = 0;
    std::string out;
    if (ReadFile(f, buf, sizeof(buf), &n, nullptr)) out.assign(buf, n);
    CloseHandle(f);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

bool WriteTextFile(const std::string& path, const std::string& text) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size();
    CloseHandle(f);
    return ok;
}

bool FileExists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string ReadWholeFile(const std::string& path, size_t maxBytes) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string out;
    std::vector<char> buf(1 << 16);
    DWORD n = 0;
    while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n) {
        out.append(buf.data(), n);
        if (out.size() > maxBytes) break;
    }
    CloseHandle(f);
    return out;
}

// Avvio sano: ini letto e gioco vivo per 90 s. Spegne sentinella e contatore.
void ConfirmBoot() {
    vr::control::WaitForConfig(120000, 0);
    if (!vr::control::g_configLoaded) return;
    Sleep(kHealthyAfterConfigMs);
    std::string dir = vr::log::Directory();
    std::string tag = ReadTextFile(dir + kPendingFile);
    DeleteFileA((dir + kBootCountFile).c_str());
    if (DeleteFileA((dir + kPendingFile).c_str()))
        vr::log::Line("UPDATE: version " + tag + " confirmed (healthy start).");
}

// Avvio con un aggiornamento ancora da confermare: conta gli avvii falliti e
// al limite rimette la versione precedente.
void CheckPendingUpdate(const std::string& dir) {
    std::string pending = dir + kPendingFile;
    if (!FileExists(pending)) return;
    std::string tag = ReadTextFile(pending);
    int failed = atoi(ReadTextFile(dir + kBootCountFile).c_str());
    if (failed >= kFailedBootsBeforeRollback) {
        std::string cur = dir + "\\d3d9.dll", prev = dir + "\\d3d9.dll.prev", bad = dir + "\\d3d9.dll.bad";
        DeleteFileA((dir + kBootCountFile).c_str());
        DeleteFileA(pending.c_str());
        if (!FileExists(prev)) {
            vr::log::Line("UPDATE: version " + tag + " died " + std::to_string(failed) +
                          " times right after starting, but there is no d3d9.dll.prev to go back to.");
            return;
        }
        DeleteFileA(bad.c_str());
        if (MoveFileA(cur.c_str(), bad.c_str()) && MoveFileA(prev.c_str(), cur.c_str())) {
            WriteTextFile(dir + kSkipFile, tag);
            vr::log::Line("UPDATE: version " + tag + " died " + std::to_string(failed) +
                          " times right after starting: the previous version is back from the next launch "
                          "(this one kept as d3d9.dll.bad). It will not be reinstalled while specopsvr_skip.txt exists.");
        } else {
            MoveFileA(bad.c_str(), cur.c_str());
            vr::log::Line("UPDATE: version " + tag + " died " + std::to_string(failed) +
                          " times right after starting; automatic rollback failed, run rollback.bat.");
        }
        return;
    }
    WriteTextFile(dir + kBootCountFile, std::to_string(failed + 1));
    vr::log::Line("UPDATE: first starts of version " + tag + " (failed so far: " + std::to_string(failed) + ").");
    std::thread(ConfirmBoot).detach();
}

using vr::pure::IsDll32;
using vr::pure::IsRevoked;
using vr::pure::JsonNumberBefore;
using vr::pure::JsonString;
using vr::pure::ParseVersion;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// GET https con WinHTTP (segue i redirect di GitHub verso i file delle release).
bool HttpGet(const std::wstring& host, const std::wstring& path, std::string* body, size_t maxBytes) {
    HINTERNET s = WinHttpOpen(L"SpecOpsVR-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 5000, 5000, 10000, 20000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: */*\r\n", static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) {
                    ok = false;
                    break;
                }
                if (!avail) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(r, buf.data(), avail, &read)) {
                    ok = false;
                    break;
                }
                body->append(buf.data(), read);
                if (body->size() > maxBytes) {
                    ok = false;
                    break;
                }
            }
        } else {
            vr::log::Line("UPDATE: HTTP status " + std::to_string(status));
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

bool SplitUrl(const std::string& url, std::wstring* host, std::wstring* path) {
    const std::string pre = "https://";
    if (url.rfind(pre, 0) != 0) return false;
    size_t slash = url.find('/', pre.size());
    if (slash == std::string::npos) return false;
    *host = Widen(url.substr(pre.size(), slash - pre.size()));
    *path = Widen(url.substr(slash));
    return true;
}

// SHA-256 esadecimale minuscolo (CNG).
std::string Sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32];
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
        static const char* hex = "0123456789abcdef";
        for (unsigned char c : digest) {
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// Il manifesto updates.json nel ramo main del repository elenca le versioni
// ritirate ("revoked") e quella raccomandata: chi ha una versione ritirata
// riceve la raccomandata anche se e' piu' vecchia (18/10: la 0.9.1 con le
// immagini doppie restava installata perche' l'aggiornamento non torna mai
// indietro). Esempio: {"revoked": ["0.9.1"], "recommended": "v0.9.2"}
// File compagni della release (companions.json): {"files": [{"name": "...",
// "size": N, "sha256": "..."}]}. Scarica quelli mancanti o diversi dagli
// allegati della stessa release. openvr_api.dll e' caricata: la vecchia viene
// rinominata .old come la d3d9.dll. Mai specopsvr.ini, mai d3d9.dll.
void RefreshCompanions(const std::string& json, const std::string& dir) {
    std::string manifestUrl;
    size_t at = 0;
    for (;;) {
        std::string u = JsonString(json, "browser_download_url", at, &at);
        if (u.empty()) break;
        if (u.size() >= 16 && _stricmp(u.substr(u.size() - 16).c_str(), "/companions.json") == 0) {
            manifestUrl = u;
            break;
        }
    }
    if (manifestUrl.empty()) return;  // release senza manifesto: solo la dll
    std::wstring host, path;
    std::string manifest;
    if (!SplitUrl(manifestUrl, &host, &path) || !HttpGet(host, path, &manifest, 1 << 16)) {
        vr::log::Line("UPDATE: cannot read companions.json.");
        return;
    }
    std::string refreshed, failed;
    for (const vr::pure::Companion& c : vr::pure::ParseCompanions(manifest)) {
        const std::string& name = c.name;
        const std::string& sha = c.sha256;
        long long size = c.size;
        std::string local = dir + "\\" + name;
        std::string cur = ReadWholeFile(local, 64u << 20);
        if (!cur.empty() && static_cast<long long>(cur.size()) == size && _stricmp(Sha256Hex(cur).c_str(), sha.c_str()) == 0)
            continue;  // identico
        std::string url;
        size_t a = 0;
        for (;;) {
            std::string u = JsonString(json, "browser_download_url", a, &a);
            if (u.empty()) break;
            if (u.size() > name.size() && _stricmp(u.substr(u.size() - name.size() - 1).c_str(), ("/" + name).c_str()) == 0) {
                url = u;
                break;
            }
        }
        std::string data;
        if (url.empty() || !SplitUrl(url, &host, &path) || !HttpGet(host, path, &data, 64u << 20) ||
            static_cast<long long>(data.size()) != size || _stricmp(Sha256Hex(data).c_str(), sha.c_str()) != 0) {
            failed += " " + name;
            continue;
        }
        std::string neu = local + ".new", old = local + ".old";
        if (!WriteTextFile(neu, data)) {
            failed += " " + name;
            continue;
        }
        DeleteFileA(old.c_str());
        bool ok;
        if (FileExists(local)) ok = MoveFileA(local.c_str(), old.c_str()) && MoveFileA(neu.c_str(), local.c_str());
        else ok = MoveFileA(neu.c_str(), local.c_str()) != 0;
        if (ok) {
            refreshed += " " + name;
            if (_stricmp(name.c_str(), "openvr_api.dll") != 0) DeleteFileA(old.c_str());  // la dll e' in uso
        } else {
            MoveFileA(old.c_str(), local.c_str());
            DeleteFileA(neu.c_str());
            failed += " " + name;
        }
    }
    if (!refreshed.empty()) vr::log::Line("UPDATE: companion files refreshed:" + refreshed + ".");
    if (!failed.empty()) vr::log::Line("UPDATE: companion files not refreshed:" + failed + ".");
}

void Run() {
    vr::control::WaitForConfig(120000, 5000);  // dopo la lettura dell'ini e l'avvio del gioco
    // Ini mai caricato dai hook (eseguibile non supportato, primo EndScene
    // oltre 120 s): "autoupdate off", canale e repository valevano zero (M5).
    if (!vr::control::g_configLoaded) vr::control::ReadUpdateSettingsFromIni();
    if (!vr::control::g_autoUpdate) {
        vr::log::Line("UPDATE: disabled (autoupdate off).");
        return;
    }
    std::string repo = vr::control::UpdateRepo();
    if (repo.empty()) repo = SPECOPSVR_REPO;
    if (repo.empty() || repo.find('/') == std::string::npos) {
        vr::log::Line("UPDATE: no repository configured.");
        return;
    }
    bool beta = vr::control::g_updateBeta;
    std::string manifest, json, tag;
    bool revoked = false;
    if (HttpGet(L"raw.githubusercontent.com", Widen("/" + repo + "/main/updates.json"), &manifest, 1 << 16))
        revoked = IsRevoked(manifest, SPECOPSVR_VERSION);
    std::string recommended = revoked ? JsonString(manifest, "recommended") : std::string();
    std::string api = !recommended.empty() ? "/releases/tags/" + recommended
                      : beta               ? "/releases?per_page=5"  // la piu' recente, anche pre-release
                                           : "/releases/latest";     // esclude le pre-release
    if (!HttpGet(L"api.github.com", Widen("/repos/" + repo + api), &json, 1 << 21)) {
        vr::log::Line("UPDATE: cannot read the releases of " + repo + ".");
        return;
    }
    size_t tagAt = 0;
    tag = JsonString(json, "tag_name", 0, &tagAt);
    if (tag.empty()) return;
    // Mai una versione ritirata: se l'ultima release e' nella lista, chi aveva
    // la versione buona la installava, al lancio dopo risultava ritirata e
    // tornava alla raccomandata, all'infinito (audit 20/10). Si va invece
    // sulla raccomandata (se piu' nuova) o si resta dove si e'.
    if (!revoked && IsRevoked(manifest, tag)) {
        std::string rec = JsonString(manifest, "recommended");
        if (rec.empty() || IsRevoked(manifest, rec) || !(ParseVersion(rec) > ParseVersion(SPECOPSVR_VERSION))) {
            vr::log::Line("UPDATE: the latest release " + tag + " was withdrawn; staying on " SPECOPSVR_VERSION ".");
            return;
        }
        json.clear();
        if (!HttpGet(L"api.github.com", Widen("/repos/" + repo + "/releases/tags/" + rec), &json, 1 << 21)) {
            vr::log::Line("UPDATE: cannot read the recommended release " + rec + ".");
            return;
        }
        tag = JsonString(json, "tag_name", 0, &tagAt);
        if (tag.empty()) return;
        vr::log::Line("UPDATE: the latest release was withdrawn; using the recommended " + tag + ".");
    }
    // Con la lista (canale beta) gli allegati cercati sono quelli della prima release.
    size_t nextRelease = json.find("\"tag_name\"", tagAt);
    if (nextRelease != std::string::npos) json = json.substr(0, nextRelease);
    std::string dir = vr::log::Directory();
    if (revoked) {
        vr::log::Line("UPDATE: version " SPECOPSVR_VERSION " was withdrawn; installing the recommended " + tag + ".");
    } else if (ParseVersion(tag) == ParseVersion(SPECOPSVR_VERSION)) {
        vr::log::Line("UPDATE: version " SPECOPSVR_VERSION " is up to date (" + std::string(beta ? "beta" : "stable") +
                      " channel).");
        RefreshCompanions(json, dir);
        return;
    } else if (ParseVersion(tag) < ParseVersion(SPECOPSVR_VERSION)) {
        vr::log::Line("UPDATE: version " SPECOPSVR_VERSION " is newer than the latest " +
                      std::string(beta ? "beta" : "stable") + " release " + tag + " (test build?).");
        return;
    }
    // Versione rimessa indietro dalla sentinella di avvio: non la si reinstalla.
    std::string skip = ReadTextFile(dir + kSkipFile);
    if (!skip.empty() && ParseVersion(skip) == ParseVersion(tag)) {
        vr::log::Line("UPDATE: version " + tag + " was rolled back after failed starts; not reinstalling it "
                      "(delete specopsvr_skip.txt to try again).");
        return;
    }
    // Allegato "d3d9.dll" della release.
    std::string url;
    size_t at = 0, assetFrom = 0, assetUrlAt = 0;
    for (;;) {
        size_t prev = at;
        std::string u = JsonString(json, "browser_download_url", at, &at);
        if (u.empty()) break;
        if (u.size() >= 9 && _stricmp(u.substr(u.size() - 9).c_str(), "/d3d9.dll") == 0) {
            url = u;
            assetFrom = prev;
            assetUrlAt = at;
            break;
        }
    }
    // Dimensione e SHA-256 dichiarati da GitHub per l'allegato (campi "size" e
    // "digest" dell'oggetto, prima di browser_download_url): una dll troncata
    // o alterata non sostituisce quella in uso (audit 19/10).
    long long wantSize = url.empty() ? -1 : JsonNumberBefore(json, "size", assetFrom, assetUrlAt);
    std::string wantDigest;
    if (!url.empty()) {
        size_t d = json.rfind("\"digest\"", assetUrlAt);
        if (d != std::string::npos && d >= assetFrom) {
            std::string v = JsonString(json, "digest", d);
            if (v.rfind("sha256:", 0) == 0) wantDigest = v.substr(7);
        }
    }
    if (url.empty()) {
        vr::log::Line("UPDATE: release " + tag + " has no d3d9.dll asset.");
        return;
    }
    vr::log::Line("UPDATE: new version " + tag + ", downloading...");
    std::wstring host, path;
    std::string dll;
    if (!SplitUrl(url, &host, &path) || !HttpGet(host, path, &dll, 64u << 20) || !IsDll32(dll)) {
        vr::log::Line("UPDATE: download failed or invalid file.");
        return;
    }
    if (wantSize > 0 && static_cast<long long>(dll.size()) != wantSize) {
        vr::log::Line("UPDATE: downloaded " + std::to_string(dll.size()) + " bytes instead of " + std::to_string(wantSize) +
                      ", not installed.");
        return;
    }
    if (!wantDigest.empty()) {
        std::string got = Sha256Hex(dll);
        if (_stricmp(got.c_str(), wantDigest.c_str()) != 0) {
            vr::log::Line("UPDATE: SHA-256 mismatch (" + got + " instead of " + wantDigest + "), not installed.");
            return;
        }
        vr::log::Line("UPDATE: SHA-256 verified.");
    } else {
        vr::log::Line("UPDATE: the release gives no SHA-256 for d3d9.dll, size checked only.");
    }
    std::string cur = dir + "\\d3d9.dll", neu = dir + "\\d3d9.dll.new", old = dir + "\\d3d9.dll.old";
    HANDLE f = CreateFileA(neu.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, dll.data(), static_cast<DWORD>(dll.size()), &written, nullptr) &&
              written == dll.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileA(old.c_str());
    if (!ok || !MoveFileA(cur.c_str(), old.c_str())) {
        vr::log::Line("UPDATE: cannot replace the dll (permissions?).");
        DeleteFileA(neu.c_str());
        return;
    }
    if (!MoveFileA(neu.c_str(), cur.c_str())) {
        MoveFileA(old.c_str(), cur.c_str());  // rimette la vecchia
        vr::log::Line("UPDATE: cannot install the new dll.");
        return;
    }
    // Sentinella per i primi avvii della nuova versione (vedi CheckPendingUpdate).
    WriteTextFile(dir + kPendingFile, tag);
    DeleteFileA((dir + kBootCountFile).c_str());
    vr::log::Line("UPDATE: version " + tag + " installed, active from the next game start.");
    vr::toast::Notify("SpecOpsVR " + tag + " installed: active from the next start");
    RefreshCompanions(json, dir);
}

}  // namespace

void Start() {
    // Avanzi di un aggiornamento precedente: la versione sostituita resta come
    // d3d9.dll.prev (rollback.bat la rimette al suo posto).
    std::string dir = vr::log::Directory();
    std::string old = dir + "\\d3d9.dll.old";
    if (FileExists(old)) MoveFileExA(old.c_str(), (dir + "\\d3d9.dll.prev").c_str(), MOVEFILE_REPLACE_EXISTING);
    DeleteFileA((dir + "\\d3d9.dll.new").c_str());
    old = dir + "\\openvr_api.dll.old";
    if (FileExists(old)) DeleteFileA(old.c_str());
    CheckPendingUpdate(dir);
    std::thread(Run).detach();
}

void OnCleanExit() { DeleteFileA((vr::log::Directory() + kBootCountFile).c_str()); }

}  // namespace vr::updater
