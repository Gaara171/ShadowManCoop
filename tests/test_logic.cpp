// Logik-Tests ohne Spiel: Paketformate (pose_codec.h) und reine Hilfsfunktionen (coop_logic.h).
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../src/coop_logic.h"
#include "../src/i18n.h"
#include "../src/pose_codec.h"

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (cond) ++g_passed;                                          \
        else { ++g_failed; std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void TestHostSettingsRoundtrip() {
    posecodec::HostSettingsInfo in;
    in.seq = 7;
    in.hpPercent = 120;
    in.sharedHp = 1;
    in.hardcore = 1;
    in.count = 3;
    in.roster[0] = 76561197960265729ull;
    in.roster[1] = 76561197960265730ull;
    in.roster[2] = 42;
    uint8_t buf[posecodec::MaxHostSettingsPacketSize()];
    const size_t n = posecodec::EncodeHostSettings(in, buf);
    posecodec::HostSettingsInfo out;
    CHECK(posecodec::DecodeHostSettings(buf, n, out));
    CHECK(out.seq == 7 && out.hpPercent == 120 && out.sharedHp == 1 && out.hardcore == 1 && out.count == 3);
    CHECK(out.roster[0] == 76561197960265729ull && out.roster[1] == 76561197960265730ull && out.roster[2] == 42 && out.roster[3] == 0);
    CHECK(!posecodec::DecodeHostSettings(buf, n - 1, out));  // abgeschnitten
    buf[0] ^= 0xFF;
    CHECK(!posecodec::DecodeHostSettings(buf, n, out));      // falsche Kennung
}

static void TestSettingsFile() {
    coop::CoopSettings s;
    CHECK(s.hpPercent == 50 && !s.sharedHp && s.showNames);  // Startwerte laut Spec
    coop::CoopSettings p = coop::ParseSettings("hp 70\nshared 1\nnames 0\n");
    CHECK(p.hpPercent == 70 && p.sharedHp && !p.showNames);
    coop::CoopSettings q = coop::ParseSettings(coop::FormatSettings(p));
    CHECK(q.hpPercent == 70 && q.sharedHp && !q.showNames);
    CHECK(coop::ParseSettings("hp 999\n").hpPercent == 200);  // begrenzt
    CHECK(coop::ParseSettings("hp 55\n").hpPercent == 50);    // 10er-Schritte
    CHECK(coop::ParseSettings("").hpPercent == 50);           // fehlende Datei = Startwerte
}

static void TestHpPercentCycle() {
    CHECK(coop::NextHpPercent(50) == 60);
    CHECK(coop::NextHpPercent(190) == 200);
    CHECK(coop::NextHpPercent(200) == 0);
}

static void TestHpScaleFactor() {
    CHECK(Near(coop::HpScaleFactor(50, 3), 2.0f));  // Beispiel aus der Spec: 50 %, Host + 2 Gaeste
    CHECK(Near(coop::HpScaleFactor(50, 1), 1.0f));  // allein: normal
    CHECK(Near(coop::HpScaleFactor(0, 4), 1.0f));
    CHECK(Near(coop::HpScaleFactor(200, 4), 7.0f));
    CHECK(Near(coop::HpScaleFactor(50, 0), 1.0f));  // unbekannt
}

static void TestRoster() {
    uint64_t r[4] = {};
    const uint64_t host = 100, a = 200, b = 300, c = 400;
    uint64_t m1[] = {a, b};
    coop::RosterUpdate(r, host, m1, 2);
    CHECK(coop::RosterNumberOf(r, host) == 1 && coop::RosterNumberOf(r, a) == 2 && coop::RosterNumberOf(r, b) == 3);
    CHECK(coop::RosterCount(r) == 3);
    uint64_t m2[] = {b, a};  // andere Reihenfolge der Mitgliederliste aendert nichts
    coop::RosterUpdate(r, host, m2, 2);
    CHECK(coop::RosterNumberOf(r, a) == 2 && coop::RosterNumberOf(r, b) == 3);
    uint64_t m3[] = {b};  // a geht: b behaelt seine Nummer (Farbe)
    coop::RosterUpdate(r, host, m3, 1);
    CHECK(coop::RosterNumberOf(r, a) == 0 && coop::RosterNumberOf(r, b) == 3 && coop::RosterCount(r) == 2);
    uint64_t m4[] = {b, c};  // c kommt: kleinste freie Nummer
    coop::RosterUpdate(r, host, m4, 2);
    CHECK(coop::RosterNumberOf(r, c) == 2);
    CHECK(coop::RosterNumberOf(r, 999) == 0);
}

static void TestColors() {
    CHECK(coop::PlayerColorRgb(1) == 0x4CAF50);  // Host gruen
    CHECK(coop::PlayerColorRgb(2) == 0xE53935);  // rot
    CHECK(coop::PlayerColorRgb(3) == 0x1E88E5);  // blau
    CHECK(coop::PlayerColorRgb(4) == 0x8E44AD);  // lila
}

static void TestDeathRoundtrip() {
    posecodec::DeathInfo list[2];
    list[0].seq = 3;
    list[0].reason = 1;
    list[0].level = 22;
    list[1].seq = 4;
    list[1].reason = 3;
    list[1].level = 9;
    uint8_t buf[posecodec::MaxDeathPacketSize()];
    const size_t n = posecodec::EncodeDeaths(0xABCD, list, 2, buf);
    posecodec::DeathInfo out[posecodec::kMaxDeaths];
    uint32_t session = 0;
    int num = 0;
    CHECK(posecodec::DecodeDeaths(buf, n, session, out, num));
    CHECK(session == 0xABCD && num == 2);
    CHECK(out[0].seq == 3 && out[0].reason == 1 && out[0].level == 22 && out[1].seq == 4 && out[1].reason == 3);
    CHECK(!posecodec::DecodeDeaths(buf, n - 1, session, out, num));
}

static void TestDeathReason() {
    using coop::DeathReason;
    // Bosskampf ist kein Grund mehr (Wunsch 26.10.: auch im Bosskampf stirbt jeder fuer sich) -> kein Parameter dafuer
    CHECK(coop::DeathReasonFor(false, false) == DeathReason::None);     // jeder fuer sich (auch im Bosskampf)
    CHECK(coop::DeathReasonFor(false, true) == DeathReason::SharedHp);  // geteilte HP: Balken leer = alle
    CHECK(coop::DeathReasonFor(true, false) == DeathReason::Hardcore);  // Hardcore: jeder Tod
    CHECK(coop::DeathReasonFor(true, true) == DeathReason::Hardcore);   // Hardcore hat Vorrang
}

static void TestBossClasses() {
    CHECK(coop::IsBossClass("AILegion") && coop::IsBossClass("AITFLegion") && coop::IsBossClass("AIMFLegion"));
    CHECK(coop::IsBossClass("AIJack") && coop::IsBossClass("AIMarco") && coop::IsBossClass("AIMilton"));
    CHECK(coop::IsBossClass("AIAvery") && coop::IsBossClass("AIBatty") && coop::IsBossClass("AISeraph"));
    CHECK(coop::IsBossClass("AISisterMatriach"));
    CHECK(!coop::IsBossClass("AIZombie") && !coop::IsBossClass("AIDuppy") && !coop::IsBossClass("AISister") && !coop::IsBossClass(""));
}

static void TestHealthScaling() {
    CHECK(Near(coop::RescaleHealth(25.0f, 1.0f, 2.0f), 50.0f));    // neuer Zombie, 3 Spieler, 50 %
    CHECK(Near(coop::RescaleHealth(50.0f, 2.0f, 1.5f), 37.5f));    // einer geht: Anteil bleibt
    CHECK(Near(coop::RescaleHealth(10.0f, 2.0f, 1.5f), 7.5f));     // halb tot bleibt halb tot
    CHECK(Near(coop::RescaleHealth(30.0f, 0.0f, 2.0f), 30.0f));    // ungueltiger alter Faktor: nichts tun
    CHECK(Near(coop::ScaleGain(40.0f, 140.0f, 2.0f), 240.0f));     // Phasenwechsel +100 -> +200
    CHECK(Near(coop::ScaleGain(40.0f, 30.0f, 2.0f), 30.0f));       // Abnahme (Schaden) bleibt
}

static void TestHardcoreFile() {
    std::map<int, uint32_t> m = coop::ParseHardcoreFile("3 1234\n5 99\nquatsch\n12 1234\n");
    CHECK(m.size() == 3 && m[3] == 1234 && m[5] == 99 && m[12] == 1234);
    const std::vector<int> slots = coop::SlotsOfRun(m, 1234);
    CHECK(slots.size() == 2 && slots[0] == 3 && slots[1] == 12);
    CHECK(coop::SlotsOfRun(m, 7).empty());
    CHECK(coop::SlotsOfRun(m, 0).empty());  // Kennung 0 = kein Hardcore-Lauf: nie etwas loeschen
    const std::map<int, uint32_t> back = coop::ParseHardcoreFile(coop::FormatHardcoreFile(m));
    CHECK(back == m);
    CHECK(coop::ParseHardcoreFile("").empty());
}

static void TestSaveFileStem() {
    CHECK(coop::SaveFileStem(3) == L"save_03");
    CHECK(coop::SaveFileStem(13) == L"save_13");
    CHECK(coop::SaveFileStem(coop::kQuickSaveSlot) == L"quicksave");  // Schnellspeichern (F5)
}

static void TestLifeRoundtrip() {
    posecodec::LifeDelta list[3];
    list[0].seq = 5;
    list[0].delta = -250;
    list[1].seq = 6;
    list[1].delta = 1000;
    uint8_t buf[posecodec::MaxLifePacketSize()];
    const size_t n = posecodec::EncodeLife(0x1234, 4200, list, 2, buf);
    posecodec::LifeDelta out[posecodec::kMaxLifeDeltas];
    uint32_t session = 0;
    int32_t value = 0;
    int num = 0;
    CHECK(posecodec::DecodeLife(buf, n, session, value, out, num));
    CHECK(session == 0x1234 && value == 4200 && num == 2 && out[0].seq == 5 && out[0].delta == -250 && out[1].delta == 1000);
    CHECK(!posecodec::DecodeLife(buf, n - 1, session, value, out, num));
}

static void TestApplyLifeDelta() {
    CHECK(coop::ApplyLifeDelta(3000, -500, 5000) == 2500);
    CHECK(coop::ApplyLifeDelta(4800, 1000, 5000) == 5000);  // nicht ueber das Maximum
    CHECK(coop::ApplyLifeDelta(200, -500, 5000) == 0);      // nicht unter 0
    CHECK(coop::ApplyLifeDelta(200, -500, 0) == 0);         // Maximum unbekannt: nur unten begrenzen
    CHECK(coop::ApplyLifeDelta(200, 500, 0) == 700);
}

// Kameramatrix (uClipMatrix) aus dem laufenden Spiel: Spieler bei 400/-405/-5900 steht mittig, unter der Bildmitte
static const float kRealClip[16] = {0.746f, 0.0f, 0.0f, 0.0f, 0.0f, 1.318f, -0.115f, -0.115f,
                                    0.0f, 0.152f, 0.994f, 0.993f, -298.585f, 1327.712f, 6072.812f, 6080.788f};

static void TestProjection() {
    const float p[3] = {400.0f, -405.0f, -5900.0f};
    float nx = 0, ny = 0, w = 0;
    CHECK(coop::ProjectToNdc(kRealClip, p, nx, ny, w));
    CHECK(std::fabs(nx) < 0.01f && ny < -0.3f && ny > -0.45f && w > 200.0f && w < 330.0f);
    const float behind[3] = {400.0f, -405.0f, -7000.0f};  // hinter der Kamera (bei z -6155, sie schaut nach +z)
    CHECK(!coop::ProjectToNdc(kRealClip, behind, nx, ny, w));
}

static void TestEdgePoint() {
    float ex = 0, ey = 0, ang = 0;
    coop::EdgePoint(3.0f, 0.0f, false, 0.1f, ex, ey, ang);  // weit rechts
    CHECK(Near(ex, 0.9f) && Near(ey, 0.0f) && Near(ang, 0.0f));
    coop::EdgePoint(0.5f, 0.2f, true, 0.1f, ex, ey, ang);  // hinter der Kamera: Richtung umdrehen
    CHECK(Near(ex, -0.9f) && ey < 0.0f && ey > -0.9f);
    coop::EdgePoint(0.0f, 0.0f, true, 0.1f, ex, ey, ang);  // genau hinten: nach unten zeigen
    CHECK(Near(ex, 0.0f) && Near(ey, -0.9f));
}

static void TestTagLook() {
    CHECK(Near(coop::TagScale(0.0f), 1.0f) && coop::TagScale(20000.0f) >= 0.45f && coop::TagScale(4000.0f) < 1.0f);
    CHECK(Near(coop::TagAlpha(0.0f), 1.0f) && coop::TagAlpha(20000.0f) >= 0.5f && coop::TagAlpha(5000.0f) < 1.0f);
    CHECK(coop::Meters(800.0f) == 10 && coop::Meters(39.0f) == 0 && coop::Meters(41.0f) == 1);
    CHECK(coop::Utf8ToLatin1("J\xC3\xBCrgen") == "J\xFCrgen");
    CHECK(coop::Utf8ToLatin1("x\xE2\x82\xACy") == "x?y");  // Euro gibt es nicht in Latin-1
}

static void TestFontTable() {
    const std::vector<uint8_t> ft = coop::KpfReadFile(
        "E:\\SteamLibrary\\steamapps\\common\\Shadow Man Remastered\\ShadowManEX01.kpf", "fonts/FE/ftable.dat");
    CHECK(ft.size() == 7176);
    coop::Glyph g[256];
    CHECK(coop::ParseFtable(ft.data(), ft.size(), g));
    CHECK(g['A' - 33].valid && g['A' - 33].page == 0 && Near(g['A' - 33].w, 38.0f));
    CHECK(g['a' - 33].valid && g['a' - 33].page == 1 && Near(g['a' - 33].u0, 0.0f));
    CHECK(g[0xFC - 33].valid && g[0xFC - 33].page == 2);  // ue
    CHECK(!g[127 - 33].valid);
    const std::vector<uint8_t> tga = coop::KpfReadFile(
        "E:\\SteamLibrary\\steamapps\\common\\Shadow Man Remastered\\ShadowManEX01.kpf", "fonts/FE/000NOMIP.tga");
    CHECK(tga.size() == 1048594 && tga[2] == 2 && tga[16] == 32);  // unkomprimiert, 32 Bit
}

static void TestBlockId() {
    CHECK(coop::BlockId16(0x12345678u) == (0x1234u ^ 0x5678u));
    CHECK(coop::BlockId16(0x00010001u) != 0);  // 0 ist "keine Kennung"
    CHECK(coop::BlockId16(0xABCD0000u) == 0xABCD);
    CHECK(coop::SectorIndexValid(3, 10) && coop::SectorIndexValid(0, 1));
    CHECK(!coop::SectorIndexValid(10, 10) && !coop::SectorIndexValid(-1, 10) && !coop::SectorIndexValid(0, 0));
}

// Schiebeblock-Zustand +0x290 (gad3 = PushGad3Block)
static void TestBlockStates() {
    // PushBlock/PushLavaBlock: 0 frisch, 2/3 rutscht, 1 fertig
    CHECK(!coop::BlockMoving(0, false) && coop::BlockMoving(2, false) && coop::BlockMoving(3, false) && !coop::BlockMoving(1, false));
    // PushGad3Block: 0 -> 7 ruht (schiebbar), 2/3 rutscht, 1 Fall-Pruefung, 4/5 faellt, 6 fertig
    CHECK(!coop::BlockMoving(0, true) && !coop::BlockMoving(7, true) && !coop::BlockMoving(6, true));
    CHECK(coop::BlockMoving(1, true) && coop::BlockMoving(2, true) && coop::BlockMoving(4, true) && coop::BlockMoving(5, true));
    CHECK(coop::BlockDoneState(false) == 1 && coop::BlockDoneState(true) == 6);
    CHECK(!coop::BlockMoving(coop::BlockDoneState(false), false) && !coop::BlockMoving(coop::BlockDoneState(true), true));
    // Spielstand mit altem Fehler: verschoben (Position != Grundposition), aber noch frisch -> reparieren
    const float base[3] = {-4992.0f, -64.0f, -10240.0f}, moved[3] = {-4837.25f, -64.0f, -10240.0f};
    CHECK(coop::BlockNeedsRepair(0, false, moved, base));
    CHECK(coop::BlockNeedsRepair(7, true, moved, base));
    CHECK(!coop::BlockNeedsRepair(0, false, base, base));   // nie geschoben
    CHECK(!coop::BlockNeedsRepair(1, false, moved, base));  // richtig fertig
    CHECK(!coop::BlockNeedsRepair(6, true, moved, base));
    CHECK(!coop::BlockNeedsRepair(3, false, moved, base));  // rutscht gerade
    const float nearly[3] = {-4992.4f, -64.0f, -10240.0f};
    CHECK(!coop::BlockNeedsRepair(0, false, nearly, base)); // Rundung
}

// Steht der Spieler im/auf einem Mitfahr-Objekt? (Box +0xD8/+0xE4, seitlich/unten 64, oben 200 Reserve)
static void TestRideContains() {
    const float mn[3] = {0, 0, 0}, mx[3] = {256, 256, 256};
    const float inside[3] = {128, 10, 128}, onTop[3] = {128, 400, 128}, edge[3] = {300, 10, 128};
    const float far[3] = {5000, 10, 128}, above[3] = {128, 500, 128}, below[3] = {128, -100, 128};
    CHECK(coop::RideContains(mn, mx, inside, 64, 200));
    CHECK(coop::RideContains(mn, mx, onTop, 64, 200));   // steht auf dem Dach (256 + 144)
    CHECK(coop::RideContains(mn, mx, edge, 64, 200));    // Rand innerhalb der Reserve
    CHECK(!coop::RideContains(mn, mx, far, 64, 200));
    CHECK(!coop::RideContains(mn, mx, above, 64, 200));  // 244 ueber dem Dach
    CHECK(!coop::RideContains(mn, mx, below, 64, 200));
}

// Verschwinden viele Behaelter/Pickups auf einmal oder schrumpft der Pool stark, wurde der Levelinhalt neu aufgebaut
// (z.B. Ring-Cutscene Level 15) - nichts davon als zerstoert/eingesammelt melden
static void TestMassVanish() {
    CHECK(!coop::MassVanish(3, 0, 9000, 9000));    // Explosion: 3 Toepfe
    CHECK(!coop::MassVanish(0, 1, 9000, 8990));    // ein Pickup eingesammelt
    CHECK(!coop::MassVanish(0, 0, 9000, 9000));
    CHECK(coop::MassVanish(37, 0, 9000, 9000));    // alle Govis + Toepfe + Platte auf einmal
    CHECK(!coop::MassVanish(2, 3, 9000, 9000));    // zusammen 5: noch normal (Schwelle 8)
    CHECK(coop::MassVanish(0, 8, 9000, 9000));     // 8 Pickups auf einmal
    CHECK(coop::MassVanish(2, 0, 9000, 5000));     // Pool um 44 % kleiner
    CHECK(!coop::MassVanish(2, 0, 9000, 8000));    // 11 % kleiner: normal
}

// Behaelter des Mitspielers hier finden: Pool-Seriennummern unterscheiden sich je nach Ladeweg (Level 12: #774 / #779)
static void TestMatchContainer() {
    const coop::ContainerRef local[] = {
        {741, 2, {100, 0, 100}},   // Topf
        {745, 2, {150, 0, 100}},   // Topf daneben (50 weg)
        {746, 1, {400, 0, 400}},   // Govi
        {760, 2, {900, 0, 900}},   // Topf
    };
    const int n = 4;
    CHECK(coop::MatchContainer(local, n, {741, 2, {100, 0, 100}}, 100, 32) == 0);  // gleiche Nummer, gleicher Ort
    CHECK(coop::MatchContainer(local, n, {755, 2, {900, 0, 900}}, 100, 32) == 3);  // andere Nummer, gleicher Ort
    CHECK(coop::MatchContainer(local, n, {741, 2, {900, 0, 900}}, 100, 32) == 3);  // Nummer gehoert hier einem anderen Topf
    CHECK(coop::MatchContainer(local, n, {999, 1, {401, 0, 399}}, 100, 32) == 2);  // Govi ueber den Ort
    CHECK(coop::MatchContainer(local, n, {999, 2, {400, 0, 400}}, 100, 32) == -1); // falsche Art
    CHECK(coop::MatchContainer(local, n, {999, 2, {120, 0, 100}}, 100, 32) == 0);  // naechster gewinnt (20 vs 30)
    CHECK(coop::MatchContainer(local, n, {999, 2, {125, 0, 160}}, 100, 32) == -1); // beide zu weit (Topf hier schon weg?)
    CHECK(coop::MatchContainer(local, 0, {741, 2, {100, 0, 100}}, 100, 32) == -1); // nichts da
}

// Pool-Durchlauf: ein nicht lesbares Objekt darf nur seinen Eintrag kosten, nicht alles dahinter (Level 12: Bloecke fehlten)
static void TestWalkPool() {
    // zwei Bloecke: 0x10000 -> 0x20000 -> Ende; Eintrag 3 in Block 1 ist kaputt, Block 2 hat 2 Objekte
    auto next = [](uintptr_t block, uintptr_t* out) {
        *out = block == 0x10000 ? 0x20000 : 0;
        return true;
    };
    std::vector<uintptr_t> seen;
    auto entry = [&seen](uintptr_t node, int) {
        const uintptr_t block = node < 0x20000 ? 0x10000 : 0x20000;
        const int e = static_cast<int>((node - block - 8) / 0x50);
        if (block == 0x10000 && e == 3) return -1;            // Objekt gerade freigegeben
        if (block == 0x10000 && e < 6) { seen.push_back(node); return 1; }
        if (block == 0x20000 && e < 2) { seen.push_back(node); return 1; }
        return 0;                                             // leer
    };
    int skipped = 0;
    CHECK(coop::WalkPool(0x10000, 100, next, entry, &skipped) == 7);  // 5 + 2: alles nach dem kaputten Eintrag ist dabei
    CHECK(skipped == 1);
    CHECK(seen.size() == 7 && seen.back() == 0x20000 + 8 + 0x50);
    seen.clear();
    CHECK(coop::WalkPool(0x10000, 3, next, entry, &skipped) == 3);  // Obergrenze
    auto broken = [](uintptr_t, uintptr_t*) { return false; };     // Kette nicht lesbar: nach Block 1 Schluss
    seen.clear();
    CHECK(coop::WalkPool(0x10000, 100, broken, entry, &skipped) == 5);
    CHECK(coop::WalkPool(0, 100, next, entry, &skipped) == 0 && skipped == 0);
}

// Inventar-Zaehler vom Mitspieler: Cadeaux bei Jaunty abgegeben -> darf nicht vom Mitspieler zurueckkommen (Log 24.10.)
static void TestAdoptInventoryCount() {
    CHECK(!coop::AdoptInventoryCount(8, false, 5, 105));      // 100 Cadeaux abgegeben: bleibt bei 5
    CHECK(!coop::AdoptInventoryCount(0, false, 4000, 3000));  // max. Leben gekauft: kein Ruecksetzen
    CHECK(!coop::AdoptInventoryCount(0, false, 3000, 4000));  // Mitspieler kaufte Leben: nicht geschenkt
    CHECK(coop::AdoptInventoryCount(8, true, 0, 250));        // Gast trifft den Host zum ersten Mal: aufholen
    CHECK(coop::AdoptInventoryCount(0, true, 3000, 4000));
    CHECK(!coop::AdoptInventoryCount(8, true, 300, 250));     // hat schon mehr
    CHECK(coop::AdoptInventoryCount(3, false, 2000, 3000));   // Schatten-Level: immer das Maximum
    CHECK(!coop::AdoptInventoryCount(3, false, 3000, 3000));
    CHECK(!coop::AdoptInventoryCount(8, true, 0, 200000));    // unsinniger Wert
}

// Sprache: zwei Fassungen je Text, Platzhalter muessen gleich sein (prueft schon der Compiler)
static void TestI18n() {
    using i18n::Lang;
    static_assert(i18n::SameFormat("a %d b %s", "x %d y %s"), "gleich");
    static_assert(!i18n::SameFormat("a %d", "a %s"), "Typ verschieden");
    static_assert(!i18n::SameFormat("a %d %d", "a %d"), "Anzahl verschieden");
    static_assert(i18n::SameFormat("100 %% %u", "%u per cent"), "%% zaehlt nicht");
    static_assert(i18n::SameFormat("0x%llX %.0f", "%llX at %.2f"), "Laengen/Genauigkeit egal, Typ zaehlt");
    static_assert(!i18n::SameFormat("%llX", "%X"), "Laenge zaehlt");
    CHECK(i18n::LangFromCode("de") == Lang::De && i18n::LangFromCode("DE") == Lang::De && i18n::LangFromCode("de-DE") == Lang::De);
    CHECK(i18n::LangFromCode("en") == Lang::En && i18n::LangFromCode("fr") == Lang::En && i18n::LangFromCode(nullptr) == Lang::En);
    CHECK(i18n::LangFromCode("") == Lang::En && i18n::LangFromCode("d") == Lang::En);
    CHECK(i18n::LangFromCfgText("seta v_width \"3840\"\nseta g_language \"de\"\n") == Lang::De);
    CHECK(i18n::LangFromCfgText("seta g_language \"en\"\n") == Lang::En);
    CHECK(i18n::LangFromCfgText("") == Lang::En);
    CHECK(i18n::LangFromCfgText("seta g_languagex \"de\"\n") == Lang::En);  // anderer Name
    i18n::SetLang(Lang::De);
    CHECK(std::string(L("Hallo %d", "Hello %d")) == "Hallo %d");
    i18n::SetLang(Lang::En);
    CHECK(std::string(L("Hallo %d", "Hello %d")) == "Hello %d");
}

static void TestConnectLobby() {
    CHECK(coop::ParseConnectLobby("+connect_lobby 109775241234567890") == 109775241234567890ull);
    CHECK(coop::ParseConnectLobby("\"E:\\Spiel\\thoth_x64.exe\" -foo +connect_lobby 42 +bar") == 42);
    CHECK(coop::ParseConnectLobby("thoth_x64.exe") == 0);
    CHECK(coop::ParseConnectLobby("+connect_lobby abc") == 0);
    CHECK(coop::ParseConnectLobby(nullptr) == 0);
}

// Bits wie kLookCvars: 0 Grosskopf, 1 Disco, 2 Flammen, 3 Grosse Schuhe, 4 Stetson, 5 Schrotflinten-Kopf, 6 Unsichtbar,
// 7 Nettie, 8 Duppie, 9 Deadwing, 10 Hund, 11 Blutunterlaufen, 12 Beta, 13 Twig Boy
static void TestLooksForForm() {
    const uint32_t duppie = 1u << 8, stetson = 1u << 4, nettie = 1u << 7, shoes = 1u << 3, bighead = 1u << 0;
    CHECK(coop::LooksForForm(duppie, 0) == 0);                    // Schalter an, Mitspieler ist aber Shadow Man
    CHECK(coop::LooksForForm(duppie | stetson, 0) == stetson);    // Hut bleibt
    CHECK(coop::LooksForForm(duppie, 3) == duppie);               // er ist wirklich Duppie
    CHECK(coop::LooksForForm(0, 3) == duppie);                    // Form 3 ohne Schalter: Duppie trotzdem bauen
    CHECK(coop::LooksForForm(duppie | nettie, 4) == nettie);
    CHECK(coop::LooksForForm(bighead | shoes, 7) == shoes);       // grosse Schuhe (Shadow Man)
    CHECK(coop::LooksForForm(bighead | shoes, 5) == bighead);     // grosser Kopf ohne Schuhe
    CHECK(coop::LooksForForm(0, 1) == 0);
}

int main() {
    TestLooksForForm();
    TestConnectLobby();
    TestBlockId();
    TestBlockStates();
    TestRideContains();
    TestMassVanish();
    TestMatchContainer();
    TestWalkPool();
    TestAdoptInventoryCount();
    TestI18n();
    TestProjection();
    TestEdgePoint();
    TestTagLook();
    TestFontTable();
    TestLifeRoundtrip();
    TestApplyLifeDelta();
    TestHardcoreFile();
    TestSaveFileStem();
    TestHealthScaling();
    TestDeathRoundtrip();
    TestDeathReason();
    TestBossClasses();
    TestHostSettingsRoundtrip();
    TestSettingsFile();
    TestHpPercentCycle();
    TestHpScaleFactor();
    TestRoster();
    TestColors();
    std::printf("%d bestanden, %d fehlgeschlagen\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
