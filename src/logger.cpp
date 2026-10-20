#include "logger.h"
#include "version.h"

#include <windows.h>
#include <dbghelp.h>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace vr::log {

namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::string g_dir;

std::string ModuleDirectory() {
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(&ModuleDirectory),
        &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    return pos == std::string::npos ? "." : full.substr(0, pos);
}

std::string Timestamp(bool withDate = false) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tmv{};
    localtime_s(&tmv, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmv, withDate ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S");
    if (!withDate) oss << '.' << std::setw(3) << std::setfill('0') << ms;
    return oss.str();
}

// Un file per sessione: le tre precedenti restano come specopsvr.1/2/3.log.
void RotateLogs(const std::string& dir) {
    std::string base = dir + "\\specopsvr";
    DeleteFileA((base + ".3.log").c_str());
    MoveFileA((base + ".2.log").c_str(), (base + ".3.log").c_str());
    MoveFileA((base + ".1.log").c_str(), (base + ".2.log").c_str());
    MoveFileA((base + ".log").c_str(), (base + ".1.log").c_str());
}

std::string WindowsVersion() {
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = {sizeof(v)};
    auto fn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"));
    if (!fn || fn(&v) != 0) return "?";
    return std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
           std::to_string(v.dwBuildNumber);
}

std::string ExeInfo() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    std::string s = path;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a))
        s += " (" + std::to_string(a.nFileSizeLow) + " byte)";
    return s;
}

void WriteHeader(const std::string& dir) {
    g_file << "==== SpecOpsVR " << SPECOPSVR_VERSION << " (built " << __DATE__ << " " << __TIME__ << ") ====\n";
    g_file << "Started: " << Timestamp(true) << "\n";
    g_file << "Game: " << ExeInfo() << "\n";
    g_file << "Windows: " << WindowsVersion() << "\n";
    std::ifstream ini(dir + "\\specopsvr.ini");
    if (ini) {
        g_file << "---- specopsvr.ini (active lines) ----\n";
        std::string line;
        while (std::getline(ini, line)) {
            size_t a = line.find_first_not_of(" \t\r");
            if (a == std::string::npos || line[a] == '#') continue;
            g_file << "  " << line << "\n";
        }
    } else {
        g_file << "specopsvr.ini missing: defaults\n";
    }
    g_file << "----\n";
}

// Crash del gioco: riga nel log con codice, indirizzo e modulo, piu' un
// minidump accanto al log (da allegare alle segnalazioni).
// Filtro del gioco sostituito dal nostro: lo si chiama dopo il minidump, cosi'
// anche il rapporto di crash del gioco viene scritto.
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

// Rapporto (riga di log + minidump) del crash del thread tid: con lo stack
// esaurito (STACK_OVERFLOW) gira su un thread a parte, perche' snprintf,
// std::string e MiniDumpWriteDump sullo stack finito facevano un secondo
// errore e il minidump non veniva mai scritto (audit 20/10).
void WriteCrashReport(EXCEPTION_POINTERS* info, DWORD tid) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    void* addr = info->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char modName[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod))
        GetModuleFileNameA(mod, modName, MAX_PATH);
    char b[512];
    std::snprintf(b, sizeof(b), "CRASH: exception 0x%08lX at %p (%s +0x%lX), thread %lu", code, addr, modName,
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)),
                  tid);
    Line(b);
    std::string dump = g_dir + "\\specopsvr_crash_" + std::to_string(GetTickCount64()) + ".dmp";
    HANDLE f = CreateFileA(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        typedef BOOL(WINAPI * MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                                   PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                                   PMINIDUMP_CALLBACK_INFORMATION);
        auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        MINIDUMP_EXCEPTION_INFORMATION mei = {tid, info, FALSE};
        if (write &&
            write(GetCurrentProcess(), GetCurrentProcessId(), f,
                  static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), &mei,
                  nullptr, nullptr))
            Line("CRASH: minidump written to " + dump);
        CloseHandle(f);
    }
}

struct CrashJob {
    EXCEPTION_POINTERS* info;
    DWORD tid;
};

DWORD WINAPI CrashReportThread(LPVOID p) {
    CrashJob* job = static_cast<CrashJob*>(p);
    WriteCrashReport(job->info, job->tid);
    return 0;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return g_prevFilter ? g_prevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        static CrashJob job;
        job = {info, GetCurrentThreadId()};
        HANDLE t = CreateThread(nullptr, 256 * 1024, &CrashReportThread, &job, 0, nullptr);
        if (t) {
            WaitForSingleObject(t, 30000);
            CloseHandle(t);
        }
    } else {
        WriteCrashReport(info, GetCurrentThreadId());
    }
    return g_prevFilter ? g_prevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) return;
    g_dir = ModuleDirectory();
    RotateLogs(g_dir);
    g_file.open(g_dir + "\\specopsvr.log", std::ios::out | std::ios::trunc);
    WriteHeader(g_dir);
    g_file.flush();
}

// Codici sempre fatali, mai usati dalle prove di lettura protette (__try):
// gestiti al primo passaggio, prima che i gestori di UE3 (che catturano le
// eccezioni nel loro main guardato) chiudano il gioco senza passare dal
// filtro (audit 19/10: i minidump mancavano dalle segnalazioni).
LONG WINAPI FatalVectoredHandler(EXCEPTION_POINTERS* info) {
    // Solo gli errori sempre fatali: istruzioni illegali e privilegiate le
    // usano di proposito alcune librerie per sondare la CPU (dentro __try), e
    // contarle come crash consumava l'unico minidump (audit 20/10). Quelle
    // vere arrivano comunque al filtro normale.
    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
        CrashFilter(info);
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI KeepCrashFilter(LPVOID) {
    // UE3 puo' sostituire il filtro all'avvio: lo si rimette per un minuto.
    for (int i = 0; i < 60; ++i) {
        Sleep(1000);
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(&CrashFilter);
        if (prev != &CrashFilter && prev != nullptr) {
            g_prevFilter = prev;
            Line("CRASH: the game installed its crash filter, chained after ours.");
        }
    }
    return 0;
}

void InstallCrashHandler() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(&CrashFilter);
    if (prev != &CrashFilter) g_prevFilter = prev;
    AddVectoredExceptionHandler(0, &FatalVectoredHandler);
    HANDLE t = CreateThread(nullptr, 0, &KeepCrashFilter, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void Line(const std::string& message) {
    std::string entry = "[" + Timestamp() + "] " + message;

    OutputDebugStringA(entry.c_str());
    OutputDebugStringA("\n");

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) {
        g_file << entry << "\n";
        // Scrittura su disco al massimo ogni 250 ms (prima a ogni riga, anche
        // sul thread di rendering); subito per errori e crash.
        static ULONGLONG lastFlush = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastFlush > 250 || message.find("ERROR") != std::string::npos ||
            message.find("CRASH") != std::string::npos) {
            g_file.flush();
            lastFlush = now;
        }
    }
}

std::string Directory() { return g_dir; }

}  // namespace vr::log
