#include "game_addrs.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "logger.h"
#include "sig_scan.h"

namespace vr::addr {

uintptr_t g[Count] = {};

namespace {

enum Kind { kCode, kData, kIat };

struct Entry {
    const char* name;
    Kind kind;
    uintptr_t steamVa;   // indirizzo nell'exe Steam (codice, globale o voce IAT)
    uintptr_t refVa;     // kData: istruzione che usa la globale nell'exe Steam
    int off;             // kCode: inizio firma - indirizzo; kData: offset dell'operando nella firma
    const char* pattern;
    const char* dll;     // kIat
    const char* fn;      // kIat: nome, oppure nullptr e ordinal
    int ordinal;
};

const Entry kEntries[Count] = {
#define SIG_CODE(name, va, skip, pat) {#name, kCode, va, 0, skip, pat, nullptr, nullptr, 0},
#define SIG_DATA(name, va, ref, off, pat) {#name, kData, va, ref, off, pat, nullptr, nullptr, 0},
#define SIG_IAT(name, va, dll, fn, ord) {#name, kIat, va, 0, 0, nullptr, dll, fn, ord},
#include "game_sigs.inc"
#undef SIG_CODE
#undef SIG_DATA
#undef SIG_IAT
};

constexpr uintptr_t kSteamImageBase = 0x00400000;
bool g_allAtSteam = false;
// Codice dell'exe Steam finche' Resolve non legge quello vero.
uintptr_t g_textBegin = 0x00401000, g_textEnd = 0x01109000;

struct Module {
    uintptr_t base = 0;
    const uint8_t* text = nullptr;
    size_t textSize = 0;
    uintptr_t textVa = 0;
};

Module ReadModule() {
    Module m;
    m.base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(m.base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 6) == 0) {
            m.textVa = m.base + sec->VirtualAddress;
            m.text = reinterpret_cast<const uint8_t*>(m.textVa);
            m.textSize = sec->Misc.VirtualSize;
            break;
        }
    }
    if (!m.text) {  // exe ricompattato senza ".text": si cerca nella prima sezione eseguibile
        sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                m.textVa = m.base + sec->VirtualAddress;
                m.text = reinterpret_cast<const uint8_t*>(m.textVa);
                m.textSize = sec->Misc.VirtualSize;
                break;
            }
        }
    }
    return m;
}

bool InText(const Module& m, uintptr_t va, size_t len) {
    return m.text && va >= m.textVa && va - m.textVa <= m.textSize && m.textSize - (va - m.textVa) >= len;
}

uintptr_t FindImportSlot(const Module& m, const char* dll, const char* fn, int ordinal) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(m.base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(m.base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(m.base + d->Name), dll) != 0) continue;
        if (!d->OriginalFirstThunk) return 0;  // senza la tabella dei nomi non si sa quale voce e'
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(m.base + d->OriginalFirstThunk);
        for (DWORD i = 0; names[i].u1.AddressOfData; ++i) {
            const DWORD v = names[i].u1.AddressOfData;
            bool hit;
            if (IMAGE_SNAP_BY_ORDINAL32(v)) {
                hit = !fn && static_cast<int>(IMAGE_ORDINAL32(v)) == ordinal;
            } else {
                auto ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(m.base + v);
                hit = fn && std::strcmp(reinterpret_cast<const char*>(ibn->Name), fn) == 0;
            }
            if (hit) return m.base + d->FirstThunk + i * sizeof(IMAGE_THUNK_DATA32);
        }
    }
    return 0;
}

bool ForceScanRequested() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string flag = exe;
    flag = flag.substr(0, flag.find_last_of("\\/")) + "\\specopsvr_forcesigscan";
    return GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

const char* Name(Id id) { return id >= 0 && id < Count ? kEntries[id].name : "?"; }

int ResolvedCount() {
    int n = 0;
    for (int i = 0; i < Count; ++i) n += g[i] != 0;
    return n;
}

bool AllAtSteamAddresses() { return g_allAtSteam; }
uintptr_t TextBegin() { return g_textBegin; }
uintptr_t TextEnd() { return g_textEnd; }

std::string MissingList() {
    std::string s;
    for (int i = 0; i < Count; ++i) {
        if (g[i]) continue;
        if (!s.empty()) s += ", ";
        s += kEntries[i].name;
    }
    return s;
}

void Resolve() {
    const Module m = ReadModule();
    if (m.text) {
        g_textBegin = m.textVa;
        g_textEnd = m.textVa + m.textSize;
    }
    const bool force = ForceScanRequested();
    const uintptr_t delta = m.base - kSteamImageBase;
    int fast = 0, scanned = 0;
    bool allSteam = true;
    char b[400];
    for (int i = 0; i < Count; ++i) {
        const Entry& e = kEntries[i];
        uintptr_t value = 0;
        if (e.kind == kIat) {
            value = FindImportSlot(m, e.dll, e.fn, e.ordinal);
            if (value && value == e.steamVa + delta) ++fast;
        } else {
            const sig::Pattern p = sig::Parse(e.pattern);
            // Inizio atteso della firma nell'exe Steam (eventualmente spostato di base).
            const uintptr_t expect = (e.kind == kCode ? e.steamVa + e.off : e.refVa) + delta;
            if (!force && InText(m, expect, p.bytes.size()) &&
                sig::MatchAt(p, m.text, m.textSize, expect - m.textVa)) {
                value = e.kind == kCode ? e.steamVa + delta : *reinterpret_cast<const uint32_t*>(expect + e.off);
                ++fast;
            } else {
                const auto hits = sig::FindAll(p, m.text, m.textSize, 2);
                if (hits.size() == 1) {
                    const uintptr_t at = m.textVa + hits[0];
                    value = e.kind == kCode ? at - e.off : *reinterpret_cast<const uint32_t*>(at + e.off);
                    ++scanned;
                    std::snprintf(b, sizeof(b), "ADDR: %s found by signature at 0x%08X (Steam exe: 0x%08X).", e.name,
                                  static_cast<unsigned>(value), static_cast<unsigned>(e.steamVa));
                } else {
                    std::snprintf(b, sizeof(b), "ADDR: %s NOT FOUND (%s): the features that need it stay off.",
                                  e.name, hits.empty() ? "no match" : "more than one match");
                }
                vr::log::Line(b);
            }
        }
        if (!value && e.kind == kIat) {
            std::snprintf(b, sizeof(b), "ADDR: import %s!%s NOT FOUND: the features that need it stay off.", e.dll,
                          e.fn ? e.fn : "(ordinal)");
            vr::log::Line(b);
        }
        if (value != e.steamVa) allSteam = false;
        g[i] = value;
    }
    g_allAtSteam = allSteam;
    std::snprintf(b, sizeof(b),
                  "ADDR: %d/%d game addresses resolved (%d at the Steam layout, %d by signature)%s%s.", ResolvedCount(),
                  static_cast<int>(Count), fast, scanned, force ? ", forced signature scan" : "",
                  allSteam ? ", identical to the Steam exe" : "");
    vr::log::Line(b);
    const std::string missing = MissingList();
    if (!missing.empty()) vr::log::Line("ADDR: missing: " + missing);
}

}  // namespace vr::addr
