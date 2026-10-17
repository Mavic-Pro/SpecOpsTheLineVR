#include "updater.h"
#include "control.h"
#include "logger.h"
#include "version.h"

#include <windows.h>
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
namespace vr::updater {

namespace {

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
            vr::log::Line("AGGIORNAMENTO: risposta HTTP " + std::to_string(status));
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

// Valore stringa di una chiave JSON a partire da "from" (niente parser: la
// risposta di GitHub ha forma fissa).
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* at = nullptr) {
    std::string k = "\"" + key + "\"";
    size_t p = json.find(k, from);
    if (p == std::string::npos) return {};
    p = json.find('"', json.find(':', p + k.size()) + 1);
    size_t e = json.find('"', p + 1);
    if (p == std::string::npos || e == std::string::npos) return {};
    if (at) *at = e;
    return json.substr(p + 1, e - p - 1);
}

std::vector<int> ParseVersion(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && out.size() < 4) {
        size_t j = i;
        while (j < v.size() && isdigit(static_cast<unsigned char>(v[j]))) ++j;
        if (j == i) break;
        out.push_back(atoi(v.substr(i, j - i).c_str()));
        if (j >= v.size() || v[j] != '.') break;
        i = j + 1;
    }
    while (out.size() < 3) out.push_back(0);
    return out;
}

bool IsDll32(const std::string& data) {
    if (data.size() < 0x200 || data[0] != 'M' || data[1] != 'Z') return false;
    uint32_t pe = *reinterpret_cast<const uint32_t*>(data.data() + 0x3C);
    if (pe + 6 > data.size() || data.compare(pe, 4, std::string("PE\0\0", 4)) != 0) return false;
    uint16_t machine = *reinterpret_cast<const uint16_t*>(data.data() + pe + 4);
    return machine == 0x014C;  // IMAGE_FILE_MACHINE_I386
}

void Run() {
    Sleep(15000);  // dopo la lettura dell'ini e l'avvio del gioco
    if (!vr::control::g_autoUpdate) {
        vr::log::Line("AGGIORNAMENTO: disattivato (autoupdate off).");
        return;
    }
    std::string repo = vr::control::UpdateRepo();
    if (repo.empty()) repo = SPECOPSVR_REPO;
    if (repo.empty() || repo.find('/') == std::string::npos) {
        vr::log::Line("AGGIORNAMENTO: nessun repository configurato.");
        return;
    }
    std::string json;
    if (!HttpGet(L"api.github.com", Widen("/repos/" + repo + "/releases/latest"), &json, 1 << 20)) {
        vr::log::Line("AGGIORNAMENTO: impossibile leggere l'ultima release di " + repo + ".");
        return;
    }
    std::string tag = JsonString(json, "tag_name");
    if (tag.empty()) return;
    if (ParseVersion(tag) <= ParseVersion(SPECOPSVR_VERSION)) {
        vr::log::Line("AGGIORNAMENTO: versione " SPECOPSVR_VERSION " aggiornata (ultima release " + tag + ").");
        return;
    }
    // Allegato "d3d9.dll" della release.
    std::string url;
    size_t at = 0;
    for (;;) {
        std::string u = JsonString(json, "browser_download_url", at, &at);
        if (u.empty()) break;
        if (u.size() >= 9 && _stricmp(u.substr(u.size() - 9).c_str(), "/d3d9.dll") == 0) {
            url = u;
            break;
        }
    }
    if (url.empty()) {
        vr::log::Line("AGGIORNAMENTO: la release " + tag + " non ha l'allegato d3d9.dll.");
        return;
    }
    vr::log::Line("AGGIORNAMENTO: nuova versione " + tag + ", scaricamento...");
    std::wstring host, path;
    std::string dll;
    if (!SplitUrl(url, &host, &path) || !HttpGet(host, path, &dll, 64u << 20) || !IsDll32(dll)) {
        vr::log::Line("AGGIORNAMENTO: scaricamento non riuscito o file non valido.");
        return;
    }
    std::string dir = vr::log::Directory();
    std::string cur = dir + "\\d3d9.dll", neu = dir + "\\d3d9.dll.new", old = dir + "\\d3d9.dll.old";
    HANDLE f = CreateFileA(neu.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, dll.data(), static_cast<DWORD>(dll.size()), &written, nullptr) &&
              written == dll.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileA(old.c_str());
    if (!ok || !MoveFileA(cur.c_str(), old.c_str())) {
        vr::log::Line("AGGIORNAMENTO: impossibile sostituire la dll (permessi?).");
        DeleteFileA(neu.c_str());
        return;
    }
    if (!MoveFileA(neu.c_str(), cur.c_str())) {
        MoveFileA(old.c_str(), cur.c_str());  // rimette la vecchia
        vr::log::Line("AGGIORNAMENTO: impossibile installare la nuova dll.");
        return;
    }
    vr::log::Line("AGGIORNAMENTO: versione " + tag + " installata, attiva dal prossimo avvio del gioco.");
}

}  // namespace

void Start() {
    // Avanzi di un aggiornamento precedente.
    std::string dir = vr::log::Directory();
    DeleteFileA((dir + "\\d3d9.dll.old").c_str());
    DeleteFileA((dir + "\\d3d9.dll.new").c_str());
    std::thread(Run).detach();
}

}  // namespace vr::updater
