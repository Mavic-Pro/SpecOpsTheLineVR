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
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
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
                  GetCurrentThreadId());
    Line(b);
    std::string dump = g_dir + "\\specopsvr_crash_" + std::to_string(GetTickCount64()) + ".dmp";
    HANDLE f = CreateFileA(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        typedef BOOL(WINAPI * MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                                   PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                                   PMINIDUMP_CALLBACK_INFORMATION);
        auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
        if (write &&
            write(GetCurrentProcess(), GetCurrentProcessId(), f,
                  static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), &mei,
                  nullptr, nullptr))
            Line("CRASH: minidump written to " + dump);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
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

void InstallCrashHandler() { SetUnhandledExceptionFilter(&CrashFilter); }

void Line(const std::string& message) {
    std::string entry = "[" + Timestamp() + "] " + message;

    OutputDebugStringA(entry.c_str());
    OutputDebugStringA("\n");

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) {
        g_file << entry << "\n";
        g_file.flush();
    }
}

std::string Directory() { return g_dir; }

}  // namespace vr::log
