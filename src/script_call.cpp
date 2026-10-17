#include "script_call.h"
#include "logger.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <map>
#include <vector>

namespace vr::script {

namespace {

// Layout verificato il 16/10 con tools/ue3.py.
constexpr uintptr_t kGNames = 0x014C1EC8;    // TArray<FNameEntry*>, testo a +0x10
constexpr uintptr_t kGObjects = 0x014D41EC;  // TArray<UObject*>
constexpr uintptr_t kOffOuter = 0x28, kOffName = 0x2C, kOffClass = 0x34;
constexpr uintptr_t kOffSuper = 0x3C;
constexpr int kVtProcessEvent = 66;
constexpr uintptr_t kOffPawnArsenal = 0x1590;  // YGamePawn.m_arsenalManager

bool SafeRead(const void* p, void* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string NameText(int32_t idx) {
    uint32_t data = 0, num = 0, entry = 0;
    if (!SafeRead(reinterpret_cast<void*>(kGNames), &data, 4) ||
        !SafeRead(reinterpret_cast<void*>(kGNames + 4), &num, 4) || idx < 0 ||
        static_cast<uint32_t>(idx) >= num ||
        !SafeRead(reinterpret_cast<void*>(data + 4 * idx), &entry, 4) || !entry)
        return {};
    char buf[64] = {};
    if (!SafeRead(reinterpret_cast<void*>(entry + 0x10), buf, sizeof(buf) - 1)) return {};
    if (buf[1] == 0) {  // UTF-16
        std::string s;
        for (int i = 0; i + 1 < 63 && buf[i]; i += 2) s += buf[i];
        return s;
    }
    return std::string(buf);
}

uint32_t Field(void* obj, uintptr_t off) {
    uint32_t v = 0;
    if (obj) SafeRead(static_cast<char*>(obj) + off, &v, 4);
    return v;
}

// Funzione UnrealScript per nome e nome della classe che la contiene.
void* FindFunction(const char* outerClass, const char* name) {
    uint32_t data = 0, num = 0;
    SafeRead(reinterpret_cast<void*>(kGObjects), &data, 4);
    SafeRead(reinterpret_cast<void*>(kGObjects + 4), &num, 4);
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0;
        if (!SafeRead(reinterpret_cast<void*>(data + 4 * i), &o, 4) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (ObjectName(obj) != name) continue;
        if (ClassName(obj) != "Function") continue;
        if (ObjectName(reinterpret_cast<void*>(Field(obj, kOffOuter))) != outerClass) continue;
        return obj;
    }
    return nullptr;
}

typedef void(__thiscall* ProcessEvent_t)(void* self, void* func, void* parms, void* result);

bool CallProcessEvent(void* obj, void* func, void* parms) {
    __try {
        void** vtable = *reinterpret_cast<void***>(obj);
        reinterpret_cast<ProcessEvent_t>(vtable[kVtProcessEvent])(obj, func, parms, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::mutex g_mutex;
std::string g_giveWeapon;
std::string g_console;
std::string g_call;

constexpr uintptr_t kOffChildren = 0x4C, kOffNext = 0x40;
constexpr uintptr_t kOffPropFlags = 0x4C, kOffPropOffset = 0x64, kOffBoolMask = 0x84;
constexpr uintptr_t kOffParmsSize = 0xA6;
constexpr uint32_t kCpfParm = 0x80, kCpfReturn = 0x400;
constexpr uintptr_t kOffPcCheatManager = 0x47C, kOffActorWorldInfo = 0xEC;

int32_t FindNameIndex(const std::string& text) {
    uint32_t data = 0, num = 0;
    SafeRead(reinterpret_cast<void*>(kGNames), &data, 4);
    SafeRead(reinterpret_cast<void*>(kGNames + 4), &num, 4);
    for (uint32_t i = 0; i < num; ++i)
        if (_stricmp(NameText(static_cast<int32_t>(i)).c_str(), text.c_str()) == 0) return static_cast<int32_t>(i);
    return -1;
}

// Funzione per nome nella classe dell'oggetto o in una superclasse.
void* FindMethod(void* obj, const std::string& name) {
    void* c = reinterpret_cast<void*>(Field(obj, kOffClass));
    for (int depth = 0; c && depth < 32; ++depth) {
        for (void* f = reinterpret_cast<void*>(Field(c, kOffChildren)); f; f = reinterpret_cast<void*>(Field(f, kOffNext)))
            if (ClassName(f) == "Function" && _stricmp(ObjectName(f).c_str(), name.c_str()) == 0) return f;
        c = reinterpret_cast<void*>(Field(c, kOffSuper));
    }
    return nullptr;
}

// Istanza (non "Default__") di una classe; "Classe#k" sceglie la k-esima.
void* FindInstance(const std::string& spec) {
    std::string cls = spec;
    int want = 0;
    size_t hash = spec.find('#');
    if (hash != std::string::npos) {
        cls = spec.substr(0, hash);
        want = atoi(spec.c_str() + hash + 1);
    }
    uint32_t data = 0, num = 0;
    SafeRead(reinterpret_cast<void*>(kGObjects), &data, 4);
    SafeRead(reinterpret_cast<void*>(kGObjects + 4), &num, 4);
    int k = 0;
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0;
        if (!SafeRead(reinterpret_cast<void*>(data + 4 * i), &o, 4) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (!IsA(obj, cls.c_str()) || ObjectName(obj).rfind("Default__", 0) == 0) continue;
        if (k++ == want) return obj;
    }
    return nullptr;
}

// Oggetto UClass con quel nome.
void* FindClassObject(const std::string& name) {
    uint32_t data = 0, num = 0;
    SafeRead(reinterpret_cast<void*>(kGObjects), &data, 4);
    SafeRead(reinterpret_cast<void*>(kGObjects + 4), &num, 4);
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0;
        if (!SafeRead(reinterpret_cast<void*>(data + 4 * i), &o, 4) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (_stricmp(ObjectName(obj).c_str(), name.c_str()) == 0 && ClassName(obj) == "Class") return obj;
    }
    return nullptr;
}

std::string FullName(void* obj) {
    std::string s;
    for (int i = 0; obj && i < 8; ++i) {
        s = ObjectName(obj) + (s.empty() ? "" : "." + s);
        obj = reinterpret_cast<void*>(Field(obj, kOffOuter));
    }
    return s;
}

void* ResolveObject(void* pc, void* pawn, const std::string& tok) {
    if (tok == "none" || tok == "0") return nullptr;
    if (tok == "pc") return pc;
    if (tok == "pawn") return pawn;
    if (tok == "cheat") return pc ? reinterpret_cast<void*>(Field(pc, kOffPcCheatManager)) : nullptr;
    if (tok == "world") return pc ? reinterpret_cast<void*>(Field(pc, kOffActorWorldInfo)) : nullptr;
    if (tok == "arsenal") return pawn ? reinterpret_cast<void*>(Field(pawn, kOffPawnArsenal)) : nullptr;
    if (tok == "weapon") return pawn ? reinterpret_cast<void*>(Field(pawn, 0x1594)) : nullptr;  // YGamePawn.m_currentWeapon
    if (tok.rfind("0x", 0) == 0) return reinterpret_cast<void*>(strtoul(tok.c_str(), nullptr, 16));
    return FindInstance(tok);
}

std::vector<std::string> SplitArgs(const std::string& line) {
    std::vector<std::string> tok;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        size_t e;
        if (line[i] == '"') {
            e = line.find('"', i + 1);
            if (e == std::string::npos) e = line.size();
            tok.push_back(line.substr(i + 1, e - i - 1));
            i = e + 1;
        } else {
            e = line.find(' ', i);
            if (e == std::string::npos) e = line.size();
            tok.push_back(line.substr(i, e - i));
            i = e;
        }
    }
    return tok;
}

// Crea il CheatManager (God, KillEnemies, ...) per i collaudi automatici.
// YGameInfo.AllowCheats nella build finale e' "return false" (bytecode
// 04 28 ...): lo si cambia per un attimo in "return true" (0x27), si chiama
// PlayerController.AddCheats e si rimette com'era.
void EnableCheats(void* pc) {
    if (!pc) return;
    if (Field(pc, kOffPcCheatManager)) {
        vr::log::Line("CHEATS: already on.");
        return;
    }
    void* allow = FindFunction("YGameInfo", "AllowCheats");
    void* add = FindMethod(pc, "AddCheats");
    uint8_t* code = allow ? reinterpret_cast<uint8_t*>(Field(allow, 0x54)) : nullptr;  // UStruct::Script
    uint8_t head[2] = {};
    if (!code || !add || !SafeRead(code, head, 2) || head[0] != 0x04 || head[1] != 0x28) {
        vr::log::Line("CHEATS: AllowCheats not recognised, nothing done.");
        return;
    }
    code[1] = 0x27;
    uint8_t parms[16] = {};
    CallProcessEvent(pc, add, parms);
    code[1] = 0x28;
    vr::log::Line(std::string("CHEATS: ") + (Field(pc, kOffPcCheatManager) ? "CheatManager created." : "not created."));
}

// "ucall <oggetto> <Funzione> [argomenti]": chiama una funzione UnrealScript.
// Gli argomenti riempiono i parametri in ordine secondo il tipo (int, byte,
// bool, float, string, name, object = pc|pawn|cheat|world|none|0x...|Classe[#k]).
void RunCall(void* pc, void* pawn, const std::string& line) {
    if (line == "!cheats") {
        EnableCheats(pc);
        return;
    }
    std::vector<std::string> tok = SplitArgs(line);
    if (tok.size() < 2) return;
    void* obj = ResolveObject(pc, pawn, tok[0]);
    if (!obj) {
        vr::log::Line("UCALL: object " + tok[0] + " not found.");
        return;
    }
    void* func = FindMethod(obj, tok[1]);
    if (!func) {
        vr::log::Line("UCALL: " + ClassName(obj) + "." + tok[1] + " not found.");
        return;
    }
    static uint8_t parms[1024];
    static wchar_t strings[8][256];
    std::memset(parms, 0, sizeof(parms));
    size_t arg = 2;
    int nstr = 0;
    void* retProp = nullptr;
    for (void* p = reinterpret_cast<void*>(Field(func, kOffChildren)); p; p = reinterpret_cast<void*>(Field(p, kOffNext))) {
        uint32_t flags = Field(p, kOffPropFlags);
        if (!(flags & kCpfParm)) continue;
        uint32_t off = Field(p, kOffPropOffset);
        if (off + 12 > sizeof(parms)) break;
        std::string type = ClassName(p);
        if (flags & kCpfReturn) {
            retProp = p;
            continue;
        }
        if (arg >= tok.size()) continue;  // opzionale non dato: zero
        const std::string& a = tok[arg++];
        uint8_t* dst = parms + off;
        if (type == "IntProperty") {
            int32_t v = atoi(a.c_str());
            std::memcpy(dst, &v, 4);
        } else if (type == "ByteProperty") {
            *dst = static_cast<uint8_t>(atoi(a.c_str()));
        } else if (type == "FloatProperty") {
            float v = static_cast<float>(atof(a.c_str()));
            std::memcpy(dst, &v, 4);
        } else if (type == "BoolProperty") {
            uint32_t v = 0;
            std::memcpy(&v, dst, 4);
            if (a == "1" || a == "true") v |= Field(p, kOffBoolMask);
            std::memcpy(dst, &v, 4);
        } else if (type == "StrProperty" && nstr < 8) {
            wchar_t* w = strings[nstr++];
            int n = 0;
            for (; n < 255 && n < static_cast<int>(a.size()); ++n) w[n] = static_cast<wchar_t>(a[n]);
            w[n] = 0;
            struct {
                wchar_t* d;
                int32_t num, max;
            } fs = {w, n + 1, n + 1};
            std::memcpy(dst, &fs, 12);
        } else if (type == "NameProperty") {
            int32_t nm[2] = {FindNameIndex(a), 0};
            std::memcpy(dst, nm, 8);
        } else if (type == "ClassProperty") {
            void* o = FindClassObject(a);
            std::memcpy(dst, &o, 4);
        } else if (type == "ObjectProperty") {
            void* o = ResolveObject(pc, pawn, a);
            std::memcpy(dst, &o, 4);
        } else {
            vr::log::Line("UCALL: parameter type " + type + " not supported.");
            return;
        }
    }
    vr::log::Line("UCALL: " + FullName(obj) + " -> " + ClassName(obj) + "." + tok[1]);
    if (!CallProcessEvent(obj, func, parms)) {
        vr::log::Line("UCALL: exception in ProcessEvent");
        return;
    }
    // Parametri di uscita (out) e strutture: stampati come float.
    for (void* p = reinterpret_cast<void*>(Field(func, kOffChildren)); p; p = reinterpret_cast<void*>(Field(p, kOffNext))) {
        uint32_t flags = Field(p, kOffPropFlags);
        if (!(flags & kCpfParm) || !((flags & 0x100) || ((flags & kCpfReturn) && ClassName(p) == "StructProperty"))) continue;
        uint32_t off = Field(p, kOffPropOffset);
        float f[3];
        std::memcpy(f, parms + off, 12);
        char buf[160];
        snprintf(buf, sizeof(buf), "UCALL: %s = (%.2f, %.2f, %.2f) / int (%d, %d, %d)", ObjectName(p).c_str(), f[0], f[1], f[2],
                 *reinterpret_cast<int32_t*>(parms + off), *reinterpret_cast<int32_t*>(parms + off + 4),
                 *reinterpret_cast<int32_t*>(parms + off + 8));
        vr::log::Line(buf);
    }
    if (!retProp) {
        vr::log::Line("UCALL: done");
        return;
    }
    uint32_t off = Field(retProp, kOffPropOffset);
    std::string type = ClassName(retProp), ret;
    uint32_t v = 0;
    std::memcpy(&v, parms + off, 4);
    if (type == "FloatProperty") {
        float f;
        std::memcpy(&f, &v, 4);
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", f);
        ret = buf;
    } else if (type == "ObjectProperty" || type == "ClassProperty") {
        ret = v ? FullName(reinterpret_cast<void*>(v)) : "None";
    } else if (type == "StrProperty") {
        const wchar_t* d = reinterpret_cast<const wchar_t*>(v);
        for (int i = 0; d && i < 200 && d[i]; ++i) ret += static_cast<char>(d[i]);
    } else if (type == "BoolProperty") {
        ret = (v & Field(retProp, kOffBoolMask)) ? "true" : "false";
    } else if (type == "ByteProperty") {
        ret = std::to_string(v & 0xFF);
    } else {
        ret = std::to_string(static_cast<int32_t>(v));
    }
    vr::log::Line("UCALL: returned " + ret);
}

void RunConsole(void* pc, const std::string& cmd) {
    static void* func = nullptr;
    if (!func) func = FindFunction("PlayerController", "ConsoleCommand");
    if (!pc || !func) {
        vr::log::Line("CONSOLE: PlayerController or function not found.");
        return;
    }
    // Parametri: FString Command +0, bool bWriteToLog +0xC, FString ritorno +0x10.
    static wchar_t text[256];
    int n = 0;
    for (; n < 255 && n < static_cast<int>(cmd.size()); ++n) text[n] = static_cast<wchar_t>(cmd[n]);
    text[n] = 0;
    struct {
        wchar_t* data;
        int32_t num, max;
        uint32_t writeToLog;
        void* retData;
        int32_t retNum, retMax;
    } parms = {text, n + 1, n + 1, 1, nullptr, 0, 0};
    vr::log::Line("CONSOLE: " + cmd);
    bool ok = CallProcessEvent(pc, func, &parms);
    if (!ok) vr::log::Line("CONSOLE: exception in ProcessEvent");
}

}  // namespace

std::string ObjectName(void* obj) {
    if (!obj) return {};
    return NameText(static_cast<int32_t>(Field(obj, kOffName)));
}

std::string ClassName(void* obj) { return ObjectName(reinterpret_cast<void*>(Field(obj, kOffClass))); }

bool IsA(void* obj, const char* base) {
    void* c = reinterpret_cast<void*>(Field(obj, kOffClass));
    for (int i = 0; c && i < 32; ++i) {
        if (ObjectName(c) == base) return true;
        c = reinterpret_cast<void*>(Field(c, kOffSuper));
    }
    return false;
}

void QueueGiveWeapon(const std::string& weaponClass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_giveWeapon = weaponClass;
}

void QueueConsole(const std::string& command) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_console = command;
}

int32_t NameIndex(const std::string& text) {
    static std::mutex m;
    static std::map<std::string, int32_t> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(text);
    if (it != cache.end() && it->second >= 0) return it->second;
    int32_t idx = FindNameIndex(text);
    cache[text] = idx;
    return idx;
}

std::string NameFromIndex(int32_t idx) { return NameText(idx); }

bool CallFunction(void* obj, const char* name, void* parms) {
    if (!obj) return false;
    static std::map<std::pair<uint32_t, std::string>, void*> cache;
    auto key = std::make_pair(Field(obj, kOffClass), std::string(name));
    auto it = cache.find(key);
    void* func = it != cache.end() ? it->second : (cache[key] = FindMethod(obj, name));
    return func && CallProcessEvent(obj, func, parms);
}

void QueueCall(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_call = line;
}

void Tick(void* pc, void* pawn) {
    std::string cls, console, call;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        cls.swap(g_giveWeapon);
        console.swap(g_console);
        call.swap(g_call);
    }
    if (!console.empty()) RunConsole(pc, console);
    if (!call.empty()) RunCall(pc, pawn, call);
    if (cls.empty()) return;
    // Le classi "_Scnd" (modalita' secondaria di un'arma) e "_AI" non sono armi
    // da dare al giocatore: GiveWeapon con YWeapon_SCAR_Scnd va in eccezione.
    if (cls.find("_Scnd") != std::string::npos || cls.find("_AI") != std::string::npos) {
        vr::log::Line("GIVEWEAPON " + cls + ": unsuitable class (secondary mode / AI), ignored.");
        return;
    }
    void* arsenal = pawn ? reinterpret_cast<void*>(Field(pawn, kOffPawnArsenal)) : nullptr;
    static void* func = nullptr;
    if (!func) func = FindFunction("YArsenalManager", "GiveWeapon");
    if (!arsenal || !func) {
        vr::log::Line("GIVEWEAPON: arsenal or function not found.");
        return;
    }
    // Parametri di YArsenalManager.GiveWeapon(string WeaponClassStr):
    // FString a +0 (dati, numero con lo zero finale, capacita'), ritorno a +0xC.
    static wchar_t text[96];
    int n = 0;
    for (; n < 95 && n < static_cast<int>(cls.size()); ++n) text[n] = static_cast<wchar_t>(cls[n]);
    text[n] = 0;
    struct {
        wchar_t* data;
        int32_t num, max;
        void* ret;
    } parms = {text, n + 1, n + 1, nullptr};
    bool ok = CallProcessEvent(arsenal, func, &parms);
    vr::log::Line("GIVEWEAPON " + cls + ": " + (ok ? (parms.ret ? "weapon given (" + ClassName(parms.ret) + ")" : "no weapon")
                                                  : "exception in ProcessEvent"));
}

}  // namespace vr::script
