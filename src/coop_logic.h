// Reine Hilfslogik der Koop-Extras (ohne Spiel- und Windows-Zugriffe) - getestet in tests\test_logic.cpp.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

namespace coop {

// Einstellungen aus dem Menue "Koop-Lobby" (Datei smcoop_settings.txt). Gegner-HP und geteilte HP gelten nur vom Host.
struct CoopSettings {
    int hpPercent = 50;      // Gegner-HP je Gast, 0..200 in 10er-Schritten
    bool sharedHp = false;   // gemeinsamer Lebensbalken
    bool showNames = true;   // Namensschilder (gilt nur fuer diesen Spieler)
};

inline int ClampHpPercent(int v) {
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return v / 10 * 10;
}

inline CoopSettings ParseSettings(const std::string& text) {
    CoopSettings s;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        char key[32] = {};
        int value = 0;
        if (std::sscanf(line.c_str(), "%31s %d", key, &value) == 2) {
            if (!std::strcmp(key, "hp")) s.hpPercent = ClampHpPercent(value);
            else if (!std::strcmp(key, "shared")) s.sharedHp = value != 0;
            else if (!std::strcmp(key, "names")) s.showNames = value != 0;
        }
        start = end + 1;
    }
    return s;
}

inline std::string FormatSettings(const CoopSettings& s) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "hp %d\nshared %d\nnames %d\n", s.hpPercent, s.sharedHp ? 1 : 0, s.showNames ? 1 : 0);
    return buf;
}

// Klick auf den Regler: +10, nach 200 wieder 0
inline int NextHpPercent(int v) { return v >= 200 ? 0 : ClampHpPercent(v + 10); }

// HP = normal x (1 + Wert x Anzahl Gaeste)
inline float HpScaleFactor(int hpPercent, int players) {
    if (players < 2) return 1.0f;
    return 1.0f + static_cast<float>(hpPercent) / 100.0f * static_cast<float>(players - 1);
}

// Faktor wechselt (Spieler kommt/geht, Regler): Leben im Verhaeltnis umrechnen - der Anteil bleibt
inline float RescaleHealth(float health, float oldFactor, float newFactor) {
    if (oldFactor <= 0.0f || newFactor <= 0.0f) return health;
    return health / oldFactor * newFactor;
}

// Leben steigt von selbst (Phasenwechsel, Heilung): den Zuwachs mit dem Faktor nehmen
inline float ScaleGain(float lastHealth, float health, float factor) {
    if (health <= lastHealth) return health;
    return lastHealth + (health - lastHealth) * factor;
}

// Spielernummern: roster[i] = Kennung von Spieler i+1 (0 = frei). Host = 1; wer geht, gibt seine Nummer frei, die anderen
// behalten ihre (und damit ihre Farbe); Neue bekommen die kleinste freie Nummer in der Reihenfolge ihres Auftauchens.
constexpr int kRosterSize = 4;
inline void RosterUpdate(uint64_t* roster, uint64_t host, const uint64_t* members, int count) {
    roster[0] = host;
    for (int i = 1; i < kRosterSize; ++i) {
        bool present = false;
        for (int k = 0; k < count; ++k)
            if (members[k] == roster[i]) present = true;
        if (!present) roster[i] = 0;
    }
    for (int k = 0; k < count; ++k) {
        if (!members[k] || members[k] == host) continue;
        bool known = false;
        for (int i = 1; i < kRosterSize; ++i)
            if (roster[i] == members[k]) known = true;
        if (known) continue;
        for (int i = 1; i < kRosterSize; ++i)
            if (!roster[i]) {
                roster[i] = members[k];
                break;
            }
    }
}

inline int RosterNumberOf(const uint64_t* roster, uint64_t id) {
    if (!id) return 0;
    for (int i = 0; i < kRosterSize; ++i)
        if (roster[i] == id) return i + 1;
    return 0;
}

inline int RosterCount(const uint64_t* roster) {
    int n = 0;
    for (int i = 0; i < kRosterSize; ++i)
        if (roster[i]) ++n;
    return n;
}

// Farben der Spieler: Host gruen, 2 rot, 3 blau, 4 lila (0xRRGGBB)
inline uint32_t PlayerColorRgb(int number) {
    switch (number) {
        case 1: return 0x4CAF50;
        case 2: return 0xE53935;
        case 3: return 0x1E88E5;
        case 4: return 0x8E44AD;
        default: return 0xE8DFC8;  // knochenweiss (unbekannt)
    }
}

// Hardcore-Buchfuehrung (smcoop_hardcore.txt im Speicherordner): je Zeile "<platz> <kennung des Hardcore-Laufs>"
inline std::map<int, uint32_t> ParseHardcoreFile(const std::string& text) {
    std::map<int, uint32_t> m;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        int slot = -1;
        unsigned id = 0;
        if (std::sscanf(text.substr(start, end - start).c_str(), "%d %u", &slot, &id) == 2 && slot >= 0 && id) m[slot] = id;
        start = end + 1;
    }
    return m;
}

inline std::string FormatHardcoreFile(const std::map<int, uint32_t>& m) {
    std::string out;
    for (const auto& kv : m) out += std::to_string(kv.first) + " " + std::to_string(kv.second) + "\n";
    return out;
}

// Plaetze eines Hardcore-Laufs (aufsteigend); Kennung 0 = kein Lauf -> nie etwas
inline std::vector<int> SlotsOfRun(const std::map<int, uint32_t>& m, uint32_t id) {
    std::vector<int> slots;
    if (!id) return slots;
    for (const auto& kv : m)
        if (kv.second == id) slots.push_back(kv.first);
    return slots;
}

// Dateiname ohne Endung: Platz 0..13 -> "save_NN", kQuickSaveSlot -> "quicksave" (F5)
constexpr int kQuickSaveSlot = 99;
inline std::wstring SaveFileStem(int slot) {
    if (slot == kQuickSaveSlot) return L"quicksave";
    wchar_t name[16];
    std::swprintf(name, 16, L"save_%02d", slot);
    return name;
}

// Geteilte HP: Aenderung eines Mitspielers auf das eigene Leben (0..max; max <= 0 = unbekannt)
inline int ApplyLifeDelta(int life, int delta, int maxLife) {
    int v = life + delta;
    if (maxLife > 0 && v > maxLife) v = maxLife;
    return v < 0 ? 0 : v;
}

// Schiebeblock-Kennung ueber beide PCs (die Pool-Seriennummern unterscheiden sich je Ladeweg): 16-Bit-Faltung der Objekt-
// Kennung aus Grundposition +0x38 und Art +0xD0 (wie bei Gegnern); 0 ist "keine Kennung"
inline uint16_t BlockId16(uint32_t objectId) {
    const uint16_t id = static_cast<uint16_t>((objectId >> 16) ^ (objectId & 0xFFFF));
    return id ? id : 1;
}

// Schiebeblock-Zustand +0x290. Jeder Block laesst sich im Spiel nur einmal schieben (Anfassen geht nur in 0 bzw. 7).
// PushBlock/PushLavaBlock: 0 frisch, 2 Start, 3 rutscht, 1 fertig. PushGad3Block (gad3): 0 -> 7 ruht, 2/3 rutscht,
// 1 Fall-Pruefung (ein Bild), 4/5 faellt, 6 fertig.
inline bool BlockMoving(int state, bool gad3) { return state >= (gad3 ? 1 : 2) && state <= 5; }
inline int BlockDoneState(bool gad3) { return gad3 ? 6 : 1; }
// Steht verschoben (Position +0x20 weg von der Grundposition +0x38), ist aber noch anfassbar (0, Gad3 auch 7)? Das gibt es im
// Spiel nicht - nur in Spielstaenden, gespeichert mit dem alten Mod-Fehler (Block vom Mitspieler verschoben, Zustand blieb 0)
inline bool BlockNeedsRepair(int state, bool gad3, const float pos[3], const float base[3]) {
    if (!(state == 0 || (gad3 && state == 7))) return false;
    const float dx = pos[0] - base[0], dy = pos[1] - base[1], dz = pos[2] - base[2];
    return dx * dx + dy * dy + dz * dz > 1.0f;
}

// Levelinhalt neu aufgebaut (z.B. Ring-Cutscene Level 15: alle Objekte bekommen neue Pool-Eintraege) statt zerstoert bzw.
// eingesammelt? Dann ist alles auf einmal weg (>= 8 Behaelter + Pickups) oder der Pool schrumpft um mehr als ein Viertel.
inline bool MassVanish(int vanishedContainers, int vanishedPickups, int poolBefore, int poolNow) {
    if (vanishedContainers + vanishedPickups >= 8) return true;
    return poolBefore > 0 && poolNow * 4 < poolBefore * 3;
}

// Behaelter (Topf, Govi, Platte) des Mitspielers hier finden. Die Pool-Seriennummern unterscheiden sich zwischen den PCs je
// nach Ladeweg (Level 12: Bloecke #774 beim einen, #779 beim anderen) -> gleiche Nummer nur am selben Ort (sameSerialTol),
// sonst der naechste gleicher Art innerhalb placeTol (Behaelter stehen fest, der Ort ist auf beiden PCs gleich). -1 = keiner.
struct ContainerRef {
    uint32_t serial;
    int kind;
    float pos[3];
};
inline int MatchContainer(const ContainerRef* local, int n, const ContainerRef& remote, float sameSerialTol, float placeTol) {
    auto dist2 = [&remote](const ContainerRef& c) {
        const float dx = c.pos[0] - remote.pos[0], dy = c.pos[1] - remote.pos[1], dz = c.pos[2] - remote.pos[2];
        return dx * dx + dy * dy + dz * dz;
    };
    for (int i = 0; i < n; ++i)
        if (local[i].serial == remote.serial && local[i].kind == remote.kind && dist2(local[i]) <= sameSerialTol * sameSerialTol)
            return i;
    int best = -1;
    float bestD2 = placeTol * placeTol;
    for (int i = 0; i < n; ++i) {
        const float d2 = dist2(local[i]);
        if (local[i].kind != remote.kind || d2 > bestD2) continue;
        best = i;
        bestD2 = d2;
    }
    return best;
}

// Objekt-Pool durchgehen: Bloecke (+0 = naechster Block, 64 Eintraege a 0x50 ab +8). entry(node, index) liefert 1 =
// aufgenommen, 0 = leer, -1 = Objekt nicht lesbar -> nur diesen Eintrag ueberspringen. Vorher brach ein einziges gerade
// freigegebenes Objekt den ganzen Durchlauf ab, alles dahinter fehlte (Level 12: Schiebebloecke nie erkannt, Objekte
// "hier nicht gefunden"). next(block, &naechster) false = Kette nicht lesbar -> Ende. *skipped = uebersprungene Eintraege.
template <typename Next, typename Entry>
inline int WalkPool(uintptr_t block, int maxNodes, Next next, Entry entry, int* skipped) {
    int n = 0;
    *skipped = 0;
    for (int b = 0; block && b < 4096 && n < maxNodes; ++b) {
        for (int e = 0; e < 64 && n < maxNodes; ++e) {
            const int r = entry(block + 8 + static_cast<uintptr_t>(e) * 0x50, n);
            if (r > 0) ++n;
            else if (r < 0) ++*skipped;
        }
        if (!next(block, &block)) break;
    }
    return n;
}

// Inventar-Zaehler des Mitspielers uebernehmen? typeId: 0 max. Leben, 3 Schatten-Level (Stufe*1000), 8 Cadeaux.
// Schatten-Level wird nie ausgegeben -> immer aufs Maximum. Cadeaux sind Waehrung (100 bei Jaunty = mehr Leben): ein
// dauernder Abgleich aufs Maximum holte die abgegebenen vom Mitspieler zurueck (endlos Leben kaufen, Log 24.10.), und
// gekauftes Leben bekam der andere geschenkt. Darum nur catchUp: der Gast holt einmal den Stand des Hosts auf, danach
// zaehlt jeder selbst (eingesammelte Cadeaux bekommen ohnehin beide ueber den Pickup-Abgleich).
inline bool AdoptInventoryCount(int typeId, bool catchUp, int mine, int remote) {
    if (remote <= mine || remote > 100000) return false;
    if (typeId == 3) return true;
    return catchUp;
}

// Steht p im/auf einem Mitfahr-Objekt (Box mn..mx = +0xD8/+0xE4)? Seitlich und unten 'side' Reserve, oben 'above'
// (wer auf dem Dach steht, faehrt mit)
inline bool RideContains(const float mn[3], const float mx[3], const float p[3], float side, float above) {
    return p[0] >= mn[0] - side && p[0] <= mx[0] + side && p[2] >= mn[2] - side && p[2] <= mx[2] + side &&
           p[1] >= mn[1] - side && p[1] <= mx[1] + above;
}

// Sektor-Index (Objekt +0xD4) gueltig fuer das Sektor-Feld des aktuellen Levels?
inline bool SectorIndexValid(int32_t index, uint32_t count) { return index >= 0 && static_cast<uint32_t>(index) < count; }

// Lobby aus "+connect_lobby <id>" (Befehlszeile beim Start durch Steam bzw. Rich-Presence-Beitritt); 0 = keine
inline uint64_t ParseConnectLobby(const char* text) {
    const char* p = text ? std::strstr(text, "+connect_lobby") : nullptr;
    unsigned long long lobby = 0;
    if (!p || std::sscanf(p, "+connect_lobby %llu", &lobby) != 1) return 0;
    return lobby;
}

// Aussehen der Mitspieler-Figur: Die Modell-Funktion 0x459250 setzt die Form nach den Geheimnis-Schaltern (Disco 2, Duppie 3,
// Nettie 4, Deadwing 9, Hund 10, Beta 11, Grosse Schuhe 7/8, Grosskopf 5/6). Beim Mitspieler selbst gilt aber nach dem Laden
// die gespeicherte Form - ein eingeschalteter Schalter heisst also nicht, dass er so aussieht. Deshalb: die form-aendernden
// Schalter nur so setzen, dass genau seine echte Form herauskommt; die uebrigen (Hut, Flammen, ...) bleiben.
// Bits wie kLookCvars in dllmain.cpp.
inline uint32_t LooksForForm(uint32_t looks, int form) {
    constexpr uint32_t kBigHead = 1u << 0, kDisco = 1u << 1, kShoes = 1u << 3, kNettie = 1u << 7, kDuppie = 1u << 8,
                       kDeadwing = 1u << 9, kDog = 1u << 10, kBeta = 1u << 12;
    constexpr uint32_t kFormBits = kBigHead | kDisco | kShoes | kNettie | kDuppie | kDeadwing | kDog | kBeta;
    uint32_t out = looks & ~kFormBits;
    switch (form) {
        case 2: out |= kDisco; break;
        case 3: out |= kDuppie; break;
        case 4: out |= kNettie; break;
        case 5: case 6: out |= kBigHead; break;
        case 7: case 8: out |= kShoes; break;
        case 9: out |= kDeadwing; break;
        case 10: out |= kDog; break;
        case 11: out |= kBeta; break;
        default: break;
    }
    return out;
}

// ---- Namensschilder ----
// Kameramatrix = uClipMatrix des RenderView-Puffers (Zeilen = Basis, Punkt als Zeilenvektor): clip = [x y z 1] * M.
// false = hinter der Kamera (w zu klein); nx/ny in -1..1 (y nach oben)
inline bool ProjectToNdc(const float* m, const float* p, float& nx, float& ny, float& w) {
    const float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    const float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    if (w <= 1.0f) {
        nx = w < 0.0f ? -cx : cx;  // Richtung fuer den Randpfeil
        ny = w < 0.0f ? -cy : cy;
        return false;
    }
    nx = cx / w;
    ny = cy / w;
    return true;
}

// Randpfeil: Punkt am Bildrand (Abstand margin) in Richtung (nx, ny) von der Mitte; hinter der Kamera umgekehrt.
// ang = Richtung in Radiant (0 = rechts, +pi/2 = oben)
inline void EdgePoint(float nx, float ny, bool behind, float margin, float& ex, float& ey, float& ang) {
    float dx = behind ? -nx : nx, dy = behind ? -ny : ny;
    if (std::fabs(dx) < 1e-6f && std::fabs(dy) < 1e-6f) dy = -1.0f;  // genau hinten: nach unten
    const float lim = 1.0f - margin;
    const float s = lim / (std::fabs(dx) > std::fabs(dy) ? std::fabs(dx) : std::fabs(dy));
    ex = dx * s;
    ey = dy * s;
    ang = std::atan2(dy, dx);
}

inline float Clamp01(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Groesse und Deckkraft nach Entfernung (Spiel-Einheiten), mit Untergrenze
inline float TagScale(float dist) { return Clamp01(1.0f - dist / 8000.0f, 0.45f, 1.0f); }
inline float TagAlpha(float dist) { return Clamp01(1.2f - dist / 6000.0f, 0.5f, 1.0f); }
// Shadow Man ist etwa 155 Einheiten gross -> 80 Einheiten je Meter
inline int Meters(float units) { return static_cast<int>(units / 80.0f + 0.5f); }

// Steam-Namen (UTF-8) fuer die Spielschrift (Latin-1); unbekannte Zeichen -> '?'
inline std::string Utf8ToLatin1(const char* s) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); p && *p;) {
        if (*p < 0x80) {
            out += static_cast<char>(*p++);
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            const unsigned c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            out += c < 0x100 ? static_cast<char>(c) : '?';
            p += 2;
        } else {
            int n = (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 1;
            while (n-- > 0 && *p) ++p;
            out += '?';
        }
    }
    return out;
}

// Schrift "FE" (fonts/FE/ftable.dat): 3 Floats Kopf, dann je Zeichen (ab '!') u0 u1 v0 v1 Breite Hoehe Seite(int32);
// Seiten 000/001/002NOMIP.tga (512x512)
struct Glyph {
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0, w = 0, h = 0;
    int page = 0;
    bool valid = false;
};
inline bool ParseFtable(const uint8_t* data, size_t size, Glyph* out) {
    if (size < 12 + 28) return false;
    for (int k = 0; k < 256; ++k) {
        const size_t off = 12 + static_cast<size_t>(k) * 28;
        out[k] = Glyph();
        if (off + 28 > size) continue;
        float f[6];
        int32_t page = -1;
        std::memcpy(f, data + off, sizeof(f));
        std::memcpy(&page, data + off + 24, 4);
        if (page < 0 || page > 2 || f[4] <= 0.0f) continue;
        out[k] = Glyph{f[0], f[1], f[2], f[3], f[4], f[5], page, true};
    }
    return true;
}

// Eine unkomprimiert gespeicherte Datei aus einem .kpf (Zip) lesen; leer = nicht gefunden
inline std::vector<uint8_t> KpfReadFile(const char* kpf, const char* name) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(kpf, "rb");
    if (!f) return out;
    auto rd16 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
    auto rd32 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); };
    _fseeki64(f, 0, SEEK_END);
    const int64_t len = _ftelli64(f);
    const int64_t tail = len < 66000 ? len : 66000;
    std::vector<uint8_t> t(static_cast<size_t>(tail));
    _fseeki64(f, len - tail, SEEK_SET);
    if (std::fread(t.data(), 1, t.size(), f) == t.size()) {
        for (int64_t i = tail - 22; i >= 0; --i) {
            if (rd32(&t[static_cast<size_t>(i)]) != 0x06054b50) continue;  // Ende des Inhaltsverzeichnisses
            const uint32_t cdSize = rd32(&t[static_cast<size_t>(i) + 12]), cdOff = rd32(&t[static_cast<size_t>(i) + 16]);
            std::vector<uint8_t> cd(cdSize);
            _fseeki64(f, cdOff, SEEK_SET);
            if (std::fread(cd.data(), 1, cd.size(), f) != cd.size()) break;
            const size_t nameLen = std::strlen(name);
            for (size_t p = 0; p + 46 <= cd.size() && rd32(&cd[p]) == 0x02014b50;) {
                const uint32_t method = rd16(&cd[p + 10]), csize = rd32(&cd[p + 20]);
                const uint32_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]), local = rd32(&cd[p + 42]);
                if (nl == nameLen && p + 46 + nl <= cd.size() && !std::memcmp(&cd[p + 46], name, nl) && method == 0) {
                    uint8_t lh[30];
                    _fseeki64(f, local, SEEK_SET);
                    if (std::fread(lh, 1, 30, f) == 30 && rd32(lh) == 0x04034b50) {
                        _fseeki64(f, static_cast<int64_t>(local) + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
                        out.resize(csize);
                        if (std::fread(out.data(), 1, csize, f) != csize) out.clear();
                    }
                    break;
                }
                p += 46 + nl + xl + cl;
            }
            break;
        }
    }
    std::fclose(f);
    return out;
}

// Gemeinsamer Tod: warum alle mitsterben (None = nur dieser Spieler stirbt). Bosskampf ist seit 26.10. kein Grund mehr
// (Wunsch des Users: auch dort stirbt jeder fuer sich); Boss bleibt nur als Wert im Protokoll.
enum class DeathReason : uint8_t { None = 0, Boss = 1, SharedHp = 2, Hardcore = 3 };
inline DeathReason DeathReasonFor(bool hardcore, bool sharedHp) {
    if (hardcore) return DeathReason::Hardcore;
    if (sharedHp) return DeathReason::SharedHp;
    return DeathReason::None;
}
// Bosse (Klassenname aus RTTI ohne "kexShadowMan"): die Fuenf (Echtformen sind Zustaende derselben Klasse), Legion in allen
// Formen, Seraph-Koenigin, Matriarchin
inline bool IsBossClass(const char* cls) {
    static const char* const kBosses[] = {"AIJack", "AIMarco", "AIMilton", "AIAvery", "AIBatty", "AILegion", "AIMFLegion",
                                          "AITFLegion", "AISeraph", "AISisterMatriach"};
    for (const char* b : kBosses)
        if (!std::strcmp(cls, b)) return true;
    return false;
}

}  // namespace coop
