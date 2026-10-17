#include "haptics_ext.h"
#include "control.h"
#include "logger.h"

#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// Feedback aptico esterno (18/10, richiesta: "integra i feedback aptici come
// ProVolver e bHaptics").
//
// ProTubeVR: API C di ForceTubeVR_API_x32.dll (stesse firme usate dalle mod VR
// a 32 bit di Crysis/Far Cry): InitRifle() avvia il collegamento Bluetooth,
// KickChannel(power, channel) da' il colpo, RumbleChannel(power, seconds,
// channel) vibra. La dll non e' distribuita con la mod: va presa dal pacchetto
// di ProTubeVR e messa accanto al gioco.
//
// bHaptics: il bHaptics Player espone un websocket su
// ws://127.0.0.1:15881/v2/feedbacks; i messaggi "Submit" di tipo "frame"
// accendono singoli motori (dotPoints index/intensity) per durationMillis.
// Posizioni: "Vest" (0-19 davanti, 20-39 dietro), "ForearmL"/"ForearmR" (0-5).
namespace vr::haptics {

namespace {

// --- ProTube ------------------------------------------------------------------
enum ForceTubeChannel { kAll, kRifle, kRifleButt, kRifleBolt, kPistol1, kPistol2, kOther, kVest };
typedef void(__cdecl* InitRifle_t)();
typedef void(__cdecl* KickChannel_t)(unsigned char power, int channel);
typedef void(__cdecl* RumbleChannel_t)(unsigned char power, float seconds, int channel);
KickChannel_t g_kick = nullptr;
RumbleChannel_t g_rumble = nullptr;

void StartProTube() {
    if (!vr::control::g_protube) return;
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&StartProTube), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    HMODULE dll = LoadLibraryA((dir + "ForceTubeVR_API_x32.dll").c_str());
    if (!dll) {
        vr::log::Line("HAPTICS: ProTube off (ForceTubeVR_API_x32.dll not found next to the game).");
        return;
    }
    auto init = reinterpret_cast<InitRifle_t>(GetProcAddress(dll, "InitRifle"));
    g_kick = reinterpret_cast<KickChannel_t>(GetProcAddress(dll, "KickChannel"));
    g_rumble = reinterpret_cast<RumbleChannel_t>(GetProcAddress(dll, "RumbleChannel"));
    if (!init || !g_kick || !g_rumble) {
        g_kick = nullptr;
        g_rumble = nullptr;
        vr::log::Line("HAPTICS: ForceTubeVR_API_x32.dll has unexpected exports, ProTube off.");
        return;
    }
    init();  // collegamento Bluetooth in background (dentro la dll)
    vr::log::Line("HAPTICS: ProTube initialised (ForceTubeVR_API_x32.dll).");
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
            WinHttpCloseHandle(ws);
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
        Sleep(15000);  // dopo la lettura dell'ini (come l'aggiornamento) e fuori dal blocco del loader
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
    if (g_kick) g_kick(static_cast<unsigned char>(80 + 175 * k), kAll);
    if (g_bhConnected) {
        int in = static_cast<int>(40 + 60 * k);
        Frame("shotR", "ForearmR", 0, 5, in, 60);
        if (twoHanded) Frame("shotL", "ForearmL", 0, 5, in * 2 / 3, 60);
        Frame("shotVest", "Vest", 0, 3, in / 2, 50);  // spalla/petto in alto davanti
    }
}

void Damage(int amount) {
    if (amount <= 0) return;
    int in = std::min(100, 30 + amount * 2);
    if (g_rumble) g_rumble(static_cast<unsigned char>(std::min(255, 60 + amount * 4)), 0.15f, kAll);
    if (g_bhConnected) Frame("damage", "Vest", 0, 39, in, 150);
}

void GrenadeThrow() {
    if (g_bhConnected) Frame("throw", "ForearmL", 0, 5, 70, 120);
}

void Melee() {
    if (g_bhConnected) Frame("melee", "ForearmL", 0, 5, 100, 100);
}

void Reload() {
    if (g_rumble) g_rumble(70, 0.08f, kAll);
    if (g_bhConnected) Frame("reload", "ForearmL", 0, 2, 50, 80);
}

}  // namespace vr::haptics
