#include "haptics_ext.h"
#include "control.h"
#include "logger.h"
#include "resource_ids.h"

#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <vector>
#include <mutex>
#include <string>
#include <thread>

// Feedback aptico esterno (18/10, richiesta: "integra i feedback aptici come
// ProVolver e bHaptics").
//
// ProTubeVR: API C di ForceTubeVR_API: InitPistol() avvia il collegamento
// Bluetooth (primo dispositivo nel canale pistol1, secondo in pistol2),
// KickChannel(power, channel) da' il colpo, RumbleChannel(power, seconds,
// channel) vibra. L'unica dll a 32 bit pubblica e' del 2020 e non gestisce i
// ProVolver (20/10): la via normale e' il ponte a 64 bit (bridge/, incorporato
// come risorsa) che carica la dll aggiornata della ProTubeVR Companion App e
// riceve i comandi su una named pipe. Una ForceTubeVR_API_x32.dll messa a mano
// accanto al gioco ha ancora la precedenza.
//
// bHaptics: il bHaptics Player espone un websocket su
// ws://127.0.0.1:15881/v2/feedbacks; i messaggi "Submit" di tipo "frame"
// accendono singoli motori (dotPoints index/intensity) per durationMillis.
// Posizioni: "Vest" (0-19 davanti, 20-39 dietro), "ForearmL"/"ForearmR" (0-5).
namespace vr::haptics {

namespace {

// --- ProTube ------------------------------------------------------------------
enum ForceTubeChannel { kAll, kRifle, kRifleButt, kRifleBolt, kPistol1, kPistol2, kOther, kVest };
typedef void(__cdecl* InitPistol_t)();
typedef void(__cdecl* KickChannel_t)(unsigned char power, int channel);
typedef void(__cdecl* RumbleChannel_t)(unsigned char power, float seconds, int channel);
KickChannel_t g_kick = nullptr;
RumbleChannel_t g_rumble = nullptr;

// Messaggio verso il ponte (stesso formato di bridge/protube_bridge.cpp).
#pragma pack(push, 1)
struct ProTubeMsg {
    unsigned char op, power, power2, channel;  // op: 1 kick, 2 rumble, 3 shot
    float seconds;
};
#pragma pack(pop)
static_assert(sizeof(ProTubeMsg) == 8, "protocollo a 8 byte");
std::mutex g_ptMutex;
std::condition_variable g_ptCv;
std::deque<ProTubeMsg> g_ptQueue;
std::atomic<bool> g_ptBridge{false};

// Canali: il primo ProVolver trovato (pistol1) e' la mano dell'arma, il
// secondo (pistol2) quella d'appoggio; "protube swap" li scambia.
int GunChannel() { return vr::control::g_protubeSwap ? kPistol2 : kPistol1; }
int SupportChannel() { return vr::control::g_protubeSwap ? kPistol1 : kPistol2; }

void Kick(unsigned char power, int channel) {
    if (g_kick) {
        g_kick(power, channel);
    } else if (g_ptBridge) {
        std::lock_guard<std::mutex> lock(g_ptMutex);
        if (g_ptQueue.size() > 16) g_ptQueue.pop_front();
        g_ptQueue.push_back({1, power, 0, static_cast<unsigned char>(channel), 0.0f});
        g_ptCv.notify_one();
    }
}

void Rumble(unsigned char power, float seconds, int channel) {
    if (g_rumble) {
        g_rumble(power, seconds, channel);
    } else if (g_ptBridge) {
        std::lock_guard<std::mutex> lock(g_ptMutex);
        if (g_ptQueue.size() > 16) g_ptQueue.pop_front();
        g_ptQueue.push_back({2, power, 0, static_cast<unsigned char>(channel), seconds});
        g_ptCv.notify_one();
    }
}

// Percorsi in Unicode: una cartella del gioco con caratteri fuori dalla
// code page ANSI (nomi utente, librerie Steam) faceva fallire estrazione e
// avvio del ponte (audit 20/10).
std::wstring SelfDirectoryW() {
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfDirectoryW), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

// Il ponte e' incorporato in d3d9.dll: lo riscrive accanto al gioco solo se
// manca o e' diverso (dopo un aggiornamento della mod).
bool ExtractBridge(const std::wstring& exe) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ExtractBridge), &self);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(IDR_PROTUBE_BRIDGE), MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(self, res) : nullptr;
    const char* data = h ? static_cast<const char*>(LockResource(h)) : nullptr;
    DWORD size = res ? SizeofResource(self, res) : 0;
    if (!data || !size) return false;
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::vector<char> cur(size + 1);
        DWORD got = 0;
        bool same = ReadFile(f, cur.data(), size + 1, &got, nullptr) && got == size && memcmp(cur.data(), data, size) == 0;
        CloseHandle(f);
        if (same) return true;
    }
    std::wstring tmp = exe + L".new";
    f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, size, &put, nullptr) && put == size;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES;  // vecchia copia ancora utilizzabile
    }
    return true;
}

// Avvia il ponte, si collega alla sua pipe e gli inoltra i comandi in coda
// (thread proprio: il gioco non aspetta mai il Bluetooth).
void BridgeThread() {
    std::wstring dir = SelfDirectoryW();
    std::wstring exe = dir + L"specopsvr_protube.exe";
    if (!ExtractBridge(exe)) {
        vr::log::Line("HAPTICS: ProTube off (cannot write specopsvr_protube.exe next to the game).");
        return;
    }
    DWORD pid = GetCurrentProcessId();
    std::string shortName = "SpecOpsVR_ProTube_" + std::to_string(pid);
    std::string pipeName = "\\\\.\\pipe\\" + shortName;
    std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(pid) + L" " +
                       std::wstring(shortName.begin(), shortName.end());
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si,
                        &pi)) {
        vr::log::Line("HAPTICS: ProTube off (cannot start specopsvr_protube.exe, error " +
                      std::to_string(GetLastError()) + ").");
        return;
    }
    CloseHandle(pi.hThread);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 300 && pipe == INVALID_HANDLE_VALUE; ++i) {  // fino a ~30 s
        pipe = CreateFileA(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        DWORD code = STILL_ACTIVE;
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            vr::log::Line(code == 4 ? "HAPTICS: ProTube off (ProTubeVR Companion App not installed, it provides "
                                      "ForceTubeVR_API_x64.dll)."
                                    : "HAPTICS: ProTube bridge exited (code " + std::to_string(code) +
                                          ", see specopsvr_protube.log).");
            return;
        }
    }
    CloseHandle(pi.hProcess);
    if (pipe == INVALID_HANDLE_VALUE) {
        vr::log::Line("HAPTICS: ProTube bridge did not answer, ProTube off.");
        return;
    }
    g_ptBridge = true;
    vr::log::Line("HAPTICS: ProTube bridge running (Companion App dll, devices in specopsvr_protube.log).");
    for (;;) {
        ProTubeMsg m;
        {
            std::unique_lock<std::mutex> lock(g_ptMutex);
            g_ptCv.wait(lock, [] { return !g_ptQueue.empty(); });
            m = g_ptQueue.front();
            g_ptQueue.pop_front();
        }
        DWORD put = 0;
        if (!WriteFile(pipe, &m, sizeof(m), &put, nullptr)) break;
    }
    g_ptBridge = false;
    CloseHandle(pipe);
    vr::log::Line("HAPTICS: ProTube bridge disconnected.");
}

void StartProTube() {
    if (!vr::control::g_protube) return;
    HMODULE dll = LoadLibraryW((SelfDirectoryW() + L"ForceTubeVR_API_x32.dll").c_str());
    if (!dll) {
        std::thread(BridgeThread).detach();
        return;
    }
    auto init = reinterpret_cast<InitPistol_t>(GetProcAddress(dll, "InitPistol"));
    g_kick = reinterpret_cast<KickChannel_t>(GetProcAddress(dll, "KickChannel"));
    g_rumble = reinterpret_cast<RumbleChannel_t>(GetProcAddress(dll, "RumbleChannel"));
    if (!init || !g_kick || !g_rumble) {
        g_kick = nullptr;
        g_rumble = nullptr;
        vr::log::Line("HAPTICS: ForceTubeVR_API_x32.dll has unexpected exports, trying the 64-bit bridge.");
        std::thread(BridgeThread).detach();
        return;
    }
    init();  // collegamento Bluetooth in background (dentro la dll)
    vr::log::Line("HAPTICS: ProTube initialised (ForceTubeVR_API_x32.dll next to the game).");
}

// --- bHaptics -----------------------------------------------------------------
std::mutex g_qMutex;
std::condition_variable g_qCv;
std::deque<std::string> g_queue;
std::atomic<bool> g_bhConnected{false};

void Enqueue(const std::string& msg) {
    if (!g_bhConnected) return;
    std::lock_guard<std::mutex> lock(g_qMutex);
    if (g_queue.size() > 32) g_queue.pop_front();
    g_queue.push_back(msg);
    g_qCv.notify_one();
}

// Un frame su una posizione: motori [first, last] all'intensita' data.
void Frame(const char* key, const char* position, int first, int last, int intensity, int ms) {
    std::string dots;
    for (int i = first; i <= last; ++i) {
        if (!dots.empty()) dots += ",";
        dots += "{\"index\":" + std::to_string(i) + ",\"intensity\":" + std::to_string(intensity) + "}";
    }
    Enqueue("{\"Submit\":[{\"Type\":\"frame\",\"Key\":\"" + std::string(key) + "\",\"Frame\":{\"position\":\"" +
            position + "\",\"dotPoints\":[" + dots + "],\"durationMillis\":" + std::to_string(ms) + "}}]}");
}

void BHapticsThread() {
    bool loggedFail = false;
    while (vr::control::g_bhaptics) {
        HINTERNET s = WinHttpOpen(L"SpecOpsVR-haptics", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET c = s ? WinHttpConnect(s, L"127.0.0.1", 15881, 0) : nullptr;
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/v2/feedbacks?app_id=specopsthelinevr&app_name=SpecOpsTheLineVR",
                                             nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                        : nullptr;
        HINTERNET ws = nullptr;
        if (r && WinHttpSetOption(r, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
            WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            ws = WinHttpWebSocketCompleteUpgrade(r, 0);
        }
        if (r) WinHttpCloseHandle(r);
        if (ws) {
            g_bhConnected = true;
            loggedFail = false;
            vr::log::Line("HAPTICS: connected to the bHaptics Player.");
            // Il Player manda messaggi di stato: vanno letti (e scartati),
            // altrimenti il buffer di ricezione si riempie e la connessione
            // cade (audit 19/10).
            std::thread reader([ws] {
                std::vector<char> buf(8192);
                for (;;) {
                    DWORD got = 0;
                    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                    if (WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &got, &type) != NO_ERROR ||
                        type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                        break;
                }
            });
            for (;;) {
                std::string msg;
                {
                    std::unique_lock<std::mutex> lock(g_qMutex);
                    g_qCv.wait_for(lock, std::chrono::seconds(5), [] { return !g_queue.empty(); });
                    if (g_queue.empty()) {
                        // ping per accorgersi se il Player e' stato chiuso
                        msg = "{\"Submit\":[]}";
                    } else {
                        msg = g_queue.front();
                        g_queue.pop_front();
                    }
                }
                if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &msg[0],
                                         static_cast<DWORD>(msg.size())) != NO_ERROR)
                    break;
            }
            g_bhConnected = false;
            vr::log::Line("HAPTICS: bHaptics Player disconnected.");
            WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            // Prima si chiude l'handle (annulla la ricezione in corso), poi si
            // aspetta il lettore: al contrario, con il socket mezzo aperto il
            // thread restava appeso per sempre (audit 20/10).
            WinHttpCloseHandle(ws);
            reader.join();
        } else if (!loggedFail) {
            loggedFail = true;
            vr::log::Line("HAPTICS: bHaptics Player not running (retrying every 10 s).");
        }
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
        Sleep(10000);
    }
}

// Forza del colpo per arma (0..1): lanciarazzi, cecchini e fucili a pompa forti.
float WeaponStrength(const std::string& cls) {
    auto has = [&](const char* k) { return cls.find(k) != std::string::npos; };
    if (has("RPG") || has("Milkor") || has("Barrett") || has("Steyr") || has("M110")) return 1.0f;
    if (has("Benelli") || has("AA12") || has("Sawed") || has("Mossberg") || has("Shotgun")) return 0.9f;
    if (has("DesertEagle") || has("Python")) return 0.75f;
    if (has("Turret") || has("Stationary") || has("M2E2") || has("Minigun")) return 0.6f;
    if (has("M9") || has("Glock") || has("FNP45") || has("SIG")) return 0.45f;
    if (has("Uzi") || has("UMP") || has("P90") || has("MP7") || has("Vector")) return 0.45f;
    return 0.6f;  // fucili d'assalto e mitragliatrici
}

}  // namespace

void Start() {
    std::thread([] {
        vr::control::WaitForConfig(120000, 2000);  // dopo la lettura dell'ini e fuori dal blocco del loader
        StartProTube();
        if (vr::control::g_bhaptics) BHapticsThread();
    }).detach();
}

void Shot(const std::string& weaponClass, bool twoHanded) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 45) return;  // pallini del fucile a pompa: un solo colpo
    last = now;
    float k = WeaponStrength(weaponClass);
    unsigned char power = static_cast<unsigned char>(80 + 175 * k);
    Kick(power, GunChannel());
    if (twoHanded) Kick(static_cast<unsigned char>(power * 2 / 3), SupportChannel());
    if (g_bhConnected) {
        int in = static_cast<int>(40 + 60 * k);
        Frame("shotR", "ForearmR", 0, 5, in, 60);
        if (twoHanded) Frame("shotL", "ForearmL", 0, 5, in * 2 / 3, 60);
        Frame("shotVest", "Vest", 0, 3, in / 2, 50);  // spalla/petto in alto davanti
    }
}

// Motori del giubbotto TactSuit (bHaptics "Vest"): davanti 0-19, dietro
// 20-39, 4 colonne per 5 righe (indice = riga * 4 + colonna). Davanti la
// colonna 0 e' a sinistra di chi lo indossa, dietro a destra (stessa faccia
// del corpo vista da fuori).
void VestFrame(const char* key, const std::vector<int>& motors, int intensity, int ms) {
    std::string dots;
    for (int i : motors) {
        if (!dots.empty()) dots += ",";
        dots += "{\"index\":" + std::to_string(i) + ",\"intensity\":" + std::to_string(intensity) + "}";
    }
    Enqueue("{\"Submit\":[{\"Type\":\"frame\",\"Key\":\"" + std::string(key) +
            "\",\"Frame\":{\"position\":\"Vest\",\"dotPoints\":[" + dots + "],\"durationMillis\":" +
            std::to_string(ms) + "}}]}");
}

void Damage(int amount, float angleRad) {
    if (amount <= 0) return;
    int in = std::min(100, 30 + amount * 2);
    Rumble(static_cast<unsigned char>(std::min(255, 60 + amount * 4)), 0.15f, kAll);
    if (!g_bhConnected) return;
    if (std::isnan(angleRad)) {
        Frame("damage", "Vest", 0, 39, in, 150);
        return;
    }
    // Colpo direzionale (audit 20/10: prima vibrava tutto il giubbotto): lato
    // davanti o dietro, e colonne verso il lato del colpo.
    float s = std::sin(angleRad), c = std::cos(angleRad);
    bool front = c >= 0.0f;
    std::vector<int> motors;
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 4; ++col) {
            // Colonna in coordinate "destra di chi indossa": -1.5 .. +1.5.
            float x = front ? (col - 1.5f) : (1.5f - col);
            float want = 1.5f * s;  // davanti/dietro: colonne centrali; di lato: le due del lato
            if (std::fabs(x - want) <= 1.0f) motors.push_back((front ? 0 : 20) + row * 4 + col);
        }
    }
    VestFrame("damage", motors, in, 150);
}

void GrenadeThrow() {
    if (g_bhConnected) Frame("throw", "ForearmL", 0, 5, 70, 120);
}

void Melee() {
    if (g_bhConnected) Frame("melee", "ForearmL", 0, 5, 100, 100);
}

void Reload() {
    Rumble(70, 0.08f, GunChannel());
    if (g_bhConnected) Frame("reload", "ForearmL", 0, 2, 50, 80);
}

}  // namespace vr::haptics
