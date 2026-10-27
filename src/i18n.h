// Sprache der Mod: Deutsch, wenn das Spiel auf Deutsch laeuft, sonst Englisch. Jeder Text, den ein Spieler sieht (Menues,
// Hinweise, Protokoll), steht als Paar L("deutsch", "englisch") im Code; der static_assert im Makro prueft beim Bauen, dass
// beide Fassungen dieselben printf-Platzhalter haben.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace i18n {

enum class Lang : uint8_t { De, En };

inline std::atomic<uint8_t> g_lang{static_cast<uint8_t>(Lang::En)};
inline void SetLang(Lang l) { g_lang = static_cast<uint8_t>(l); }
inline Lang Current() { return static_cast<Lang>(g_lang.load()); }

// Naechster printf-Platzhalter ab p: Laenge (h, hh, l, ll, z, j, t, L, I64) + Typ-Buchstabe + Stern-Argumente. false = keiner mehr
struct FormatSpec {
    char length[4] = {};
    char type = 0;
    int stars = 0;  // * fuer Breite/Genauigkeit verbraucht je ein Argument
};
constexpr bool NextSpec(const char*& p, FormatSpec& out) {
    while (*p) {
        if (*p != '%') { ++p; continue; }
        ++p;
        if (*p == '%') { ++p; continue; }
        FormatSpec s;
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') ++p;
        if (*p == '*') { ++s.stars; ++p; }
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == '.') {
            ++p;
            if (*p == '*') { ++s.stars; ++p; }
            while (*p >= '0' && *p <= '9') ++p;
        }
        int n = 0;
        while (n < 3 && (*p == 'h' || *p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L' || *p == 'I' || *p == '6' ||
                         *p == '4')) {
            s.length[n++] = *p++;
        }
        s.type = *p;
        if (*p) ++p;
        out = s;
        return true;
    }
    return false;
}

constexpr bool SameFormat(const char* a, const char* b) {
    for (;;) {
        FormatSpec sa, sb;
        const bool ha = NextSpec(a, sa), hb = NextSpec(b, sb);
        if (ha != hb) return false;
        if (!ha) return true;
        if (sa.type != sb.type || sa.stars != sb.stars) return false;
        for (int i = 0; i < 4; ++i)
            if (sa.length[i] != sb.length[i]) return false;
    }
}

// "de", "DE", "de-DE", "de_AT" -> Deutsch; alles andere (auch leer/nullptr) -> Englisch
inline Lang LangFromCode(const char* code) {
    if (!code) return Lang::En;
    const char c0 = code[0] | 0x20, c1 = code[0] ? (code[1] | 0x20) : 0;
    if (c0 != 'd' || c1 != 'e') return Lang::En;
    return (code[2] == 0 || code[2] == '-' || code[2] == '_') ? Lang::De : Lang::En;
}

// Zeile 'seta g_language "xx"' aus kexengine.cfg; fehlt sie -> Englisch
inline Lang LangFromCfgText(const std::string& cfg) {
    const std::string key = "seta g_language \"";
    size_t pos = 0;
    while ((pos = cfg.find(key, pos)) != std::string::npos) {
        if (pos == 0 || cfg[pos - 1] == '\n' || cfg[pos - 1] == '\r') {
            const size_t start = pos + key.size(), end = cfg.find('"', start);
            if (end != std::string::npos) return LangFromCode(cfg.substr(start, end - start).c_str());
        }
        pos += key.size();
    }
    return Lang::En;
}

inline const char* Pick(const char* de, const char* en) { return Current() == Lang::De ? de : en; }

}  // namespace i18n

// Text in der aktuellen Sprache; beide Fassungen muessen dieselben Platzhalter haben (sonst Build-Fehler)
#define L(de, en) ([] { static_assert(::i18n::SameFormat(de, en), "Platzhalter de/en verschieden"); }(), ::i18n::Pick(de, en))
