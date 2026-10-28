#pragma once
// Firme di byte con jolly ("8B 44 ?? 04"): parsing e ricerca. Header puro
// (niente Windows), usato da game_addrs.cpp e da tests/unit_tests.cpp.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vr::sig {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> fixed;  // 1 = il byte deve coincidere, 0 = jolly
    size_t anchor = 0;           // primo byte fisso: lo si cerca con memchr
    bool valid = false;
};

inline Pattern Parse(const char* text) {
    Pattern p;
    for (const char* s = text; *s;) {
        if (*s == ' ') {
            ++s;
            continue;
        }
        if (s[0] == '?' && s[1] == '?') {
            p.bytes.push_back(0);
            p.fixed.push_back(0);
            s += 2;
            continue;
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = hex(s[0]), lo = s[0] ? hex(s[1]) : -1;
        if (hi < 0 || lo < 0) return Pattern{};
        p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p.fixed.push_back(1);
        s += 2;
    }
    for (size_t i = 0; i < p.fixed.size(); ++i) {
        if (p.fixed[i]) {
            p.anchor = i;
            p.valid = true;
            break;
        }
    }
    return p;
}

inline bool MatchAt(const Pattern& p, const uint8_t* data, size_t size, size_t pos) {
    if (!p.valid || pos > size || size - pos < p.bytes.size()) return false;
    for (size_t i = 0; i < p.bytes.size(); ++i) {
        if (p.fixed[i] && data[pos + i] != p.bytes[i]) return false;
    }
    return true;
}

// Posizioni delle corrispondenze (al massimo maxHits): chi chiama vuole
// sapere se ce n'e' esattamente una.
inline std::vector<size_t> FindAll(const Pattern& p, const uint8_t* data, size_t size, size_t maxHits = 2) {
    std::vector<size_t> hits;
    if (!p.valid || size < p.bytes.size()) return hits;
    const uint8_t a = p.bytes[p.anchor];
    const size_t last = size - p.bytes.size();  // ultima partenza possibile
    size_t pos = 0;
    while (pos <= last) {
        const void* f = std::memchr(data + pos + p.anchor, a, last - pos + 1);
        if (!f) break;
        size_t start = static_cast<size_t>(static_cast<const uint8_t*>(f) - data) - p.anchor;
        if (MatchAt(p, data, size, start)) {
            hits.push_back(start);
            if (hits.size() >= maxHits) break;
        }
        pos = start + 1;
    }
    return hits;
}

}  // namespace vr::sig
