// Ponte ProTubeVR a 64 bit (20/10, richiesta: "procedi al ponte").
//
// Il gioco e' a 32 bit, ma l'unica ForceTubeVR_API_x32.dll pubblica e' del
// 2020 (niente ProVolver ne' canali). La dll aggiornata esiste solo a 64 bit e
// arriva con la ProTubeVR Companion App (gratuita su Steam): questo eseguibile
// la carica da li' e riceve i comandi della mod su una named pipe.
//
// Uso: specopsvr_protube.exe <pid del gioco> <nome pipe, senza \\.\pipe\>
// Messaggi (8 byte, vedi ProTubeMsg in haptics_ext.cpp): op 1 kick, 2 rumble,
// 3 shot. Esce quando il gioco termina o la pipe si chiude.
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

#pragma pack(push, 1)
struct ProTubeMsg {
    uint8_t op, power, power2, channel;
    float seconds;
};
#pragma pack(pop)
static_assert(sizeof(ProTubeMsg) == 8, "protocollo a 8 byte");

typedef void(__cdecl* InitPistol_t)();
typedef void(__cdecl* KickChannel_t)(unsigned char power, int channel);
typedef void(__cdecl* RumbleChannel_t)(unsigned char power, float seconds, int channel);
typedef void(__cdecl* ShotChannel_t)(unsigned char kick, unsigned char rumble, float seconds, int channel);
typedef const char*(__cdecl* ListConnected_t)();

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Cartelle "steamapps" di tutte le librerie Steam (SteamPath dal registro +
// le voci "path" di libraryfolders.vdf).
std::vector<std::wstring> SteamLibraries() {
    std::vector<std::wstring> libs;
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return libs;
    std::wstring steam(buf);
    libs.push_back(steam + L"\\steamapps");
    std::ifstream f(steam + L"\\steamapps\\libraryfolders.vdf");
    std::stringstream ss;
    ss << f.rdbuf();
    std::string vdf = ss.str();
    std::regex re("\"path\"\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(vdf.begin(), vdf.end(), re), end; it != end; ++it) {
        std::string p = (*it)[1];
        std::string unescaped;
        for (size_t i = 0; i < p.size(); ++i) {
            if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
            unescaped += p[i];
        }
        libs.push_back(Widen(unescaped) + L"\\steamapps");
    }
    return libs;
}

// Prima una dll messa a mano accanto all'eseguibile, poi la Companion App.
std::wstring FindApiDll() {
    std::wstring local = ExeDir() + L"ForceTubeVR_API_x64.dll";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    for (const auto& lib : SteamLibraries()) {
        std::wstring p = lib + L"\\common\\ProTubeVR Companion-App\\build\\ForceTubeVR_API_x64.dll";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return L"";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring logPath = ExeDir() + L"specopsvr_protube.log";
    g_log = _wfopen(logPath.c_str(), L"w");
    if (argc < 3) {
        Log("usage: specopsvr_protube.exe <game pid> <pipe name>");
        return 2;
    }
    DWORD parentPid = static_cast<DWORD>(_wtoi(argv[1]));
    // Solo il nome sulla riga di comando: il "\\" iniziale di \\.\pipe\ non sopravvive.
    std::wstring pipeName = std::wstring(L"\\\\.\\pipe\\") + argv[2];

    // Il gioco chiuso (anche per crash) chiude anche il ponte.
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent) {
        Log("game process %lu not found", parentPid);
        return 3;
    }
    CreateThread(nullptr, 0, [](LPVOID h) -> DWORD {
        WaitForSingleObject(static_cast<HANDLE>(h), INFINITE);
        Log("game closed, exiting");
        ExitProcess(0);
    }, parent, 0, nullptr);

    std::wstring dllPath = FindApiDll();
    if (dllPath.empty()) {
        Log("ForceTubeVR_API_x64.dll not found: install the ProTubeVR Companion App (free on Steam).");
        return 4;
    }
    std::wstring dllDir = dllPath.substr(0, dllPath.find_last_of(L"\\/"));
    SetDllDirectoryW(dllDir.c_str());  // dipendenze della dll (runtime VC) dalla sua cartella
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    if (!dll) {
        Log("cannot load %s (error %lu)", Narrow(dllPath).c_str(), GetLastError());
        return 5;
    }
    auto init = reinterpret_cast<InitPistol_t>(GetProcAddress(dll, "InitPistol"));
    auto kick = reinterpret_cast<KickChannel_t>(GetProcAddress(dll, "KickChannel"));
    auto rumble = reinterpret_cast<RumbleChannel_t>(GetProcAddress(dll, "RumbleChannel"));
    auto shot = reinterpret_cast<ShotChannel_t>(GetProcAddress(dll, "ShotChannel"));
    auto list = reinterpret_cast<ListConnected_t>(GetProcAddress(dll, "ListConnectedForceTube"));
    if (!init || !kick || !rumble || !shot) {
        Log("%s has unexpected exports", Narrow(dllPath).c_str());
        return 6;
    }
    Log("loaded %s", Narrow(dllPath).c_str());
    // Pipe prima di InitPistol: se il Bluetooth tarda, la mod si collega
    // comunque e non rinuncia dopo 30 s lasciando il ponte acceso (audit 20/10).
    HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 0, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        Log("CreateNamedPipe %s failed (%lu)", Narrow(pipeName).c_str(), GetLastError());
        return 7;
    }

    // InitPistol: il primo ProVolver trovato va nel canale pistol1 (mano
    // dell'arma), il secondo in pistol2 (mano d'appoggio).
    init();

    // Registro dei dispositivi collegati quando cambiano.
    if (list) {
        CreateThread(nullptr, 0, [](LPVOID fn) -> DWORD {
            std::string last;
            for (;;) {
                const char* s = reinterpret_cast<ListConnected_t>(fn)();
                std::string cur = s ? s : "";
                if (cur != last) {
                    std::string flat;
                    for (char c : cur)
                        if (c != '\n' && c != '\t') flat += c;
                    Log("devices: %s", flat.c_str());
                    last = cur;
                }
                Sleep(3000);
            }
        }, reinterpret_cast<LPVOID>(list), 0, nullptr);
    }

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        Log("ConnectNamedPipe failed (%lu)", GetLastError());
        return 8;
    }
    Log("game connected");
    for (;;) {
        ProTubeMsg m;
        DWORD got = 0;
        if (!ReadFile(pipe, &m, sizeof(m), &got, nullptr) || got != sizeof(m)) break;
        switch (m.op) {
            case 1: kick(m.power, m.channel); break;
            case 2: rumble(m.power, m.seconds, m.channel); break;
            case 3: shot(m.power, m.power2, m.seconds, m.channel); break;
            default: break;
        }
    }
    Log("pipe closed, exiting");
    return 0;
}
