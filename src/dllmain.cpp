// SMCoop Probe - Phase 0/1 (Analyse-Werkzeug)
// Wird als xaudio2_9redist.dll vom Spiel geladen, oeffnet eine Konsole und hilft,
// das Spielerobjekt (kexShadowManPlayer) zu untersuchen und einen zweiten Spieler zu erzeugen.
//
// Hotkeys (im Spiel):
// Der Spieler wird automatisch gesucht, sobald er existiert.
// ACHTUNG: Im Spiel sind F5 = Schnellspeichern, F7 = Schnellladen, F10 = Screenshot belegt.
//   F6   Neuen Schnappschuss speichern
//   F8   Mit Schnappschuss vergleichen, Rohdaten + Analyse ins Log
//   F9   Zweiten Spieler erzeugen (Experiment)
//   F11  Hardware-Watchpoint: wer schreibt die Knochen des Spielers?

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "coop_logic.h"
#include "i18n.h"
#include "net.h"
#include "overlay.h"
#include "pose_codec.h"

// Proxy: alle Exporte der echten xaudio2_9redist.dll an die umbenannte Original-DLL weiterleiten.
// Die Ordinalnummern muessen stimmen, weil das Spiel per Ordinal importiert.
#pragma comment(linker, "/EXPORT:XAudio2Create=xaudio2_9redist_orig.XAudio2Create,@1")
#pragma comment(linker, "/EXPORT:CreateAudioReverb=xaudio2_9redist_orig.CreateAudioReverb,@2")
#pragma comment(linker, "/EXPORT:CreateAudioVolumeMeter=xaudio2_9redist_orig.CreateAudioVolumeMeter,@3")
#pragma comment(linker, "/EXPORT:CreateFX=xaudio2_9redist_orig.CreateFX,@4")
#pragma comment(linker, "/EXPORT:X3DAudioCalculate=xaudio2_9redist_orig.X3DAudioCalculate,@5")
#pragma comment(linker, "/EXPORT:X3DAudioInitialize=xaudio2_9redist_orig.X3DAudioInitialize,@6")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// Werte fuer thoth_x64.exe vom 27.11.2022 (aktuelle Steam-Version)
constexpr uintptr_t kPlayerVtableRva       = 0x725160;
constexpr uintptr_t kGameInstanceVtableRva = 0x6EAA20;
constexpr uintptr_t kGPlayerRva            = 0xF617D8;  // globaler Spielerzeiger (gesetzt von GetPlayer 0x458680)
// Zentrales Spielobjekt (Getter 0x2E98B0, statisch). Daraus baut das Spiel u.a. den Steam-Status ("level{:02}").
constexpr uintptr_t kGameStateRva          = 0xF9C1A0;
constexpr uintptr_t kGameStateInGameOff    = 0x26;      // byte: 1 = im Spiel, 0 = Hauptmenue (Hintergrundszene)
constexpr uintptr_t kGameStateLevelOff     = 0x2C;      // int: aktuelle Level-Nummer

// Erfolgs-Sperre, genau wie die Pruefung des Spiels (0x2BF0F3): gesperrt, wenn
//   GameState+0x40 != 0           (Abenteuer-Flags, mit dem Spielstand gespeichert; Cheats setzen Bit 0x08)
//   oder [0xD837D0]+8 Bit 0 gesetzt (Cheats in dieser Sitzung)
// Beides setzt nur die Cheat-Funktion 0x2EA6F0 (Konsolen-Cheats/Cheat-Codes). Der Mod ruft sie nie auf -
// wir ueberwachen es trotzdem.
constexpr uintptr_t kGameStateFlagsOff     = 0x40;
constexpr uintptr_t kSessionCheatsPtrRva   = 0xD837D0;
struct CheatState { uint8_t adventure; bool session; };
CheatState GetCheatState(uintptr_t base) {
    CheatState s{*reinterpret_cast<uint8_t*>(base + kGameStateRva + kGameStateFlagsOff), false};
    const uintptr_t sess = *reinterpret_cast<uintptr_t*>(base + kSessionCheatsPtrRva);
    if (sess) s.session = (*reinterpret_cast<uint32_t*>(sess + 8) & 1) != 0;
    return s;
}
bool CheatsUsed(uintptr_t base) {
    const CheatState s = GetCheatState(base);
    return s.adventure != 0 || s.session;
}

struct LevelInfo { int level; bool inGame; };
LevelInfo GetLevelInfo(uintptr_t base) {
    return {*reinterpret_cast<int*>(base + kGameStateRva + kGameStateLevelOff),
            *reinterpret_cast<uint8_t*>(base + kGameStateRva + kGameStateInGameOff) != 0};
}

using PlayerBonesFn = void (*)(void*);
PlayerBonesFn g_playerBonesOrig = nullptr;  // Original von 0x45EEA0 (Spieler-Knochen), nach dem Hook gesetzt
void* InstallInlineHook(uintptr_t target, size_t prologueLen, void* detour);
constexpr size_t    kDumpSize              = 0x2000;

FILE*                g_log = nullptr;
int                  g_instance = 0;  // weitere Spiel-Instanz auf demselben PC (Test): 1 = smcoop_log2/cmd2, 2 = ..3, 3 = ..4
uintptr_t            g_base = 0;
std::vector<uintptr_t> g_players;
std::vector<uint8_t>   g_snapshot;
uintptr_t            g_snapshotAddr = 0;
bool                 g_watch = false;

// Log: Zeilen kommen in eine Warteschlange, ein eigener Thread schreibt sie in Datei und Konsole.
// So kann eine blockierende Konsole (z.B. Markiermodus nach Klick ins Fenster) nie das Spiel oder das
// Netzwerk anhalten.
CRITICAL_SECTION g_logLock;
std::vector<std::string> g_logQueue;
HANDLE g_logEvent = nullptr;
constexpr size_t kMaxLogQueue = 20000;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!g_logEvent) return;  // Logger noch nicht gestartet
    EnterCriticalSection(&g_logLock);
    if (g_logQueue.size() < kMaxLogQueue) g_logQueue.emplace_back(buf);
    LeaveCriticalSection(&g_logLock);
    SetEvent(g_logEvent);
}

DWORD WINAPI LoggerThread(LPVOID) {
    std::vector<std::string> batch;
    for (;;) {
        WaitForSingleObject(g_logEvent, 500);
        EnterCriticalSection(&g_logLock);
        batch.swap(g_logQueue);
        LeaveCriticalSection(&g_logLock);
        for (const std::string& line : batch) {
            if (g_log) fputs(line.c_str(), g_log);
            fputs(line.c_str(), stdout);
        }
        if (g_log && !batch.empty()) fflush(g_log);
        batch.clear();
    }
    return 0;
}

// Sprache vor der ersten Protokollzeile: g_language aus kexengine.cfg (Spieltexte sind noch nicht geladen; danach
// uebernimmt DetectLanguageFromGame). Fehlt die Datei oder der Eintrag -> Englisch.
void DetectLanguageFromConfig() {
    PWSTR saved = nullptr;
    std::string text;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)) && saved) {
        const std::wstring path = std::wstring(saved) + L"\\Nightdive Studios\\Shadowman EX\\kexengine.cfg";
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            char buf[4096];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
    }
    if (saved) CoTaskMemFree(saved);
    i18n::SetLang(i18n::LangFromCfgText(text));
}

void StartLogger() {
    InitializeCriticalSection(&g_logLock);
    g_logEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, LoggerThread, nullptr, 0, nullptr)) CloseHandle(t);
}

// Markiermodus der Konsole abschalten: ein Klick ins Fenster wuerde sonst jede Ausgabe anhalten
void DisableConsoleQuickEdit() {
    HANDLE in = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(in, &mode)) SetConsoleMode(in, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    CloseHandle(in);
}

// Akustische Rueckmeldung fuer Hotkeys (frueher fuer manuelle Tests). Standardmaessig aus.
constexpr bool kSoundSignals = false;
void Signal(DWORD freq, DWORD durationMs) {
    if (kSoundSignals) Beep(freq, durationMs);
}

// Liest Speicher sicher (stuerzt nicht ab, wenn die Adresse ungueltig ist). Direkt mit SEH statt ReadProcessMemory: das
// meldete Bkav Pro auf Nexus/VirusTotal zusammen mit Write... als verdaechtig (Injektor-Muster), obwohl nur der eigene
// Prozess gelesen wird.
bool SafeRead(uintptr_t addr, void* out, size_t size) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Schreibt sicher in den eigenen Prozess (Gegenstueck zu SafeRead)
bool SafeWrite(uintptr_t addr, const void* in, size_t size) {
    __try {
        std::memcpy(reinterpret_cast<void*>(addr), in, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Prueft ueber RTTI, ob an base+rva wirklich die vtable der erwarteten Klasse liegt.
bool VerifyVtable(uintptr_t rva, const char* expectedName) {
    uintptr_t colPtr = 0;
    if (!SafeRead(g_base + rva - 8, &colPtr, sizeof(colPtr))) return false;
    uint32_t tdRva = 0;
    if (!SafeRead(colPtr + 0x0C, &tdRva, sizeof(tdRva))) return false;
    char name[128] = {};
    if (!SafeRead(g_base + tdRva + 0x10, name, sizeof(name) - 1)) return false;
    return std::strcmp(name, expectedName) == 0;
}

// Prueft, ob an der gemerkten Adresse noch ein kexShadowManPlayer liegt.
bool PlayerStillValid(uintptr_t addr) {
    uintptr_t vt = 0;
    return addr && SafeRead(addr, &vt, sizeof(vt)) && vt == g_base + kPlayerVtableRva;
}

// Laut Analyse (Phase 0): Position X/Y/Z liegt bei +0x20/+0x24/+0x28.
void LogPosition(uintptr_t addr) {
    float p[3] = {};
    if (SafeRead(addr + 0x20, p, sizeof(p)))
        Log(L("  Position: X=%.1f  Y=%.1f  Z=%.1f\n", "  Position: X=%.1f  Y=%.1f  Z=%.1f\n"), p[0], p[1], p[2]);
}

// Durchsucht den beschreibbaren Speicher nach Objekten, deren erstes Feld == vtable ist.
// Der Lesepuffer liegt in einer eigenen VirtualAlloc-Region, die uebersprungen wird,
// sonst findet der Scanner Kopien der Treffer in seinem eigenen Puffer.
std::vector<uintptr_t> FindInstances(uintptr_t vtableRva) {
    constexpr size_t kChunk = 1 << 20;
    static uint8_t* buffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    std::vector<uintptr_t> found;
    if (!buffer) return found;
    const uintptr_t vt = g_base + vtableRva;
    const uintptr_t bufLo = reinterpret_cast<uintptr_t>(buffer);
    ULONG_PTR stackLo = 0, stackHi = 0;
    GetCurrentThreadStackLimits(&stackLo, &stackHi);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t regionSize = mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                              (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
                              !(mbi.Protect & PAGE_GUARD);
        const bool ours = (regionBase <= bufLo && bufLo < regionBase + regionSize) ||
                          (regionBase < stackHi && stackLo < regionBase + regionSize);
        if (readable && !ours) {
            for (size_t pos = 0; pos < regionSize; pos += kChunk) {
                const size_t want = (regionSize - pos < kChunk) ? regionSize - pos : kChunk;
                const size_t got = SafeRead(regionBase + pos, buffer, want) ? want : 0;
                for (size_t i = 0; i + 8 <= got; i += 8) {
                    uintptr_t v;
                    std::memcpy(&v, buffer + i, 8);
                    if (v == vt) found.push_back(regionBase + pos + i);
                }
            }
        }
        addr = regionBase + regionSize;
    }
    return found;
}

void CmdSnapshot() {
    if (g_players.empty()) { Log(L("  Noch kein Spieler gefunden.\n", "  No player found yet.\n")); Signal(300, 300); return; }
    g_snapshotAddr = g_players[0];
    g_snapshot.assign(kDumpSize, 0);
    SafeRead(g_snapshotAddr, g_snapshot.data(), kDumpSize);
    LogPosition(g_snapshotAddr);
    Log(L("  Schnappschuss von 0x%llX gespeichert. Jetzt bewegen, stehen bleiben und F8 druecken.\n", "  Snapshot of 0x%llX saved. Now move, stand still and press F8.\n"),
        (unsigned long long)g_snapshotAddr);
}

void CmdFindPlayers() {
    Log(L("\nSuche Spielerobjekt... (nicht bewegen)\n", "\nSearching player object... (don't move)\n"));
    g_players = FindInstances(kPlayerVtableRva);
    if (g_players.empty()) {
        Log(L("  Kein Spieler gefunden. Bist du schon in einem Level (nicht im Hauptmenue)?\n", "  No player found. Are you in a level yet (not in the main menu)?\n"));
        Signal(300, 300);
        return;
    }
    for (size_t i = 0; i < g_players.size(); ++i)
        Log(L("  Spieler #%zu bei 0x%llX\n", "  Player #%zu at 0x%llX\n"), i, (unsigned long long)g_players[i]);
    auto gi = FindInstances(kGameInstanceVtableRva);
    for (auto a : gi) Log(L("  GameInstance bei 0x%llX\n", "  GameInstance at 0x%llX\n"), (unsigned long long)a);
    CmdSnapshot();
    Signal(1000, 150);  // fertig: jetzt darf sich der Spieler bewegen
}

bool PlausibleFloat(float f) { return std::isfinite(f) && std::fabs(f) < 1.0e6f && (f == 0.0f || std::fabs(f) > 1.0e-4f); }

void CmdDiff() {
    if (g_snapshot.empty()) { Log(L("\n[F8] Noch kein Schnappschuss.\n", "\n[F8] No snapshot yet.\n")); Signal(300, 300); return; }
    if (!PlayerStillValid(g_snapshotAddr)) {
        Log(L("\n[F8] ACHTUNG: Spielerobjekt bei 0x%llX existiert nicht mehr (wurde neu erstellt).\n", "\n[F8] WARNING: player object at 0x%llX no longer exists (was recreated).\n"),
            (unsigned long long)g_snapshotAddr);
        Log(L("     Suche neu - danach bitte die Bewegung wiederholen und F8 druecken.\n", "     Searching again - then repeat the movement and press F8.\n"));
        CmdFindPlayers();
        return;
    }
    std::vector<uint8_t> now(kDumpSize, 0);
    SafeRead(g_snapshotAddr, now.data(), kDumpSize);
    Log(L("\n[F8] Spieler bei 0x%llX\n", "\n[F8] Player at 0x%llX\n"), (unsigned long long)g_snapshotAddr);
    LogPosition(g_snapshotAddr);
    Log(L("  Geaenderte float-Werte (Offset: alt -> neu):\n", "  Changed float values (offset: old -> new):\n"));
    int shown = 0;
    for (size_t off = 0; off + 4 <= kDumpSize; off += 4) {
        float a, b;
        std::memcpy(&a, &g_snapshot[off], 4);
        std::memcpy(&b, &now[off], 4);
        if (!PlausibleFloat(a) || !PlausibleFloat(b) || std::fabs(a - b) < 0.01f) continue;
        Log(L("  +0x%04zX: %12.3f -> %12.3f\n", "  +0x%04zX: %12.3f -> %12.3f\n"), off, a, b);
        if (++shown >= 80) { Log(L("  ... (gekuerzt)\n", "  ... (truncated)\n")); break; }
    }
    if (!shown) Log(L("  Nichts geaendert.\n", "  Nothing changed.\n"));
    // Rohdaten fuer die genaue Auswertung speichern (vorher/nachher)
    static int diffNo = 0;
    ++diffNo;
    char name[64];
    std::snprintf(name, sizeof(name), "smcoop_dump_%d_a.bin", diffNo);
    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(g_snapshot.data(), 1, kDumpSize, f); std::fclose(f); }
    std::snprintf(name, sizeof(name), "smcoop_dump_%d_b.bin", diffNo);
    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(now.data(), 1, kDumpSize, f); std::fclose(f); }
    Log(L("  Rohdaten gespeichert: smcoop_dump_%d_a.bin / _b.bin\n", "  Raw data saved: smcoop_dump_%d_a.bin / _b.bin\n"), diffNo);
    g_snapshot = now;  // naechstes F8 vergleicht ab hier
    Signal(1000, 100); Signal(1400, 100);
}

std::vector<size_t> LoadWatchList() {
    std::vector<size_t> offs;
    if (FILE* f = std::fopen("smcoop_watch.txt", "r")) {
        char line[64];
        while (std::fgets(line, sizeof(line), f)) {
            size_t v = std::strtoull(line, nullptr, 0);
            if (v || line[0] == '0') offs.push_back(v);
        }
        std::fclose(f);
    }
    return offs;
}

void PrintWatch() {
    static std::vector<size_t> offs;
    static DWORD lastLoad = 0;
    if (GetTickCount() - lastLoad > 2000) { offs = LoadWatchList(); lastLoad = GetTickCount(); }
    if (g_players.empty() || offs.empty()) return;
    std::string line = "\r";
    char buf[64];
    for (size_t off : offs) {
        float f = 0;
        SafeRead(g_players[0] + off, &f, 4);
        std::snprintf(buf, sizeof(buf), "+0x%zX=%.2f  ", off, f);
        line += buf;
    }
    std::fputs(line.c_str(), stdout);
}

// --- Phase 1: Aufrufzaehler fuer die virtuellen Funktionen von kexShadowManPlayer ---
// Jeder vtable-Eintrag wird auf einen kleinen Maschinencode-Thunk umgebogen:
//   mov rax, &zaehler ; lock inc qword [rax] ; mov rax, original ; jmp rax
// rax ist unter Win64 kein Parameterregister, die Argumente (rcx, rdx, r8, r9) bleiben unberuehrt.
constexpr int kVtableEntries = 7;
volatile LONG64 g_vcalls[kVtableEntries];
volatile LONG64 g_skinCallsPlayer = 0, g_skinCallsOther = 0;  // Aufrufe von AddSkin (siehe Phase-1-Geist)
std::atomic<uintptr_t> g_lastPlayerAnim{0}, g_lastPlayerModel{0};
constexpr int kMaxCallers = 16;
volatile uintptr_t g_callerRva[kMaxCallers];
volatile LONG64 g_callerCount[kMaxCallers];

// Knochen eines beliebigen Spielerobjekts ausgeben (anim = player+0x368)
void LogBonesOf(const char* label, uintptr_t player) {
    if (!player) return;
    const uintptr_t anim = player + 0x368;
    uintptr_t data = 0, count = 0;
    SafeRead(anim + 0x180, &data, 8);
    SafeRead(anim + 0x188, &count, 8);
    float pos[3] = {};
    SafeRead(player + 0x20, pos, sizeof(pos));
    Log(L("[%s] 0x%llX Pos=%.1f/%.1f/%.1f Knochen-Daten=0x%llX Anzahl=%llu\n", "[%s] 0x%llX pos=%.1f/%.1f/%.1f bone data=0x%llX count=%llu\n"), label, (unsigned long long)player, pos[0], pos[1],
        pos[2], (unsigned long long)data, (unsigned long long)count);
    uint64_t f[0x40] = {};  // anim-Struktur +0x00..+0x1FF als qwords
    SafeRead(anim, f, sizeof(f));
    Log(L("  anim-Felder:", "  anim fields:"));
    for (int i = 0; i < 0x40; ++i) {
        if (i % 8 == 0) Log(L("\n   +%03X:", "\n   +%03X:"), i * 8);
        Log(L(" %llX", " %llX"), (unsigned long long)f[i]);
    }
    Log(L("\n", "\n"));
    for (uintptr_t i = 0; i < count && i < 2; ++i) {
        float m[8] = {};
        SafeRead(data + i * 0x40, m, sizeof(m));
        Log(L("  Knochen %llu: q=[%.2f %.2f %.2f %.2f] p=[%.1f %.1f %.1f]\n", "  Bone %llu: q=[%.2f %.2f %.2f %.2f] p=[%.1f %.1f %.1f]\n"), (unsigned long long)i, m[0], m[1], m[2], m[3], m[4], m[5], m[6]);
    }
}

void LogCallers() {
    Log(L("[Aufrufer von AddSkin fuer den Spieler]\n", "[Callers of AddSkin for the player]\n"));
    for (int i = 0; i < kMaxCallers && g_callerRva[i]; ++i)
        Log(L("  Ruecksprung 0x%llX : %lld Aufrufe\n", "  Return 0x%llX : %lld calls\n"), (unsigned long long)g_callerRva[i], (long long)g_callerCount[i]);
}

// Debug: Animationskomponente des Spielers und erste Knochen-Matrizen ausgeben
void LogPlayerAnim() {
    const uintptr_t anim = g_lastPlayerAnim;
    if (!anim) { Log(L("[Anim] noch keine Daten\n", "[Anim] no data yet\n")); return; }
    uintptr_t data = 0, count = 0;
    SafeRead(anim + 0x180, &data, 8);
    SafeRead(anim + 0x188, &count, 8);
    const uintptr_t obj = g_players.empty() ? 0 : g_players[0];
    Log(L("[Anim] anim=0x%llX (obj%+lld) model=0x%llX  Knochen-Daten=0x%llX Anzahl=%llu\n", "[Anim] anim=0x%llX (obj%+lld) model=0x%llX  bone data=0x%llX count=%llu\n"),
        (unsigned long long)anim, obj ? (long long)(anim - obj) : 0LL, (unsigned long long)g_lastPlayerModel.load(),
        (unsigned long long)data, (unsigned long long)count);
    uint64_t c8 = 0, e0 = 0;
    uint16_t w198 = 0;
    SafeRead(anim + 0xC8, &c8, 8);
    SafeRead(anim + 0xE0, &e0, 8);
    SafeRead(anim + 0x198, &w198, 2);
    Log(L("[Anim] +0xC8=%llu  +0xE0=%llu  (Summe = Anzahl kopierter Matrizen)  +0x198=%u\n", "[Anim] +0xC8=%llu  +0xE0=%llu  (sum = number of copied matrices)  +0x198=%u\n"),
        (unsigned long long)c8, (unsigned long long)e0, w198);
    for (uintptr_t i = 0; i < count && i < 3; ++i) {
        float m[16] = {};
        SafeRead(data + i * 0x40, m, sizeof(m));
        Log(L("  Knochen %llu: [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f]\n", "  Bone %llu: [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f]\n"),
            (unsigned long long)i, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    // Klassennamen (RTTI) der beteiligten Objekte
    auto rttiName = [](uintptr_t objAddr) -> std::string {
        uintptr_t vt = 0, col = 0;
        uint32_t td = 0;
        char name[96] = {};
        if (!SafeRead(objAddr, &vt, 8) || !SafeRead(vt - 8, &col, 8) || !SafeRead(col + 0x0C, &td, 4) ||
            !SafeRead(g_base + td + 0x10, name, sizeof(name) - 1))
            return "?";
        return name;
    };
    const uintptr_t model = g_lastPlayerModel;
    uintptr_t b8 = 0;
    SafeRead(model + 0xB8, &b8, 8);
    Log(L("[RTTI] anim: %s | model: %s | model+0xB8: %s\n", "[RTTI] anim: %s | model: %s | model+0xB8: %s\n"), rttiName(anim).c_str(), rttiName(model).c_str(), rttiName(b8).c_str());
    if (obj) {
        uintptr_t p110 = 0, p118 = 0;
        SafeRead(obj + 0x110, &p110, 8);
        SafeRead(obj + 0x118, &p118, 8);
        Log(L("[RTTI] Spieler+0x110 = 0x%llX (%s) | Spieler+0x118 = 0x%llX (%s)\n", "[RTTI] player+0x110 = 0x%llX (%s) | player+0x118 = 0x%llX (%s)\n"), (unsigned long long)p110,
            p110 ? rttiName(p110).c_str() : "-", (unsigned long long)p118, p118 ? rttiName(p118).c_str() : "-");
        if (p118) {
            uint8_t raw[0x60] = {};
            SafeRead(p118, raw, sizeof(raw));
            Log(L("  +0x118-Objekt roh: ", "  +0x118 object raw: "));
            for (int i = 0; i < 0x60; i += 8) Log(L("%016llX ", "%016llX "), *reinterpret_cast<unsigned long long*>(raw + i));
            Log(L("\n", "\n"));
            // Zeiger im Objekt verfolgen und die ersten Werte als float zeigen (Suche nach Weltkoordinaten)
            for (int i = 0x10; i < 0x60; i += 8) {
                const uintptr_t p = *reinterpret_cast<uintptr_t*>(raw + i);
                if (p < 0x10000 || (p >> 47)) continue;
                float f[12] = {};
                if (!SafeRead(p, f, sizeof(f))) continue;
                Log(L("  [+0x%02X] -> 0x%llX (%s): ", "  [+0x%02X] -> 0x%llX (%s): "), i, (unsigned long long)p, rttiName(p).c_str());
                for (float x : f) Log(L("%.2f ", "%.2f "), x);
                Log(L("\n", "\n"));
            }
        }
    }
    // Modell-Daten, die AddSkin pro Frame kopiert (model+0x90 Zeiger, model+0x98 Anzahl 4-Byte-Werte)
    uintptr_t mdata = 0, mcount = 0, m88 = 0, mb8 = 0;
    SafeRead(model + 0x90, &mdata, 8);
    SafeRead(model + 0x98, &mcount, 8);
    SafeRead(model + 0x88, &m88, 8);
    SafeRead(model + 0xB8, &mb8, 8);
    Log(L("[Modell] +0x88=0x%llX  +0x90=0x%llX  +0x98=%llu  +0xB8=0x%llX\n", "[Model] +0x88=0x%llX  +0x90=0x%llX  +0x98=%llu  +0xB8=0x%llX\n"), (unsigned long long)m88,
        (unsigned long long)mdata, (unsigned long long)mcount, (unsigned long long)mb8);
    if (mdata) {
        float v[24] = {};
        SafeRead(mdata, v, sizeof(v));
        Log(L("  erste Werte als float: ", "  first values as float: "));
        for (float x : v) Log(L("%.2f ", "%.2f "), x);
        uint32_t u[12] = {};
        SafeRead(mdata, u, sizeof(u));
        Log(L("\n  erste Werte als hex:   ", "\n  first values as hex:   "));
        for (uint32_t x : u) Log(L("%08X ", "%08X "), x);
        Log(L("\n", "\n"));
    }
}

void HookPlayerVtable() {
    auto* vt = reinterpret_cast<uintptr_t*>(g_base + kPlayerVtableRva);
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) { Log(L("Hook: VirtualAlloc fehlgeschlagen\n", "Hook: VirtualAlloc failed\n")); return; }
    DWORD old;
    VirtualProtect(vt, sizeof(uintptr_t) * kVtableEntries, PAGE_READWRITE, &old);
    for (int i = 0; i < kVtableEntries; ++i) {
        uint8_t* t = code + i * 32;
        const uint64_t counter = reinterpret_cast<uint64_t>(&g_vcalls[i]);
        const uint64_t orig = vt[i];
        uint8_t* p = t;
        *p++ = 0x48; *p++ = 0xB8; std::memcpy(p, &counter, 8); p += 8;   // mov rax, imm64
        *p++ = 0xF0; *p++ = 0x48; *p++ = 0xFF; *p++ = 0x00;                // lock inc qword [rax]
        *p++ = 0x48; *p++ = 0xB8; std::memcpy(p, &orig, 8); p += 8;      // mov rax, imm64
        *p++ = 0xFF; *p++ = 0xE0;                                           // jmp rax
        vt[i] = reinterpret_cast<uintptr_t>(t);
    }
    VirtualProtect(vt, sizeof(uintptr_t) * kVtableEntries, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    Log(L("Hook: %d vtable-Eintraege von kexShadowManPlayer umgeleitet.\n", "Hook: %d vtable entries of kexShadowManPlayer redirected.\n"), kVtableEntries);
}

void LogVcallRates() {
    static LONG64 last[kVtableEntries];
    static DWORD lastTick = GetTickCount();
    const DWORD now = GetTickCount();
    const double secs = (now - lastTick) / 1000.0;
    if (secs < 5.0) return;
    char buf[256];
    static LONG64 lastSkinP = 0, lastSkinO = 0;
    Log(L("[AddSkin/s] Spieler=%.1f  andere=%.1f\n", "[AddSkin/s] player=%.1f  others=%.1f\n"), (g_skinCallsPlayer - lastSkinP) / secs, (g_skinCallsOther - lastSkinO) / secs);
    lastSkinP = g_skinCallsPlayer;
    lastSkinO = g_skinCallsOther;
    int n = std::snprintf(buf, sizeof(buf), "[Zaehler/s]");
    for (int i = 0; i < kVtableEntries; ++i) {
        const LONG64 c = g_vcalls[i];
        n += std::snprintf(buf + n, sizeof(buf) - n, "  v%d=%.1f", i, (c - last[i]) / secs);
        last[i] = c;
    }
    Log(L("%s\n", "%s\n"), buf);
    lastTick = now;
}

// --- Phase 1: Geist zeichnen ---
// 0x497350 traegt eine animierte Figur in die Renderliste ein:
//   void AddSkin(RenderList* list, Object* obj, void* model, AnimComponent* anim)
// Wir rufen sie fuer den Spieler ein zweites Mal auf, mit kurz vertauschter Weltmatrix-Position.
constexpr uintptr_t kAddSkinRva = 0x497350;
constexpr size_t    kAddSkinPrologue = 17;  // mov rax,rsp / push rbp,r12-r15 / lea rbp,[rsp-80h]
using AddSkinFn = void (*)(void*, void*, void*, void*);
AddSkinFn g_addSkinOrig = nullptr;
std::atomic<bool> g_ghostOn{false};  // alte Geist-Experimente (Knochen verschieben) - aus

// Experiment-Konfiguration, live aus smcoop_ghost.txt gelesen (jede Sekunde):
//   offset <x> <y> <z>      Verschiebung des Geistes
//   pos <hex-offset>        Positions-Kopie im Spielerobjekt, die verschoben wird (mehrfach erlaubt)
struct GhostConfig {
    int streamDelayMs = -1;  // >= 0: Spieler 2 spielt aufgezeichnete Knochen von Spieler 1 mit dieser Verzoegerung ab
    int mode = 0;  // 0 = normal + Geist, 1 = nur verschoben, 2 = erst Geist, dann normal, 3 = Spieler nicht zeichnen
    float offset[3] = {0.0f, 0.0f, 0.0f};  // Versatz von Spieler 2 (nur fuer Tests; im Netzwerkspiel 0)
    int numPos = 0;
    uint32_t pos[32] = {};
};
GhostConfig g_ghostCfg[2];
std::atomic<int> g_ghostCfgIdx{0};

void LoadGhostConfig() {
    FILE* f = std::fopen("smcoop_ghost.txt", "r");
    if (!f) return;
    GhostConfig c;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        float x, y, z;
        unsigned off;
        int mode, delay;
        if (std::sscanf(line, "mode %d", &mode) == 1) c.mode = mode;
        else if (std::sscanf(line, "stream %d", &delay) == 1) c.streamDelayMs = delay;
        else if (std::sscanf(line, "offset %f %f %f", &x, &y, &z) == 3) { c.offset[0] = x; c.offset[1] = y; c.offset[2] = z; }
        else if (std::sscanf(line, "pos %x", &off) == 1 && c.numPos < 32) c.pos[c.numPos++] = off;
    }
    std::fclose(f);
    const int next = 1 - g_ghostCfgIdx.load();
    g_ghostCfg[next] = c;
    g_ghostCfgIdx = next;
}

// --- Phase 1b: zweiter, echter Spieler ---
// Create (0x458450) baut ein neues kexShadowManPlayer-Objekt, Init (0x4596B0) laedt Animationen und Modell.
// Der Konstruktor meldet sich beim Cutscene-Manager als Darsteller 0 und 1 an; das setzen wir zurueck.
constexpr uintptr_t kPlayerCreateRva = 0x458450;
constexpr uintptr_t kPlayerInitRva   = 0x4596B0;
constexpr uintptr_t kCutsceneMgrRva  = 0x2AAAF0;
// Bis zu drei Mitspieler (net::kMaxPeers): je Platz eine eigene Figur ("Spieler 2" .. "Spieler 4")
constexpr int kMaxRemote = net::kMaxPeers;
std::atomic<int> g_spawnRequest{0};                // 1 + Platz = Figur angefordert, -1 = wird gerade erzeugt, 0 = nichts
std::atomic<uintptr_t> g_avatar[kMaxRemote];       // Figur je Mitspieler-Platz
std::atomic<uintptr_t> g_avatarOwner[kMaxRemote];  // g_player beim Erzeugen; aendert er sich (Laden), wird die Figur verworfen

// Alle je erzeugten Spieler-2-Objekte. Verworfene werden (noch) nicht freigegeben und bleiben beim Spiel
// fuer "OnGameLoad" angemeldet - den Aufruf fangen wir fuer sie ab (siehe HookedOnGameLoad).
constexpr int kMaxPlayer2 = 64;
std::atomic<uintptr_t> g_allPlayer2[kMaxPlayer2];
std::atomic<int> g_numPlayer2{0};

bool IsOurPlayer2(uintptr_t p) {
    const int n = g_numPlayer2;
    for (int i = 0; i < n && i < kMaxPlayer2; ++i)
        if (g_allPlayer2[i] == p) return true;
    return false;
}

// Platz der aktuellen Figur p (-1 = keine aktuelle Mitspieler-Figur)
int AvatarSlot(uintptr_t p) {
    if (!p) return -1;
    for (int i = 0; i < kMaxRemote; ++i)
        if (g_avatar[i] == p) return i;
    return -1;
}

// Nur mit F9 (Test ohne Netz) erzeugte Figur: spiegelt den eigenen Spieler. Ohne diesen Schalter liefen Figuren, die nach dem
// Gehen des letzten Mitspielers uebrig waren, durch den Testweg (Knochen-Update auf einer Figur aus dem alten Level).
std::atomic<bool> g_localTestFigure{false};

// Spielerobjekt wird abgebaut (Destruktor 0x458D00; die Speicherverwaltung ruft ihn beim Level- oder Spielstandwechsel
// ueber 0x469070 auf, nicht ueber vtable[1]): Figuren, die an ihm haengen oder es selbst sind, sofort verwerfen.
// g_avatarOwner allein reicht nicht - das neue Spielerobjekt liegt oft an derselben Adresse (Log 23.10.: 0xDC11A070 in
// Level 6 und 14), die alte Figur wurde weiter gezeichnet, ihr Modell war schon freigegeben -> Absturz im Renderer
// (0x34F482, Material-Feld leer).
constexpr uintptr_t kPlayerDtorRva = 0x458D00;
using PlayerDtorFn = void (*)(void*);
PlayerDtorFn g_playerDtorOrig = nullptr;

void HookedPlayerDtor(void* self) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(self);
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        if (g_avatar[pi] != p && g_avatarOwner[pi] != p) continue;
        Log(L("[Spieler2] Spielerobjekt 0x%llX abgebaut - Figur %d verworfen\n", "[Player2] player object 0x%llX destroyed - figure %d dropped\n"), static_cast<unsigned long long>(p), pi + 1);
        g_avatar[pi] = 0;
        g_avatarOwner[pi] = 0;
    }
    const int n = g_numPlayer2;
    for (int i = 0; i < n && i < kMaxPlayer2; ++i)
        if (g_allPlayer2[i] == p) g_allPlayer2[i] = 0;  // die Adresse kann das naechste echte Spielerobjekt bekommen
    g_playerDtorOrig(self);
}

void* InstallInlineHook(uintptr_t target, size_t prologueLen, void* detour);

void HookPlayerDtor() {
    // push rdi / sub rsp,30h / mov qword [rsp+20h],-2
    static const uint8_t expected[15] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kPlayerDtorRva), expected, sizeof(expected)) == 0)
        g_playerDtorOrig = reinterpret_cast<PlayerDtorFn>(InstallInlineHook(g_base + kPlayerDtorRva, sizeof(expected), reinterpret_cast<void*>(HookedPlayerDtor)));
    Log(L("Hook Spieler-Abbau: %s\n", "Hook player destruction: %s\n"), g_playerDtorOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// OnGameLoad (0x461190): vom Spieler-Konstruktor als Ereignis "OnGameLoad" angemeldet. Meldet den Spieler
// erneut als Cutscene-Darsteller an und raeumt Unterobjekte auf -> fuer Spieler 2 falsch (Absturz beim Laden).
constexpr uintptr_t kOnGameLoadRva = 0x461190;
using OnGameLoadFn = void (*)(void*);
OnGameLoadFn g_onGameLoadOrig = nullptr;

void HookedOnGameLoad(void* player) {
    if (IsOurPlayer2(reinterpret_cast<uintptr_t>(player))) return;
    g_onGameLoadOrig(player);
}

// Die ersten 14 Bytes von OnGameLoad enthalten einen relativen call (E8 rel32), der so nicht verschoben werden
// kann. Das Trampolin kopiert die 9 Bytes davor und ersetzt den call durch "mov rax, ziel; call rax".
void HookOnGameLoad() {
    const uintptr_t target = g_base + kOnGameLoadRva;
    static const uint8_t expected[10] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8};
    if (std::memcmp(reinterpret_cast<void*>(target), expected, sizeof(expected)) != 0) {
        Log(L("Hook OnGameLoad: unerwartete Bytes - uebersprungen\n", "Hook OnGameLoad: unexpected bytes - skipped\n"));
        return;
    }
    int32_t rel;
    std::memcpy(&rel, reinterpret_cast<void*>(target + 10), 4);
    const uint64_t callTarget = target + 14 + rel;
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return;
    uint8_t* p = t;
    std::memcpy(p, reinterpret_cast<void*>(target), 9); p += 9;            // push rbx; sub rsp,20h; mov rbx,rcx
    *p++ = 0x48; *p++ = 0xB8; std::memcpy(p, &callTarget, 8); p += 8;     // mov rax, ziel
    *p++ = 0xFF; *p++ = 0xD0;                                              // call rax
    const uint64_t back = target + 14;
    *p++ = 0xFF; *p++ = 0x25; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; std::memcpy(p, &back, 8);  // jmp zurueck
    g_onGameLoadOrig = reinterpret_cast<OnGameLoadFn>(t);
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(target), 14, PAGE_EXECUTE_READWRITE, &old);
    auto* code = reinterpret_cast<uint8_t*>(target);
    const uint64_t dst = reinterpret_cast<uint64_t>(&HookedOnGameLoad);
    code[0] = 0xFF; code[1] = 0x25; code[2] = 0; code[3] = 0; code[4] = 0; code[5] = 0; std::memcpy(code + 6, &dst, 8);
    VirtualProtect(reinterpret_cast<void*>(target), 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 14);
    Log(L("Hook OnGameLoad: aktiv\n", "Hook OnGameLoad: active\n"));
}
float g_player2Offset[3] = {0.0f, 0.0f, 120.0f};

void PlacePlayer2(uintptr_t p1, uintptr_t p2) {
    // Positions-/Matrix-Kopien des Spielers uebernehmen und verschieben
    static const uint32_t kMatrixBlocks[] = {0x50, 0x200};       // 4x4-Matrizen, Position in Zeile 4 (+0x30)
    static const uint32_t kPosCopies[] = {0x20, 0x1F0, 0x508, 0x540, 0x574, 0x580, 0x5BC};
    for (uint32_t m : kMatrixBlocks) {
        std::memcpy(reinterpret_cast<void*>(p2 + m), reinterpret_cast<void*>(p1 + m), 0x40);
        float* t = reinterpret_cast<float*>(p2 + m + 0x30);
        for (int i = 0; i < 3; ++i) t[i] += g_player2Offset[i];
    }
    for (uint32_t o : kPosCopies) {
        float* d = reinterpret_cast<float*>(p2 + o);
        const float* s = reinterpret_cast<const float*>(p1 + o);
        for (int i = 0; i < 3; ++i) d[i] = s[i] + g_player2Offset[i];
    }
    std::memcpy(reinterpret_cast<void*>(p2 + 0x18), reinterpret_cast<void*>(p1 + 0x18), 4);  // Blickrichtung
}

extern std::atomic<uintptr_t> g_spawningAvatar;
// Laeuft im Spiel-Thread (aus dem AddSkin-Hook). Gibt false zurueck, wenn eine Ausnahme auftrat.
bool SpawnPlayer2Guarded(uintptr_t p1, uintptr_t* out) {
    __try {
        using CreateFn = void* (*)();
        using InitFn = void (*)(void*);
        using MgrFn = void* (*)();
        using SlotFn = void** (*)(void*, int);
        void* mgr = reinterpret_cast<MgrFn>(g_base + kCutsceneMgrRva)();
        auto slotFn = reinterpret_cast<SlotFn>((*reinterpret_cast<uintptr_t**>(mgr))[9]);
        void* s0 = *slotFn(mgr, 0);
        void* s1 = *slotFn(mgr, 1);
        void* p2 = reinterpret_cast<CreateFn>(g_base + kPlayerCreateRva)();
        *slotFn(mgr, 0) = s0;
        *slotFn(mgr, 1) = s1;
        if (!p2) return false;
        g_spawningAvatar = reinterpret_cast<uintptr_t>(p2);  // Init startet keine Spiel-Animationen auf der Figur
        reinterpret_cast<InitFn>(g_base + kPlayerInitRva)(p2);
        g_spawningAvatar = 0;
        *out = reinterpret_cast<uintptr_t>(p2);
        PlacePlayer2(p1, *out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 0x45EEA0(player): Knochen fuer Koerper (+0x368) und beide Haende (+0x1D1D0, +0x1D410) berechnen.
// Das Spiel ruft das jeden Frame fuer den echten Spieler auf (aus dem Spieler-Subsystem 0x458530).
constexpr uintptr_t kPlayerUpdateBonesRva = 0x45EEA0;

// Animationszustand von Spieler 1 auf Spieler 2 spiegeln (Test, spaeter kommen die Daten vom Netzwerk).
// Animationskomponente (player+0x368):
//   +0x00..+0x27  Abspielzustand (Bild, Geschwindigkeit, Status)
//   +0x68..+0x7F  aktuelle Animation (Bild, Nummer)
//   +0xC0/+0xC8   Gelenkliste A (Zeiger/Anzahl), Eintrag 0x180 Bytes
//   +0xD8/+0xE0   Gelenkliste B (Zeiger/Anzahl), Eintrag 0x40 Bytes
//   +0x198        Platz im GPU-Knochenpuffer (sbBoneTransforms), von UpdateBones direkt beschrieben
//   +0x1A0..      Welt-Transformation (Position +0x1A0, Vorframe-Position +0x1D8); +0x1D0 = Zeiger auf Besitzer-Matrix
bool MirrorAnimGuarded(uintptr_t p1, uintptr_t p2) {
    __try {
        const uintptr_t a1 = p1 + 0x368, a2 = p2 + 0x368;
        std::memcpy(reinterpret_cast<void*>(a2 + 0x00), reinterpret_cast<void*>(a1 + 0x00), 0x28);
        std::memcpy(reinterpret_cast<void*>(a2 + 0x68), reinterpret_cast<void*>(a1 + 0x68), 0x18);
        struct { uint32_t off; size_t stride; } lists[] = {{0xC0, 0x180}, {0xD8, 0x40}};
        for (const auto& l : lists) {
            const uintptr_t src = *reinterpret_cast<uintptr_t*>(a1 + l.off);
            const uintptr_t dst = *reinterpret_cast<uintptr_t*>(a2 + l.off);
            const size_t n1 = *reinterpret_cast<size_t*>(a1 + l.off + 8);
            const size_t n2 = *reinterpret_cast<size_t*>(a2 + l.off + 8);
            if (src && dst && n1 == n2) std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), n1 * l.stride);
        }
        // CPU-Kopie der Knochen (+0x180, je 0x40 Bytes: Quaternion, Position, Vorframe-Quaternion, Vorframe-Position).
        // Die GPU-Knochen schreibt UpdateBones direkt in den Knochenpuffer; diese Kopie nutzt das Spiel u.a. fuer den
        // Begrenzungsrahmen (Sichtbarkeitstest). Ohne sie wird Spieler 2 am Weltnullpunkt vermutet und aussortiert.
        {
            float* src = *reinterpret_cast<float**>(a1 + 0x180);
            float* dst = *reinterpret_cast<float**>(a2 + 0x180);
            const size_t n1 = *reinterpret_cast<size_t*>(a1 + 0x188);
            const size_t n2 = *reinterpret_cast<size_t*>(a2 + 0x188);
            if (src && dst && n1 == n2 && n1 <= 128) {
                std::memcpy(dst, src, n1 * 0x40);
                for (size_t b = 0; b < n1; ++b)
                    for (int i = 0; i < 3; ++i) { dst[b * 16 + 4 + i] += g_player2Offset[i]; dst[b * 16 + 12 + i] += g_player2Offset[i]; }
            }
        }
        std::memcpy(reinterpret_cast<void*>(a2 + 0x1A0), reinterpret_cast<void*>(a1 + 0x1A0), 0x30);
        std::memcpy(reinterpret_cast<void*>(a2 + 0x1D8), reinterpret_cast<void*>(a1 + 0x1D8), 0x28);
        float* posA = reinterpret_cast<float*>(a2 + 0x1A0);
        float* posB = reinterpret_cast<float*>(a2 + 0x1D8);
        for (int i = 0; i < 3; ++i) { posA[i] += g_player2Offset[i]; posB[i] += g_player2Offset[i]; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Analyse-Werkzeug: Einzelschritt-Tracer ---
// Fuehrt eine Funktion mit gesetztem Trap-Flag aus und merkt sich alle Sprungziele innerhalb [lo, hi).
constexpr int kMaxTrace = 600;
uintptr_t g_trace[kMaxTrace];
volatile LONG g_traceCount = 0;
volatile LONG g_traceSteps = 0;
DWORD g_traceThread = 0;
uintptr_t g_traceLo = 0, g_traceHi = 0, g_traceLast = 0, g_selfLo = 0, g_selfHi = 0;
std::atomic<bool> g_tracing{false};
bool g_traceEntered = false;
std::atomic<int> g_traceRequest{0};

LONG CALLBACK TraceHandler(EXCEPTION_POINTERS* ep) {
    if (!g_tracing || ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || GetCurrentThreadId() != g_traceThread)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = ep->ContextRecord->Rip;
    const bool inSelf = rip >= g_selfLo && rip < g_selfHi;
    if (!inSelf) g_traceEntered = true;
    if (inSelf && g_traceEntered) {  // nach dem Spielcode zurueck in unserem Mod -> fertig
        g_tracing = false;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rip >= g_traceLo && rip < g_traceHi) {
        // nur "Spruenge" merken: Ziel liegt nicht direkt hinter der vorigen Instruktion
        if (g_traceLast == 0 || rip < g_traceLast || rip > g_traceLast + 15) {
            const LONG i = g_traceCount;
            if (i < kMaxTrace) { g_trace[i] = rip; g_traceCount = i + 1; }
        }
        g_traceLast = rip;
    }
    if (InterlockedIncrement(&g_traceSteps) > 3000000) { g_tracing = false; return EXCEPTION_CONTINUE_EXECUTION; }
    ep->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void TracedCall(void (*fn)(void*), void* arg, uintptr_t lo, uintptr_t hi) {
    static void* veh = AddVectoredExceptionHandler(1, TraceHandler);
    (void)veh;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&TracedCall), &self);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(self) + dos->e_lfanew);
    g_selfLo = reinterpret_cast<uintptr_t>(self);
    g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;
    g_traceLo = lo; g_traceHi = hi; g_traceLast = 0; g_traceCount = 0; g_traceSteps = 0; g_traceEntered = false;
    g_traceThread = GetCurrentThreadId();
    g_tracing = true;
    __writeeflags(__readeflags() | 0x100);  // Trap-Flag: Ausnahme nach der naechsten Instruktion
    fn(arg);
    g_tracing = false;
}

void LogTrace(const char* label) {
    Log(L("[Trace %s] %ld Schritte, %ld Sprungziele:", "[Trace %s] %ld steps, %ld jump targets:"), label, (long)g_traceSteps, (long)g_traceCount);
    for (LONG i = 0; i < g_traceCount; ++i) {
        if (i % 12 == 0) Log(L("\n  ", "\n  "));
        Log(L(" %llX", " %llX"), (unsigned long long)(g_trace[i] - g_base));
    }
    Log(L("\n", "\n"));
}

bool UpdatePlayer2BonesGuarded(uintptr_t p2) {
    __try {
        auto fn = g_playerBonesOrig ? g_playerBonesOrig : reinterpret_cast<void (*)(void*)>(g_base + kPlayerUpdateBonesRva);
        fn(reinterpret_cast<void*>(p2));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DrawPlayer2Guarded(void* list, uintptr_t p2) {
    __try {
        void* mesh = *reinterpret_cast<void**>(p2 + 0x1D0E0);
        if (!mesh) return true;
        g_addSkinOrig(list, reinterpret_cast<void*>(p2), mesh, reinterpret_cast<void*>(p2 + 0x368));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Hook auf 0x45EEA0 (Spieler-Knochen). Das Spiel ruft ihn im Animations-Zeitfenster auf, in dem der
// GPU-Knochenpuffer (Zeiger bei 0xD8B6A8) gemappt ist. Nur dann landen Knochen auf der Grafikkarte.
// Direkt nach dem echten Spieler aktualisieren wir hier Spieler 2.
//
// "Pose" = alles, was man braucht, um eine Figur zu zeichnen: Weltmatrix (+0x200) und die fertigen Knochen
// (69 x 0x40: Quaternion, Position, Vorframe-Quaternion, Vorframe-Position). Genau das wird spaeter uebers
// Netzwerk geschickt. Fuer Tests ohne Netzwerk zeichnen wir die Posen von Spieler 1 auf und spielen sie verzoegert ab.
constexpr uintptr_t kGpuBoneBufferRva  = 0xD8B6A8;  // gemappter GPU-Knochenpuffer (nur im Animations-Zeitfenster gueltig)
constexpr uintptr_t kGpuBoneEnabledRva = 0xF9C234;  // == 1, wenn GPU-Knochen geschrieben werden
constexpr size_t    kMaxBones          = 128;

struct Pose {
    DWORD time;
    uint32_t numBones;
    float world[16];                 // Spieler +0x200 (Zeile 4 = Position)
    float bones[kMaxBones * 16];
};

constexpr int kPoseHistory = 1024;   // ca. 6 s bei 160 FPS
Pose* g_poseHistory = nullptr;
int g_poseHead = 0;                  // naechster Schreibplatz

void RecordPose(uintptr_t p1) {
    if (!g_poseHistory) g_poseHistory = static_cast<Pose*>(VirtualAlloc(nullptr, sizeof(Pose) * kPoseHistory, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_poseHistory) return;
    const uintptr_t a = p1 + 0x368;
    const float* bones = *reinterpret_cast<float**>(a + 0x180);
    const size_t n = *reinterpret_cast<size_t*>(a + 0x188);
    if (!bones || n == 0 || n > kMaxBones) return;
    Pose& p = g_poseHistory[g_poseHead];
    p.time = GetTickCount();
    p.numBones = static_cast<uint32_t>(n);
    std::memcpy(p.world, reinterpret_cast<void*>(p1 + 0x200), sizeof(p.world));
    std::memcpy(p.bones, bones, n * 0x40);
    g_poseHead = (g_poseHead + 1) % kPoseHistory;
}

// Juengste Pose, die mindestens delayMs alt ist
const Pose* FindDelayedPose(int delayMs) {
    if (!g_poseHistory) return nullptr;
    const DWORD now = GetTickCount();
    for (int i = 1; i <= kPoseHistory; ++i) {
        const Pose& p = g_poseHistory[(g_poseHead - i + kPoseHistory) % kPoseHistory];
        if (p.numBones == 0) return nullptr;
        if (now - p.time >= static_cast<DWORD>(delayMs)) return &p;
    }
    return nullptr;
}

// Pose auf Spieler 2 anwenden: Weltmatrix, CPU-Knochen (Begrenzungsrahmen) und GPU-Knochen (sein Platz im Puffer)
bool ApplyPoseGuarded(uintptr_t p2, const Pose& src, const float offset[3]) {
    // Kopie mit Versatz (nur fuer Tests; Netzwerk-Posen kommen spaeter ohne Versatz)
    static Pose pose;
    pose.time = src.time;
    pose.numBones = src.numBones;
    std::memcpy(pose.world, src.world, sizeof(pose.world));
    std::memcpy(pose.bones, src.bones, src.numBones * 0x40);
    for (int i = 0; i < 3; ++i) pose.world[12 + i] += offset[i];
    for (uint32_t b = 0; b < pose.numBones; ++b)
        for (int i = 0; i < 3; ++i) { pose.bones[b * 16 + 4 + i] += offset[i]; pose.bones[b * 16 + 12 + i] += offset[i]; }
    __try {
        const uintptr_t a = p2 + 0x368;
        std::memcpy(reinterpret_cast<void*>(p2 + 0x200), pose.world, sizeof(pose.world));
        std::memcpy(reinterpret_cast<void*>(p2 + 0x20), &pose.world[12], 12);
        std::memcpy(reinterpret_cast<void*>(p2 + 0x80), &pose.world[12], 12);
        float* cpu = *reinterpret_cast<float**>(a + 0x180);
        const size_t n = *reinterpret_cast<size_t*>(a + 0x188);
        if (!cpu || n != pose.numBones) return true;
        std::memcpy(cpu, pose.bones, n * 0x40);
        // GPU-Platz holen (UpdateBones vergibt ihn beim ersten Aufruf) und Knochen direkt hineinschreiben
        uint16_t slot = *reinterpret_cast<uint16_t*>(a + 0x198);
        if (slot == 0) { g_playerBonesOrig(reinterpret_cast<void*>(p2)); slot = *reinterpret_cast<uint16_t*>(a + 0x198); }
        uint8_t* gpu = *reinterpret_cast<uint8_t**>(g_base + kGpuBoneBufferRva);
        if (slot && gpu && *reinterpret_cast<int*>(g_base + kGpuBoneEnabledRva) == 1)
            std::memcpy(gpu - 0x2000 + static_cast<size_t>(slot) * 0x2000, pose.bones, n * 0x40);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------- Netzwerk: eigene Pose senden, Pose des Mitspielers abspielen ----------------
constexpr DWORD kSendIntervalMs   = 33;   // ca. 30 Pakete/s
constexpr DWORD kInterpDelayMs    = 100;  // Wiedergabe so weit hinter dem Sender (Puffer gegen Schwankungen)
constexpr DWORD kRemoteTimeoutMs  = 2000; // danach gilt der Mitspieler als weg
constexpr DWORD kHeartbeatMs      = 250;  // im Menue/Pause (keine neue Pose) letzte Pose so oft erneut senden

// Millisekunden-Uhr mit hoher Aufloesung (GetTickCount tickt nur alle ~16 ms -> sichtbares Zittern)
DWORD NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<DWORD>((c.QuadPart * 1000) / freq.QuadPart);
}

// ---------------- Gegenstaende in den Haenden (Waffen) ----------------
// Ausgeruestete Gegenstaende sind kexShadowManQuestObject-Objekte. Die Hand-Strukturen des Spielers (+0x1D198 links,
// +0x1D3D8 rechts) zeigen auf einen Knoten (+0x10 Gegenstandsdefinition, +0x20 Typ, +0x30 Objekt). Dessen Rueckruf
// 0x474510 setzt das Objekt jeden Frame an Gelenk 9 (links) bzw. 13 (rechts) und zeichnet es mit AddMesh(0xCD8030, obj).
// Der Violator (Typ 0x1E) hat einen eigenen Weg (0x474470, mehrteiliges Modell mit drehendem Lauf).
// Gesendet wird pro Hand der Typ und die Lage des Objekts relativ zum Handknochen. Der Empfaenger zeichnet eine eigene
// Kopie eines Gegenstandsobjekts mit passendem Modell an der Hand von Spieler 2 - nur Darstellung, dem Spiel unbekannt.
constexpr uintptr_t kAddMeshRva           = 0x4967B0;  // AddMesh(Liste, Objekt): starres Modell in die Renderliste
constexpr uintptr_t kMeshListRva          = 0xCD8030;
constexpr uintptr_t kHeldItemCallbackRva  = 0x474510;  // Rueckruf(Knoten): Gegenstand an die Hand setzen und zeichnen
constexpr uintptr_t kItemDefsRva          = 0xD92BD0;  // Gegenstandsdefinitionen, je 0xC0 Bytes (+0x20 Modellpfad)
constexpr int       kItemDefCount         = 0x295;
constexpr int       kViolatorType         = 0x1E;
constexpr uintptr_t kSetItemModelRva      = 0x2E8C70;  // (obj, Modellnummer, 1): setzt obj+0x108/+0x110/+0x118
constexpr uintptr_t kQuestObjectVtableRva = 0x722BD8;
constexpr size_t    kItemObjCopy          = 0x300;     // AddMesh liest bis +0x240
constexpr int       kHandBone[posecodec::kNumHands] = {9, 13};
constexpr DWORD     kHeldItemTimeoutMs    = 150;       // so lange nicht gezeichnet -> Hand gilt als leer
// Aufladen der Shadowgun: Zeit je Hand (float, 0..3) bei 0xF62EE8 / 0xF62EEC. Der Rueckruf zeichnet das Leuchten mit
// 0x473A90(-, Handgelenk-Matrix, Winkel, Versatz, Stufe); Winkel/Versatz sind Konstanten je Hand.
constexpr uintptr_t kHandChargeRva        = 0xF62EE8;
constexpr uintptr_t kChargeFxRva          = 0x473A90;

using AddMeshFn = uint64_t (*)(void*, void*, void*, void*);
AddMeshFn g_addMeshOrig = nullptr;
using HeldItemFn = void (*)(void*);
HeldItemFn g_heldItemOrig = nullptr;

// Waehrend des Rueckrufs: welches Objekt er bearbeitet und ob AddMesh dafuer aufgerufen wurde (Spiel-Thread)
uintptr_t g_curHeldObj = 0;
bool g_curHeldDrawn = false;

SRWLOCK g_handsLock = SRWLOCK_INIT;
posecodec::HandItem g_localHands[posecodec::kNumHands];
DWORD g_localHandTime[posecodec::kNumHands] = {};
posecodec::HandItem g_avatarHands[kMaxRemote][posecodec::kNumHands];  // zuletzt angewendete Hand-Daten je Mitspieler

// Vorlage fuer die eigenen Objektkopien: Schnappschuss eines echten Gegenstandsobjekts (bevorzugt aus der Hand)
alignas(16) uint8_t g_itemTemplate[kItemObjCopy];
uintptr_t g_itemTemplateOwner = 0;  // g_player, zu dem die Vorlage gehoert (nach dem Laden neu holen)
bool g_itemTemplateFromHand = false;

void NoteItemTemplate(uintptr_t obj, bool fromHand) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (!gp || (g_itemTemplateOwner == gp && (g_itemTemplateFromHand || !fromHand))) return;
    if (!SafeRead(obj, g_itemTemplate, kItemObjCopy)) return;
    g_itemTemplateOwner = gp;
    g_itemTemplateFromHand = fromHand;
}

const char* ItemName(int type) {
    if (type < 0 || type >= kItemDefCount) return "-";
    const char* path = *reinterpret_cast<const char**>(g_base + kItemDefsRva + static_cast<size_t>(type) * 0xC0 + 0x20);
    if (!path) return "?";
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Knochen k als 4x4-Matrix (Zeilen X/Y/Z-Achse, Position) - gleiche Werte wie Gelenkliste A (+0x428)
bool BoneMatrix(uintptr_t player, int k, float m[16]) {
    const uintptr_t a = player + 0x368;
    const float* bones = *reinterpret_cast<float**>(a + 0x180);
    const size_t n = *reinterpret_cast<size_t*>(a + 0x188);
    if (!bones || static_cast<size_t>(k) >= n) return false;
    posecodec::QuatToMatrix(bones + k * 16, m);
    m[12] = bones[k * 16 + 4]; m[13] = bones[k * 16 + 5]; m[14] = bones[k * 16 + 6]; m[15] = 1.0f;
    return true;
}

// rel = W * B^-1 (Zeilenvektoren, B orthonormal); rel: Achsen X/Y/Z und Position, je 3 floats
void RelativeTo(const float* W, const float* B, float rel[12]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rel[i * 3 + j] = W[i * 4] * B[j * 4] + W[i * 4 + 1] * B[j * 4 + 1] + W[i * 4 + 2] * B[j * 4 + 2];
    const float d[3] = {W[12] - B[12], W[13] - B[13], W[14] - B[14]};
    for (int j = 0; j < 3; ++j) rel[9 + j] = d[0] * B[j * 4] + d[1] * B[j * 4 + 1] + d[2] * B[j * 4 + 2];
}

// W = rel * B
void ApplyRelative(const float rel[12], const float* B, float W[16]) {
    for (int i = 0; i < 4; ++i) {
        const float* r = rel + i * 3;
        for (int j = 0; j < 3; ++j) W[i * 4 + j] = r[0] * B[j] + r[1] * B[4 + j] + r[2] * B[8 + j] + (i == 3 ? B[12 + j] : 0.0f);
        W[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
    }
}

// Lokalen Begrenzungsrahmen (min xyz, max xyz) mit W in die Welt bringen
void TransformBox(const float lb[6], const float* W, float out[6]) {
    for (int i = 0; i < 3; ++i) { out[i] = 3.4e38f; out[3 + i] = -3.4e38f; }
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {lb[(c & 1) ? 3 : 0], lb[(c & 2) ? 4 : 1], lb[(c & 4) ? 5 : 2]};
        for (int j = 0; j < 3; ++j) {
            const float v = p[0] * W[j] + p[1] * W[4 + j] + p[2] * W[8 + j] + W[12 + j];
            if (v < out[j]) out[j] = v;
            if (v > out[3 + j]) out[3 + j] = v;
        }
    }
}

// Sender: Lage eines gerade an die Hand gesetzten Gegenstands relativ zum Handknochen merken
void NoteLocalHeldItem(uintptr_t obj, int type) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    int hand = -1;
    if (!gp || !SafeRead(obj + 0x120, &hand, 4) || hand < 0 || hand >= posecodec::kNumHands) return;
    float B[16], W[16];
    if (!BoneMatrix(gp, kHandBone[hand], B) || !SafeRead(obj + 0x200, W, sizeof(W))) return;
    posecodec::HandItem hi;
    hi.type = static_cast<int16_t>(type);
    RelativeTo(W, B, hi.rel);
    SafeRead(g_base + kHandChargeRva + hand * 4, &hi.charge, 4);
    AcquireSRWLockExclusive(&g_handsLock);
    g_localHands[hand] = hi;
    g_localHandTime[hand] = NowMs();
    ReleaseSRWLockExclusive(&g_handsLock);
    NoteItemTemplate(obj, true);
}

void HookedHeldItem(void* node) {
    const uintptr_t n = reinterpret_cast<uintptr_t>(node);
    uintptr_t obj = 0;
    int type = -1;
    SafeRead(n + 0x30, &obj, 8);
    SafeRead(n + 0x20, &type, 4);
    g_curHeldObj = obj;
    g_curHeldDrawn = false;
    g_heldItemOrig(node);
    g_curHeldObj = 0;
    if (!obj) return;
    bool shown = g_curHeldDrawn;
    if (!shown && type == kViolatorType) {  // eigener Zeichenweg ohne AddMesh: "nicht versteckt" genuegt
        uint8_t hidden = 1;
        SafeRead(obj + 0x124, &hidden, 1);
        shown = hidden == 0;
    }
    if (shown) NoteLocalHeldItem(obj, type);
}

void HookHeldItem() {
    static const uint8_t expected[15] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kHeldItemCallbackRva), expected, sizeof(expected)) != 0) {
        Log(L("Hook Waffen-Rueckruf: unerwartete Bytes - uebersprungen\n", "Hook weapon callback: unexpected bytes - skipped\n"));
        return;
    }
    g_heldItemOrig = reinterpret_cast<HeldItemFn>(InstallInlineHook(g_base + kHeldItemCallbackRva, sizeof(expected), reinterpret_cast<void*>(HookedHeldItem)));
    Log(L("Hook Waffen-Rueckruf: %s\n", "Hook weapon callback: %s\n"), g_heldItemOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// Empfaenger: Modell je Gegenstandstyp (einmal ueber die Spielfunktion 0x2E8C70 auf einer Objektkopie geladen)
struct ItemModel { uintptr_t r108, r110, geom; bool tried; };
ItemModel g_itemModels[kItemDefCount];
uintptr_t g_itemModelsOwner = 0;

bool LoadItemModelGuarded(int modelId, ItemModel& m) {
    alignas(16) static uint8_t scratch[kItemObjCopy];
    std::memcpy(scratch, g_itemTemplate, kItemObjCopy);
    // Zeiger der Vorlage entfernen, sonst gibt die Funktion deren Modell frei
    *reinterpret_cast<uintptr_t*>(scratch + 0x108) = 0;
    *reinterpret_cast<uintptr_t*>(scratch + 0x110) = 0;
    *reinterpret_cast<uintptr_t*>(scratch + 0x118) = 0;
    __try {
        reinterpret_cast<char (*)(void*, int, char)>(g_base + kSetItemModelRva)(scratch, modelId, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    m.r108 = *reinterpret_cast<uintptr_t*>(scratch + 0x108);
    m.r110 = *reinterpret_cast<uintptr_t*>(scratch + 0x110);
    m.geom = *reinterpret_cast<uintptr_t*>(scratch + 0x118);
    return m.geom != 0;
}

const ItemModel* GetItemModel(int type) {
    if (type < 0 || type >= kItemDefCount) return nullptr;
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (g_itemModelsOwner != gp) {  // neues Level / neu geladen: Ressourcen koennen weg sein
        std::memset(g_itemModels, 0, sizeof(g_itemModels));
        g_itemModelsOwner = gp;
    }
    ItemModel& m = g_itemModels[type];
    if (!m.tried) {
        m.tried = true;
        const bool ok = LoadItemModelGuarded(type, m);
        Log(L("[Waffen] Modell %d (%s) fuer Spieler 2 %s\n", "[Weapons] model %d (%s) for player 2 %s\n"), type, ItemName(type), ok ? L("geladen", "loaded") : L("NICHT geladen", "NOT loaded"));
        if (!ok) m.geom = 0;
    }
    return m.geom ? &m : nullptr;
}

// je Mitspieler eigene Objektkopien (die Renderliste haelt die Zeiger bis zum Zeichnen)
alignas(16) uint8_t g_itemFake[kMaxRemote][posecodec::kNumHands][0x400];
alignas(16) uint8_t g_itemFakeNode[kMaxRemote][posecodec::kNumHands][0x40];
float g_itemPrev[kMaxRemote][posecodec::kNumHands][16];
int g_itemPrevType[kMaxRemote][posecodec::kNumHands] = {{-1, -1}, {-1, -1}, {-1, -1}};
static_assert(kMaxRemote == 3, "g_itemPrevType-Vorbelegung anpassen");

bool DrawChargeGuarded(int hand, const float* B, float charge) {
    const float angle = hand == 0 ? *reinterpret_cast<const float*>(g_base + 0x72C6A4) : 0.0f;
    const float offset[3] = {*reinterpret_cast<const float*>(g_base + 0x72CA1C), 0.0f,
                             *reinterpret_cast<const float*>(g_base + (hand == 0 ? 0x72D178 : 0x72C940))};
    alignas(16) float m[16];
    std::memcpy(m, B, sizeof(m));
    __try {
        reinterpret_cast<void (*)(void*, const float*, float, const float*, int)>(g_base + kChargeFxRva)(nullptr, m, angle, offset,
                                                                                                       static_cast<int>(charge));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool AddMeshGuarded(void* obj) {
    __try {
        g_addMeshOrig(reinterpret_cast<void*>(g_base + kMeshListRva), obj, nullptr, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Im AddSkin-Hook (Hauptansicht, ein Mal pro Frame) direkt nach der Figur des Mitspielers pi aufrufen
void DrawPlayer2Items(int pi, uintptr_t p2) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (!g_addMeshOrig || !gp || g_itemTemplateOwner != gp) return;  // noch keine Vorlage in diesem Level
    posecodec::HandItem hands[posecodec::kNumHands];
    AcquireSRWLockShared(&g_handsLock);
    std::memcpy(hands, g_avatarHands[pi], sizeof(hands));
    ReleaseSRWLockShared(&g_handsLock);
    for (int h = 0; h < posecodec::kNumHands; ++h) {
        const int type = hands[h].type;
        const ItemModel* m = type >= 0 ? GetItemModel(type) : nullptr;
        float B[16], W[16], lb[6];
        if (!m || !BoneMatrix(p2, kHandBone[h], B) || !SafeRead(m->r108 + 0xEC, lb, sizeof(lb))) {
            g_itemPrevType[pi][h] = -1;
            continue;
        }
        ApplyRelative(hands[h].rel, B, W);
        uint8_t* f = g_itemFake[pi][h];
        uint8_t* node = g_itemFakeNode[pi][h];
        std::memcpy(f, g_itemTemplate, kItemObjCopy);
        *reinterpret_cast<uintptr_t*>(f + 0x108) = m->r108;
        *reinterpret_cast<uintptr_t*>(f + 0x110) = m->r110;
        *reinterpret_cast<uintptr_t*>(f + 0x118) = m->geom;
        std::memcpy(f + 0x50, W, sizeof(W));
        std::memcpy(f + 0x200, W, sizeof(W));
        std::memcpy(f + 0x20, W + 12, 12);
        std::memcpy(f + 0x130, g_itemPrevType[pi][h] == type ? g_itemPrev[pi][h] : W, sizeof(W));  // Vorframe (Bewegungsunschaerfe)
        TransformBox(lb, W, reinterpret_cast<float*>(f + 0xD8));
        *reinterpret_cast<int*>(f + 0x120) = h;
        f[0x124] = 0;
        std::memset(node, 0, 0x40);
        *reinterpret_cast<uintptr_t*>(node + 0x10) = g_base + kItemDefsRva + static_cast<size_t>(type) * 0xC0;
        *reinterpret_cast<int*>(node + 0x20) = type;
        *reinterpret_cast<int*>(node + 0x28) = 4;
        *reinterpret_cast<uintptr_t*>(node + 0x30) = reinterpret_cast<uintptr_t>(f);
        *reinterpret_cast<uintptr_t*>(f + 0x1A8) = reinterpret_cast<uintptr_t>(node);
        if (!AddMeshGuarded(f)) {
            Log(L("[Waffen] Zeichnen von %s fehlgeschlagen - Modell gesperrt\n", "[Weapons] drawing %s failed - model blocked\n"), ItemName(type));
            g_itemModels[type].geom = 0;
            g_itemPrevType[pi][h] = -1;
            continue;
        }
        std::memcpy(g_itemPrev[pi][h], W, sizeof(W));
        g_itemPrevType[pi][h] = type;
        static bool chargeBroken = false;
        if (hands[h].charge > 0.0f && !chargeBroken && !DrawChargeGuarded(h, B, hands[h].charge)) {
            chargeBroken = true;
            Log(L("[Waffen] Aufladen zeichnen fehlgeschlagen - abgeschaltet\n", "[Weapons] drawing charge failed - disabled\n"));
        }
        // Kontrolle (alle 10 s): Lage der Kopie neben der echten Waffe derselben Hand (im Test mit sich selbst
        // muss die Differenz dem Versatz aus smcoop_ghost.txt entsprechen)
        static DWORD lastCheck[posecodec::kNumHands] = {};
        const DWORD now = NowMs();
        const float* testOffset = g_ghostCfg[g_ghostCfgIdx.load()].offset;
        const bool testMode = testOffset[0] != 0.0f || testOffset[1] != 0.0f || testOffset[2] != 0.0f;
        if (testMode && now - lastCheck[h] > 10000) {
            lastCheck[h] = now;
            uintptr_t node = 0, obj = 0;
            float own[16] = {};
            SafeRead(gp + (h == 0 ? 0x1D198 : 0x1D3D8), &node, 8);
            if (node) SafeRead(node + 0x30, &obj, 8);
            if (obj) SafeRead(obj + 0x200, own, sizeof(own));
            Log(L("[Waffen] Kontrolle Hand %d (%s): Kopie bei %.1f/%.1f/%.1f, eigene Waffe bei %.1f/%.1f/%.1f (Differenz %.1f/%.1f/%.1f)\n", "[Weapons] check hand %d (%s): copy at %.1f/%.1f/%.1f, own weapon at %.1f/%.1f/%.1f (difference %.1f/%.1f/%.1f)\n"),
                h, ItemName(type), W[12], W[13], W[14], own[12], own[13], own[14], W[12] - own[12], W[13] - own[13], W[14] - own[14]);
        }
    }
}

// Ausgehendes Paket (vom Spiel-Thread gefuellt, vom Probe-Thread verschickt)
SRWLOCK g_outLock = SRWLOCK_INIT;
uint8_t g_outPacket[posecodec::MaxPacketSize()];
size_t g_outSize = 0;
uint32_t g_outSeq = 0;
std::atomic<bool> g_outReady{false};

// Empfangene Posen je Mitspieler-Platz (vom Probe-Thread gefuellt, vom Spiel-Thread gelesen, g_remoteLock)
constexpr int kRemoteRing = 32;
SRWLOCK g_remoteLock = SRWLOCK_INIT;
struct RemoteRing {
    posecodec::NetPose poses[kRemoteRing];
    int count = 0, head = 0;
    DWORD lastRecv = 0;
    int64_t clockOffset = INT64_MAX;  // min(Empfangszeit - Sendezeit)
};
RemoteRing g_remote[kMaxRemote];
// Mitspieler im selben Level im Spiel (Netz-Thread schreibt)
std::atomic<bool> g_remoteActive[kMaxRemote];
// Mitspieler hat ein anderes Skelett als seine Figur hier (z.B. waehrend des Umbaus) -> ausblenden
std::atomic<bool> g_remoteModelMismatch[kMaxRemote];
bool AnyRemoteActive() {
    for (int i = 0; i < kMaxRemote; ++i)
        if (g_remoteActive[i]) return true;
    return false;
}
volatile LONG64 g_netSent = 0, g_netRecv = 0;

// ---------------- Schuesse ----------------
// Alle Geschosse (Spieler und Gegner) entstehen ueber 0x25EF10(Besitzer-Knoten, Ziel-Knoten, Typ, &Position,
// &Richtung, Sektor, hatZiel, ohneMunition). Ist der Besitzer der Knoten des eigenen Spielers (+0x1A8), merken wir den
// Schuss und schicken ihn mit den naechsten Paketen (mehrfach, gegen Verlust). Der Empfaenger erzeugt dasselbe Geschoss
// ohne Munitionsverbrauch mit Spieler 2 als Besitzer - zeitlich passend zur verzoegert abgespielten Pose.
// Flug, Einschlag und Geraeusche des Geschosses macht das Spiel dann selbst.
constexpr uintptr_t kSpawnProjectileRva = 0x25EF10;
constexpr int       kProjectileTypes    = 0x3F;      // g_npCachedWeaponRTTIList hat 0x3F Eintraege
constexpr DWORD     kShotResendMs       = 600;       // so lange wird ein Schuss in jedem Paket wiederholt
// Ladung/Staerke: die Waffe setzt 0xDB220C direkt vor dem Schuss auf die angesammelte Ladung (ms) und danach auf 0;
// das Shadowgun-Geschoss liest daraus seine Stufe (Sound SGSHOT-L<n>, Schaden).
constexpr uintptr_t kShotPowerRva       = 0xDB220C;
float g_replayVolume = 1.0f;  // Lautstaerke-Faktor fuer Geraeusche beim Nachspielen (nach Entfernung)
using SpawnProjectileFn = void* (*)(void*, void*, int, const float*, const float*, int, char, char);
SpawnProjectileFn g_spawnProjOrig = nullptr;
bool g_replayingShot = false;  // waehrend wir selbst ein Geschoss des Mitspielers erzeugen (Spiel-Thread)

SRWLOCK g_shotsLock = SRWLOCK_INIT;
constexpr int kShotRing = 32;
posecodec::ShotEvent g_localShots[kShotRing];  // eigene Schuesse (Ring)
int g_localShotHead = 0;
uint16_t g_localShotSeq = 0;
// empfangene, noch nicht abgespielte Schuesse je Mitspieler (FIFO, g_remoteLock)
posecodec::ShotEvent g_pendingShots[kMaxRemote][kShotRing];
int g_pendingCount[kMaxRemote] = {};
uint16_t g_lastRemoteShotSeq[kMaxRemote] = {};
bool g_haveRemoteShotSeq[kMaxRemote] = {};
volatile LONG64 g_shotsSent = 0, g_shotsPlayed = 0;

bool g_inLocalSpawn = false;  // Analyse: gerade laeuft das Erzeugen eines eigenen Geschosses

// Calabash (Typ 9): entsteht ohne Besitzer in den Animations-Rueckrufen der Hand (0x467B80 links/Knochen 9, 0x467C70
// rechts/Knochen 13; beide holen sich den eigenen Spieler). Ihre Explosion (Typ 10, ebenfalls ohne Besitzer) erzeugt das
// Geschoss selbst (0x267308) - die schicken wir nicht, beim Mitspieler zuendet seine nachgespielte Calabash genauso:
// Feuer, Schaden fuer jeden in der Naehe, Calabash-Platten gehen kaputt.
constexpr int       kCalabashType = 9;
constexpr uintptr_t kCalabashPlaceLo = 0x467B80, kCalabashPlaceHi = 0x467D52;
bool IsCalabashPlacement(int type, uintptr_t retAddr) {
    const uintptr_t r = retAddr - g_base;
    return type == kCalabashType && r >= kCalabashPlaceLo && r < kCalabashPlaceHi;
}

// Debug-Befehl "calabash": der Rueckruf der linken Hand (verbraucht eine Calabash, ohne -> Fehler-Ton)
bool PlaceCalabashGuarded() {
    __try {
        reinterpret_cast<void (*)()>(g_base + kCalabashPlaceLo)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* HookedSpawnProjectile(void* owner, void* target, int type, const float* pos, const float* dir, int sector, char hasTarget,
                            char noAmmo) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    uintptr_t node = 0;
    const bool calabash = !owner && IsCalabashPlacement(type, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const bool local = !g_replayingShot && gp &&
                       (calabash || (owner && SafeRead(gp + 0x1A8, &node, 8) && node == reinterpret_cast<uintptr_t>(owner)));
    g_inLocalSpawn = local;
    void* proj = g_spawnProjOrig(owner, target, type, pos, dir, sector, hasTarget, noAmmo);
    g_inLocalSpawn = false;
    if (!proj || !local || !net::IsRunning()) return proj;
    posecodec::ShotEvent s;
    s.type = static_cast<int16_t>(type);
    s.sector = static_cast<int16_t>(sector);
    SafeRead(reinterpret_cast<uintptr_t>(pos), s.pos, sizeof(s.pos));
    SafeRead(reinterpret_cast<uintptr_t>(dir), s.dir, sizeof(s.dir));
    SafeRead(g_base + kShotPowerRva, &s.power, 4);
    s.timeMs = NowMs();
    AcquireSRWLockExclusive(&g_shotsLock);
    s.seq = ++g_localShotSeq;
    g_localShots[g_localShotHead] = s;
    g_localShotHead = (g_localShotHead + 1) % kShotRing;
    ReleaseSRWLockExclusive(&g_shotsLock);
    if (const LONG64 sent = InterlockedIncrement64(&g_shotsSent); sent <= 20 || sent % 25 == 0 || calabash)
        Log(L("[Schuss] eigener %s Typ %d bei %.0f/%.0f/%.0f Richtung %.2f/%.2f/%.2f Sektor %d\n", "[Shot] own %s type %d at %.0f/%.0f/%.0f direction %.2f/%.2f/%.2f sector %d\n"), calabash ? "Calabash" : L("Schuss", "shot"), type,
            s.pos[0], s.pos[1], s.pos[2], s.dir[0], s.dir[1], s.dir[2], sector);
    return proj;
}

void HookSpawnProjectile() {
    static const uint8_t expected[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kSpawnProjectileRva), expected, sizeof(expected)) != 0) {
        Log(L("Hook Geschosse: unerwartete Bytes - uebersprungen\n", "Hook projectiles: unexpected bytes - skipped\n"));
        return;
    }
    g_spawnProjOrig = reinterpret_cast<SpawnProjectileFn>(
        InstallInlineHook(g_base + kSpawnProjectileRva, sizeof(expected), reinterpret_cast<void*>(HookedSpawnProjectile)));
    Log(L("Hook Geschosse: %s\n", "Hook projectiles: %s\n"), g_spawnProjOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// --- Analyse: Sounds aus Waffen- und Geschoss-Code protokollieren (Sound per Name 0x3EA090, per Nummer 0x3E9CB0) ---
using PlaySoundNameFn = uint64_t (*)(const char*, void*, float, float);
using PlaySoundIdFn = uint64_t (*)(int, char, float, float, char);
PlaySoundNameFn g_playSoundNameOrig = nullptr;
PlaySoundIdFn g_playSoundIdOrig = nullptr;
volatile LONG g_soundLogLines = 0;

void NoteSound(const char* name, int id, uintptr_t retAddr) {
    const uintptr_t r = retAddr - g_base;
    const bool weaponCode = (r >= 0x470000 && r < 0x47A000) || (r >= 0x258000 && r < 0x285000);
    if (!weaponCode && !g_inLocalSpawn && !g_replayingShot) return;
    if (InterlockedIncrement(&g_soundLogLines) > 80) return;
    char safeName[64] = "-";
    if (name) SafeRead(reinterpret_cast<uintptr_t>(name), safeName, sizeof(safeName) - 1);
    Log(L("[Sound] %s id=%d von 0x%llX%s\n", "[Sound] %s id=%d from 0x%llX%s\n"), safeName, id, (unsigned long long)r,
        g_inLocalSpawn ? L(" (beim eigenen Schuss)", " (own shot)") : (g_replayingShot ? L(" (beim Nachspielen)", " (replaying)") : ""));
}

// (Name, -, Tonhoehe, Lautstaerke)
uint64_t HookedPlaySoundName(const char* name, void* rdx, float pitch, float volume) {
    NoteSound(name, -1, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (g_replayingShot) volume *= g_replayVolume;
    return g_playSoundNameOrig(name, rdx, pitch, volume);
}

uint64_t HookedPlaySoundId(int id, char flag, float a, float b, char e) {
    NoteSound(nullptr, id, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return g_playSoundIdOrig(id, flag, a, b, e);
}

void HookSounds() {
    static const uint8_t nameExp[14] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x48, 0x8B, 0xD1};
    static const uint8_t idExp[15] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x7C, 0x24, 0x40};
    if (std::memcmp(reinterpret_cast<void*>(g_base + 0x3EA090), nameExp, sizeof(nameExp)) == 0)
        g_playSoundNameOrig = reinterpret_cast<PlaySoundNameFn>(InstallInlineHook(g_base + 0x3EA090, sizeof(nameExp), reinterpret_cast<void*>(HookedPlaySoundName)));
    if (std::memcmp(reinterpret_cast<void*>(g_base + 0x3E9CB0), idExp, sizeof(idExp)) == 0)
        g_playSoundIdOrig = reinterpret_cast<PlaySoundIdFn>(InstallInlineHook(g_base + 0x3E9CB0, sizeof(idExp), reinterpret_cast<void*>(HookedPlaySoundId)));
    Log(L("Hook Sounds (Analyse): Name %s, Nummer %s\n", "Hook sounds (analysis): name %s, number %s\n"), g_playSoundNameOrig ? L("aktiv", "active") : "-", g_playSoundIdOrig ? L("aktiv", "active") : "-");
}

// Eigene Schuesse der letzten kShotResendMs (aelteste zuerst) fuer das naechste Paket
int CollectRecentShots(DWORD now, posecodec::ShotEvent* out) {
    int n = 0;
    AcquireSRWLockShared(&g_shotsLock);
    for (int i = kShotRing; i >= 1 && n < posecodec::kMaxShots; --i) {
        const posecodec::ShotEvent& s = g_localShots[(g_localShotHead - i + kShotRing) % kShotRing];
        if (s.seq != 0 && now - s.timeMs < kShotResendMs) out[n++] = s;
    }
    ReleaseSRWLockShared(&g_shotsLock);
    return n;
}

// Empfang (Netz-Thread): neue Schuesse des Mitspielers pi in die Warteschlange; Aufrufer haelt g_remoteLock
void QueueRemoteShotsLocked(int pi, const posecodec::NetPose& p, bool reconnect) {
    if (reconnect) g_haveRemoteShotSeq[pi] = false;
    for (int k = 0; k < p.numShots; ++k) {
        const posecodec::ShotEvent& s = p.shots[k];
        const int16_t diff = static_cast<int16_t>(s.seq - g_lastRemoteShotSeq[pi]);
        if (g_haveRemoteShotSeq[pi] && diff <= 0 && diff > -1000) continue;  // schon bekommen
        g_lastRemoteShotSeq[pi] = s.seq;
        g_haveRemoteShotSeq[pi] = true;
        if (g_pendingCount[pi] < kShotRing) g_pendingShots[pi][g_pendingCount[pi]++] = s;
    }
}

bool ReplayShotGuarded(void* owner, const posecodec::ShotEvent& s, const float pos[3]) {
    int32_t* power = reinterpret_cast<int32_t*>(g_base + kShotPowerRva);
    const int32_t oldPower = *power;
    __try {
        *power = s.power;  // Staerke des Mitspieler-Schusses nur fuer diesen Aufruf
        g_spawnProjOrig(owner, nullptr, s.type, pos, s.dir, s.sector, 0, 1);
        *power = oldPower;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *power = oldPower;
        return false;
    }
}

// Abstand zum eigenen Spieler (-1 = unbekannt)
float DistanceToLocalPlayer(const float pos[3]) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    float me[3] = {};
    if (!gp || !SafeRead(gp + 0x20, me, sizeof(me))) return -1.0f;
    const float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Calabash des Mitspielers nur in Sichtweite nachspielen: weit weg aktualisiert das Spiel sie nicht - sie wuerde erst
// explodieren, wenn man spaeter hinkommt. Die Platten kommen auch dann ueber die Behaelter-Liste.
constexpr float kCalabashReplayRange = 4000.0f;

// Mitspieler-Schuesse leiser, je weiter weg (die Schuss-Geraeusche des Spiels sind nicht raeumlich)
float ShotVolumeAt(const float pos[3]) {
    const float d = DistanceToLocalPlayer(pos);
    if (d < 0) return 1.0f;
    constexpr float kNear = 300.0f, kFar = 4000.0f, kMin = 0.1f;
    if (d <= kNear) return 1.0f;
    if (d >= kFar) return kMin;
    return 1.0f - (1.0f - kMin) * (d - kNear) / (kFar - kNear);
}

// Spiel-Thread: faellige Schuesse des Mitspielers pi abspielen (Zeitpunkt wie die gezeigte Pose: jetzt - Verzoegerung)
void ReplayRemoteShots(int pi, uintptr_t p2, const float offset[3]) {
    posecodec::ShotEvent due[kShotRing];
    int numDue = 0;
    AcquireSRWLockExclusive(&g_remoteLock);
    const int64_t target = static_cast<int64_t>(NowMs()) - g_remote[pi].clockOffset - kInterpDelayMs;
    int keep = 0;
    for (int i = 0; i < g_pendingCount[pi]; ++i) {
        if (static_cast<int64_t>(g_pendingShots[pi][i].timeMs) <= target) due[numDue++] = g_pendingShots[pi][i];
        else g_pendingShots[pi][keep++] = g_pendingShots[pi][i];
    }
    g_pendingCount[pi] = keep;
    ReleaseSRWLockExclusive(&g_remoteLock);
    if (!numDue || !g_spawnProjOrig) return;
    void* owner = nullptr;
    SafeRead(p2 + 0x1A8, &owner, 8);
    static bool broken = false;
    for (int i = 0; i < numDue && !broken; ++i) {
        const posecodec::ShotEvent& s = due[i];
        if (s.type < 0 || s.type >= kProjectileTypes) continue;
        const float pos[3] = {s.pos[0] + offset[0], s.pos[1] + offset[1], s.pos[2] + offset[2]};
        if (s.type == kCalabashType) {
            const float d = DistanceToLocalPlayer(pos);
            if (d > kCalabashReplayRange) {
                Log(L("[Schuss] Mitspieler-Calabash %.0f entfernt - nicht nachgespielt (zu weit weg)\n", "[Shot] other player's Calabash %.0f away - not replayed (too far)\n"), d);
                continue;
            }
        }
        g_replayVolume = ShotVolumeAt(pos);
        g_replayingShot = true;
        // Calabash wie im Original ohne Besitzer: ihre Explosion trifft dann auch den eigenen Spieler hier
        const bool ok = ReplayShotGuarded(s.type == kCalabashType ? nullptr : owner, s, pos);
        g_replayingShot = false;
        if (!ok) {
            broken = true;
            Log(L("[Schuss] Geschoss des Mitspielers erzeugen fehlgeschlagen - abgeschaltet\n", "[Shot] creating the other player's projectile failed - disabled\n"));
        } else if (const LONG64 played = InterlockedIncrement64(&g_shotsPlayed); played <= 20 || played % 25 == 0 || s.type == kCalabashType) {
            Log(L("[Schuss] Mitspieler-%s Typ %d abgespielt bei %.0f/%.0f/%.0f (Staerke %d, Lautstaerke %.2f)\n", "[Shot] other player's %s type %d replayed at %.0f/%.0f/%.0f (strength %d, volume %.2f)\n"),
                s.type == kCalabashType ? "Calabash" : L("Schuss", "shot"), s.type, pos[0], pos[1], pos[2], s.power, g_replayVolume);
        }
    }
}

extern posecodec::BlockState g_movingBlocks[posecodec::kMaxMovingBlocks];
extern int g_numMovingBlocks;
void NotePoseBlocks(const posecodec::NetPose& np);
int32_t g_remoteSector[kMaxRemote] = {-1, -1, -1};  // Sektor je Mitspieler aus der abgespielten Pose (Spiel-Thread)
// Stand je Mitspieler (Netz-Thread schreibt, Spiel-Thread liest), u.a. fuer "Gast folgt dem Host"
std::atomic<int> g_peerLevel[kMaxRemote];
std::atomic<bool> g_peerInGame[kMaxRemote];
std::atomic<bool> g_peerFresh[kMaxRemote];
// Platz des Hosts unter den Mitspielern (-1 = ich bin Host / keiner)
int HostSlot() { return net::HostPeer(); }
// Paket vom Host (Welt-Stand, Gegner-Positionen ...) - nur Gaeste nehmen so etwas an
bool FromHost(int from) { return !net::IsHost() && from >= 0 && from == HostSlot(); }
bool HostActive() {
    const int h = HostSlot();
    return h >= 0 && g_remoteActive[h];
}

// ---- Koop-Extras: Einstellungen und Spielernummern ----
// Eigene Einstellungen in smcoop_settings.txt (Menue "Koop-Lobby"). Gegner-HP, geteilte HP und Hardcore gelten vom Host:
// er schickt sie alle 2 s mit den Spielernummern ("SMO1", roster[i] = Kennung von Spieler i+1). Nummer und Farbe sind bei
// allen gleich: Host 1, danach in der Reihenfolge des Beitritts; wer geht, gibt seine Nummer frei.
coop::CoopSettings g_localSettings;
std::atomic<bool> g_hardcoreRun{false};      // Host/allein: laufendes Abenteuer ist Hardcore
SRWLOCK g_settingsLock = SRWLOCK_INIT;
posecodec::HostSettingsInfo g_hostSettings;  // (g_settingsLock) Host: eigene, Gast: zuletzt vom Host empfangene
bool g_hostSettingsKnown = false;            // (g_settingsLock) Gast: schon etwas vom Host bekommen
std::atomic<bool> g_hostSettingsDirty{true};  // Host: sofort neu senden

void LoadSettings() {
    std::string text;
    if (FILE* f = std::fopen("smcoop_settings.txt", "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) text += line;
        std::fclose(f);
    }
    g_localSettings = coop::ParseSettings(text);
    Log(L("[Einstellungen] geladen: Gegner-HP %d %%, geteilte HP %s, Namen %s\n", "[Settings] loaded: enemy HP %d %%, shared HP %s, names %s\n"), g_localSettings.hpPercent,
        g_localSettings.sharedHp ? L("an", "on") : L("aus", "off"), g_localSettings.showNames ? L("an", "on") : L("aus", "off"));
}

void SaveSettings() {
    if (FILE* f = std::fopen("smcoop_settings.txt", "w")) {
        std::fputs(coop::FormatSettings(g_localSettings).c_str(), f);
        std::fclose(f);
    }
    g_hostSettingsDirty = true;
}

// Gast = Mitglied einer fremden Lobby bzw. Netz laeuft und ich bin nicht Host
bool IsGuest() {
    if (net::LobbyMode()) return net::GetLobbyState() == net::LobbyState::Member;
    return net::IsRunning() && !net::IsHost();
}

posecodec::HostSettingsInfo CurrentHostSettings() {
    AcquireSRWLockShared(&g_settingsLock);
    posecodec::HostSettingsInfo h = g_hostSettings;
    const bool known = g_hostSettingsKnown;
    ReleaseSRWLockShared(&g_settingsLock);
    if (!IsGuest()) {  // Host oder allein: eigene Werte (roster pflegt der Netz-Thread)
        h.hpPercent = static_cast<uint8_t>(g_localSettings.hpPercent);
        h.sharedHp = g_localSettings.sharedHp ? 1 : 0;
        h.hardcore = g_hardcoreRun ? 1 : 0;
    } else if (!known) {
        h = posecodec::HostSettingsInfo();
        h.hpPercent = 0;  // bis der Host sich meldet: nichts veraendern
    }
    return h;
}

int HpPercent() { return CurrentHostSettings().hpPercent; }
bool SharedHpOn() { return CurrentHostSettings().sharedHp != 0; }
bool HardcoreRunOn() { return CurrentHostSettings().hardcore != 0; }
int PlayerCount() {
    if (!net::IsRunning()) return 1;
    const int n = coop::RosterCount(CurrentHostSettings().roster);
    return n > 0 ? n : 1;
}
int LocalPlayerNumber() {
    if (!IsGuest()) return 1;
    return coop::RosterNumberOf(CurrentHostSettings().roster, net::LocalId());
}
int PlayerNumberOfPeer(int peer) { return coop::RosterNumberOf(CurrentHostSettings().roster, net::PeerId(peer)); }

// Netz-Thread: Host pflegt die Spielernummern und schickt alles alle 2 s (bei Aenderung sofort)
void SendHostSettings() {
    if (IsGuest()) return;
    static DWORD last = 0;
    if (!g_hostSettingsDirty && GetTickCount() - last < 2000) return;
    last = GetTickCount();
    g_hostSettingsDirty = false;
    uint64_t members[kMaxRemote] = {};
    int count = 0;
    for (int pi = 0; pi < kMaxRemote; ++pi)
        if (const uint64_t id = net::PeerId(pi)) members[count++] = id;
    AcquireSRWLockExclusive(&g_settingsLock);
    uint64_t before[4];
    std::memcpy(before, g_hostSettings.roster, sizeof(before));
    coop::RosterUpdate(g_hostSettings.roster, net::LocalId(), members, count);
    const bool changed = std::memcmp(before, g_hostSettings.roster, sizeof(before)) != 0;
    g_hostSettings.seq++;
    g_hostSettings.hpPercent = static_cast<uint8_t>(g_localSettings.hpPercent);
    g_hostSettings.sharedHp = g_localSettings.sharedHp ? 1 : 0;
    g_hostSettings.hardcore = g_hardcoreRun ? 1 : 0;
    g_hostSettings.count = static_cast<uint8_t>(coop::RosterCount(g_hostSettings.roster));
    const posecodec::HostSettingsInfo h = g_hostSettings;
    ReleaseSRWLockExclusive(&g_settingsLock);
    if (changed) Log(L("[Einstellungen] Spielernummern: %d Spieler\n", "[Settings] player numbers: %d players\n"), h.count);
    if (!net::IsRunning()) return;
    uint8_t buf[posecodec::MaxHostSettingsPacketSize()];
    net::Send(buf, posecodec::EncodeHostSettings(h, buf));
}

// Netz-Thread: false, wenn es kein Einstellungs-Paket war
bool HandleHostSettingsPacket(int from, const uint8_t* data, size_t size) {
    posecodec::HostSettingsInfo h;
    if (!posecodec::DecodeHostSettings(data, size, h)) return false;
    if (!FromHost(from)) return true;
    AcquireSRWLockExclusive(&g_settingsLock);
    const bool changed = !g_hostSettingsKnown || g_hostSettings.hpPercent != h.hpPercent || g_hostSettings.sharedHp != h.sharedHp ||
                         g_hostSettings.hardcore != h.hardcore || std::memcmp(g_hostSettings.roster, h.roster, sizeof(h.roster)) != 0;
    g_hostSettings = h;
    g_hostSettingsKnown = true;
    ReleaseSRWLockExclusive(&g_settingsLock);
    if (changed)
        Log(L("[Einstellungen] vom Host: Gegner-HP %d %%, geteilte HP %s, Hardcore %s, %d Spieler, ich bin Spieler %d\n", "[Settings] from host: enemy HP %d %%, shared HP %s, hardcore %s, %d players, I am player %d\n"), h.hpPercent,
            h.sharedHp ? L("an", "on") : L("aus", "off"), h.hardcore ? L("ja", "yes") : L("nein", "no"), h.count, coop::RosterNumberOf(h.roster, net::LocalId()));
    return true;
}

void LogSettings() {
    const posecodec::HostSettingsInfo h = CurrentHostSettings();
    Log(L("[Einstellungen] Gegner-HP %d %%, geteilte HP %s, Hardcore %s, Namen %s, %d Spieler, ich bin Spieler %d\n", "[Settings] enemy HP %d %%, shared HP %s, hardcore %s, names %s, %d players, I am player %d\n"), h.hpPercent,
        h.sharedHp ? L("an", "on") : L("aus", "off"), h.hardcore ? L("ja", "yes") : L("nein", "no"), g_localSettings.showNames ? L("an", "on") : L("aus", "off"), PlayerCount(),
        LocalPlayerNumber());
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        char name[64];
        net::PeerName(pi, name, sizeof(name));
        if (name[0]) Log(L("  Platz %d: %s = Spieler %d\n", "  Slot %d: %s = player %d\n"), pi + 1, name, PlayerNumberOfPeer(pi));
    }
}

// ---- Aussehen: Geheimnisse wie "Spiele als Duppie" ----
// Die Spieler-Form +0x1D184 waehlt Modell und Skelett (Tabelle 0x6B7460, je 24 Byte: .msh, .skn, Texturen): 0 Shadow Man,
// 1 Mike, 2 Disco-Kleidung, 3 Duppie, 4 Nettie, 5/6 Grosskopf, 7/8 Grosse Schuhe, 9 Deadwing, 10 Hund, 11 Beta.
// 0x459B90(spieler) setzt die Grundform (0/1 je nach Welt), 0x459250(spieler) liest die Geheimnis-Cvars (GetBool 0x1B82D0),
// setzt die Form, laedt das Modell und haengt z.B. den Stetson an (0x47CAC0). So macht es auch die Aenderungs-Funktion der
// Cvars (0x458EF0). Spieler 2 ist eine Kopie des eigenen Spielers und bekam dadurch immer das EIGENE Aussehen; mit anderem
// Skelett passte die Knochenzahl nicht und der Mitspieler wurde ausgeblendet. Jetzt schickt jeder Form + Cvar-Bits mit der
// Pose ('F'); der Empfaenger baut Spieler 2 mit den Cvars des Mitspielers neu (der GetBool-Hook liefert waehrenddessen
// dessen Werte). "Unsichtbar" (0xDBA500) prueft das Zeichnen des Spielers bei 0x4A0957: ist der eigene Spieler unsichtbar,
// zeichnen wir Spieler 2 dort selbst; ist der Mitspieler unsichtbar, nur seine Waffen.
constexpr uint32_t kPlayerFormOff = 0x1D184;
constexpr uintptr_t kCvarGetBoolRva = 0x1B82D0;
constexpr uintptr_t kPlayerBaseFormRva = 0x459B90;
constexpr uintptr_t kPlayerSetupModelRva = 0x459250;
constexpr uintptr_t kPlayerFormShadTableRva = 0x6B7448;  // je Form 1 Byte: 1 = Shadow-Man-Variante (0, 5, 7, 11)
constexpr uintptr_t kInvisibleCheckCallRva = 0x4A0957;  // call GetBool(g_invisiblemode) vor dem Zeichnen des Spielers
constexpr uintptr_t kPlayerDrawListRva = 0xCD8050;      // Liste, in die 0x4A0980 den Spieler einhaengt
struct LookCvar { uintptr_t rva; const char* name; };
static const LookCvar kLookCvars[] = {
    {0xDB9FF0, "Grosskopf"}, {0xDBA080, "Disco-Kleidung"}, {0xDBA230, "Flammen"}, {0xDBA2C0, "Grosse Schuhe"},
    {0xDBA350, "Stetson"},   {0xDBA3E0, "Schrotflinten-Kopf"}, {0xDBA500, "Unsichtbar"}, {0xDBA590, "Nettie"},
    {0xDBA620, "Duppie"},    {0xDBA6B0, "Deadwing"}, {0xDBA740, "Hund"}, {0xDBA7D0, "Blutunterlaufen"},
    {0xDBA860, "Beta"},      {0xDBA8F0, "Twig Boy"}};  // Reihenfolge = Bits (coop::LooksForForm nutzt sie)
// Name eines Aussehens-Geheimnisses fuers Protokoll (die Tabelle oben ist deutsch)
const char* LookCvarName(size_t i) {
    static const char* const kEn[] = {"Big head", "Disco outfit", "Flames", "Big shoes", "Stetson", "Shotgun head", "Invisible",
                                      "Nettie",   "Duppie",       "Deadwing", "Dog",     "Bloodshot", "Beta",       "Twig Boy"};
    static_assert(std::size(kEn) == std::size(kLookCvars), "je Geheimnis ein englischer Name");
    return i18n::Pick(kLookCvars[i].name, kEn[i]);
}
constexpr uint32_t kLookInvisibleBit = 1u << 6;
using CvarGetBoolFn = bool (*)(void*, int);
CvarGetBoolFn g_cvarGetBoolOrig = nullptr;
std::atomic<bool> g_lookOverride{false};  // nur waehrend Spieler 2 neu gebaut wird (Spiel-Thread)
uint32_t g_lookOverrideBits = 0;
std::atomic<uint32_t> g_remoteLooks[kMaxRemote];
void* PatchCallRel32(uintptr_t callRva, uintptr_t expectedRva, void* detour);

bool HookedCvarGetBool(void* cvar, int def) {
    if (g_lookOverride.load(std::memory_order_relaxed)) {
        const uintptr_t c = reinterpret_cast<uintptr_t>(cvar) - g_base;
        for (size_t i = 0; i < std::size(kLookCvars); ++i)
            if (kLookCvars[i].rva == c) return ((g_lookOverrideBits >> i) & 1) != 0;
    }
    return g_cvarGetBoolOrig(cvar, def);
}

uint32_t ReadLookBitsGuarded() {
    uint32_t bits = 0;
    __try {
        for (size_t i = 0; i < std::size(kLookCvars); ++i)
            if (g_cvarGetBoolOrig(reinterpret_cast<void*>(g_base + kLookCvars[i].rva), 0)) bits |= 1u << i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return bits;
}

std::string LookNames(uint32_t bits) {
    std::string s;
    for (size_t i = 0; i < std::size(kLookCvars); ++i)
        if (bits & (1u << i)) s += (s.empty() ? "" : ", ") + std::string(LookCvarName(i));
    return s.empty() ? L("keine", "none") : s;
}

// Grundform statt 0x459B90: die richtet sich nach dem EIGENEN Level/Story-Stand (Level 19 z.B. nach Flag 0x20), der
// Mitspieler kann dort aber schon die andere Form haben. Byte-Tabelle 0x6B7448 je Form: 1 = Shadow-Man-Variante -> 0, sonst 1.
bool RebuildPlayer2LooksGuarded(uintptr_t p2, int form, uint32_t looks) {
    bool ok = false;
    g_lookOverrideBits = looks;
    g_lookOverride = true;
    __try {
        float keep[3];
        std::memcpy(keep, reinterpret_cast<void*>(p2 + 8), sizeof(keep));  // wie 0x458EF0
        if (form >= 0 && form < 12)
            *reinterpret_cast<int*>(p2 + kPlayerFormOff) = *reinterpret_cast<uint8_t*>(g_base + kPlayerFormShadTableRva + form) ? 0 : 1;
        else
            reinterpret_cast<void (*)(uintptr_t)>(g_base + kPlayerBaseFormRva)(p2);
        reinterpret_cast<void (*)(uintptr_t)>(g_base + kPlayerSetupModelRva)(p2);
        std::memcpy(reinterpret_cast<void*>(p2 + 8), keep, sizeof(keep));
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_lookOverride = false;
    return ok;
}

// Spiel-Thread (HookedPlayerBones): Figur des Mitspielers pi an seine Form/Geheimnisse anpassen
void UpdatePlayer2Looks(int pi, uintptr_t p2, int form, uint32_t looks) {
    static uintptr_t appliedP2[kMaxRemote] = {};
    static uint32_t appliedLooksArr[kMaxRemote] = {};
    static int appliedFormArr[kMaxRemote] = {-1, -1, -1};
    static DWORD lastTryArr[kMaxRemote] = {};
    uint32_t& appliedLooks = appliedLooksArr[pi];
    int& appliedForm = appliedFormArr[pi];
    DWORD& lastTry = lastTryArr[pi];
    g_remoteLooks[pi] = looks;
    if (p2 == appliedP2[pi] && looks == appliedLooks && form == appliedForm) return;
    const int before = *reinterpret_cast<int*>(p2 + kPlayerFormOff);
    // Gebaut wird nach seiner ECHTEN Form: ein eingeschalteter Duppie-Schalter (g_duppiemode) heisst nicht, dass er Duppie ist -
    // nach dem Laden gilt die gespeicherte Form. Sonst passt das Modell nicht (z.B. 33 statt 69 Knochen) -> Figur unsichtbar
    // und eingefroren (Tueren, Gegner sehen sie an der falschen Stelle).
    const uint32_t buildLooks = coop::LooksForForm(looks, form);
    if (p2 != appliedP2[pi] && before == form && buildLooks == coop::LooksForForm(ReadLookBitsGuarded(), before)) {  // passt schon
        appliedP2[pi] = p2; appliedLooks = looks; appliedForm = form;
        return;
    }
    const DWORD now = GetTickCount();
    if (now - lastTry < 1000) return;
    lastTry = now;
    const bool ok = RebuildPlayer2LooksGuarded(p2, form, buildLooks);
    appliedP2[pi] = p2; appliedLooks = looks; appliedForm = form;
    if (!ok) {
        Log(L("[Aussehen] Spieler 2 neu aufbauen fehlgeschlagen (Ausnahme)\n", "[Looks] rebuilding player 2 failed (exception)\n"));
        return;
    }
    Log(L("[Aussehen] Mitspieler: Form %d, Geheimnisse: %s -> Spieler 2 neu aufgebaut (Form %d -> %d, %zu Knochen)\n", "[Looks] other player: form %d, secrets: %s -> player 2 rebuilt (form %d -> %d, %zu bones)\n"), form,
        LookNames(looks).c_str(), before, *reinterpret_cast<int*>(p2 + kPlayerFormOff),
        *reinterpret_cast<size_t*>(p2 + 0x368 + 0x188));
}

// Ersetzt den Aufruf GetBool(g_invisiblemode) beim Zeichnen des eigenen Spielers: ist er unsichtbar, faellt der normale
// Weg (AddSkin-Hook) weg - dann Spieler 2 hier einhaengen.
bool HookedInvisibleCheck(void* cvar, int def) {
    const bool invisible = g_cvarGetBoolOrig ? g_cvarGetBoolOrig(cvar, def)
                                             : reinterpret_cast<CvarGetBoolFn>(g_base + kCvarGetBoolRva)(cvar, def);
    for (int pi = 0; invisible && pi < kMaxRemote; ++pi) {
        const uintptr_t p2 = g_avatar[pi];
        if (!p2 || !net::IsRunning() || !g_remoteActive[pi] || g_remoteModelMismatch[pi]) continue;
        void* list = reinterpret_cast<void*>(g_base + kPlayerDrawListRva);
        if (!(g_remoteLooks[pi] & kLookInvisibleBit) && !DrawPlayer2Guarded(list, p2)) {
            Log(L("[Spieler2] Zeichnen fehlgeschlagen - abgeschaltet\n", "[Player2] drawing failed - disabled\n"));
            g_avatar[pi] = 0;
        } else {
            DrawPlayer2Items(pi, p2);
        }
    }
    return invisible;
}

void HookLooks() {
    // push rdi / push r14 / push r15 / sub rsp,40h / mov qword [rsp+20h],-2
    static const uint8_t expected[19] = {0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40,
                                         0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kCvarGetBoolRva), expected, sizeof(expected)) == 0)
        g_cvarGetBoolOrig = reinterpret_cast<CvarGetBoolFn>(
            InstallInlineHook(g_base + kCvarGetBoolRva, sizeof(expected), reinterpret_cast<void*>(HookedCvarGetBool)));
    Log(L("Hook Aussehen (Cvar GetBool): %s\n", "Hook looks (cvar GetBool): %s\n"), g_cvarGetBoolOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
    const bool inv = PatchCallRel32(kInvisibleCheckCallRva, kCvarGetBoolRva, reinterpret_cast<void*>(HookedInvisibleCheck)) != nullptr;
    Log(L("Hook Unsichtbar-Pruefung: %s\n", "Hook invisibility check: %s\n"), inv ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

void QueueOutgoingPose(uintptr_t p1) {
    static DWORD lastSend = 0;
    const DWORD now = NowMs();
    if (now - lastSend < kSendIntervalMs) return;
    lastSend = now;
    const uintptr_t a = p1 + 0x368;
    const float* bones = *reinterpret_cast<float**>(a + 0x180);
    const size_t n = *reinterpret_cast<size_t*>(a + 0x188);
    if (!bones || n == 0 || n > kMaxBones) return;
    const LevelInfo li = GetLevelInfo(g_base);
    // Gegenstaende in den Haenden: nur, was in den letzten kHeldItemTimeoutMs wirklich an der Hand gezeichnet wurde
    posecodec::HandItem hands[posecodec::kNumHands];
    AcquireSRWLockShared(&g_handsLock);
    for (int h = 0; h < posecodec::kNumHands; ++h)
        if (g_localHandTime[h] && now - g_localHandTime[h] < kHeldItemTimeoutMs) hands[h] = g_localHands[h];
    ReleaseSRWLockShared(&g_handsLock);
    static int lastTypes[posecodec::kNumHands] = {-2, -2};
    if (hands[0].type != lastTypes[0] || hands[1].type != lastTypes[1]) {
        lastTypes[0] = hands[0].type;
        lastTypes[1] = hands[1].type;
        Log(L("[Waffen] Du haeltst: links %s, rechts %s\n", "[Weapons] you hold: left %s, right %s\n"), ItemName(hands[0].type), ItemName(hands[1].type));
    }
    posecodec::ShotEvent shots[posecodec::kMaxShots];
    const int numShots = CollectRecentShots(now, shots);
    const int32_t sector = *reinterpret_cast<int32_t*>(p1 + 0xD4);  // fuer Gegner, die den Mitspieler verfolgen
    static uint32_t looks = 0;
    static DWORD lastLooks = 0;
    if (g_cvarGetBoolOrig && now - lastLooks >= 500) {
        lastLooks = now;
        const uint32_t l = ReadLookBitsGuarded();
        if (l != looks) Log(L("[Aussehen] Deine Geheimnisse: %s\n", "[Looks] your secrets: %s\n"), LookNames(l).c_str());
        looks = l;
    }
    const int32_t form = *reinterpret_cast<int32_t*>(p1 + kPlayerFormOff);
    AcquireSRWLockExclusive(&g_outLock);
    g_outSize = posecodec::Encode(++g_outSeq, now, static_cast<uint16_t>(li.level), li.inGame, reinterpret_cast<const float*>(p1 + 0x200),
                                  bones, static_cast<uint32_t>(n), hands, shots, numShots, g_movingBlocks, g_numMovingBlocks,
                                  sector, form, looks, g_outPacket);
    g_outReady = true;
    ReleaseSRWLockExclusive(&g_outLock);
}

void SendWorldState();
bool HandleWorldPacket(int from, const uint8_t* data, size_t size);
void SendStates();
bool HandleStatePacket(int from, const uint8_t* data, size_t size);
void SendKills();
bool HandleKillPacket(int from, const uint8_t* data, size_t size);
void SendDamage();
bool HandleDamagePacket(int from, const uint8_t* data, size_t size);
void SendEnemies();
bool HandleEnemyPacket(int from, const uint8_t* data, size_t size);
void SendEvents();
bool HandleEventPacket(int from, const uint8_t* data, size_t size);
void SendHostSnapshot();
bool HandleSnapPacket(int from, const uint8_t* data, size_t size);
bool HandleTablePacket(int from, const uint8_t* data, size_t size);
void SendCutscenes();
bool HandleCutscenePacket(int from, const uint8_t* data, size_t size);
bool HandleQuestPacket(int from, const uint8_t* data, size_t size);
void SendProgress();
bool HandleProgressPacket(int from, const uint8_t* data, size_t size);
void SendDeaths();
bool HandleDeathPacket(int from, const uint8_t* data, size_t size);
void SendLife();
bool HandleLifePacket(int from, const uint8_t* data, size_t size);

// Probe-Thread: senden und empfangen
void PumpNetwork() {
    if (!net::IsRunning()) return;
    SendHostSettings();
    static DWORD lastWorldSend = 0;
    if (NowMs() - lastWorldSend >= 250 && g_outSize > 0) {  // Welt-Zustand komplett, 4x pro Sekunde (Schiebebloecke fluessig)
        lastWorldSend = NowMs();
        SendWorldState();
        SendStates();
        SendKills();
        SendDamage();
        SendEvents();
    }
    static DWORD lastProgressSend = 0;
    if (NowMs() - lastProgressSend >= 2000 && g_outSize > 0) {  // Fortschritt (Flags + Zaehler)
        lastProgressSend = NowMs();
        SendProgress();
    }
    SendHostSnapshot();  // nur wenn der Spiel-Thread einen neuen Stand bereitgelegt hat (Host, alle 2 s)
    static DWORD lastCutSend = 0;
    if (NowMs() - lastCutSend >= 250) {  // gestartete Cutscenes (nur die der letzten 5 s)
        lastCutSend = NowMs();
        SendCutscenes();
        SendDeaths();
    }
    static DWORD lastEnemySend = 0;
    if (NowMs() - lastEnemySend >= 100 && g_outSize > 0) {  // Gegner-Positionen (nur der Host hat welche)
        lastEnemySend = NowMs();
        SendEnemies();
        SendLife();
    }
    // Neue Pose senden; im Menue/Pause (keine neue Pose) die letzte als Lebenszeichen wiederholen,
    // damit der Mitspieler uns nicht nach 2 s ausblendet
    static DWORD lastSent = 0;
    const bool newPose = g_outReady.exchange(false);
    if (newPose || (g_outSize > 0 && NowMs() - lastSent > kHeartbeatMs)) {
        uint8_t buf[posecodec::MaxPacketSize()];
        AcquireSRWLockShared(&g_outLock);
        const size_t n = g_outSize;
        std::memcpy(buf, g_outPacket, n);
        ReleaseSRWLockShared(&g_outLock);
        net::Send(buf, n);
        lastSent = NowMs();
        InterlockedIncrement64(&g_netSent);
    }
    uint8_t in[posecodec::MaxPacketSize() + 64];
    static_assert(posecodec::MaxTablePacketSize() <= sizeof(in) && posecodec::MaxKillPacketSize() <= sizeof(in) && posecodec::MaxDamagePacketSize() <= sizeof(in) && posecodec::MaxSnapPacketSize() <= sizeof(in) &&
                      posecodec::MaxProgressPacketSize() <= sizeof(in),
                  "Empfangspuffer zu klein");
    for (int i = 0; i < 96; ++i) {
        int from = -1;
        const size_t n = net::Receive(in, sizeof(in), &from);
        if (!n) break;
        if (from < 0 || from >= kMaxRemote) continue;
        static posecodec::NetPose decoded;
        if (HandleWorldPacket(from, in, n) || HandleStatePacket(from, in, n) || HandleKillPacket(from, in, n) ||
            HandleEnemyPacket(from, in, n) || HandleEventPacket(from, in, n) || HandleSnapPacket(from, in, n) ||
            HandleTablePacket(from, in, n) || HandleCutscenePacket(from, in, n) || HandleDamagePacket(from, in, n) ||
            HandleProgressPacket(from, in, n) || HandleQuestPacket(from, in, n) || HandleHostSettingsPacket(from, in, n) ||
            HandleDeathPacket(from, in, n) || HandleLifePacket(from, in, n))
            continue;
        if (!posecodec::Decode(in, n, decoded)) continue;
        InterlockedIncrement64(&g_netRecv);
        const DWORD now = NowMs();
        RemoteRing& r = g_remote[from];
        AcquireSRWLockExclusive(&g_remoteLock);
        QueueRemoteShotsLocked(from, decoded, r.count == 0 || now - r.lastRecv > kRemoteTimeoutMs);
        const bool repeat = r.count > 0 && r.poses[(r.head - 1 + kRemoteRing) % kRemoteRing].seq == decoded.seq;
        if (!repeat) {  // Lebenszeichen (gleiche Pose) nur als "noch da" werten, nicht erneut einreihen
            const int64_t off = static_cast<int64_t>(now) - static_cast<int64_t>(decoded.timeMs);
            if (off < r.clockOffset || now - r.lastRecv > kRemoteTimeoutMs) r.clockOffset = off;  // Uhrenabgleich
            r.poses[r.head] = decoded;
            r.head = (r.head + 1) % kRemoteRing;
            if (r.count < kRemoteRing) ++r.count;
        }
        r.lastRecv = now;
        ReleaseSRWLockExclusive(&g_remoteLock);
    }
    // je Mitspieler: nur zeigen, wenn Daten frisch sind und beide im Spiel im selben Level sind
    const LevelInfo local = GetLevelInfo(g_base);
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        RemoteRing& r = g_remote[pi];
        const bool connected = net::PeerConnected(pi);
        uint16_t remoteLevel = 0;
        bool remoteInGame = false;
        AcquireSRWLockShared(&g_remoteLock);
        const bool fresh = connected && r.count > 0 && NowMs() - r.lastRecv < kRemoteTimeoutMs;
        if (fresh) {
            const posecodec::NetPose& newest = r.poses[(r.head - 1 + kRemoteRing) % kRemoteRing];
            remoteLevel = newest.level;
            remoteInGame = newest.inGame;
        }
        ReleaseSRWLockShared(&g_remoteLock);
        const bool active = fresh && remoteInGame && local.inGame && remoteLevel == local.level;
        g_peerFresh[pi] = fresh;
        g_peerInGame[pi] = remoteInGame;
        g_peerLevel[pi] = fresh ? static_cast<int>(remoteLevel) : -1;
        static int lastState[kMaxRemote] = {-1, -1, -1};  // 0 = keine Daten, 1 = anderes Level/Menue, 2 = sichtbar
        const int state = !fresh ? 0 : (active ? 2 : 1);
        if (state != lastState[pi]) {
            char name[64];
            net::PeerName(pi, name, sizeof(name));
            if (!name[0]) std::snprintf(name, sizeof(name), L("Platz %d", "Player %d"), pi + 1);
            if (state == 0 && lastState[pi] != -1) Log(L("\n[Netz] Mitspieler %s weg (keine Daten)\n", "\n[Net] player %s gone (no data)\n"), name);
            else if (state == 1) Log(L("\n[Netz] Mitspieler %s verbunden, aber woanders (er: Level %u %s, du: Level %d %s)\n", "\n[Net] player %s connected, but elsewhere (them: level %u %s, you: level %d %s)\n"), name, remoteLevel,
                                     remoteInGame ? L("im Spiel", "in game") : L("im Menue", "in menu"), local.level, local.inGame ? L("im Spiel", "in game") : L("im Menue", "in menu"));
            else if (state == 2) Log(L("\n[Netz] Mitspieler %s im selben Level (%d) - sichtbar\n", "\n[Net] player %s in the same level (%d) - visible\n"), name, local.level);
            lastState[pi] = state;
        }
        g_remoteActive[pi] = active;
        if (!active) {  // Schuesse aus einem anderen Level / Menue nicht spaeter abspielen
            AcquireSRWLockExclusive(&g_remoteLock);
            g_pendingCount[pi] = 0;
            ReleaseSRWLockExclusive(&g_remoteLock);
        }
        // Solange der Mitspieler Daten schickt, muss seine Figur existieren (auch nach dem Laden eines Levels)
        int none = 0;
        if (active && !g_avatar[pi]) g_spawnRequest.compare_exchange_strong(none, pi + 1);
    }
}

// Interpolierte Pose des Mitspielers pi fuer "jetzt - kInterpDelayMs" (in Senderzeit)
bool SampleRemotePose(int pi, posecodec::NetPose& out) {
    static posecodec::NetPose a, b;
    const RemoteRing& r = g_remote[pi];
    AcquireSRWLockShared(&g_remoteLock);
    if (r.count == 0) { ReleaseSRWLockShared(&g_remoteLock); return false; }
    const int64_t target = static_cast<int64_t>(NowMs()) - r.clockOffset - kInterpDelayMs;
    const posecodec::NetPose* older = nullptr;
    const posecodec::NetPose* newer = nullptr;
    for (int i = 1; i <= r.count; ++i) {
        const posecodec::NetPose& p = r.poses[(r.head - i + kRemoteRing) % kRemoteRing];
        if (static_cast<int64_t>(p.timeMs) <= target) { older = &p; break; }
        newer = &p;
    }
    if (!older) older = newer;          // Puffer noch zu kurz: aelteste nehmen
    if (!newer) newer = older;          // keine neuere: letzte halten
    a = *older;
    b = *newer;
    ReleaseSRWLockShared(&g_remoteLock);
    const float span = static_cast<float>(static_cast<int64_t>(b.timeMs) - static_cast<int64_t>(a.timeMs));
    const float t = span > 0 ? static_cast<float>(target - static_cast<int64_t>(a.timeMs)) / span : 1.0f;
    posecodec::Lerp(a, b, t < 0 ? 0 : (t > 1 ? 1 : t), out);
    return true;
}

// NetPose -> Spielformat (Vorframe-Teil aus der zuletzt angewendeten Pose des Mitspielers pi)
void NetPoseToPose(int pi, const posecodec::NetPose& np, Pose& out) {
    static float prevBonesArr[kMaxRemote][kMaxBones][8];
    static uint32_t prevCountArr[kMaxRemote] = {};
    float (&prevBones)[kMaxBones][8] = prevBonesArr[pi];
    uint32_t& prevCount = prevCountArr[pi];
    out.time = GetTickCount();
    out.numBones = np.numBones;
    posecodec::QuatToMatrix(np.rootRot, out.world);
    out.world[12] = np.rootPos[0]; out.world[13] = np.rootPos[1]; out.world[14] = np.rootPos[2]; out.world[15] = 1.0f;
    for (uint32_t b = 0; b < np.numBones; ++b) {
        float* d = out.bones + b * 16;
        std::memcpy(d, np.boneRot[b], 16);
        d[4] = np.bonePos[b][0]; d[5] = np.bonePos[b][1]; d[6] = np.bonePos[b][2]; d[7] = 0.0f;
        if (prevCount == np.numBones) std::memcpy(d + 8, prevBones[b], 32);
        else std::memcpy(d + 8, d, 32);
        std::memcpy(prevBones[b], d, 32);
    }
    prevCount = np.numBones;
}

void RunGameThreadCommands(uintptr_t gp);

// Spiel-Thread: Pose, Aussehen, Waffen und Schuesse des Mitspielers pi auf seine Figur p2
void ApplyRemotePlayer(int pi, uintptr_t p1, uintptr_t p2, const GhostConfig& cfg) {
    static posecodec::NetPose np;
    static Pose pose;
    if (!g_remoteActive[pi] || !SampleRemotePose(pi, np)) return;
    NotePoseBlocks(np);  // Schiebebloecke zeitgleich mit der Figur dessen, der schiebt
    g_remoteSector[pi] = np.sector;
    // nicht waehrend eines Levelwechsels: die Figur haengt dann noch am alten Level (Neuaufbau warf eine Ausnahme)
    if (np.form >= 0 && g_cvarGetBoolOrig && g_avatarOwner[pi] == p1) UpdatePlayer2Looks(pi, p2, np.form, np.looks);
    const size_t ownBones = *reinterpret_cast<size_t*>(p2 + 0x368 + 0x188);
    const bool mismatch = np.numBones != ownBones;
    if (mismatch != g_remoteModelMismatch[pi].load()) {
        g_remoteModelMismatch[pi] = mismatch;
        if (mismatch) Log(L("\n[Netz] Mitspieler %d hat ein anderes Modell (%u statt %zu Knochen) - ausgeblendet\n", "\n[Net] player %d has a different model (%u instead of %zu bones) - hidden\n"), pi + 1, np.numBones, ownBones);
        else Log(L("\n[Netz] Modell von Mitspieler %d passt wieder - sichtbar\n", "\n[Net] model of player %d matches again - visible\n"), pi + 1);
    }
    if (mismatch) return;
    NetPoseToPose(pi, np, pose);
    if (!ApplyPoseGuarded(p2, pose, cfg.offset)) {
        Log(L("[Spieler2] Netz-Pose anwenden fehlgeschlagen - Figur %d abgeschaltet\n", "[Player2] applying network pose failed - figure %d disabled\n"), pi + 1);
        g_avatar[pi] = 0;
        return;
    }
    AcquireSRWLockExclusive(&g_handsLock);
    std::memcpy(g_avatarHands[pi], np.hands, sizeof(g_avatarHands[pi]));
    ReleaseSRWLockExclusive(&g_handsLock);
    static int lastTypes[kMaxRemote][posecodec::kNumHands] = {{-2, -2}, {-2, -2}, {-2, -2}};
    if (np.hands[0].type != lastTypes[pi][0] || np.hands[1].type != lastTypes[pi][1]) {
        lastTypes[pi][0] = np.hands[0].type;
        lastTypes[pi][1] = np.hands[1].type;
        Log(L("[Waffen] Mitspieler %d haelt: links %s, rechts %s\n", "[Weapons] player %d holds: left %s, right %s\n"), pi + 1, ItemName(lastTypes[pi][0]), ItemName(lastTypes[pi][1]));
    }
    ReplayRemoteShots(pi, p2, cfg.offset);
}

void HookedPlayerBones(void* player) {
    const uintptr_t p1 = reinterpret_cast<uintptr_t>(player);
    const uintptr_t current = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (p1 != current) {  // Diagnose (Kex-Assert array.h:1205 in der Hand-Animation): wen aktualisiert das Spiel sonst?
        static std::map<uintptr_t, bool> logged;
        bool avatar = false;
        for (int pi = 0; pi < kMaxRemote; ++pi)
            if (g_avatar[pi] == p1) avatar = true;
        if (!avatar && !logged[p1]) {
            logged[p1] = true;
            Log(L("[Diagnose] Spiel aktualisiert Spielerfigur 0x%llX (nicht du, keine aktuelle Mitspieler-Figur)\n", "[Diagnosis] game updates player figure 0x%llX (not you, no current player figure)\n"),
                static_cast<unsigned long long>(p1));
        }
    }
    g_playerBonesOrig(player);
    if (p1 != current) return;
    RunGameThreadCommands(p1);
    RecordPose(p1);
    if (net::IsRunning()) QueueOutgoingPose(p1);
    const GhostConfig& cfg = g_ghostCfg[g_ghostCfgIdx.load()];
    if (net::IsRunning()) {  // Netzwerk-Modus: Pose jedes Mitspielers auf seine Figur
        for (int pi = 0; pi < kMaxRemote; ++pi) {
            const uintptr_t p2 = g_avatar[pi];
            if (p2) ApplyRemotePlayer(pi, p1, p2, cfg);
        }
        return;
    }
    const uintptr_t p2 = g_avatar[0];
    if (!p2 || !g_localTestFigure) return;  // uebrige Netz-Figur (letzter Mitspieler gegangen): nicht als Testfigur benutzen
    if (cfg.streamDelayMs >= 0) {  // Verfolger-Modus: aufgezeichnete Pose abspielen
        if (const Pose* pose = FindDelayedPose(cfg.streamDelayMs))
            if (!ApplyPoseGuarded(p2, *pose, cfg.offset)) { Log(L("[Spieler2] Pose anwenden fehlgeschlagen - abgeschaltet\n", "[Player2] applying pose failed - disabled\n")); g_avatar[0] = 0; }
        return;
    }
    std::memcpy(g_player2Offset, cfg.offset, sizeof(g_player2Offset));  // aus smcoop_ghost.txt
    PlacePlayer2(p1, p2);
    if (!MirrorAnimGuarded(p1, p2)) { Log(L("[Spieler2] Animation spiegeln fehlgeschlagen - abgeschaltet\n", "[Player2] mirroring animation failed - disabled\n")); g_avatar[0] = 0; return; }
    // Zweimal: UpdateBones kopiert zuerst "aktuell" nach "Vorframe". Die gespiegelten Gelenkdaten tragen noch
    // die Weltlage von Spieler 1; nach dem ersten Durchlauf stimmt "aktuell", der zweite macht "Vorframe" passend.
    // Sonst sieht das Spiel einen Sprung um den Versatz und verwischt Spieler 2 (Bewegungsunschaerfe).
    if (!UpdatePlayer2BonesGuarded(p2) || !UpdatePlayer2BonesGuarded(p2)) {
        Log(L("[Spieler2] Knochen-Update fehlgeschlagen - abgeschaltet\n", "[Player2] bone update failed - disabled\n"));
        g_avatar[0] = 0;
    }
}

void HookPlayerBones() {
    static const uint8_t expected[17] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x10,
                                         0x81, 0x30, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xD9};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kPlayerUpdateBonesRva), expected, sizeof(expected)) != 0) {
        Log(L("Hook PlayerBones: unerwartete Bytes - uebersprungen\n", "Hook PlayerBones: unexpected bytes - skipped\n"));
        return;
    }
    g_playerBonesOrig = reinterpret_cast<PlayerBonesFn>(
        InstallInlineHook(g_base + kPlayerUpdateBonesRva, sizeof(expected), reinterpret_cast<void*>(HookedPlayerBones)));
    Log(L("Hook PlayerBones: %s\n", "Hook PlayerBones: %s\n"), g_playerBonesOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// --- Spielstand: Speichern (vtable[2] = 0x461330) und Laden (vtable[3] = 0x4621B0) des Spielers beobachten ---
// Verdacht: Beim Speichern werden auch Spieler-2-Objekte in den Datenstrom geschrieben, beim Laden erwartet das
// Spiel aber nur einen Spieler -> der Rest des Spielstands wird verschoben gelesen (Spieler faellt ins Nichts).
using PlayerStreamFn = uint64_t (*)(void*, void*, void*, void*);
PlayerStreamFn g_playerSaveOrig = nullptr, g_playerLoadOrig = nullptr;

void LogStreamCall(const char* what, void* self) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(self);
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    void* frames[12] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
    char chain[256] = {};
    size_t len = 0;
    for (USHORT i = 0; i < n && len + 12 < sizeof(chain); ++i) {
        const uintptr_t f = reinterpret_cast<uintptr_t>(frames[i]);
        if (f >= g_base && f < g_base + 0x2000000) len += std::snprintf(chain + len, sizeof(chain) - len, " %llX", (unsigned long long)(f - g_base));
    }
    Log(L("[Spielstand] %s fuer 0x%llX (%s) | Aufrufer:%s\n", "[Savegame] %s for 0x%llX (%s) | callers:%s\n"), what, (unsigned long long)p,
        p == gp ? L("echter Spieler", "real player") : (IsOurPlayer2(p) ? L("SPIELER 2", "PLAYER 2") : L("anderes Spielerobjekt", "other player object")), chain);
}

// Globaler Objekt-Pool, den die Speicherroutine 0x328000 abarbeitet:
// [0xF5EEA8] -> Block { +0x00 naechster Block, +0x38 64 Eintraege zu je 0x50 Bytes (Eintrag +0x00 = Objekt) }
constexpr uintptr_t kObjectPoolRva = 0xF5EEA8;

[[maybe_unused]] void LogObjectPoolPlayers(const char* when) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    const uintptr_t playerVt = g_base + kPlayerVtableRva;
    uintptr_t block = 0;
    SafeRead(g_base + kObjectPoolRva, &block, 8);
    int blockNo = 0, found = 0;
    Log(L("[Pool] %s: echter Spieler 0x%llX\n", "[Pool] %s: real player 0x%llX\n"), when, (unsigned long long)gp);
    while (block && blockNo < 4096) {
        for (int i = 0; i < 64; ++i) {
            const uintptr_t entry = block + 0x38 + i * 0x50;
            uintptr_t obj = 0, vt = 0;
            if (!SafeRead(entry, &obj, 8) || !obj) continue;
            if (!SafeRead(obj, &vt, 8) || vt != playerVt) continue;
            uint32_t raw[20] = {};
            SafeRead(entry, raw, sizeof(raw));
            Log(L("  Block %d Eintrag %d (0x%llX): Objekt 0x%llX (%s) | Felder:", "  Block %d entry %d (0x%llX): object 0x%llX (%s) | fields:"), blockNo, i, (unsigned long long)entry,
                (unsigned long long)obj, obj == gp ? L("echter Spieler", "real player") : (IsOurPlayer2(obj) ? L("SPIELER 2", "PLAYER 2") : L("anderer Spieler", "other player")));
            for (int k = 2; k < 20; ++k) Log(L(" %08X", " %08X"), raw[k]);
            Log(L("\n", "\n"));
            ++found;
        }
        SafeRead(block, &block, 8);
        ++blockNo;
    }
    Log(L("  %d Spieler-Eintraege in %d Bloecken\n", "  %d player entries in %d blocks\n"), found, blockNo);
}

// Das Einrichten eines Spielers (Init 0x4596B0) biegt den festen Pool-Eintrag "der Spieler" auf sich selbst um.
// Nach dem Erzeugen von Spieler 2 zeigt er dadurch auf Spieler 2 -> der Spielstand enthaelt Spieler 2 statt des
// echten Spielers (beim Laden faellt man ins Nichts). Hier wird jeder Eintrag, der auf eins unserer Spieler-2-Objekte
// zeigt, wieder auf den echten Spieler gesetzt.
int RestorePoolPlayerEntries(uintptr_t realPlayer) {
    uintptr_t block = 0;
    SafeRead(g_base + kObjectPoolRva, &block, 8);
    int fixed = 0, blockNo = 0;
    while (block && blockNo++ < 4096) {
        for (int i = 0; i < 64; ++i) {
            auto* entryObj = reinterpret_cast<uintptr_t*>(block + 0x38 + i * 0x50);
            if (*entryObj && IsOurPlayer2(*entryObj)) { *entryObj = realPlayer; ++fixed; }
        }
        block = *reinterpret_cast<uintptr_t*>(block);
    }
    return fixed;
}

// ---------------- Analyse: Befehlsdatei und Welt-Beobachter ----------------
// smcoop_cmd.txt im Spielordner wird jede Sekunde gelesen und danach geloescht. Die Befehle laufen im Spiel-Thread:
//   tp <serie> [h]              eigenen Spieler zum Objekt mit dieser Pool-Seriennummer versetzen (h Einheiten hoeher)
//   tpxyz <x> <y> <z> <sektor>  eigenen Spieler an eine Position versetzen (Sektor-Index wie Objekt +0xD4)
//   face <serie>                eigenen Spieler zum Objekt drehen
//   watch on|off                Welt-Beobachter: Aenderungen in Tueren, Toren, Pickups usw. ins Log
SRWLOCK g_cmdLock = SRWLOCK_INIT;
std::vector<std::string> g_cmds;

void ReadCommandFile() {
    char name[32] = "smcoop_cmd.txt";  // weitere Instanz (Test): eigene Datei
    if (g_instance > 0) std::snprintf(name, sizeof(name), "smcoop_cmd%d.txt", g_instance + 1);
    FILE* f = std::fopen(name, "r");
    if (!f) return;
    char line[256];
    AcquireSRWLockExclusive(&g_cmdLock);
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty()) g_cmds.push_back(s);
    }
    ReleaseSRWLockExclusive(&g_cmdLock);
    std::fclose(f);
    DeleteFileA(name);
}

// Pool-Eintrag (Knoten, 0x50 Bytes): +0x10 Definition, +0x18 Rueckruf, +0x20 Typ, +0x24 Zustand (0x10 = geloescht),
// +0x28 Flags, +0x2C belegt, +0x30 Objekt, +0x38 Seriennummer = (Block-Nr. - 1) * 64 + Eintrag + 1 (stabil; neue Bloecke
// werden vorne in die Liste gehaengt, die Listenposition aendert sich also), +0x40 Block-Nr.
// Ruft fn(node, obj, serial) fuer jeden belegten Eintrag auf.
template <typename Fn>
void ForEachPoolNode(Fn fn) {
    uintptr_t block = 0;
    SafeRead(g_base + kObjectPoolRva, &block, 8);
    for (int b = 0; block && b < 4096; ++b) {
        for (int e = 0; e < 64; ++e) {
            const uintptr_t node = block + 8 + e * 0x50;
            uintptr_t obj = 0;
            uint32_t serial = 0;
            if (!SafeRead(node + 0x30, &obj, 8) || !obj || !SafeRead(node + 0x38, &serial, 4)) continue;
            fn(node, obj, serial);
        }
        SafeRead(block, &block, 8);
    }
}

uintptr_t PoolNodeBySerial(uint32_t serial) {
    uintptr_t found = 0;
    ForEachPoolNode([&](uintptr_t node, uintptr_t, uint32_t s) { if (s == serial) found = node; });
    return found;
}

std::string ClassNameOf(uintptr_t obj) {
    static std::map<uintptr_t, std::string> cache;
    uintptr_t vt = 0, col = 0;
    if (!SafeRead(obj, &vt, 8)) return "?";
    auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    uint32_t td = 0;
    char name[96] = {};
    std::string n = "?";
    if (SafeRead(vt - 8, &col, 8) && SafeRead(col + 0x0C, &td, 4) && SafeRead(g_base + td + 0x10, name, sizeof(name) - 1)) {
        n = name;
        if (n.rfind(".?AV", 0) == 0) n = n.substr(4);
        if (n.size() > 2 && n.compare(n.size() - 2, 2, "@@") == 0) n.resize(n.size() - 2);
        if (n.rfind("kexShadowMan", 0) == 0) n = n.substr(12);
    }
    cache[vt] = n;
    return n;
}

// Spieler versetzen wie der Konsolenbefehl "setpos" (0x468990), aber ohne dessen Cheat-Markierung (0x2EA6F0):
// 0x460E40(Spieler, &Position, Sektor-Index)
constexpr uintptr_t kPlayerSetPosRva = 0x460E40;
// sectorIndex = Objekt +0xD4 (Index in das Sektor-Array [0xF5ED78], je 0x210 Bytes); setpos uebergibt Sektor +0x1CC.
void TeleportPlayerGuarded(uintptr_t p, const float target[3], int sectorIndex) {
    __try {
        const uintptr_t arr = *reinterpret_cast<uintptr_t*>(g_base + 0xF5ED78);
        const uintptr_t sectors = *reinterpret_cast<uintptr_t*>(arr);
        const uint32_t count = *reinterpret_cast<uint32_t*>(arr + 0xC);
        if (sectorIndex < 0 || static_cast<uint32_t>(sectorIndex) >= count) { Log(L("[Befehl] Sektor %d ungueltig\n", "[Command] sector %d invalid\n"), sectorIndex); return; }
        const int sectorArg = *reinterpret_cast<int*>(sectors + static_cast<size_t>(sectorIndex) * 0x210 + 0x1CC);
        Log(L("[Befehl] Sektor-Index %d -> id %d, Wert +0x1CC %d\n", "[Command] sector index %d -> id %d, value +0x1CC %d\n"), sectorIndex,
            *reinterpret_cast<int*>(sectors + static_cast<size_t>(sectorIndex) * 0x210 + 0x1C8), sectorArg);
        float pos[3] = {target[0], target[1], target[2]};
        reinterpret_cast<void (*)(void*, float*, int)>(g_base + kPlayerSetPosRva)(reinterpret_cast<void*>(p), pos, sectorArg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(L("[Befehl] Versetzen fehlgeschlagen (Ausnahme)\n", "[Command] moving failed (exception)\n"));
    }
}

// Welt-Beobachter: Objekte bestimmter Klassen in Spielernaehe, alle 250 ms verglichen (4-Byte-Werte bis +0x400)
constexpr size_t kWatchBytes = 0x400;
struct WatchedObj {
    uintptr_t obj = 0;
    uint32_t serial = 0;
    int type = 0;
    std::string cls;
    uint8_t snap[kWatchBytes] = {};
    uint8_t changes[kWatchBytes / 4] = {};
    int logged = 0;
    bool seen = false;
};
std::map<uint32_t, WatchedObj> g_watched;  // Schluessel: Seriennummer
bool g_worldWatch = false;

bool IsWatchedClass(const std::string& c) {
    static const char* kWords[] = {"Door", "Block", "Coffin", "Quest", "DarkSoul", "LightSoul", "Gate", "Panel", "Piston", "Plank",
                                   "Lift", "Cage", "Lock", "Lever", "Altar", "Hatch", "Valve", "Switch", "Ring", "Ziggurat"};
    if (c == "Object") return true;
    for (const char* w : kWords)
        if (c.find(w) != std::string::npos) return true;
    return false;
}

void WorldWatchTick(uintptr_t gp) {
    float me[3] = {};
    SafeRead(gp + 0x20, me, sizeof(me));
    for (auto& kv : g_watched) kv.second.seen = false;
    static uint8_t buf[kWatchBytes];
    ForEachPoolNode([&](uintptr_t node, uintptr_t obj, uint32_t serial) {
        if (obj == gp) return;
        auto it = g_watched.find(serial);
        if (it != g_watched.end() && it->second.obj != obj) {  // Platz neu belegt
            Log(L("[Welt] %s #%u ersetzt durch %s\n", "[World] %s #%u replaced by %s\n"), it->second.cls.c_str(), serial, ClassNameOf(obj).c_str());
            g_watched.erase(it);
            it = g_watched.end();
        }
        if (it == g_watched.end()) {
            const std::string cls = ClassNameOf(obj);
            if (!IsWatchedClass(cls)) return;
            float pos[3] = {};
            SafeRead(obj + 0x20, pos, sizeof(pos));
            const float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
            if (dx * dx + dy * dy + dz * dz > 3000.0f * 3000.0f) return;
            WatchedObj w;
            w.obj = obj;
            w.serial = serial;
            w.cls = cls;
            SafeRead(node + 0x20, &w.type, 4);
            if (!SafeRead(obj, w.snap, kWatchBytes)) return;
            it = g_watched.emplace(serial, w).first;
        }
        WatchedObj& w = it->second;
        w.seen = true;
        if (!SafeRead(obj, buf, kWatchBytes)) return;
        for (size_t i = 0; i < kWatchBytes / 4; ++i) {
            uint32_t o, n;
            std::memcpy(&o, w.snap + i * 4, 4);
            std::memcpy(&n, buf + i * 4, 4);
            if (o == n) continue;
            if (w.changes[i] < 255) ++w.changes[i];
            if (w.changes[i] <= 3 && w.logged < 60) {  // dauernd wechselnde Werte (Zeit, Animation) nur 3x
                ++w.logged;
                float fo, fn;
                std::memcpy(&fo, &o, 4);
                std::memcpy(&fn, &n, 4);
                Log(L("[Welt] %s(%s) #%u +0x%03zX: %08X -> %08X (%.3f -> %.3f)%s\n", "[World] %s(%s) #%u +0x%03zX: %08X -> %08X (%.3f -> %.3f)%s\n"), w.cls.c_str(), ItemName(w.type), serial, i * 4, o, n,
                    fo, fn, w.changes[i] == 3 ? " [ab jetzt still]" : "");
            }
        }
        std::memcpy(w.snap, buf, kWatchBytes);
    });
    for (auto it = g_watched.begin(); it != g_watched.end();) {
        if (!it->second.seen) {
            Log(L("[Welt] %s(%s) #%u verschwunden\n", "[World] %s(%s) #%u disappeared\n"), it->second.cls.c_str(), ItemName(it->second.type), it->first);
            it = g_watched.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------- Welt-Sync: Pickups ----------------
// Pickups (Cadeaux, Munition, Gesundheit, Dunkle Seelen) sind Pool-Objekte. Die Seriennummer ist auf
// beiden PCs gleich, solange das Level gleich aufgebaut ist; zur Sicherheit muessen auch Typ und Ort passen.
// Verschwindet ein Pickup, waehrend der eigene Spieler daneben steht, hat er es eingesammelt. Jede Seite schickt jede
// Sekunde die Liste ihrer in diesem Level eingesammelten Pickups ("SMW1"); der Empfaenger wendet sie an:
//   geteilt:  das Objekt wird auf den eigenen Spieler gesetzt und sein Rueckruf aufgerufen -> das Spiel sammelt es
//             ganz normal ein (Inventar, Levelzaehler, Fortschritt, Geraeusch)
//   entfernen: nur entfernen (0x312650), der andere bekommt nichts
constexpr uintptr_t kObjectNodeCallbackRva = 0x3115C0;  // Rueckruf der Object/QuestObject-Knoten (ruft Definition +0x78)
constexpr uintptr_t kRemoveNodeRva         = 0x312650;  // Objekt samt Knoten entfernen (so endet das Aufsammeln)
constexpr uintptr_t kDarkSoulVtableRva     = 0x6EA838;
constexpr uintptr_t kObjectVtableRva       = 0x6EA588;  // kexShadowManObject (Cadeaux, Munition, Gesundheit, ...)
constexpr float     kPickupNearMe          = 300.0f;    // so nah muss ich sein, damit ein Verschwinden "meins" ist
constexpr float     kPickupSamePlace       = 300.0f;    // so weit darf die Position auf beiden PCs abweichen
enum class PickupMode { Shared, Remove };
PickupMode g_pickupMode = PickupMode::Shared;

bool IsCollectibleType(int t) {
    return (t >= 1 && t <= 33) || t == 0x162 || t == 0x163 || t == 0x220 || t == 0x221 || t == 0x230 || t == 0x261 || t == 0x293;
}

struct PoolNodeInfo {
    uintptr_t node, obj, vt, cb;
    uint32_t serial;
    int type, state;
    float pos[3];
};
constexpr int kMaxPoolNodes = 8192;
PoolNodeInfo g_poolSnap[kMaxPoolNodes];

// Ein Pool-Eintrag in i: 1 = aufgenommen, 0 = leer, -1 = nicht lesbar (Objekt gerade freigegeben)
int ReadPoolEntryGuarded(uintptr_t node, PoolNodeInfo& i) {
    __try {
        const uintptr_t obj = *reinterpret_cast<uintptr_t*>(node + 0x30);
        if (!obj) return 0;
        i.node = node;
        i.obj = obj;
        i.vt = *reinterpret_cast<uintptr_t*>(obj);
        i.cb = *reinterpret_cast<uintptr_t*>(node + 0x18);
        i.serial = *reinterpret_cast<uint32_t*>(node + 0x38);
        i.type = *reinterpret_cast<int*>(node + 0x20);
        i.state = *reinterpret_cast<int*>(node + 0x24);
        std::memcpy(i.pos, reinterpret_cast<void*>(obj + 0x20), sizeof(i.pos));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int g_poolSkipped = 0;  // Diagnose: nicht lesbare Eintraege im letzten Schnappschuss

// Schneller Schnappschuss aller belegten Pool-Eintraege (direkte Lesezugriffe, je Eintrag abgesichert - siehe coop::WalkPool)
int SnapshotPoolGuarded() {
    uintptr_t first = 0;
    if (!SafeRead(g_base + kObjectPoolRva, &first, 8)) return 0;
    auto next = [](uintptr_t block, uintptr_t* out) { return SafeRead(block, out, 8); };
    auto entry = [](uintptr_t node, int n) { return ReadPoolEntryGuarded(node, g_poolSnap[n]); };
    return coop::WalkPool(first, kMaxPoolNodes, next, entry, &g_poolSkipped);
}

struct TrackedPickup {
    uintptr_t node = 0, obj = 0, vt = 0;
    int type = 0;
    float pos[3] = {};
    bool seen = false;
};
std::map<uint32_t, TrackedPickup> g_trackedPickups;
SRWLOCK g_worldLock = SRWLOCK_INIT;
std::vector<posecodec::PickupInfo> g_localPickups;   // in diesem Level von mir eingesammelt (Netz-Thread liest)
std::vector<posecodec::PickupInfo> g_remotePickups[kMaxRemote];  // Liste je Mitspieler (Netz-Thread schreibt)
uint16_t g_remotePickupsLevel[kMaxRemote] = {};
uint16_t g_localPickupsLevel = 0;
uintptr_t g_worldPlayer = 0;
int g_worldLevel = -1;

// Die Aufsammel-Pruefung (z.B. Cadeaux 0x3397D0) testet, ob sich der Begrenzungsrahmen des Spielers (+0x1D644, min/max)
// mit dem des Objekts (+0xD8..+0xEC) bzw. dessen Position (+0x20) ueberschneidet. Fuer einen Aufruf des Objekt-Rueckrufs
// legen wir den Spieler-Rahmen ueber das Objekt und stellen ihn danach wieder her -> das Spiel sammelt es ganz normal ein.
constexpr uint32_t kPlayerBoxOff = 0x1D644;
// Dunkle Seelen pruefen dagegen die Spielerposition (+0x20) -> fuer den Aufruf auch die auf das Objekt setzen.
bool ProxyCollectGuarded(uintptr_t gp, const TrackedPickup& t) {
    float saved[6], savedPos[3];
    float* box = reinterpret_cast<float*>(gp + kPlayerBoxOff);
    float* ppos = reinterpret_cast<float*>(gp + 0x20);
    // Die Aufsammel-Logik laeuft nur in der Aktualisierungs-Phase (GameState +0x94 == 0, z.B. 0x311B10 fuer Quest-
    // Gegenstaende); wir laufen im Knochen-Update -> Phase fuer diesen Aufruf auf 0 setzen.
    int* phase = reinterpret_cast<int*>(g_base + kGameStateRva + 0x94);
    const int savedPhase = *phase;
    bool restored = true;
    __try {
        std::memcpy(saved, box, sizeof(saved));
        std::memcpy(savedPos, ppos, sizeof(savedPos));
        restored = false;
        *phase = 0;
        const float* ob = reinterpret_cast<const float*>(t.obj + 0xD8);
        const float* op = reinterpret_cast<const float*>(t.obj + 0x20);
        for (int i = 0; i < 3; ++i) {
            box[i] = (ob[i] < op[i] ? ob[i] : op[i]) - 8.0f;
            box[3 + i] = (ob[3 + i] > op[i] ? ob[3 + i] : op[i]) + 8.0f;
            ppos[i] = op[i];
        }
        // Typ-Logik der Gegenstandsdefinition (+0x78) direkt aufrufen. Der Knoten-Rueckruf 0x3115C0 tut das nur in der
        // Aktualisierungs-Phase (GameState +0x94 == 0), wir laufen aber im Knochen-Update.
        const uintptr_t def = *reinterpret_cast<uintptr_t*>(t.node + 0x10);
        const uintptr_t think = def ? *reinterpret_cast<uintptr_t*>(def + 0x78) : 0;
        const uintptr_t cb = *reinterpret_cast<uintptr_t*>(t.node + 0x18);
        if (think) reinterpret_cast<void (*)(void*)>(think)(reinterpret_cast<void*>(t.node));
        else if (cb) reinterpret_cast<void (*)(void*)>(cb)(reinterpret_cast<void*>(t.node));
        std::memcpy(box, saved, sizeof(saved));
        std::memcpy(ppos, savedPos, sizeof(savedPos));
        *phase = savedPhase;
        restored = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!restored) { std::memcpy(box, saved, sizeof(saved)); std::memcpy(ppos, savedPos, sizeof(savedPos)); *phase = savedPhase; }
        return false;
    }
}

bool RemoveNodeGuarded(uintptr_t node) {
    __try {
        reinterpret_cast<void (*)(void*)>(g_base + kRemoveNodeRva)(reinterpret_cast<void*>(node));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Behaelter (Govis mit Dunklen Seelen, Toepfe mit Cadeaux) ----
// Zerbricht ein Behaelter, waehrend beide im Level sind, trifft der uebertragene Schuss ihn auch beim anderen. War der andere
// nicht da (oder ging der Schuss daneben), steht sein Behaelter noch -> er koennte den Inhalt ein zweites Mal holen.
// Deshalb fuehrt jede Seite eine Liste der in diesem Level verschwundenen Behaelter. Existiert ein Behaelter aus der Liste
// des Mitspielers hier noch, wird er mit einem lautlosen Shadowgun-Geschoss zerstoert; sein Inhalt entsteht dann ganz
// normal und wird ueber die Pickup-Liste (Typ + Ort) automatisch mit eingesammelt, falls der Mitspieler ihn schon hat.
constexpr uintptr_t kGoviVtableRva          = 0x70C400;  // kexShadowManAIGovi
constexpr uintptr_t kExplodeObjectVtableRva = 0x71F7D0;  // kexShadowManExplodeObject (Toepfe)
// kexShadowManCalabashObject: Veve-Platte, Unterart von ExplodeObject (gleiches Zerbrechen vt[6]), nur die Calabash-Explosion
// zerbricht sie. Laeuft als zerbrechliches Objekt mit: so ist sie auch bei einem weit entfernten Mitspieler gleich weg.
constexpr uintptr_t kCalabashObjectVtableRva = 0x720B88;
constexpr float     kContainerSamePlace     = 100.0f;  // gleiche Seriennummer: so nah muss der Behaelter hier stehen
constexpr float     kContainerMatchPlace    = 32.0f;   // andere Seriennummer: derselbe Behaelter steht hier genau dort
struct TrackedContainer {
    uintptr_t node = 0, obj = 0, vt = 0;
    int kind = 0;  // 1 Govi, 2 zerbrechliches Objekt (ExplodeObject: Topf, Fass, Kiste, Bretter, Moebel ...)
    int type = 0;  // Gegenstands-Typ (Modell), nur fuers Log
    int sector = 0;
    float pos[3] = {};
    bool seen = false;
};
std::map<uint32_t, TrackedContainer> g_trackedContainers;
std::vector<posecodec::PickupInfo> g_localBroken;    // hier verschwundene Behaelter (Netz-Thread liest)
std::vector<posecodec::PickupInfo> g_remoteBroken[kMaxRemote];  // Liste je Mitspieler (Netz-Thread schreibt)
struct BreakAttempt { int tries = 0; DWORD last = 0; };
std::map<uint32_t, BreakAttempt> g_breakAttempts;    // lokale Seriennummer -> Versuche

const char* ContainerName(const TrackedContainer& c) {
    if (c.kind == 1) return "Govi";
    return c.vt == g_base + kCalabashObjectVtableRva ? L("Calabash-Platte", "Calabash plate") : ItemName(c.type);
}

constexpr uintptr_t kExplodePotRva = 0x42D7D0;  // ExplodeObject vtable[6]: zerbrechen (Effekte, Scherben, Inhalt, Knoten weg)
bool ExplodePotGuarded(uintptr_t obj) {
    __try {
        reinterpret_cast<void (*)(void*)>(g_base + kExplodePotRva)(reinterpret_cast<void*>(obj));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Govis lassen sich aus grosser Entfernung nicht treffen (dort ist nichts aktiv) -> erst versuchen, wenn der eigene
// Spieler in der Naehe ist; er kann den Govi vorher ohnehin nicht erreichen. Toepfe gehen ueberall (direkter Aufruf).
constexpr float kGoviBreakRange = 2500.0f;
void BreakRemoteBrokenContainers(const float me[3]) {
    if (!g_spawnProjOrig) return;
    std::vector<posecodec::PickupInfo> remote;
    AcquireSRWLockShared(&g_worldLock);
    for (int pi = 0; pi < kMaxRemote; ++pi)
        if (g_remoteActive[pi] && g_remotePickupsLevel[pi] == static_cast<uint16_t>(g_worldLevel))
            remote.insert(remote.end(), g_remoteBroken[pi].begin(), g_remoteBroken[pi].end());
    ReleaseSRWLockShared(&g_worldLock);
    if (remote.empty()) return;
    std::vector<coop::ContainerRef> here;
    here.reserve(g_trackedContainers.size());
    for (const auto& kv : g_trackedContainers)
        here.push_back({kv.first, kv.second.kind, {kv.second.pos[0], kv.second.pos[1], kv.second.pos[2]}});
    for (const auto& r : remote) {
        // Zuordnung ueber Nummer + Ort, sonst ueber den Ort (Seriennummern je PC verschieden, je nach Ladeweg)
        const int idx = coop::MatchContainer(here.data(), static_cast<int>(here.size()),
                                             {r.serial, r.type, {r.pos[0], r.pos[1], r.pos[2]}}, kContainerSamePlace, kContainerMatchPlace);
        if (idx < 0) continue;  // hier schon weg
        const uint32_t serial = here[idx].serial;
        const TrackedContainer& c = g_trackedContainers[serial];
        if (c.kind == 1) {
            const float mx = c.pos[0] - me[0], my = c.pos[1] - me[1], mz = c.pos[2] - me[2];
            if (mx * mx + my * my + mz * mz > kGoviBreakRange * kGoviBreakRange) continue;  // spaeter, wenn ich nah bin
        }
        BreakAttempt& a = g_breakAttempts[serial];
        if (a.tries >= 40 || GetTickCount() - a.last < 400) continue;
        if (a.tries == 0) Log(L("[Welt] %s (#%u) ist beim Mitspieler zerstoert - zerstoere es hier auch\n", "[World] %s (#%u) was destroyed by the other player - destroying it here too\n"), ContainerName(c), serial);
        const int attempt = a.tries++;
        a.last = GetTickCount();
        bool ok = true;
        if (c.kind == 2) {
            ok = ExplodePotGuarded(c.obj);  // Topf: die Zerbrech-Funktion des Spiels direkt (wie bei einem Treffer)
        } else {
            // Govi: waagerechter Shadowgun-Schuss von der Seite (Richtung wechselt pro Versuch), volle Staerke, ohne Ton
            static const float kDirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
            const float* d = kDirs[attempt % 4];
            posecodec::ShotEvent s;
            s.type = 1;
            s.sector = static_cast<int16_t>(c.sector);
            s.dir[0] = d[0] * 50.0f;
            s.dir[2] = d[1] * 50.0f;
            s.power = 10000;
            const float pos[3] = {c.pos[0] - d[0] * 120.0f, c.pos[1] + 30.0f, c.pos[2] - d[1] * 120.0f};
            void* owner = nullptr;
            for (int pi = 0; pi < kMaxRemote && !owner; ++pi)
                if (const uintptr_t a = g_avatar[pi]) SafeRead(a + 0x1A8, &owner, 8);
            g_replayVolume = 0.0f;
            g_replayingShot = true;
            ok = ReplayShotGuarded(owner, s, pos);
            g_replayingShot = false;
        }
        if (!ok) { a.tries = 99; Log(L("[Welt] Zerstoeren fehlgeschlagen (Ausnahme)\n", "[World] destroying failed (exception)\n")); }
    }
}

// ---- Schiebebloecke ----
// Zustand +0x290: siehe coop::BlockMoving (Startposition +0x29C). Die Position steht in mehreren Kopien, die beim
// Schieben gemeinsam mitlaufen. Jede Seite schickt fuer verschobene Bloecke Position + Version; die Version zaehlt bei jedem
// eigenen Schieben hoch. Der andere uebernimmt eine neuere Version (oder die gleiche, wenn er sie nicht selbst erzeugt hat),
// aber nur, solange sein Block ruht, und versetzt dann alle Positionskopien. Nachzuegler bekommen so den Endzustand.
// Dabei bekommt der Block hier sofort den Fertig-Zustand (wie beim Schieber): sonst ist er hier noch anfassbar, rutscht in
// die eigene Figur, das Spiel startet einen zweiten Schub und setzt die Figur an die Blockkante (Spieler wird "angezogen",
// Block landet zwei Felder weiter - beim Schieber in ihm drin).
// Klassen: PushBlock 0x722A98, PushGad3Block 0x722A18 (Gad-Tempel), PushLavaBlock 0x722A58 (gleicher Aufbau, teilen sich
// die vtable bis auf Zerstoeren/Update). Zuordnung ueber BlockId16 (Grundposition +0x38 + Art +0xD0; +0x38 schreibt das
// Schieben nicht) - die Pool-Seriennummern unterscheiden sich zwischen den PCs je nach Ladeweg.
constexpr uintptr_t kPushBlockVtableRva = 0x722A98;
constexpr uintptr_t kPushGad3BlockVtableRva = 0x722A18;
constexpr uintptr_t kPushLavaBlockVtableRva = 0x722A58;
constexpr uint32_t  kPushStateOff = 0x290;
static const uint32_t kBlockPosCopies[] = {0x20, 0x80, 0xC0, 0xD8, 0xE4, 0xF0, 0x160, 0x1F0, 0x230};
struct TrackedBlock {
    uintptr_t obj = 0;
    float pos[3] = {};
    int state = 0;
    uint16_t version = 0;        // zuletzt bekannter Stand
    uint16_t remoteVersion = 0;  // hoechste vom Mitspieler gesehene Version
    bool owner = false;          // diese Version stammt von uns
    bool seen = false;
    bool gad3 = false;           // PushGad3Block (andere Zustaende)
    uint16_t waitLogged = 0;     // Diagnose: Mitspieler-Stand, fuer den "bewegt sich hier gerade" schon im Log steht
};
std::map<uint32_t, TrackedBlock> g_trackedBlocks;
std::vector<posecodec::PickupInfo> g_localBlocks;   // ruhende verschobene Bloecke fuers Welt-Paket (Netz-Thread liest)
// Zielstand je Block vom Mitspieler: aus dem Welt-Paket (ruhend) und aus jeder Pose (gerade in Bewegung)
struct BlockTarget { uint16_t version = 0; float pos[3] = {}; };
std::map<uint32_t, BlockTarget> g_blockTargets;     // Netz-Thread schreibt, Spiel-Thread liest (g_worldLock)
posecodec::BlockState g_movingBlocks[posecodec::kMaxMovingBlocks];  // eigene, gerade geschobene Bloecke (Spiel-Thread)
int g_numMovingBlocks = 0;
// Bewegte Bloecke aus der abgespielten (interpolierten) Pose des Mitspielers - laufen genau mit seiner Figur (Spiel-Thread)
struct PoseBlockTarget { uint16_t version = 0; float pos[3] = {}; DWORD time = 0; };
std::map<uint32_t, PoseBlockTarget> g_poseBlockTargets;
constexpr DWORD kPoseBlockFreshMs = 300;  // so lange nach der letzten Pose mit dem Block gilt sie (danach Welt-Paket)

void NotePoseBlocks(const posecodec::NetPose& np) {
    const DWORD now = GetTickCount();
    static std::map<uint32_t, bool> unknownLogged;  // Diagnose: einmal je Kennung
    for (int k = 0; k < np.numBlocks; ++k) {
        const uint32_t id = np.blocks[k].serial;
        if (!g_trackedBlocks.count(id) && !unknownLogged[id]) {
            unknownLogged[id] = true;
            Log(L("[Welt] Mitspieler schiebt Block #%u - hier kein Schiebeblock mit dieser Kennung\n", "[World] other player pushes block #%u - no push block with this id here\n"), id);
        }
        PoseBlockTarget& t = g_poseBlockTargets[np.blocks[k].serial];
        t.version = np.blocks[k].version;
        std::memcpy(t.pos, np.blocks[k].pos, sizeof(t.pos));
        t.time = now;
    }
}

void SetBlockTargetLocked(uint32_t serial, uint16_t version, const float pos[3]) {
    BlockTarget& t = g_blockTargets[serial];
    if (static_cast<int16_t>(version - t.version) < 0) return;  // aelterer Stand
    t.version = version;
    std::memcpy(t.pos, pos, sizeof(t.pos));
}

bool ReadBlockGuarded(uintptr_t obj, float pos[3], int* state) {
    __try {
        std::memcpy(pos, reinterpret_cast<void*>(obj + 0x20), 12);
        *state = *reinterpret_cast<int*>(obj + kPushStateOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MoveBlockGuarded(uintptr_t obj, const float delta[3]) {
    __try {
        for (uint32_t off : kBlockPosCopies) {
            float* v = reinterpret_cast<float*>(obj + off);
            for (int i = 0; i < 3; ++i) v[i] += delta[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetBlockStateGuarded(uintptr_t obj, int state) {
    __try {
        *reinterpret_cast<int*>(obj + kPushStateOff) = state;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Spiel-Thread, jedes Bild: eigenes Schieben erkennen, bewegte Bloecke fuer die Pose sammeln, Mitspieler-Stand weich anfahren
void BlockFrameUpdate() {
    g_numMovingBlocks = 0;
    if (g_trackedBlocks.empty()) return;
    std::map<uint32_t, BlockTarget> targets;
    if (AnyRemoteActive()) {
        AcquireSRWLockShared(&g_worldLock);
        targets = g_blockTargets;
        ReleaseSRWLockShared(&g_worldLock);
    }
    for (auto& kv : g_trackedBlocks) {
        TrackedBlock& b = kv.second;
        int state = 0;
        if (!b.obj || !ReadBlockGuarded(b.obj, b.pos, &state)) continue;
        const bool moving = coop::BlockMoving(state, b.gad3);
        if (!coop::BlockMoving(b.state, b.gad3) && moving) {  // hier beginnt jemand zu schieben (das kann nur der eigene Spieler sein)
            b.version = static_cast<uint16_t>((b.version > b.remoteVersion ? b.version : b.remoteVersion) + 1);
            b.owner = true;
            Log(L("[Welt] Du schiebst Schiebeblock #%u (Stand %u)\n", "[World] you are pushing block #%u (version %u)\n"), kv.first, b.version);
        }
        b.state = state;
        if (moving && b.owner && g_numMovingBlocks < posecodec::kMaxMovingBlocks) {
            posecodec::BlockState& m = g_movingBlocks[g_numMovingBlocks++];
            m.serial = static_cast<uint16_t>(kv.first);
            m.version = b.version;
            std::memcpy(m.pos, b.pos, sizeof(m.pos));
        }
        // Ziel: Stand aus der abgespielten Pose (schon zeitlich geglaettet, passt zur Figur) vor dem Welt-Paket (Endstand)
        auto wt = targets.find(kv.first);
        auto pt = g_poseBlockTargets.find(kv.first);
        const bool hasWorld = wt != targets.end();
        const bool hasPose = pt != g_poseBlockTargets.end() && GetTickCount() - pt->second.time < kPoseBlockFreshMs;
        if (hasWorld && wt->second.version > b.remoteVersion) b.remoteVersion = wt->second.version;
        if (hasPose && pt->second.version > b.remoteVersion) b.remoteVersion = pt->second.version;
        const bool fromPose = hasPose && (!hasWorld || pt->second.version >= wt->second.version);
        if (!fromPose && !hasWorld) continue;
        const uint16_t v = fromPose ? pt->second.version : wt->second.version;
        const float* tp = fromPose ? pt->second.pos : wt->second.pos;
        const bool newer = v > b.version || (v == b.version && !b.owner);
        if (moving) {  // bewegt sich hier gerade -> nicht dazwischenfunken
            if (newer && b.waitLogged != v) {
                b.waitLogged = v;
                Log(L("[Welt] Schiebeblock #%u: Mitspieler-Stand %u, hier bewegt er sich gerade (Zustand %d) - warte\n", "[World] push block #%u: other player's version %u, moving here right now (state %d) - waiting\n"), kv.first, v, state);
            }
            continue;
        }
        if (!newer) continue;
        const int done = coop::BlockDoneState(b.gad3);
        if (state != done) {  // schon geschoben -> hier nicht mehr anfassbar (sonst zweiter Schub, Figur wird angezogen)
            if (!SetBlockStateGuarded(b.obj, done)) { Log(L("[Welt] Schiebeblock-Zustand setzen fehlgeschlagen (Ausnahme)\n", "[World] setting push block state failed (exception)\n")); continue; }
            Log(L("[Welt] Schiebeblock #%u: Zustand %d -> %d (vom Mitspieler geschoben, hier nicht mehr anfassbar)\n", "[World] push block #%u: state %d -> %d (pushed by the other player, no longer pushable here)\n"), kv.first, state, done);
            b.state = done;
        }
        float d[3] = {tp[0] - b.pos[0], tp[1] - b.pos[1], tp[2] - b.pos[2]};
        const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (dist2 > 0.01f) {
            // Welt-Paket: weich hingleiten (bei grossen Spruengen, z.B. Nachzuegler, direkt); Pose: direkt, ist schon weich
            if (!fromPose && dist2 > 4.0f && dist2 < 1000.0f * 1000.0f)
                for (float& c : d) c *= 0.25f;
            if (!MoveBlockGuarded(b.obj, d)) { Log(L("[Welt] Schiebeblock versetzen fehlgeschlagen (Ausnahme)\n", "[World] moving push block failed (exception)\n")); continue; }
            for (int i = 0; i < 3; ++i) b.pos[i] += d[i];
        }
        if (v != b.version) Log(L("[Welt] Schiebeblock #%u wird vom Mitspieler verschoben (Stand %u)\n", "[World] push block #%u is being moved by the other player (version %u)\n"), kv.first, v);
        b.version = v;
        b.owner = false;
    }
}

// ---- Mitfahr-Objekte ----
// Teleport-Aufzuege (AS2Lift, UGLift): Signal +0x120 = 3 mit Ziel +0x12C startet die Fahrt (ueber ein Ereignis). Am Ende
// versetzt das Update (vtable[4]) den eigenen Spieler (GetPlayer) und die Ansicht (0x2D4870 -> 0xF998D0: +0x550..+0x567,
// Sektor +0xA40 und +0x2030) um die Fahrtstrecke - ohne zu pruefen, ob er im Aufzug steht. Startete ein Mitspieler die
// Fahrt (Ereignis-Abgleich), sprang der eigene Spieler mit, egal wo er war (Level 22 getestet: durch die Welt gefallen).
// Schutz: Update umbiegen. Stand der eigene Spieler vorher nicht im Objekt (coop::RideContains, Box +0xD8/+0xE4) und hat
// ihn das Update versetzt, Spieler und Ansicht zuruecksetzen. Wer drinsteht, faehrt wie im Spiel mit.
// Objekte, die auf Beruehrung des eigenen Spielers reagieren (Kontaktliste Spieler +0x568, vtable[5]): steht die Figur eines
// Mitspielers darauf, bekommt das Objekt hier dieselbe Reaktion - sonst schwebt seine Figur ueber einer Saeule, die bei ihm
// laengst eingesunken ist, bzw. ueber einem Brett, das bei ihm gebrochen ist.
enum class RideTouch : uint8_t {
    None,
    SinkCounter,  // LavaPlatform: Beruehrung setzt +0x290 = 0x18 (sinkt, solange jemand draufsteht)
    Plank,        // Plank: Beruehrung in Zustand 0 -> 0x441DA0 (Tabelle 1002, bricht, Zustand 1)
};
struct RideClass {
    uintptr_t vtRva;
    const char* name;
    bool movesPlayer;  // Update versetzt GetPlayer selbst (sonst traegt ihn die Physik)
    bool movesCamera;  // ... und die Ansicht
    RideTouch touch;
};
// Nicht dabei (geprueft): CableCar (setzt den Spieler zur Seilbahn-Cutscene - die holt ohnehin alle), LifeAltar (eigene
// Benutz-Szene), SwampBoat (feste Bootsfahrt durchs Level, setzt jeden Spieler in sein Boot), G2Hammer (Falle).
const RideClass kRideClasses[] = {
    {0x71F2F0, "AS2Lift", true, true, RideTouch::None},
    {0x723BE8, "UGLift", true, true, RideTouch::None},
    {0x7212A8, "G1RingBottom", true, false, RideTouch::None},  // Ring hebt den Spieler bei jedem Schritt mit (Signal +0x120)
    {0x721768, "G2Ring", true, false, RideTouch::None},        // ebenso
    {0x721D20, "HookedCage", true, false, RideTouch::None},    // Kaefig zieht den Spieler mit, sobald Signal 3 kam (+0x294 = 1)
    {0x721FE0, "LavaPlatform", false, false, RideTouch::SinkCounter},
    {0x722728, "Plank", false, false, RideTouch::Plank},
};
constexpr int kSinkTouchFrames = 0x18;
constexpr uintptr_t kPlankBreakRva = 0x441DA0;
constexpr int kPlankBrokenValue = 1002;  // Tabellen-Eintrag eines gebrochenen Bretts
constexpr float kAvatarTouchSide = 16.0f, kAvatarTouchAbove = 64.0f;
constexpr size_t kRideClassCount = sizeof(kRideClasses) / sizeof(kRideClasses[0]);
constexpr float kRideSide = 64.0f, kRideAbove = 200.0f;
constexpr uintptr_t kViewStateRva = 0xF998D0;       // Rueckgabe von 0x2D4870 (Ansicht/Kamera des Spielers)
constexpr uintptr_t kPlayerRelinkRva = 0x29D590;    // Spieler: Teilobjekt +0x1B0 aktualisieren, Matrizen nach +0x130
constexpr uintptr_t kObjLightsDirtyRva = 0x3330F0;  // Lichter um ein Objekt neu
using ObjUpdateFn = void (*)(uintptr_t);
uint8_t PlayerDeadFlag(uintptr_t gp);
ObjUpdateFn g_rideUpdateOrig[kRideClassCount] = {};
std::map<uintptr_t, DWORD> g_rideLogged;  // Objekt -> letzte "bleibt stehen"-Meldung (Spiel-Thread)

struct RideSnapshot {
    uintptr_t gp = 0;
    int level = -1;
    bool inside = false;
    uint8_t player[0x170 - 0x20] = {};  // +0x20..+0x16F: Position, Winkel, Matrizen, Sektor +0xD4
    uint8_t view[0x18] = {};            // Ansicht +0x550..+0x567
    int32_t viewSector = 0, viewSector2 = 0;
};

bool TakeRideSnapshotGuarded(uintptr_t obj, uintptr_t gp, bool camera, RideSnapshot* s) {
    __try {
        float box[6];
        std::memcpy(box, reinterpret_cast<void*>(obj + 0xD8), sizeof(box));
        std::memcpy(s->player, reinterpret_cast<void*>(gp + 0x20), sizeof(s->player));
        s->gp = gp;
        s->inside = coop::RideContains(box, box + 3, reinterpret_cast<const float*>(gp + 0x20), kRideSide, kRideAbove);
        if (camera) {
            const uintptr_t v = g_base + kViewStateRva;
            std::memcpy(s->view, reinterpret_cast<void*>(v + 0x550), sizeof(s->view));
            s->viewSector = *reinterpret_cast<int32_t*>(v + 0xA40);
            s->viewSector2 = *reinterpret_cast<int32_t*>(v + 0x2030);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// true = Spieler war versetzt und ist zurueckgesetzt
bool RestoreRideSnapshotGuarded(const RideSnapshot& s, bool camera) {
    __try {
        if (std::memcmp(reinterpret_cast<void*>(s.gp + 0x20), s.player, 12) == 0) return false;  // nicht versetzt
        std::memcpy(reinterpret_cast<void*>(s.gp + 0x20), s.player, sizeof(s.player));
        reinterpret_cast<ObjUpdateFn>(g_base + kObjLightsDirtyRva)(s.gp);
        reinterpret_cast<ObjUpdateFn>(g_base + kPlayerRelinkRva)(s.gp);
        if (camera) {
            const uintptr_t v = g_base + kViewStateRva;
            std::memcpy(reinterpret_cast<void*>(v + 0x550), s.view, sizeof(s.view));
            *reinterpret_cast<int32_t*>(v + 0xA40) = s.viewSector;
            *reinterpret_cast<int32_t*>(v + 0x2030) = s.viewSector2;
            const uintptr_t cam = v + 0x9F0;  // wie das Aufzug-Update: [cam] vt[0x18](cam)
            reinterpret_cast<ObjUpdateFn>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(cam) + 0x18))(cam);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int GetStateGuarded(int type, const float pos[3]);

// Steht die Figur eines Mitspielers (im selben Level) auf dem Objekt?
bool AvatarOnRide(uintptr_t obj) {
    float box[6] = {};
    if (!SafeRead(obj + 0xD8, box, sizeof(box))) return false;
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        const uintptr_t a = g_avatar[pi];
        float pos[3] = {};
        if (!g_remoteActive[pi] || !a || !SafeRead(a + 0x20, pos, sizeof(pos))) continue;
        if (coop::RideContains(box, box + 3, pos, kAvatarTouchSide, kAvatarTouchAbove)) return true;
    }
    return false;
}

bool PlankBreakGuarded(uintptr_t obj) {
    __try {
        reinterpret_cast<ObjUpdateFn>(g_base + kPlankBreakRva)(obj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Beruehrungs-Reaktion fuer die Figur eines Mitspielers nachbilden (vor dem Update, wie die Kontaktpruefung im Spiel)
void ApplyRideTouch(uintptr_t obj, RideTouch touch) {
    int state = 0;
    if (!SafeRead(obj + 0x290, &state, 4)) return;
    if (touch == RideTouch::SinkCounter) {
        if (AvatarOnRide(obj)) SafeWrite(obj + 0x290, &kSinkTouchFrames, 4);
        return;
    }
    if (touch != RideTouch::Plank || state != 0) return;
    int type = 0;
    float base[3] = {};
    const bool onIt = AvatarOnRide(obj);
    // auch ohne Figur: laut Tabelle schon gebrochen (Mitspieler war hier, als ich weit weg war, oder Spielstand)
    const bool broken = !onIt && SafeRead(obj + 0xD0, &type, 4) && SafeRead(obj + 0x38, base, sizeof(base)) &&
                        GetStateGuarded(type, base) == kPlankBrokenValue;
    if (!onIt && !broken) return;
    const bool ok = PlankBreakGuarded(obj);
    Log(L("[Mitfahren] Brett bricht hier auch (%s)%s\n", "[Ride] plank breaks here too (%s)%s\n"), onIt ? L("Mitspieler steht drauf", "other player stands on it") : L("laut Tabelle schon gebrochen", "already broken according to the table"),
        ok ? "" : L(" - FEHLER (Ausnahme)", " - ERROR (exception)"));
}

void RideUpdate(uintptr_t obj, size_t idx) {
    const RideClass& rc = kRideClasses[idx];
    if (rc.touch != RideTouch::None) ApplyRideTouch(obj, rc.touch);
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    RideSnapshot s;
    const bool armed = rc.movesPlayer && gp && TakeRideSnapshotGuarded(obj, gp, rc.movesCamera, &s);
    if (armed) s.level = GetLevelInfo(g_base).level;
    g_rideUpdateOrig[idx](obj);
    if (!armed || s.inside) return;
    // gleicher Spieler, gleiches Level, lebt (Tod/Levelwechsel waehrend des Updates: nichts anfassen)
    if (*reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva) != gp || GetLevelInfo(g_base).level != s.level || PlayerDeadFlag(gp))
        return;
    if (!RestoreRideSnapshotGuarded(s, rc.movesCamera)) return;
    DWORD& last = g_rideLogged[obj];
    if (GetTickCount() - last > 5000) {
        last = GetTickCount();
        Log(L("[Mitfahren] %s: eigener Spieler war nicht drin - bleibt stehen\n", "[Ride] %s: own player was not inside - stays put\n"), rc.name);
    }
}

template <size_t I>
void RideUpdateThunk(uintptr_t obj) {
    RideUpdate(obj, I);
}
template <size_t... I>
constexpr std::array<ObjUpdateFn, sizeof...(I)> MakeRideThunks(std::index_sequence<I...>) {
    return {{&RideUpdateThunk<I>...}};
}
const std::array<ObjUpdateFn, kRideClassCount> kRideThunks = MakeRideThunks(std::make_index_sequence<kRideClassCount>{});

void HookRideUpdates() {
    int n = 0;
    for (size_t i = 0; i < kRideClassCount; ++i) {
        auto* vt = reinterpret_cast<uintptr_t*>(g_base + kRideClasses[i].vtRva);
        if (vt[4] < g_base + 0x1000 || vt[4] >= g_base + 0x700000) continue;  // keine Funktion der EXE -> falsche vtable
        DWORD old = 0;
        if (!VirtualProtect(&vt[4], sizeof(uintptr_t), PAGE_READWRITE, &old)) continue;
        g_rideUpdateOrig[i] = reinterpret_cast<ObjUpdateFn>(vt[4]);
        vt[4] = reinterpret_cast<uintptr_t>(kRideThunks[i]);
        VirtualProtect(&vt[4], sizeof(uintptr_t), old, &old);
        ++n;
    }
    Log(L("Hook Mitfahr-Objekte: %d von %zu Klassen\n", "Hook ride objects: %d of %zu classes\n"), n, kRideClassCount);
}

const char* PickupName(uintptr_t vt, int type) {
    return vt == g_base + kDarkSoulVtableRva ? "Dunkle Seele" : ItemName(type);
}

// Ein Einsammel-Ereignis des Mitspielers. Schluessel aus Seriennummer, Typ und gerundetem Ort: Seriennummern dynamisch
// entstandener Objekte (Seele aus einem Govi, Cadeaux aus einer Vase) werden spaeter fuer andere Objekte wiederverwendet.
struct AppliedPickup {
    uint32_t localSerial = 0;  // hier zugeordnetes Objekt (0 = noch keins)
    int tries = 0;
    bool done = false;         // hier eingesammelt -> nie wieder anwenden
};
std::map<uint64_t, AppliedPickup> g_remoteEvents[kMaxRemote];  // je Mitspieler
std::map<uint32_t, bool> g_claimedLocal;  // lokale Seriennummern, die wir wegen des Mitspielers einsammeln

uint64_t RemoteEventKey(const posecodec::PickupInfo& r) {
    auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(std::floor(v / 16.0f)) & 0xFFF); };
    return (static_cast<uint64_t>(r.serial) << 48) | (static_cast<uint64_t>(r.type & 0xFFF) << 36) | (q(r.pos[0]) << 24) |
           (q(r.pos[1]) << 12) | q(r.pos[2]);
}

void ApplyRemotePickupsFrom(int pi, uintptr_t gp);
void ApplyRemotePickups(uintptr_t gp) {
    for (int pi = 0; pi < kMaxRemote; ++pi)
        if (g_remoteActive[pi]) ApplyRemotePickupsFrom(pi, gp);  // nur, wenn der Mitspieler im selben Level ist
}

int ReadLocalLife();
bool SetLocalLifeGuarded(int v);
void ApplyRemotePickupsFrom(int pi, uintptr_t gp) {
    std::vector<posecodec::PickupInfo> remote;
    AcquireSRWLockShared(&g_worldLock);
    if (g_remotePickupsLevel[pi] == static_cast<uint16_t>(g_worldLevel)) remote = g_remotePickups[pi];
    ReleaseSRWLockShared(&g_worldLock);
    static bool broken = false;
    for (const auto& r : remote) {
        if (broken) return;
        AppliedPickup& ev = g_remoteEvents[pi][RemoteEventKey(r)];
        if (ev.done || ev.tries >= 30) continue;  // erledigt bzw. klappt nicht (z.B. Spieler kann gerade nichts aufnehmen)
        if (ev.localSerial && !g_trackedPickups.count(ev.localSerial)) {  // zugeordnetes Objekt ist weg -> eingesammelt
            ev.done = true;
            continue;
        }
        if (!ev.localSerial) {
            // Zuordnung: gleiche Seriennummer (Level-Objekt) oder sonst das naechste noch freie Objekt gleichen Typs
            // in der Naehe (dynamisch entstanden, z.B. Seele aus einem Govi)
            auto close = [&](const TrackedPickup& t, float& d2) {
                const float dx = t.pos[0] - r.pos[0], dy = t.pos[1] - r.pos[1], dz = t.pos[2] - r.pos[2];
                d2 = dx * dx + dy * dy + dz * dz;
                return t.type == r.type && d2 <= kPickupSamePlace * kPickupSamePlace;
            };
            float d2 = 0;
            auto it = g_trackedPickups.find(r.serial);
            if (it != g_trackedPickups.end() && !g_claimedLocal.count(r.serial) && close(it->second, d2)) {
                ev.localSerial = r.serial;
            } else {
                float best = 1e30f;
                for (const auto& kv : g_trackedPickups) {
                    if (g_claimedLocal.count(kv.first) || !close(kv.second, d2) || d2 >= best) continue;
                    best = d2;
                    ev.localSerial = kv.first;
                }
            }
            if (!ev.localSerial) continue;  // gibt es hier (noch) nicht
            g_claimedLocal[ev.localSerial] = true;
            const TrackedPickup& t = g_trackedPickups[ev.localSerial];
            Log(L("[Welt] Mitspieler hat %s (#%u) eingesammelt - %s (hier #%u)\n", "[World] other player collected %s (#%u) - %s (here #%u)\n"), PickupName(t.vt, r.type), r.serial,
                g_pickupMode == PickupMode::Shared ? "sammle es auch ein" : "entferne es", ev.localSerial);
        }
        const TrackedPickup& t = g_trackedPickups[ev.localSerial];
        ++ev.tries;
        const int lifeBefore = SharedHpOn() ? ReadLocalLife() : -1;  // geteilte HP: Heilung kommt als Delta des Sammlers
        const bool ok = g_pickupMode == PickupMode::Shared ? ProxyCollectGuarded(gp, t) : RemoveNodeGuarded(t.node);
        if (lifeBefore >= 0 && ReadLocalLife() != lifeBefore) SetLocalLifeGuarded(lifeBefore);
        if (!ok) {
            broken = true;
            Log(L("[Welt] Pickup anwenden fehlgeschlagen (Ausnahme) - Pickup-Sync abgeschaltet\n", "[World] applying pickup failed (exception) - pickup sync disabled\n"));
        }
    }
}

// ---- Objekt-Zustaende (Tueren, Schloesser, ...) ----
// Der SaveManager (0xF9C680, vt 0x706810) fuehrt eine Tabelle, Eintraege a 0x18 ab +0x90 (Anzahl +0xC090, hoechstens 0x800):
// Ort[3] (= Objekt +0x38), Level-Gruppe +0xC, Art +0x10 (= Objekt +0xD0), Wert +0x14. Objekte schreiben ihren Zustand dort
// hinein (Tuer offen, Schloss offen, ...) und lesen ihn beim Start; sie wird mitgespeichert. Wir haengen uns an beide
// Schreib-Funktionen, schicken Aenderungen ("SMS1") und setzen sie beim Mitspieler mit der Original-Funktion. Lebende
// Objekte neu anstossen: GenericDoor liest den Wert beim (Neu-)Start (Zustand +0x2A4 = 0) und steht danach offen/zu.
constexpr uintptr_t kSaveMgrRva           = 0xF9C680;
constexpr uintptr_t kSetObjStateRva       = 0x33F580;  // (mgr, obj, wert): Schluessel aus obj +0xD0 / +0x38
constexpr uintptr_t kSetStateRva          = 0x33F670;  // (mgr, art, &ort, wert)
constexpr uintptr_t kGetStateRva          = 0x33F7B0;  // (mgr, art, &ort) -> wert oder -1
constexpr uintptr_t kGenericDoorVtableRva = 0x720948;
constexpr uint32_t  kDoorStateOff         = 0x2A4;
using SetObjStateFn = void (*)(void*, uintptr_t, int);
using SetStateFn = void (*)(void*, int, const float*, int);
using GetStateFn = int (*)(void*, int, const float*);
SetObjStateFn g_setObjStateOrig = nullptr;
SetStateFn g_setStateOrig = nullptr;
bool g_applyingRemoteState = false;              // Spiel-Thread: eigene Uebernahme nicht wieder als Aenderung melden
uint32_t g_stateSession = 1;                     // zufaellig je Spielstart
uint32_t g_stateSeq = 0;
std::vector<posecodec::ObjState> g_localStates;   // meine Aenderungen in diesem Level, neueste hinten (g_worldLock)
std::vector<posecodec::ObjState> g_remoteStates[kMaxRemote];  // Liste je Mitspieler (g_worldLock)
uint32_t g_remoteStateSession[kMaxRemote] = {};
uint16_t g_remoteStatesLevel[kMaxRemote] = {};
bool g_resetAppliedStates[kMaxRemote] = {};                  // neue Sitzung des Mitspielers (g_worldLock)
struct StateKey {
    int type;
    float pos[3];
    bool operator<(const StateKey& o) const {
        if (type != o.type) return type < o.type;
        return std::memcmp(pos, o.pos, sizeof(pos)) < 0;
    }
};
std::map<StateKey, uint32_t> g_appliedStateSeq[kMaxRemote];  // je Mitspieler und Objekt die zuletzt uebernommene Folgenummer (Spiel-Thread)

void* SaveMgr() { return reinterpret_cast<void*>(g_base + kSaveMgrRva); }

int GetStateGuarded(int type, const float pos[3]) {
    __try {
        return reinterpret_cast<GetStateFn>(g_base + kGetStateRva)(SaveMgr(), type, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool SetStateGuarded(int type, const float pos[3], int value) {
    __try {
        g_setStateOrig(SaveMgr(), type, pos, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Pickups und Behaelter haben ihren eigenen Sync. Ihr Tabellen-Eintrag (z.B. Cadeaux 1002 = eingesammelt) darf nicht
// vorher ankommen: der Cadeaux-Code liest ihn (0x33995C) und wuerde das Objekt dann ohne Gutschrift entfernen.
bool IsStateSyncExcluded(const std::string& cls) {
    return cls == "Object" || cls == "QuestObject" || cls == "DarkSoul" || cls == "ExplodeObject" || cls == "AIGovi";
}

// Normale Tuer: 2 = angestossen/geht auf, 0 = geht von selbst wieder zu. Das laeuft bei beiden ueber die Naehe der Spieler
// (siehe Tueren unten) und wird nicht geschickt - sonst schliesst die Tuer beim einen, waehrend der andere davorsteht.
// Sie schreibt "zu" ohne Objekt (0x33F670), daher auch an der aufrufenden Stelle erkennen.
constexpr uintptr_t kGenericDoorCodeLo = 0x427C30, kGenericDoorCodeHi = 0x429800;
bool IsDoorMotion(const std::string& cls, uintptr_t callerRva, int oldValue, int value) {
    const bool door = cls == "GenericDoor" || (cls == "-" && callerRva >= kGenericDoorCodeLo && callerRva < kGenericDoorCodeHi);
    auto motion = [](int v) { return v == -1 || v == 0 || v == 2; };
    return door && motion(oldValue) && motion(value);
}

void NoteLocalState(int type, const float pos[3], int oldValue, int value, uintptr_t obj, uintptr_t caller) {
    // gleicher Wert oder erstes Anlegen mit 0 (Grundzustand) ist keine Aenderung
    if (g_applyingRemoteState || oldValue == value || (oldValue == -1 && value == 0)) return;
    const std::string cls = obj ? ClassNameOf(obj) : "-";
    const uintptr_t callerRva = caller - g_base;
    const bool excluded = IsStateSyncExcluded(cls);
    const bool motion = IsDoorMotion(cls, callerRva, oldValue, value);
    Log(L("[Zustand] %s Art %d bei %.0f/%.0f/%.0f: %d -> %d (Aufrufer 0x%llX)%s\n", "[State] %s type %d at %.0f/%.0f/%.0f: %d -> %d (caller 0x%llX)%s\n"), cls.c_str(), type, pos[0], pos[1], pos[2], oldValue,
        value, static_cast<unsigned long long>(callerRva), excluded ? L(" - eigener Sync", " - own sync") : (motion ? L(" - Tuer-Bewegung, laeuft ueber Naehe", " - door motion, runs via proximity") : ""));
    if (excluded || motion) return;
    posecodec::ObjState s;
    s.type = type;
    s.value = value;
    s.seq = ++g_stateSeq;
    std::memcpy(s.pos, pos, sizeof(s.pos));
    AcquireSRWLockExclusive(&g_worldLock);
    for (auto it = g_localStates.begin(); it != g_localStates.end(); ++it)
        if (it->type == type && std::memcmp(it->pos, pos, sizeof(s.pos)) == 0) { g_localStates.erase(it); break; }
    if (g_localStates.size() < 1024) g_localStates.push_back(s);
    ReleaseSRWLockExclusive(&g_worldLock);
}

void HookedSetObjState(void* mgr, uintptr_t obj, int value) {
    int type = 0;
    float pos[3] = {};
    const bool ok = SafeRead(obj + 0xD0, &type, 4) && SafeRead(obj + 0x38, pos, sizeof(pos));
    const int old = ok ? GetStateGuarded(type, pos) : -2;
    g_setObjStateOrig(mgr, obj, value);
    if (ok) NoteLocalState(type, pos, old, value, obj, reinterpret_cast<uintptr_t>(_ReturnAddress()));
}

void HookedSetState(void* mgr, int type, const float* pos, int value) {
    const int old = GetStateGuarded(type, pos);
    g_setStateOrig(mgr, type, pos, value);
    NoteLocalState(type, pos, old, value, 0, reinterpret_cast<uintptr_t>(_ReturnAddress()));
}

void HookObjectStates() {
    // beide beginnen mit mov [rsp+8],rbx / mov [rsp+10h],rbp / mov [rsp+18h],rsi
    static const uint8_t expected[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
    g_stateSession = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    if (std::memcmp(reinterpret_cast<void*>(g_base + kSetObjStateRva), expected, sizeof(expected)) == 0)
        g_setObjStateOrig = reinterpret_cast<SetObjStateFn>(
            InstallInlineHook(g_base + kSetObjStateRva, sizeof(expected), reinterpret_cast<void*>(HookedSetObjState)));
    if (std::memcmp(reinterpret_cast<void*>(g_base + kSetStateRva), expected, sizeof(expected)) == 0)
        g_setStateOrig = reinterpret_cast<SetStateFn>(
            InstallInlineHook(g_base + kSetStateRva, sizeof(expected), reinterpret_cast<void*>(HookedSetState)));
    Log(L("Hook Objekt-Zustaende: Objekt %s, Art/Ort %s\n", "Hook object states: object %s, type/place %s\n"), g_setObjStateOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"), g_setStateOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// Lebendes Objekt zu Art + Ort (im letzten Pool-Schnappschuss)
// Art (+0xD0) und Grundposition (+0x38) passen? Je Objekt abgesichert: ein gerade freigegebenes Objekt im Schnappschuss
// bricht die Suche sonst ab ("Objekt hier nicht gefunden", Level 12)
bool StateKeyMatchesGuarded(uintptr_t obj, int type, const float pos[3]) {
    __try {
        return *reinterpret_cast<int*>(obj + 0xD0) == type && std::memcmp(reinterpret_cast<void*>(obj + 0x38), pos, 12) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t FindStateObjectGuarded(int type, const float pos[3], int n) {
    for (int i = 0; i < n; ++i)
        if (StateKeyMatchesGuarded(g_poolSnap[i].obj, type, pos)) return g_poolSnap[i].obj;
    return 0;
}

// Objekte, die ihren Tabellen-Eintrag beim (Neu-)Start lesen: Zustandsfeld auf 0 setzen, sobald sie ruhen
struct RestartRule {
    uintptr_t vtRva;
    uint32_t stateOff;
    int idleA, idleB;  // nur in diesen Zustaenden neu starten
    bool onlyNew;      // nur, wenn der Eintrag neu angelegt wurde (Neustart verschiebt sonst ein zweites Mal)
    int signal;        // != 0: statt Neustart das Ereignis-Signal +0x120 setzen (wie es der Schalter ueber sein Ereignis tut)
    int unlocked;      // != 0: dieser Tabellen-Wert heisst "entriegelt" -> ruht das Objekt noch, bekommt es das Signal (Abgleich)
};
const RestartRule kRestartRules[] = {
    {kGenericDoorVtableRva, kDoorStateOff, 1, 3, false, 0, 0},  // GenericDoor: Wert Bit 1 -> offen (3), sonst zu (1)
    {0x720888, 0x290, 1, 1, false, 0, 0},                        // SwampLockedDoor: Eintrag vorhanden -> schwingt auf (2 -> 3)
    {0x720BC8, 0x290, 1, 1, true, 0, 0},                         // PadlockSwamp: Eintrag vorhanden -> faellt ab (4)
    // AsylumLockedDoor: verriegelt = Zustand 1; Signal 1 (Ereignis-Handler 0x2EEC28) -> schreibt 9999, entriegelt (2 -> 3)
    {0x720848, 0x290, 1, 1, false, 1, 9999},
    // Per Treffer geoeffnet (der nachgespielte Schuss kann beim anderen danebengehen oder in einem ruhenden Bereich landen);
    // alle lesen ihren Eintrag nur in Zustand 0 und warten sonst verschlossen in 1:
    {0x720B48, 0x290, 1, 1, false, 0, 0},  // FlameDoorObject (0x42F0B0): Eintrag vorhanden -> offen (2)
    {0x720B08, 0x290, 1, 1, false, 0, 0},  // ExplodeFlameDoor (0x42F210): ebenso
    {0x721B98, 0x290, 1, 1, false, 0, 0},  // GenericPadLockDoor (0x43A840): Eintrag 9999 -> offen (3)
};
struct PendingRestart {
    uintptr_t obj;
    const RestartRule* rule;
    DWORD since;
};
std::vector<PendingRestart> g_pendingRestarts;  // Spiel-Thread

const RestartRule* FindRestartRule(uintptr_t vt) {
    for (const RestartRule& r : kRestartRules)
        if (vt == g_base + r.vtRva) return &r;
    return nullptr;
}

// 1 = neu gestartet, 0 = ruht noch nicht, -1 = Ausnahme
int TryRestartGuarded(uintptr_t obj, const RestartRule& rule) {
    __try {
        int* state = reinterpret_cast<int*>(obj + rule.stateOff);
        if (*state != rule.idleA && *state != rule.idleB) return 0;
        if (rule.signal) *reinterpret_cast<int*>(obj + 0x120) = rule.signal;
        else *state = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Spiel-Thread, etwa jede Sekunde: Objekte, deren Tabellen-Eintrag "entriegelt" sagt, die aber noch verriegelt ruhen
// (Nachricht verloren, Mitspieler kam spaeter dazu, ...), bekommen das Signal
int ReadIntGuarded(uintptr_t addr) {
    __try {
        return *reinterpret_cast<int*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void ReconcileUnlockedObjects(int n) {
    for (int i = 0; i < n; ++i) {
        const PoolNodeInfo& p = g_poolSnap[i];
        const RestartRule* rule = FindRestartRule(p.vt);
        if (!rule || !rule->signal || !rule->unlocked) continue;
        const int state = ReadIntGuarded(p.obj + rule->stateOff);
        if (state != rule->idleA && state != rule->idleB) continue;
        if (ReadIntGuarded(p.obj + 0x120) == rule->signal) continue;  // hat es schon
        int type = 0;
        float pos[3] = {};
        if (!SafeRead(p.obj + 0xD0, &type, 4) || !SafeRead(p.obj + 0x38, pos, sizeof(pos))) continue;
        if (GetStateGuarded(type, pos) != rule->unlocked) continue;
        if (TryRestartGuarded(p.obj, *rule) == 1)
            Log(L("[Zustand] %s #%u laut Tabelle entriegelt, war aber noch zu - bekommt das Signal\n", "[State] %s #%u unlocked according to the table but still closed - gets the signal\n"), ClassNameOf(p.obj).c_str(), p.serial);
    }
}

// Spiel-Thread: wartende Neustarts versuchen (bis 10 s)
void RunPendingRestarts() {
    for (auto it = g_pendingRestarts.begin(); it != g_pendingRestarts.end();) {
        const int r = TryRestartGuarded(it->obj, *it->rule);
        if (r != 0 || GetTickCount() - it->since > 10000) {
            if (r == 1)
                Log(L("[Zustand] %s %s\n", "[State] %s %s\n"), ClassNameOf(it->obj).c_str(),
                    it->rule->signal ? L("bekommt das Signal (wie vom Schalter)", "gets the signal (like from the switch)") : L("neu gestartet (uebernimmt den Stand des Mitspielers)", "restarted (takes over the other player's state)"));
            else Log(L("[Zustand] Neustart %s\n", "[State] restart %s\n"), r < 0 ? L("fehlgeschlagen (Ausnahme)", "failed (exception)") : L("aufgegeben (Objekt ruht nicht)", "given up (object not idle)"));
            it = g_pendingRestarts.erase(it);
        } else {
            ++it;
        }
    }
}

// Spiel-Thread: neue Zustaende des Mitspielers uebernehmen (n = Eintraege im Pool-Schnappschuss)
void ApplyRemoteStatesFrom(int pi, int n);
void ApplyRemoteStates(int n) {
    if (!g_setStateOrig) return;
    for (int pi = 0; pi < kMaxRemote; ++pi) ApplyRemoteStatesFrom(pi, n);
}

void ApplyRemoteStatesFrom(int pi, int n) {
    std::vector<posecodec::ObjState> remote;
    AcquireSRWLockExclusive(&g_worldLock);
    if (g_resetAppliedStates[pi]) {
        g_appliedStateSeq[pi].clear();
        g_resetAppliedStates[pi] = false;
    }
    if (g_remoteActive[pi] && g_remoteStatesLevel[pi] == static_cast<uint16_t>(g_worldLevel)) remote = g_remoteStates[pi];
    ReleaseSRWLockExclusive(&g_worldLock);
    for (const auto& r : remote) {
        StateKey key{r.type, {r.pos[0], r.pos[1], r.pos[2]}};
        uint32_t& applied = g_appliedStateSeq[pi][key];
        if (r.seq <= applied) continue;
        applied = r.seq;
        const int old = GetStateGuarded(r.type, r.pos);
        if (old == r.value || old == -2) continue;
        // nur fuer Objekte, die es hier gibt (und die keinen eigenen Sync haben)
        const uintptr_t obj = FindStateObjectGuarded(r.type, r.pos, n);
        if (!obj) { Log(L("[Zustand] Mitspieler: Art %d bei %.0f/%.0f/%.0f -> %d, Objekt hier nicht gefunden\n", "[State] other player: type %d at %.0f/%.0f/%.0f -> %d, object not found here\n"), r.type, r.pos[0], r.pos[1], r.pos[2], r.value); continue; }
        const std::string cls = ClassNameOf(obj);
        if (IsStateSyncExcluded(cls) || IsDoorMotion(cls, 0, old, r.value)) continue;  // (falls der andere eine aeltere Version hat)
        g_applyingRemoteState = true;
        const bool ok = SetStateGuarded(r.type, r.pos, r.value);
        g_applyingRemoteState = false;
        if (!ok) { Log(L("[Zustand] Uebernehmen fehlgeschlagen (Ausnahme)\n", "[State] applying failed (exception)\n")); continue; }
        uintptr_t vt = 0;
        SafeRead(obj, &vt, 8);
        const RestartRule* rule = FindRestartRule(vt);
        const char* how = L("wirkt erst nach Neuladen", "takes effect after reloading");
        if (rule && rule->onlyNew && old != -1) {
            how = L("war schon gesetzt, kein Neustart", "already set, no restart");
        } else if (rule) {
            how = L("wird neu gestartet", "will be restarted");
            g_pendingRestarts.push_back({obj, rule, GetTickCount()});
        }
        Log(L("[Zustand] Mitspieler: %s Art %d bei %.0f/%.0f/%.0f: %d -> %d (%s)\n", "[State] other player: %s type %d at %.0f/%.0f/%.0f: %d -> %d (%s)\n"), cls.c_str(), r.type, r.pos[0], r.pos[1], r.pos[2], old,
            r.value, how);
    }
}

// Netz-Thread
void SendStates() {
    posecodec::ObjState list[posecodec::kMaxStates];
    int num = 0;
    uint16_t level = 0;
    AcquireSRWLockShared(&g_worldLock);
    level = g_localPickupsLevel;
    for (auto it = g_localStates.rbegin(); it != g_localStates.rend() && num < posecodec::kMaxStates; ++it) list[num++] = *it;
    ReleaseSRWLockShared(&g_worldLock);
    uint8_t buf[posecodec::MaxStatePacketSize()];
    net::Send(buf, posecodec::EncodeStates(g_stateSession, level, list, num, buf));
}

// Netz-Thread: false, wenn es kein Zustands-Paket war
bool HandleStatePacket(int from, const uint8_t* data, size_t size) {
    static posecodec::ObjState list[posecodec::kMaxStates];
    uint32_t session = 0;
    uint16_t level = 0;
    int num = 0;
    if (!posecodec::DecodeStates(data, size, session, level, list, num)) return false;
    AcquireSRWLockExclusive(&g_worldLock);
    if (session != g_remoteStateSession[from]) {
        g_remoteStateSession[from] = session;
        g_resetAppliedStates[from] = true;
    }
    g_remoteStatesLevel[from] = level;
    g_remoteStates[from].assign(list, list + num);
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// ---- Tueren, die auf Spieler-Naehe reagieren ----
// Schwingtueren (kexShadowManSwingDoor) und normale Tueren (kexShadowManGenericDoor) gehen auf, wenn der Spieler sie
// anstoesst (Kollision -> vt[5]), und schwingen zu bzw. gehen zu, wenn er wieder weg ist (Aktualisierung vt[4], Spieler =
// g_player, Position +0x20). Damit der andere das Oeffnen sieht:
//  - Aktualisierung mit dem naeheren der beiden Spielern: ist Spieler 2 naeher, steht der eigene Spieler fuer diesen einen
//    Aufruf an dessen Stelle -> die Tuer geht erst zu, wenn beide weg sind
//  - Spieler 2 hat keine Kollision -> steht er im Tuerrahmen (+0xD8 min, +0xE4 max, etwas groesser), stossen wir sie fuer ihn an
//  - Asyl-Tueren (kexShadowManAsylumLockedDoor) gehen ganz ueber die Naehe auf (Abstand zur Tuermitte +0x294) - dort
//    reicht der Tausch in der Aktualisierung; ihr Funktionsanfang ist fuer einen Inline-Hook zu kurz -> vtable-Eintrag
using DoorUpdateFn = void (*)(uintptr_t);
struct ProximityDoor {
    const char* name;
    uintptr_t vtRva, updateRva, touchRva;  // touchRva 0 = kein Anstossen noetig
    uint32_t stateOff;
    int touchA, touchB;     // nur in diesen Zustaenden anstossen (zu / geht gerade zu)
    uint8_t prologue[15];   // erwarteter Funktionsanfang der Aktualisierung (Laenge = prologueLen; 0 = vtable-Eintrag tauschen)
    size_t prologueLen;
    DoorUpdateFn orig;
};
// Name einer Tuer-Art fuers Protokoll (die Tabelle unten nennt sie deutsch)
const char* DoorName(const char* de) {
    if (!std::strcmp(de, "Schwingtuer")) return L("Schwingtuer", "swing door");
    if (!std::strcmp(de, "Tuer")) return L("Tuer", "door");
    if (!std::strcmp(de, "Asyl-Tuer")) return L("Asyl-Tuer", "asylum door");
    if (!std::strcmp(de, "Hoch-Tuer")) return L("Hoch-Tuer", "up/down door");
    return de;
}
ProximityDoor g_proxDoors[] = {
    // mov rax,rsp / mov [rax+10h],rbx / mov [rax+18h],rbp / mov [rax+20h],rsi
    {"Schwingtuer", 0x720988, 0x429EC0, 0x42A440, 0x290, 0, 0,
     {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20}, 15, nullptr},
    // mov rax,rsp / push rbp,rbx,rdi,r13,r15 / lea rbp,[rax-5Fh]; Zustand +0x2A4: 1 zu, 4 geht zu
    {"Tuer", kGenericDoorVtableRva, 0x427C30, 0x4296E0, kDoorStateOff, 1, 4,
     {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x57, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1}, 14, nullptr},
    {"Asyl-Tuer", 0x720848, 0x42B1A0, 0, 0x290, 0, 0, {}, 0, nullptr},
    // kexShadowManUpDownDoor: faehrt bei Spieler < 200 auf; mov [rsp+10h],rbx / mov [rsp+18h],rsi / push rdi / sub rsp,50h
    {"Hoch-Tuer", 0x7207C8, 0x4297F0, 0, 0x290, 0, 0,
     {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x50}, 15, nullptr},
};

// vtable-Eintrag (Index) auf detour umbiegen; liefert die bisherige Funktion
void* PatchVtableEntry(uintptr_t vtRva, int index, void* detour) {
    void** slot = reinterpret_cast<void**>(g_base + vtRva) + index;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* orig = *slot;
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return orig;
}
constexpr float kDoorTouchMargin = 80.0f;

// Figur eines aktiven Mitspielers, die naeher an pos ist als p (-1 = keine)
int NearestActiveAvatar(const float* pos, const float* p) {
    auto dist2 = [pos](const float* q) {
        const float x = q[0] - pos[0], y = q[1] - pos[1], z = q[2] - pos[2];
        return x * x + y * y + z * z;
    };
    float best = dist2(p);
    int slot = -1;
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        const uintptr_t a = g_avatar[pi];
        if (!a || !g_remoteActive[pi]) continue;
        const float d = dist2(reinterpret_cast<const float*>(a + 0x20));
        if (d < best) {
            best = d;
            slot = pi;
        }
    }
    return slot;
}

// fn(obj) aufrufen, waehrend der eigene Spieler an der Stelle der naechsten Mitspieler-Figur steht (wenn noetig);
// false = nicht getauscht
bool CallAsNearestPlayerGuarded(uintptr_t obj, DoorUpdateFn fn) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    float saved[3];
    __try {
        if (!gp) return false;
        float* me = reinterpret_cast<float*>(gp + 0x20);
        const int slot = NearestActiveAvatar(reinterpret_cast<float*>(obj + 0x20), me);
        if (slot < 0) return false;
        const float* other = reinterpret_cast<float*>(g_avatar[slot] + 0x20);
        std::memcpy(saved, me, sizeof(saved));
        std::memcpy(me, other, sizeof(saved));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    __try {
        fn(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    std::memcpy(reinterpret_cast<float*>(gp + 0x20), saved, sizeof(saved));
    return true;
}

void HookedSwingDoorUpdate(uintptr_t door) {
    if (AnyRemoteActive() && CallAsNearestPlayerGuarded(door, g_proxDoors[0].orig)) return;
    g_proxDoors[0].orig(door);
}

void HookedGenericDoorUpdate(uintptr_t door) {
    if (AnyRemoteActive() && CallAsNearestPlayerGuarded(door, g_proxDoors[1].orig)) return;
    g_proxDoors[1].orig(door);
}

void HookedAsylumDoorUpdate(uintptr_t door) {
    if (AnyRemoteActive() && CallAsNearestPlayerGuarded(door, g_proxDoors[2].orig)) return;
    g_proxDoors[2].orig(door);
}

void HookedUpDownDoorUpdate(uintptr_t door) {
    if (AnyRemoteActive() && CallAsNearestPlayerGuarded(door, g_proxDoors[3].orig)) return;
    g_proxDoors[3].orig(door);
}

// 1 = angestossen, 0 = nicht noetig, -1 = Ausnahme
int TouchDoorForRemoteGuarded(uintptr_t door, const ProximityDoor& pd, uintptr_t gp, const float other[3]) {
    float saved[3];
    __try {
        const int state = *reinterpret_cast<int*>(door + pd.stateOff);
        if (state != pd.touchA && state != pd.touchB) return 0;  // geht schon auf / steht offen
        const float* mn = reinterpret_cast<float*>(door + 0xD8);
        const float* mx = reinterpret_cast<float*>(door + 0xE4);
        const float m = kDoorTouchMargin;
        if (other[0] < mn[0] - m || other[0] > mx[0] + m || other[2] < mn[2] - m || other[2] > mx[2] + m ||
            other[1] < mn[1] - 200.0f || other[1] > mx[1])
            return 0;
        float* me = reinterpret_cast<float*>(gp + 0x20);
        std::memcpy(saved, me, sizeof(saved));
        std::memcpy(me, other, sizeof(saved));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    int result = 1;
    __try {
        reinterpret_cast<DoorUpdateFn>(g_base + pd.touchRva)(door);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    std::memcpy(reinterpret_cast<float*>(gp + 0x20), saved, sizeof(saved));
    return result;
}

// Spiel-Thread (WorldSyncTick, n = Eintraege im Pool-Schnappschuss)
void TouchDoorsForRemote(uintptr_t gp, int n) {
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        const uintptr_t p2 = g_avatar[pi];
        if (!g_remoteActive[pi] || !p2) continue;
        float other[3];
        if (!SafeRead(p2 + 0x20, other, sizeof(other))) continue;
        for (int i = 0; i < n; ++i) {
            for (const ProximityDoor& pd : g_proxDoors) {
                if (!pd.orig || !pd.touchRva || g_poolSnap[i].vt != g_base + pd.vtRva) continue;
                const int r = TouchDoorForRemoteGuarded(g_poolSnap[i].obj, pd, gp, other);
                if (r == 1) Log(L("[Welt] %s #%u von Mitspieler %d angestossen\n", "[World] %s #%u triggered by player %d\n"), DoorName(pd.name), g_poolSnap[i].serial, pi + 1);
                else if (r < 0) Log(L("[Welt] %s #%u anstossen fehlgeschlagen (Ausnahme)\n", "[World] %s #%u trigger failed (exception)\n"), DoorName(pd.name), g_poolSnap[i].serial);
            }
        }
    }
}

void HookProximityDoors() {
    void* detours[] = {reinterpret_cast<void*>(HookedSwingDoorUpdate), reinterpret_cast<void*>(HookedGenericDoorUpdate),
                       reinterpret_cast<void*>(HookedAsylumDoorUpdate), reinterpret_cast<void*>(HookedUpDownDoorUpdate)};
    static_assert(sizeof(detours) / sizeof(detours[0]) == sizeof(g_proxDoors) / sizeof(g_proxDoors[0]), "detours");
    for (size_t i = 0; i < sizeof(g_proxDoors) / sizeof(g_proxDoors[0]); ++i) {
        ProximityDoor& pd = g_proxDoors[i];
        const uintptr_t slotValue = *(reinterpret_cast<uintptr_t*>(g_base + pd.vtRva) + 4);
        if (pd.prologueLen == 0) {  // vt[4] tauschen, wenn er auf die erwartete Funktion zeigt
            if (slotValue == g_base + pd.updateRva)
                pd.orig = reinterpret_cast<DoorUpdateFn>(PatchVtableEntry(pd.vtRva, 4, detours[i]));
        } else if (std::memcmp(reinterpret_cast<void*>(g_base + pd.updateRva), pd.prologue, pd.prologueLen) == 0) {
            pd.orig = reinterpret_cast<DoorUpdateFn>(InstallInlineHook(g_base + pd.updateRva, pd.prologueLen, detours[i]));
        }
        Log(L("Hook %s: %s\n", "Hook %s: %s\n"), DoorName(pd.name), pd.orig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
    }
}

// ---- Gegner: Tod synchron ----
// Gegner (Klassen kexShadowManAI*) haben Lebenspunkte als float bei +0x2D8. Treffer sammeln Schaden in +0x2D4
// (DeferredDamageByProjectile 0x35CEB0); 0x35D1C0 zieht ihn jede Runde ab, bei <= 0 laeuft der normale Tod (Animation,
// Beute, bei nicht wiederkehrenden Gegnern Tabellen-Wert 2 = "Enemy Killed and won't respawn"). Stirbt hier ein Gegner,
// geht seine Seriennummer an den Mitspieler ("SMK1"); dort bekommt derselbe Gegner tödlichen gesammelten Schaden und
// stirbt auf demselben Weg. Govis haben ihren eigenen Sync (Behaelter).
constexpr uint32_t kAiHealthOff = 0x2D8;
constexpr uint32_t kAiDamageOff = 0x2D4;
struct TrackedEnemy {
    uintptr_t obj = 0;
    uint32_t id = 0;          // Kennung ueber beide PCs: Hash aus Gruppe (+0xD0) und Grundposition (+0x38)
    uint32_t vtRva = 0;       // Klasse (Gegenprobe)
    int type = 0;             // Gruppe (+0xD0)
    bool alive = false;
    bool remoteKill = false;  // stirbt, weil der Mitspieler ihn getoetet hat -> nicht zuruecksenden
    DWORD remoteKillAt = 0;
    bool seen = false;
    float pos[3] = {};
    float lastHealth = 0.0f;  // Lebenspunkte beim letzten Durchlauf (Abfall = eigener Treffer)
    float myDamage = 0.0f;    // Summe des eigenen Schadens in diesem Level (geht an den Mitspieler)
    DWORD myDamageAt = 0;
    float hpFactor = 1.0f;    // angewandter Gegner-HP-Faktor (1 = Lebenspunkte wie im Spiel)
};
std::map<uint32_t, TrackedEnemy> g_trackedEnemies;  // Spiel-Thread
// Gegner-HP je Mitspieler: Objekt -> (Kennung, angewandter Faktor). Ueberlebt das Austragen aus g_trackedEnemies (sonst
// wuerde ein wieder auftauchender Gegner doppelt skaliert); beim Tod geloescht, damit ein neu aufgestellter neu zaehlt.
std::map<uintptr_t, std::pair<uint32_t, float>> g_hpApplied;  // Spiel-Thread
std::map<uintptr_t, bool> g_enemyVtables;           // vtable -> Gegner? (Spiel-Thread)
std::vector<posecodec::KillInfo> g_localKills;      // meine Toetungen in diesem Level (g_worldLock)
std::vector<posecodec::KillInfo> g_remoteKills[kMaxRemote];  // Liste je Mitspieler (g_worldLock)
uint32_t g_remoteKillSession[kMaxRemote] = {};
uint16_t g_remoteKillsLevel[kMaxRemote] = {};
bool g_resetAppliedKills[kMaxRemote] = {};                  // (g_worldLock)
std::map<uint32_t, uint32_t> g_appliedKillSeq[kMaxRemote];  // je Mitspieler: Kennung -> zuletzt uebernommene Folgenummer (Spiel-Thread)
uint32_t g_killSeq = 0;
// Die Pool-Seriennummern unterscheiden sich zwischen den PCs, sobald einer das Level anders betreten hat (Spielstand laden
// vs. Levelwechsel; im Steam-Test lagen alle Gegner versetzt). Gleich sind Gruppe +0xD0 und Grundposition +0x38 (Schluessel
// der Zustandstabelle) -> daraus die Kennung. Kennung -> Seriennummer hier (Spiel-Thread, jeder Durchlauf neu)
std::map<uint32_t, uint32_t> g_enemyById;
// Schaden an Gegnern ("SMD1"): damit beide denselben Boss/Gegner gemeinsam besiegen
std::vector<posecodec::DamageInfo> g_localDamageOut;  // (g_worldLock)
std::vector<posecodec::DamageInfo> g_remoteDamage[kMaxRemote];  // je Mitspieler (g_worldLock)
uint16_t g_remoteDamageLevel[kMaxRemote] = {};                  // (g_worldLock)
std::map<uint32_t, float> g_appliedDamage[kMaxRemote];          // je Mitspieler: Kennung -> uebernommene Schadens-Summe (Spiel-Thread)

uint32_t EnemyIdGuarded(uintptr_t obj) {
    uint8_t key[16];
    __try {
        std::memcpy(key, reinterpret_cast<void*>(obj + 0x38), 12);
        std::memcpy(key + 12, reinterpret_cast<void*>(obj + 0xD0), 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    uint32_t h = 2166136261u;  // FNV-1a
    for (uint8_t b : key) h = (h ^ b) * 16777619u;
    return h ? h : 1;
}

bool IsEnemyVtable(uintptr_t vt, uintptr_t obj) {
    auto it = g_enemyVtables.find(vt);
    if (it != g_enemyVtables.end()) return it->second;
    const std::string cls = ClassNameOf(obj);
    const bool enemy = cls.size() > 2 && cls.compare(0, 2, "AI") == 0 && cls != "AIGovi";
    g_enemyVtables[vt] = enemy;
    return enemy;
}

bool ReadFloatGuarded(uintptr_t addr, float* v) {
    __try {
        *v = *reinterpret_cast<float*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteFloatGuarded(uintptr_t addr, float v) {
    __try {
        *reinterpret_cast<float*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// toedlichen gesammelten Schaden setzen -> stirbt beim naechsten Durchlauf auf normalem Weg
bool KillEnemyGuarded(uintptr_t obj) {
    __try {
        const float health = *reinterpret_cast<float*>(obj + kAiHealthOff);
        if (health <= 0.0f) return false;
        *reinterpret_cast<float*>(obj + kAiDamageOff) = health + 10000.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Spiel-Thread (WorldSyncTick, nach dem Pool-Schnappschuss)
void TrackEnemies(int n) {
    const DWORD now = GetTickCount();
    const float factor = coop::HpScaleFactor(HpPercent(), PlayerCount());
    static float loggedFactor = 1.0f;
    if (factor != loggedFactor) {
        loggedFactor = factor;
        Log(L("[Gegner-HP] Faktor %.2f (%d Spieler, %d %%)\n", "[Enemy HP] factor %.2f (%d players, %d %%)\n"), factor, PlayerCount(), HpPercent());
    }
    for (auto& kv : g_trackedEnemies) kv.second.seen = false;
    g_enemyById.clear();
    for (int i = 0; i < n; ++i) {
        const PoolNodeInfo& p = g_poolSnap[i];
        if (p.state == 0x10 || !IsEnemyVtable(p.vt, p.obj)) continue;
        float health = 0.0f;
        if (!ReadFloatGuarded(p.obj + kAiHealthOff, &health)) continue;
        TrackedEnemy& e = g_trackedEnemies[p.serial];
        if (e.obj != p.obj) {  // neu (oder neu gespawnt auf demselben Platz)
            e = TrackedEnemy();
            e.obj = p.obj;
            e.alive = health > 0.0f;
            e.lastHealth = health;
            SafeRead(p.obj + 0xD0, &e.type, sizeof(e.type));
            e.id = EnemyIdGuarded(p.obj);
            e.vtRva = static_cast<uint32_t>(p.vt - g_base);
            const auto applied = g_hpApplied.find(p.obj);
            if (applied != g_hpApplied.end() && applied->second.first == e.id) e.hpFactor = applied->second.second;
        }
        e.seen = true;
        if (e.alive && health > 0.0f) {
            if (e.hpFactor != factor) {  // neu oder Faktor geaendert: Anteil bleibt
                health = coop::RescaleHealth(health, e.hpFactor, factor);
                WriteFloatGuarded(p.obj + kAiHealthOff, health);
                e.hpFactor = factor;
                g_hpApplied[p.obj] = {e.id, factor};
                e.lastHealth = health;  // kein Treffer
            } else if (factor != 1.0f && health > e.lastHealth) {  // Phasenwechsel / Heilung: Zuwachs skalieren
                health = coop::ScaleGain(e.lastHealth, health, factor);
                WriteFloatGuarded(p.obj + kAiHealthOff, health);
                e.lastHealth = health;
            }
        }
        if (e.id) g_enemyById[e.id] = p.serial;
        std::memcpy(e.pos, p.pos, sizeof(e.pos));
        if (e.alive && health > 0.0f && health < e.lastHealth) {  // eigener Treffer (fremder Schaden setzt lastHealth selbst)
            e.myDamage += e.lastHealth - health;
            e.myDamageAt = now;
        }
        e.lastHealth = health;
        if (e.alive && health <= 0.0f) {
            e.alive = false;
            g_hpApplied.erase(p.obj);
            e.hpFactor = 1.0f;
            if (e.remoteKill) continue;
            posecodec::KillInfo k;
            k.serial = e.id;
            k.seq = ++g_killSeq;
            k.type = static_cast<int32_t>(e.vtRva);
            std::memcpy(k.pos, p.pos, sizeof(k.pos));
            AcquireSRWLockExclusive(&g_worldLock);
            if (g_localKills.size() >= 256) g_localKills.erase(g_localKills.begin());
            g_localKills.push_back(k);
            ReleaseSRWLockExclusive(&g_worldLock);
            Log(L("[Gegner] %s #%u getoetet\n", "[Enemies] %s #%u killed\n"), ClassNameOf(p.obj).c_str(), p.serial);
        } else if (!e.alive && health > 0.0f) {
            e.alive = true;  // wiederbelebt / neu aufgestellt (Skalierung im naechsten Durchlauf)
            e.remoteKill = false;
            e.hpFactor = 1.0f;
        }
    }
    for (auto it = g_trackedEnemies.begin(); it != g_trackedEnemies.end();)
        it = it->second.seen ? std::next(it) : g_trackedEnemies.erase(it);
    // eigene Schadens-Summen der zuletzt (5 s) getroffenen Gegner fuer den Mitspieler bereitlegen
    std::vector<posecodec::DamageInfo> out;
    for (const auto& kv : g_trackedEnemies) {
        const TrackedEnemy& e = kv.second;
        if (!e.id || e.myDamage <= 0.0f || now - e.myDamageAt > 5000 || out.size() >= static_cast<size_t>(posecodec::kMaxDamage)) continue;
        posecodec::DamageInfo d;
        d.serial = e.id;
        d.type = static_cast<int32_t>(e.vtRva);
        d.total = e.myDamage;
        std::memcpy(d.pos, e.pos, sizeof(d.pos));
        out.push_back(d);
    }
    AcquireSRWLockExclusive(&g_worldLock);
    g_localDamageOut.swap(out);
    ReleaseSRWLockExclusive(&g_worldLock);
}

// Gegenstueck zu einem Gegner des Mitspielers ueber die Kennung (und dieselbe Klasse); nullptr = gibt es hier (noch) nicht
TrackedEnemy* FindLocalEnemy(uint32_t id, int32_t vtRva) {
    const auto m = g_enemyById.find(id);
    if (m == g_enemyById.end()) return nullptr;
    auto it = g_trackedEnemies.find(m->second);
    if (it == g_trackedEnemies.end() || static_cast<int32_t>(it->second.vtRva) != vtRva) return nullptr;
    return &it->second;
}

// Spiel-Thread: Toetungen des Mitspielers uebernehmen. Gibt es den Gegner hier (noch) nicht, bleibt die Toetung offen und wird
// spaeter angewendet, sobald er auftaucht. Wer noch lebt, obwohl er schon getoetet wurde (Gegner
// ausser Reichweite wird nicht aktualisiert), bekommt den toedlichen Schaden alle 3 s erneut.
void ApplyRemoteKills() {
    const DWORD now = GetTickCount();
    for (int pi = 0; pi < kMaxRemote; ++pi) {
    std::vector<posecodec::KillInfo> remote;
    AcquireSRWLockExclusive(&g_worldLock);
    if (g_resetAppliedKills[pi]) {
        g_appliedKillSeq[pi].clear();
        g_resetAppliedKills[pi] = false;
    }
    if (g_remoteActive[pi] && g_remoteKillsLevel[pi] == static_cast<uint16_t>(g_worldLevel)) remote = g_remoteKills[pi];
    ReleaseSRWLockExclusive(&g_worldLock);
    for (const auto& k : remote) {
        uint32_t& applied = g_appliedKillSeq[pi][k.serial];
        if (k.seq <= applied) continue;
        TrackedEnemy* e = FindLocalEnemy(k.serial, k.type);
        if (!e) continue;  // gibt es hier (noch) nicht -> beim naechsten Durchlauf wieder versuchen
        applied = k.seq;
        if (!e->alive) continue;  // ist hier schon tot
        if (KillEnemyGuarded(e->obj)) {
            e->remoteKill = true;
            e->remoteKillAt = now;
            Log(L("[Gegner] Mitspieler %d hat %s (Kennung %08X, hier #%u) getoetet - stirbt hier auch (weit weg: sobald er aktiv wird)\n", "[Enemies] player %d killed %s (id %08X, here #%u) - dies here too (far away: once it becomes active)\n"),
                pi + 1, ClassNameOf(e->obj).c_str(), k.serial, g_enemyById[k.serial]);
        }
    }
    }
    for (auto& kv : g_trackedEnemies) {
        TrackedEnemy& e = kv.second;
        if (!e.remoteKill || !e.alive || now - e.remoteKillAt < 3000) continue;
        e.remoteKillAt = now;
        float pending = 0.0f;
        if (ReadFloatGuarded(e.obj + kAiDamageOff, &pending) && pending < e.lastHealth) KillEnemyGuarded(e.obj);
    }
}

// Spiel-Thread: Schaden des Mitspielers abziehen (Differenz seiner Summe zur zuletzt uebernommenen)
void ApplyRemoteDamage() {
    for (int pi = 0; pi < kMaxRemote; ++pi) {
    std::vector<posecodec::DamageInfo> remote;
    AcquireSRWLockShared(&g_worldLock);
    if (g_remoteActive[pi] && g_remoteDamageLevel[pi] == static_cast<uint16_t>(g_worldLevel)) remote = g_remoteDamage[pi];
    ReleaseSRWLockShared(&g_worldLock);
    for (const auto& d : remote) {
        float& applied = g_appliedDamage[pi][d.serial];
        if (d.total < applied - 0.01f) {  // Mitspieler hat neu angefangen (Level neu geladen)
            applied = d.total;
            continue;
        }
        const float delta = d.total - applied;
        if (delta < 0.5f) continue;
        TrackedEnemy* e = FindLocalEnemy(d.serial, d.type);
        if (!e) continue;
        applied = d.total;
        if (!e->alive) continue;
        float health = 0.0f;
        if (!ReadFloatGuarded(e->obj + kAiHealthOff, &health) || health <= 0.0f) continue;
        if (health - delta <= 0.5f) {
            KillEnemyGuarded(e->obj);  // stirbt normal; die Toetung geht an den Mitspieler zurueck (falls er noch lebt)
        } else if (WriteFloatGuarded(e->obj + kAiHealthOff, health - delta)) {
            e->lastHealth = health - delta;  // nicht als eigener Treffer zaehlen
        }
    }
    }
}

// Netz-Thread
void SendKills() {
    posecodec::KillInfo list[posecodec::kMaxKills];
    int num = 0;
    uint16_t level = 0;
    AcquireSRWLockShared(&g_worldLock);
    level = g_localPickupsLevel;
    for (auto it = g_localKills.rbegin(); it != g_localKills.rend() && num < posecodec::kMaxKills; ++it) list[num++] = *it;
    ReleaseSRWLockShared(&g_worldLock);
    uint8_t buf[posecodec::MaxKillPacketSize()];
    net::Send(buf, posecodec::EncodeKills(g_stateSession, level, list, num, buf));
}

// Netz-Thread
void SendDamage() {
    posecodec::DamageInfo list[posecodec::kMaxDamage];
    int num = 0;
    uint16_t level = 0;
    AcquireSRWLockShared(&g_worldLock);
    level = g_localPickupsLevel;
    for (const auto& d : g_localDamageOut)
        if (num < posecodec::kMaxDamage) list[num++] = d;
    ReleaseSRWLockShared(&g_worldLock);
    if (!num) return;
    uint8_t buf[posecodec::MaxDamagePacketSize()];
    net::Send(buf, posecodec::EncodeDamage(g_stateSession, level, list, num, buf));
}

// Netz-Thread: false, wenn es kein Gegner-Paket war
bool HandleKillPacket(int from, const uint8_t* data, size_t size) {
    static posecodec::KillInfo list[posecodec::kMaxKills];
    uint32_t session = 0;
    uint16_t level = 0;
    int num = 0;
    if (!posecodec::DecodeKills(data, size, session, level, list, num)) return false;
    AcquireSRWLockExclusive(&g_worldLock);
    if (session != g_remoteKillSession[from]) {
        g_remoteKillSession[from] = session;
        g_resetAppliedKills[from] = true;
    }
    g_remoteKillsLevel[from] = level;
    g_remoteKills[from].assign(list, list + num);
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// Netz-Thread: false, wenn es kein Schadens-Paket war
bool HandleDamagePacket(int from, const uint8_t* data, size_t size) {
    static posecodec::DamageInfo list[posecodec::kMaxDamage];
    uint32_t session = 0;
    uint16_t level = 0;
    int num = 0;
    if (!posecodec::DecodeDamage(data, size, session, level, list, num)) return false;
    AcquireSRWLockExclusive(&g_worldLock);
    g_remoteDamageLevel[from] = level;
    g_remoteDamage[from].assign(list, list + num);
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// ---- Gegner: verfolgen den naeheren Spieler ----
// Beide PCs simulieren die Gegner selbst. Damit sie auf beiden Seiten denselben Spieler verfolgen, laeuft das Update eines
// Gegners (0x356020, einziger Aufruf aus dem Pool-Rueckruf 0x358C50 bei 0x358C86) mit dem naeheren Spieler: ist Spieler 2
// naeher, ist er fuer diesen Aufruf "der Spieler" (g_player 0xF617D8 zeigt auf ihn, sein Sektor +0xD4 kommt aus der Pose).
// Treffer, Blut usw. landen so bei seiner Figur und nicht bei uns. Die Lebenspunkte liegen aber im gemeinsamen Inventar
// (0xDB21E0 + 0x1C + Typ*4, Typ 0/1 = 5000, Typ 2 = 10000) -> merken und gesunkene Werte zuruecksetzen. Der Mitspieler
// bekommt den Schaden in seinem eigenen Spiel von seinem Gegner.
constexpr uintptr_t kAiUpdateRva       = 0x356020;
constexpr uintptr_t kAiUpdateCallRva   = 0x358C86;  // call rel32 auf 0x356020
constexpr uintptr_t kInventoryCountRva = 0xDB21FC;
using AiUpdateFn = void (*)(uintptr_t);
AiUpdateFn g_aiUpdateOrig = nullptr;
DWORD g_lastDamageDropLog = 0;

// Ausfuehrbaren Speicher in +-2 GB Naehe der EXE holen (fuer einen rel32-Sprung)
void* AllocNearImage(size_t size) {
    for (uintptr_t a = (g_base & ~uintptr_t(0xFFFF)) - 0x10000; a > g_base - 0x70000000; a -= 0x10000) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return nullptr;
}

// call rel32 an callRva auf detour umbiegen (ueber einen nahen Sprung-Stub), nur wenn er auf expectedRva zeigt;
// liefert das bisherige Ziel (nullptr = nicht gepatcht)
void* PatchCallRel32(uintptr_t callRva, uintptr_t expectedRva, void* detour) {
    uint8_t* site = reinterpret_cast<uint8_t*>(g_base + callRva);
    if (site[0] != 0xE8) return nullptr;
    int32_t rel = 0;
    std::memcpy(&rel, site + 1, 4);
    void* oldTarget = site + 5 + rel;
    if (oldTarget != reinterpret_cast<void*>(g_base + expectedRva)) return nullptr;
    uint8_t* stub = static_cast<uint8_t*>(AllocNearImage(32));
    if (!stub) return nullptr;
    stub[0] = 0xFF; stub[1] = 0x25; std::memset(stub + 2, 0, 4);  // jmp [rip+0]
    std::memcpy(stub + 6, &detour, 8);
    const int64_t newRel = reinterpret_cast<int64_t>(stub) - reinterpret_cast<int64_t>(site + 5);
    if (newRel < INT32_MIN || newRel > INT32_MAX) return nullptr;
    const int32_t r = static_cast<int32_t>(newRel);
    DWORD old = 0;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    std::memcpy(site + 1, &r, 4);
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return oldTarget;
}

// Sektor fuer die Figur des Mitspielers: aus seiner Pose, sonst der eigene der Figur - nur wenn er im Sektor-Feld des
// aktuellen Levels liegt ([0xF5ED78], Anzahl +0xC). Nach einem Level-/Teddy-Wechsel des Mitspielers gehoert die Nummer aus
// seiner Pose kurz noch zu seinem anderen Level; Gegner-Code (0x366DC0) nimmt sie ungeprueft als Index (Kex-Assert array.h:684).
// -1 = nicht verwendbar (dann laeuft der Aufruf mit dem eigenen Spieler)
int32_t AvatarSectorFor(int slot, uintptr_t p2) {
    int32_t sector = g_remoteSector[slot] >= 0 ? g_remoteSector[slot] : -1;
    if (sector < 0) SafeRead(p2 + 0xD4, &sector, 4);
    uintptr_t arr = 0;
    uint32_t count = 0;
    if (!SafeRead(g_base + 0xF5ED78, &arr, 8) || !arr || !SafeRead(arr + 0xC, &count, 4)) return -1;
    return coop::SectorIndexValid(sector, count) ? sector : -1;
}

// 1 = mit Spieler 2 gerechnet (dabei verworfener Schaden in *dropped), 0 = nicht noetig, -1 = Ausnahme
int AiUpdateAsNearestGuarded(uintptr_t ai, int* dropped) {
    uintptr_t* playerSlot = reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    const uintptr_t gp = *playerSlot;
    uintptr_t p2 = 0;
    int* inv = reinterpret_cast<int*>(g_base + kInventoryCountRva);
    int savedInv[3];
    int32_t savedSector = 0;
    __try {
        if (!gp) return 0;
        const int slot = NearestActiveAvatar(reinterpret_cast<float*>(ai + 0x20), reinterpret_cast<float*>(gp + 0x20));
        if (slot < 0) return 0;
        p2 = g_avatar[slot];
        if (!p2 || g_avatarOwner[slot] != gp) return 0;  // Figur gehoert zu einem frueheren Spielerobjekt (Laden)
        const int32_t sector = AvatarSectorFor(slot, p2);
        if (sector < 0) return 0;
        std::memcpy(savedInv, inv, sizeof(savedInv));
        savedSector = *reinterpret_cast<int32_t*>(p2 + 0xD4);
        *reinterpret_cast<int32_t*>(p2 + 0xD4) = sector;
        *playerSlot = p2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    int result = 1;
    __try {
        g_aiUpdateOrig(ai);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    *playerSlot = gp;
    *reinterpret_cast<int32_t*>(p2 + 0xD4) = savedSector;
    for (int k = 0; k < 3; ++k)
        if (inv[k] < savedInv[k]) {
            *dropped += savedInv[k] - inv[k];
            inv[k] = savedInv[k];
        }
    return result;
}

void HookedAiUpdate(uintptr_t ai) {
    if (AnyRemoteActive()) {
        int dropped = 0;
        const int r = AiUpdateAsNearestGuarded(ai, &dropped);
        if (dropped && GetTickCount() - g_lastDamageDropLog > 3000) {
            g_lastDamageDropLog = GetTickCount();
            Log(L("[Gegner] %s greift den Mitspieler an - Schaden %d hier verworfen\n", "[Enemies] %s attacks the other player - damage %d discarded here\n"), ClassNameOf(ai).c_str(), dropped);
        }
        if (r != 0) return;
    }
    g_aiUpdateOrig(ai);
}

void HookAiUpdate() {
    // Original zuerst setzen: ab dem Patch kann der Spiel-Thread sofort in HookedAiUpdate landen
    g_aiUpdateOrig = reinterpret_cast<AiUpdateFn>(g_base + kAiUpdateRva);
    if (!PatchCallRel32(kAiUpdateCallRva, kAiUpdateRva, reinterpret_cast<void*>(HookedAiUpdate))) g_aiUpdateOrig = nullptr;
    Log(L("Hook Gegner (naeherer Spieler): %s\n", "Hook enemies (nearer player): %s\n"), g_aiUpdateOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// ---- Spieler bekommt Schaden: nie fuer die Figur des Mitspielers ----
// 0x46E030(spieler, schaden, quelle, art): zieht Leben im Inventar ab (Inventar vt[0x28](1, -schaden) -> Lebensanzeige,
// Treffer-Sound), Reaktion, bei 0 Tod. Greift ein Gegner bei uns die Figur des Mitspielers an, ist sie waehrend seines
// Updates "der Spieler" - dann wuerde das bei uns ablaufen. Der Mitspieler bekommt den Treffer in seinem eigenen Spiel.
// Die Funktion hat mehr Argumente als gedacht (ein C++-Umweg, der nur 4 weiterreicht, liess das Spiel beim eigenen Treffer
// abstuerzen). Deshalb ein kleiner Maschinencode-Vorschalter, der Register und Stack unangetastet laesst:
//   mov rax, &g_avatar / cmp rcx, [rax] / je ignorieren / cmp rcx, [rax+8] / je / cmp rcx, [rax+16] / je / jmp [Original]
//   ignorieren: mov rax, &g_ignoredHits / lock inc dword [rax] / ret
constexpr uintptr_t kPlayerDamageRva = 0x46E030;
volatile LONG g_ignoredHits = 0;
bool g_playerDamageHooked = false;
static_assert(sizeof(std::atomic<uintptr_t>) == sizeof(uintptr_t), "g_avatar muss aus schlichten Zeigern bestehen");
static_assert(kMaxRemote == 3, "Maschinencode in HookPlayerDamage vergleicht genau drei Figuren");

void* MakeTrampoline(uintptr_t target, size_t prologueLen) {
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    std::memcpy(tramp, reinterpret_cast<void*>(target), prologueLen);
    uint8_t* p = tramp + prologueLen;
    const uint64_t back = target + prologueLen;
    *p++ = 0xFF; *p++ = 0x25; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    std::memcpy(p, &back, 8);
    return tramp;
}

void PatchJump(uintptr_t target, size_t prologueLen, const void* dst) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(target), prologueLen, PAGE_EXECUTE_READWRITE, &old);
    auto* t = reinterpret_cast<uint8_t*>(target);
    const uint64_t d = reinterpret_cast<uint64_t>(dst);
    t[0] = 0xFF; t[1] = 0x25; t[2] = 0; t[3] = 0; t[4] = 0; t[5] = 0;
    std::memcpy(t + 6, &d, 8);
    for (size_t i = 14; i < prologueLen; ++i) t[i] = 0x90;
    VirtualProtect(reinterpret_cast<void*>(target), prologueLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, prologueLen);
}

void HookPlayerDamage() {
    // mov [rsp+18h],rbx / mov [rsp+20h],rsi / push rdi / sub rsp,80h
    static const uint8_t expected[18] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
                                         0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
    const uintptr_t target = g_base + kPlayerDamageRva;
    if (std::memcmp(reinterpret_cast<void*>(target), expected, sizeof(expected)) == 0) {
        void* tramp = MakeTrampoline(target, sizeof(expected));
        auto* s = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (tramp && s) {
            const uint64_t p2Addr = reinterpret_cast<uint64_t>(&g_avatar[0]);
            const uint64_t hitsAddr = reinterpret_cast<uint64_t>(&g_ignoredHits);
            const uint64_t orig = reinterpret_cast<uint64_t>(tramp);
            size_t i = 0;
            s[i++] = 0x48; s[i++] = 0xB8; std::memcpy(s + i, &p2Addr, 8); i += 8;  // mov rax, &g_avatar
            s[i++] = 0x48; s[i++] = 0x3B; s[i++] = 0x08;                          // cmp rcx, [rax]
            s[i++] = 0x74; s[i++] = 0x1A;                                         // je ignorieren (+26)
            s[i++] = 0x48; s[i++] = 0x3B; s[i++] = 0x48; s[i++] = 0x08;           // cmp rcx, [rax+8]
            s[i++] = 0x74; s[i++] = 0x14;                                         // je ignorieren (+20)
            s[i++] = 0x48; s[i++] = 0x3B; s[i++] = 0x48; s[i++] = 0x10;           // cmp rcx, [rax+16]
            s[i++] = 0x74; s[i++] = 0x0E;                                         // je ignorieren (+14)
            s[i++] = 0xFF; s[i++] = 0x25; s[i++] = 0; s[i++] = 0; s[i++] = 0; s[i++] = 0;  // jmp [rip+0]
            std::memcpy(s + i, &orig, 8); i += 8;
            s[i++] = 0x48; s[i++] = 0xB8; std::memcpy(s + i, &hitsAddr, 8); i += 8;  // ignorieren: mov rax, &g_ignoredHits
            s[i++] = 0xF0; s[i++] = 0xFF; s[i++] = 0x00;                            // lock inc dword [rax]
            s[i++] = 0xC3;                                                          // ret
            FlushInstructionCache(GetCurrentProcess(), s, i);
            PatchJump(target, sizeof(expected), s);
            g_playerDamageHooked = true;
        }
    }
    Log(L("Hook Spieler-Schaden: %s\n", "Hook player damage: %s\n"), g_playerDamageHooked ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// Spiel-Thread: ab und zu melden, dass Treffer auf die Figur des Mitspielers ignoriert wurden
void ReportIgnoredHits() {
    static LONG reported = 0;
    static DWORD last = 0;
    const LONG now = g_ignoredHits;
    if (now != reported && GetTickCount() - last > 3000) {
        Log(L("[Gegner] %ld Treffer auf die Figur des Mitspielers hier ignoriert\n", "[Enemies] %ld hits on the other player's figure ignored here\n"), now - reported);
        reported = now;
        last = GetTickCount();
    }
}

// ---- Gegner: Positionen vom Host ----
// Trotz gleicher Ziele laufen die beiden Simulationen auseinander. Der Host (kleinere Sitzungsnummer) schickt etwa 10x pro
// Sekunde Position und Blickrichtung (+0x18) der lebenden Gegner in der Naehe beider Spieler ("SME1"); beim anderen werden
// sie jedes Bild ein Stueck dorthin gezogen (alle Positionskopien + Rahmen verschoben, wie bei den Schiebebloecken).
static const uint32_t kEnemyPosCopies[] = {0x20, 0x80, 0xD8, 0xE4, 0x160, 0x1F0, 0x230};
constexpr float kEnemySyncRange = 4000.0f;   // Gegner so nah an einem Spieler werden geschickt
constexpr float kEnemySnapDist  = 1500.0f;   // weiter weg -> direkt hinsetzen
constexpr float kEnemyFollow    = 0.12f;     // Anteil der Abweichung pro Bild
std::vector<posecodec::EnemyPos> g_localEnemyPos;   // Host: zu sendende Liste (g_worldLock)
std::vector<posecodec::EnemyPos> g_remoteEnemyPos;  // vom Host (g_worldLock)
uint16_t g_remoteEnemyLevel = 0;
DWORD g_remoteEnemyTime = 0;

bool IsEnemyHost() { return net::IsHost(); }  // feste Rolle aus smcoop_net.txt (udppair: A, Steam: kleinere ID)

bool ReadEnemyPoseGuarded(uintptr_t obj, float pos[3], float* yaw) {
    __try {
        std::memcpy(pos, reinterpret_cast<void*>(obj + 0x20), 12);
        *yaw = *reinterpret_cast<float*>(obj + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MoveEnemyGuarded(uintptr_t obj, const float d[3], float yaw) {
    __try {
        for (uint32_t off : kEnemyPosCopies) {
            float* v = reinterpret_cast<float*>(obj + off);
            for (int i = 0; i < 3; ++i) v[i] += d[i];
        }
        *reinterpret_cast<float*>(obj + 0x18) = yaw;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Spiel-Thread (WorldSyncTick): Host sammelt die Gegner in der Naehe beider Spieler
void CollectEnemyPositions(uintptr_t gp) {
    static int lastRole = -1;
    const int role = IsEnemyHost() ? 1 : 0;
    if (role != lastRole && role >= 0) {
        Log(L("[Gegner] Positionen: %s\n", "[Enemies] positions: %s\n"), role ? L("ich bin Host (schicke sie)", "I am the host (sending them)") : L("Mitspieler ist Host (meine Gegner folgen ihm)", "other player is the host (my enemies follow them)"));
    }
    lastRole = role;
    std::vector<posecodec::EnemyPos> list;
    if (IsEnemyHost() && AnyRemoteActive()) {
        float me[3] = {}, others[kMaxRemote][3] = {};
        bool haveOther[kMaxRemote] = {};
        SafeRead(gp + 0x20, me, sizeof(me));
        for (int pi = 0; pi < kMaxRemote; ++pi)
            haveOther[pi] = g_avatar[pi] && g_remoteActive[pi] && SafeRead(g_avatar[pi] + 0x20, others[pi], sizeof(others[pi]));
        std::vector<std::pair<float, posecodec::EnemyPos>> found;
        for (const auto& kv : g_trackedEnemies) {
            if (!kv.second.alive || !kv.second.id) continue;
            posecodec::EnemyPos e;
            e.serial = kv.second.id;  // Kennung, nicht die Seriennummer (die unterscheidet sich zwischen den PCs)
            if (!ReadEnemyPoseGuarded(kv.second.obj, e.pos, &e.yaw)) continue;
            auto d2 = [&e](const float* p) {
                const float x = p[0] - e.pos[0], y = p[1] - e.pos[1], z = p[2] - e.pos[2];
                return x * x + y * y + z * z;
            };
            float best = d2(me);
            for (int pi = 0; pi < kMaxRemote; ++pi)
                if (haveOther[pi]) best = (std::min)(best, d2(others[pi]));
            if (best > kEnemySyncRange * kEnemySyncRange) continue;
            found.push_back({best, e});
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < found.size() && i < static_cast<size_t>(posecodec::kMaxEnemyPos); ++i) list.push_back(found[i].second);
    }
    AcquireSRWLockExclusive(&g_worldLock);
    g_localEnemyPos.swap(list);
    ReleaseSRWLockExclusive(&g_worldLock);
}

// Spiel-Thread, jedes Bild: nicht-Host zieht seine Gegner zum Stand des Hosts
void EnemyFrameUpdate() {
    if (!HostActive() || IsEnemyHost() || g_trackedEnemies.empty()) return;
    std::vector<posecodec::EnemyPos> remote;
    AcquireSRWLockShared(&g_worldLock);
    if (g_remoteEnemyLevel == static_cast<uint16_t>(g_worldLevel) && GetTickCount() - g_remoteEnemyTime < 1000) remote = g_remoteEnemyPos;
    ReleaseSRWLockShared(&g_worldLock);
    for (const auto& e : remote) {
        const auto m = g_enemyById.find(e.serial);
        if (m == g_enemyById.end()) continue;
        auto it = g_trackedEnemies.find(m->second);
        if (it == g_trackedEnemies.end() || !it->second.alive) continue;
        float pos[3], yaw = 0.0f;
        if (!ReadEnemyPoseGuarded(it->second.obj, pos, &yaw)) continue;
        float d[3] = {e.pos[0] - pos[0], e.pos[1] - pos[1], e.pos[2] - pos[2]};
        const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (dist2 < 1.0f) continue;
        if (dist2 < kEnemySnapDist * kEnemySnapDist)
            for (float& c : d) c *= kEnemyFollow;
        MoveEnemyGuarded(it->second.obj, d, e.yaw);
    }
}

// Netz-Thread
void SendEnemies() {
    posecodec::EnemyPos list[posecodec::kMaxEnemyPos];
    int num = 0;
    uint16_t level = 0;
    AcquireSRWLockShared(&g_worldLock);
    level = g_localPickupsLevel;
    for (const auto& e : g_localEnemyPos)
        if (num < posecodec::kMaxEnemyPos) list[num++] = e;
    ReleaseSRWLockShared(&g_worldLock);
    if (!num) return;
    uint8_t buf[posecodec::MaxEnemyPacketSize()];
    net::Send(buf, posecodec::EncodeEnemies(level, list, num, buf));
}

// Netz-Thread: false, wenn es kein Gegner-Positions-Paket war
bool HandleEnemyPacket(int from, const uint8_t* data, size_t size) {
    static posecodec::EnemyPos list[posecodec::kMaxEnemyPos];
    uint16_t level = 0;
    int num = 0;
    if (!posecodec::DecodeEnemies(data, size, level, list, num)) return false;
    if (!FromHost(from)) return true;  // Gegner-Positionen gelten nur vom Host
    AcquireSRWLockExclusive(&g_worldLock);
    g_remoteEnemyLevel = level;
    g_remoteEnemyPos.assign(list, list + num);
    g_remoteEnemyTime = GetTickCount();
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// ---- Phase 4: Gast uebernimmt die Welt des Hosts ----
// Der Host schickt alle 2 s seine Zustandstabelle fuer die aktuelle Level-Gruppe ("SMH1", Stuecke a 64 Eintraege). Der Gast
// gleicht ab, was bei ihm anders ist: Tueren/Schloesser ueber die Regeln (Neustart/Signal), Gegner mit Wert 2 sterben,
// Pickups mit 1002 (eingesammelt) und zerstoerte Behaelter verschwinden. Quest-Gegenstaende bleiben (kommt mit "gleicher
// Fortschritt"). Jeder Eintrag wird je Wert nur einmal angewendet.
constexpr DWORD kSnapIntervalMs = 2000;
std::vector<posecodec::SnapEntry> g_hostSnapOut;      // Host: zu sendende Eintraege (g_worldLock)
int16_t g_hostSnapOutGroup = -1;                      // (g_worldLock)
std::map<StateKey, int> g_hostSnapIn;                 // Gast: empfangener Stand des Hosts (g_worldLock)
uint16_t g_hostSnapInLevel = 0;                       // (g_worldLock)
std::map<StateKey, int> g_snapApplied;                // Gast: angewendeter Wert je Eintrag (Spiel-Thread)

int CurrentLevelGroupGuarded() {
    __try {
        const uintptr_t gs = reinterpret_cast<uintptr_t (*)()>(g_base + 0x2E98B0)();
        const int id = *reinterpret_cast<int*>(gs + 0x2C);
        return reinterpret_cast<int (*)(int)>(g_base + 0x2EA000)(id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Tabelle lesen (Eintraege der Gruppe group); false bei Ausnahme
bool ReadStateTableGuarded(int group, posecodec::SnapEntry* out, int maxOut, int* count) {
    *count = 0;
    __try {
        const uintptr_t mgr = g_base + kSaveMgrRva;
        const size_t n = *reinterpret_cast<size_t*>(mgr + 0xC090);
        if (n > 0x800) return false;
        for (size_t i = 0; i < n && *count < maxOut; ++i) {
            const uintptr_t e = mgr + 0x90 + i * 0x18;
            if (*reinterpret_cast<int*>(e + 0xC) != group) continue;
            posecodec::SnapEntry& s = out[(*count)++];
            std::memcpy(s.pos, reinterpret_cast<void*>(e), 12);
            s.type = *reinterpret_cast<int*>(e + 0x10);
            s.value = *reinterpret_cast<int*>(e + 0x14);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- ganze Zustandstabelle (alle Level-Gruppen, "SMT1") ----
// Der Gast uebernimmt die Eintraege des Hosts fuer alle Gruppen ausser seiner aktuellen (dort leben die Objekte, das macht
// der Abgleich oben): nur in die Tabelle schreiben bzw. anhaengen, so wie 0x33F510 es tut. Je Eintrag nur, wenn sich der
// Wert beim Host seit der letzten Uebernahme geaendert hat - eigener Fortschritt des Gasts in anderen Leveln bleibt also.
// Nach dem Laden eines Spielstands (g_tableResync) gilt wieder alles vom Host.
struct TableKey {
    int group, type;
    float pos[3];
    bool operator<(const TableKey& o) const {
        if (group != o.group) return group < o.group;
        if (type != o.type) return type < o.type;
        return std::memcmp(pos, o.pos, sizeof(pos)) < 0;
    }
};
std::vector<posecodec::TableEntry> g_hostTableOut;  // Host: zu senden (g_worldLock)
std::map<TableKey, int> g_hostTableIn;              // Gast: Tabelle des Hosts (g_worldLock)
bool g_hostTableDirty = false;                      // (g_worldLock)
std::map<TableKey, int> g_tableApplied;             // Gast: uebernommener Wert je Eintrag (Spiel-Thread)
std::atomic<bool> g_tableResync{false};

bool ReadWholeTableGuarded(posecodec::TableEntry* out, int maxOut, int* count) {
    *count = 0;
    __try {
        const uintptr_t mgr = g_base + kSaveMgrRva;
        const size_t n = *reinterpret_cast<size_t*>(mgr + 0xC090);
        if (n > 0x800) return false;
        for (size_t i = 0; i < n && *count < maxOut; ++i) {
            const uintptr_t e = mgr + 0x90 + i * 0x18;
            posecodec::TableEntry& t = out[(*count)++];
            std::memcpy(t.pos, reinterpret_cast<void*>(e), 12);
            t.group = *reinterpret_cast<int*>(e + 0xC);
            t.type = *reinterpret_cast<int*>(e + 0x10);
            t.value = *reinterpret_cast<int*>(e + 0x14);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 0 = schon gleich, 1 = Wert geaendert, 2 = angehaengt, -1 = Fehler/Tabelle voll
int WriteTableEntryGuarded(const TableKey& k, int value) {
    __try {
        const uintptr_t mgr = g_base + kSaveMgrRva;
        size_t* count = reinterpret_cast<size_t*>(mgr + 0xC090);
        const size_t n = *count;
        if (n > 0x800) return -1;
        for (size_t i = 0; i < n; ++i) {
            const uintptr_t e = mgr + 0x90 + i * 0x18;
            if (*reinterpret_cast<int*>(e + 0xC) != k.group || *reinterpret_cast<int*>(e + 0x10) != k.type ||
                std::memcmp(reinterpret_cast<void*>(e), k.pos, 12) != 0)
                continue;
            int* v = reinterpret_cast<int*>(e + 0x14);
            if (*v == value) return 0;
            *v = value;
            return 1;
        }
        if (n >= 0x800) return -1;
        const uintptr_t e = mgr + 0x90 + n * 0x18;
        std::memcpy(reinterpret_cast<void*>(e), k.pos, 12);
        *reinterpret_cast<int*>(e + 0xC) = k.group;
        *reinterpret_cast<int*>(e + 0x10) = k.type;
        *reinterpret_cast<int*>(e + 0x14) = value;
        *count = n + 1;
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Netz-Thread: false, wenn es kein Tabellen-Paket war
bool HandleTablePacket(int from, const uint8_t* data, size_t size) {
    static posecodec::TableEntry list[posecodec::kTablePerChunk];
    posecodec::SnapHeader h{};
    if (!posecodec::DecodeTable(data, size, h, list)) return false;
    if (!FromHost(from)) return true;  // nur Gaeste uebernehmen, nur vom Host
    static bool logged = false;
    if (!logged) { logged = true; Log(L("[Host-Welt] ganze Tabelle vom Host: Stueck %u/%u, %u Eintraege insgesamt\n", "[Host world] full table from host: part %u/%u, %u entries total\n"), h.chunk + 1, h.chunks, h.total); }
    AcquireSRWLockExclusive(&g_worldLock);
    for (int i = 0; i < h.num; ++i) {
        TableKey key{list[i].group, list[i].type, {list[i].pos[0], list[i].pos[1], list[i].pos[2]}};
        auto it = g_hostTableIn.find(key);
        if (it != g_hostTableIn.end() && it->second == list[i].value) continue;
        g_hostTableIn[key] = list[i].value;
        g_hostTableDirty = true;
    }
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// Spiel-Thread (etwa jede Sekunde): Gast schreibt den Stand des Hosts fuer die anderen Level in seine Tabelle
void ApplyHostTable() {
    if (net::IsHost() || !HostActive()) return;
    if (g_tableResync.exchange(false)) g_tableApplied.clear();
    const int group = CurrentLevelGroupGuarded();
    if (group < 0) return;
    static int lastGroup = -1;
    std::map<TableKey, int> host;
    AcquireSRWLockExclusive(&g_worldLock);
    if (g_hostTableDirty || group != lastGroup) {
        host = g_hostTableIn;
        g_hostTableDirty = false;
    }
    ReleaseSRWLockExclusive(&g_worldLock);
    lastGroup = group;
    int changed = 0, added = 0;
    for (const auto& kv : host) {
        if (kv.first.group == group) continue;  // aktuelle Gruppe: Objekte leben, siehe ApplyHostSnapshot
        auto ap = g_tableApplied.find(kv.first);
        if (ap != g_tableApplied.end() && ap->second == kv.second) continue;
        const int r = WriteTableEntryGuarded(kv.first, kv.second);
        if (r < 0) break;
        g_tableApplied[kv.first] = kv.second;
        if (r == 1) ++changed;
        if (r == 2) ++added;
    }
    if (changed || added)
        Log(L("[Host-Welt] andere Level: %d Eintraege geaendert, %d neu (Tabelle des Hosts)\n", "[Host world] other levels: %d entries changed, %d new (host's table)\n"), changed, added);
}

// Pool-Index des Objekts zu Art + Ort (-1 = nicht da)
int FindStateIndexGuarded(int type, const float pos[3], int n) {
    for (int i = 0; i < n; ++i)
        if (StateKeyMatchesGuarded(g_poolSnap[i].obj, type, pos)) return i;
    return -1;
}

// Quest-Gegenstaende haben keinen Tabellen-Eintrag; ob es sie noch gibt, steht im gespeicherten Objekt-Pool. Der Host
// schickt seine Liste (Art + Ort, "SMQ1"), der Gast entfernt, was beim Host fehlt (Besitz kommt ueber die Story-Flags).
std::vector<posecodec::QuestItem> g_hostQuestOut;  // Host: zu senden (g_worldLock)
std::vector<posecodec::QuestItem> g_hostQuestIn;   // Gast: Liste des Hosts (g_worldLock)
uint16_t g_hostQuestInLevel = 0;                   // (g_worldLock)
bool g_hostQuestInValid = false;                   // (g_worldLock)

bool IsQuestItemKeyGuarded(uintptr_t obj, posecodec::QuestItem* q) {
    __try {
        q->type = *reinterpret_cast<int*>(obj + 0xD0);
        std::memcpy(q->pos, reinterpret_cast<void*>(obj + 0x38), 12);
        return !(q->pos[0] == 0.0f && q->pos[1] == 0.0f && q->pos[2] == 0.0f);  // Ursprung = getragen / Platzhalter
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::vector<posecodec::QuestItem> CollectQuestItems(int n) {
    std::vector<posecodec::QuestItem> list;
    for (int i = 0; i < n && list.size() < static_cast<size_t>(posecodec::kMaxQuestItems); ++i) {
        const PoolNodeInfo& p = g_poolSnap[i];
        if (p.state == 0x10 || ClassNameOf(p.obj) != "QuestObject") continue;
        posecodec::QuestItem q;
        if (IsQuestItemKeyGuarded(p.obj, &q)) list.push_back(q);
    }
    return list;
}

// Spiel-Thread (WorldSyncTick): Host legt seinen Stand bereit
void CollectHostSnapshot(int n) {
    static DWORD last = 0;
    if (!net::IsHost() || !AnyRemoteActive() || GetTickCount() - last < kSnapIntervalMs) return;
    last = GetTickCount();
    const int group = CurrentLevelGroupGuarded();
    if (group < 0) return;
    std::vector<posecodec::SnapEntry> list(0x800);
    int count = 0;
    if (!ReadStateTableGuarded(group, list.data(), static_cast<int>(list.size()), &count)) return;
    list.resize(count);
    std::vector<posecodec::QuestItem> quests = CollectQuestItems(n);
    static int tableTick = 0;
    std::vector<posecodec::TableEntry> table;
    if (++tableTick >= 5) {  // ganze Tabelle (alle Level) etwa alle 10 s
        tableTick = 0;
        table.resize(0x800);
        int tcount = 0;
        if (ReadWholeTableGuarded(table.data(), static_cast<int>(table.size()), &tcount)) table.resize(tcount);
        else table.clear();
        static bool logged = false;
        if (!logged) { logged = true; Log(L("[Host-Welt] ganze Tabelle wird gesendet: %d Eintraege\n", "[Host world] sending full table: %d entries\n"), tcount); }
    }
    AcquireSRWLockExclusive(&g_worldLock);
    g_hostSnapOut.swap(list);
    g_hostSnapOutGroup = static_cast<int16_t>(group);
    g_hostQuestOut.swap(quests);
    if (!table.empty()) g_hostTableOut.swap(table);
    ReleaseSRWLockExclusive(&g_worldLock);
}

// Netz-Thread: Host verschickt den bereitgelegten Stand (einmal je Bereitstellung)
void SendHostSnapshot() {
    std::vector<posecodec::SnapEntry> list;
    int16_t group = -1;
    uint16_t level = 0;
    std::vector<posecodec::QuestItem> quests;
    std::vector<posecodec::TableEntry> table;
    AcquireSRWLockExclusive(&g_worldLock);
    list.swap(g_hostSnapOut);
    quests.swap(g_hostQuestOut);
    table.swap(g_hostTableOut);
    group = g_hostSnapOutGroup;
    level = g_localPickupsLevel;
    ReleaseSRWLockExclusive(&g_worldLock);
    if (!table.empty()) {
        const int chunks = static_cast<int>((table.size() + posecodec::kTablePerChunk - 1) / posecodec::kTablePerChunk);
        uint8_t buf[posecodec::MaxTablePacketSize()];
        for (int c = 0; c < chunks; ++c) {
            posecodec::SnapHeader h{};
            h.level = level;
            h.group = group;
            h.chunk = static_cast<uint16_t>(c);
            h.chunks = static_cast<uint16_t>(chunks);
            h.total = static_cast<uint16_t>(table.size());
            const size_t first = static_cast<size_t>(c) * posecodec::kTablePerChunk;
            h.num = static_cast<uint16_t>((std::min)(table.size() - first, static_cast<size_t>(posecodec::kTablePerChunk)));
            net::Send(buf, posecodec::EncodeTable(h, table.data() + first, buf));
        }
    }
    if (list.empty()) return;
    {
        uint8_t qbuf[posecodec::MaxQuestPacketSize()];
        net::Send(qbuf, posecodec::EncodeQuests(level, quests.data(), static_cast<int>(quests.size()), qbuf));
    }
    const int chunks = static_cast<int>((list.size() + posecodec::kSnapPerChunk - 1) / posecodec::kSnapPerChunk);
    uint8_t buf[posecodec::MaxSnapPacketSize()];
    for (int c = 0; c < chunks; ++c) {
        posecodec::SnapHeader h{};
        h.level = level;
        h.group = group;
        h.chunk = static_cast<uint16_t>(c);
        h.chunks = static_cast<uint16_t>(chunks);
        h.total = static_cast<uint16_t>(list.size());
        const size_t first = static_cast<size_t>(c) * posecodec::kSnapPerChunk;
        h.num = static_cast<uint16_t>((std::min)(list.size() - first, static_cast<size_t>(posecodec::kSnapPerChunk)));
        net::Send(buf, posecodec::EncodeSnap(h, list.data() + first, buf));
    }
}

// Netz-Thread: false, wenn es kein Quest-Listen-Paket war
bool HandleQuestPacket(int from, const uint8_t* data, size_t size) {
    static posecodec::QuestItem list[posecodec::kMaxQuestItems];
    uint16_t level = 0;
    int num = 0;
    if (!posecodec::DecodeQuests(data, size, level, list, num)) return false;
    if (!FromHost(from)) return true;
    AcquireSRWLockExclusive(&g_worldLock);
    g_hostQuestIn.assign(list, list + num);
    g_hostQuestInLevel = level;
    g_hostQuestInValid = true;
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

bool OwnsItemGuarded(int itemId);

// Spiel-Thread (etwa jede Sekunde): Gast entfernt Quest-Gegenstaende, die beim Host schon aufgehoben sind - aber nur, wenn
// er selbst den Gegenstand schon besitzt (sonst hebt er ihn selbst auf; Waffen werden nicht mehr laufend geteilt)
void ApplyHostQuestList(int n) {
    if (net::IsHost() || !HostActive()) return;
    std::vector<posecodec::QuestItem> host;
    bool valid = false;
    AcquireSRWLockShared(&g_worldLock);
    if (g_hostQuestInValid && g_hostQuestInLevel == static_cast<uint16_t>(g_worldLevel)) {
        host = g_hostQuestIn;
        valid = true;
    }
    ReleaseSRWLockShared(&g_worldLock);
    if (!valid) return;
    for (int i = 0; i < n; ++i) {
        const PoolNodeInfo& p = g_poolSnap[i];
        if (p.state == 0x10 || ClassNameOf(p.obj) != "QuestObject") continue;
        posecodec::QuestItem q;
        if (!IsQuestItemKeyGuarded(p.obj, &q)) continue;
        bool atHost = false;
        for (const auto& h : host)
            if (h.type == q.type && std::memcmp(h.pos, q.pos, sizeof(q.pos)) == 0) { atHost = true; break; }
        if (atHost || !OwnsItemGuarded(q.type)) continue;
        if (RemoveNodeGuarded(p.node))
            Log(L("[Host-Welt] Quest-Gegenstand #%u (Art %d bei %.0f/%.0f/%.0f) hat der Host schon, du auch - entfernt\n", "[Host world] quest item #%u (type %d at %.0f/%.0f/%.0f): the host already has it, so do you - removed\n"), p.serial,
                q.type, q.pos[0], q.pos[1], q.pos[2]);
    }
}

// Netz-Thread: false, wenn es kein Host-Stand-Paket war
bool HandleSnapPacket(int from, const uint8_t* data, size_t size) {
    static posecodec::SnapEntry list[posecodec::kSnapPerChunk];
    posecodec::SnapHeader h{};
    if (!posecodec::DecodeSnap(data, size, h, list)) return false;
    if (!FromHost(from)) return true;  // nur Gaeste uebernehmen, nur vom Host
    AcquireSRWLockExclusive(&g_worldLock);
    if (h.level != g_hostSnapInLevel) g_hostSnapIn.clear();
    g_hostSnapInLevel = h.level;
    for (int i = 0; i < h.num; ++i) {
        StateKey key{list[i].type, {list[i].pos[0], list[i].pos[1], list[i].pos[2]}};
        g_hostSnapIn[key] = list[i].value;
    }
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// Spiel-Thread (WorldSyncTick, etwa jede Sekunde): Gast gleicht seine Welt an den Host an
void ApplyHostSnapshot(int n) {
    if (net::IsHost() || !HostActive() || !g_setStateOrig) return;
    std::map<StateKey, int> snap;
    AcquireSRWLockShared(&g_worldLock);
    if (g_hostSnapInLevel == static_cast<uint16_t>(g_worldLevel)) snap = g_hostSnapIn;
    ReleaseSRWLockShared(&g_worldLock);
    int doors = 0, enemies = 0, removed = 0, tableOnly = 0;
    for (const auto& kv : snap) {
        auto ap = g_snapApplied.find(kv.first);
        if (ap != g_snapApplied.end() && ap->second == kv.second) continue;
        const int value = kv.second;
        const int old = GetStateGuarded(kv.first.type, kv.first.pos);
        if (old == -2) continue;
        g_snapApplied[kv.first] = value;
        if (old == value) continue;
        const int idx = FindStateIndexGuarded(kv.first.type, kv.first.pos, n);
        const std::string cls = idx >= 0 ? ClassNameOf(g_poolSnap[idx].obj) : "-";
        if (cls == "QuestObject") continue;  // kommt mit "gleicher Fortschritt"
        g_applyingRemoteState = true;
        const bool ok = SetStateGuarded(kv.first.type, kv.first.pos, value);
        g_applyingRemoteState = false;
        if (!ok) continue;
        if (idx < 0) { ++tableOnly; continue; }
        const PoolNodeInfo& p = g_poolSnap[idx];
        if (IsEnemyVtable(p.vt, p.obj)) {
            if (value == 2 && KillEnemyGuarded(p.obj)) ++enemies;
        } else if (cls == "Object" || cls == "DarkSoul") {
            if (value == 1002 && RemoveNodeGuarded(p.node)) ++removed;  // beim Host eingesammelt
        } else if (cls == "ExplodeObject" || cls == "AIGovi") {
            if (RemoveNodeGuarded(p.node)) ++removed;  // beim Host zerstoert
        } else if (const RestartRule* rule = FindRestartRule(p.vt)) {
            if (!(rule->onlyNew && old != -1)) {
                g_pendingRestarts.push_back({p.obj, rule, GetTickCount()});
                ++doors;
            }
        } else {
            ++tableOnly;
        }
        Log(L("[Host-Welt] %s #%u Art %d: %d -> %d\n", "[Host world] %s #%u type %d: %d -> %d\n"), cls.c_str(), p.serial, kv.first.type, old, value);
    }
    if (doors || enemies || removed || tableOnly)
        Log(L("[Host-Welt] uebernommen: %d Tueren/Schloesser, %d Gegner, %d entfernt, %d nur Tabelle\n", "[Host world] applied: %d doors/locks, %d enemies, %d removed, %d table only\n"), doors, enemies, removed, tableOnly);
}

// ---- Phase 4: gleicher Fortschritt ----
// Story-Fortschritt steht im Bitfeld des SaveManagers ([mgr+0x140A8], Laenge in Bytes mgr+0x140B4, Namen CF_... in
// 0xCA2A90 + id*8): Gad-Kraefte, Prismen, tote Bosse, Teddy-Warps, besuchte Level ... Beide schicken alle 2 s ihren Stand
// ("SMF2"); fehlende Flags setzt jeder ueber die Spiel-Funktion Set (vt[1] 0x33EE50, mit ihren Nebenwirkungen, z.B.
// Kraft freischalten). Dazu das Maximum der Inventar-Zaehler 0 (max. Leben), 3 (Schatten-Level = Stufe*1000), 8 (Cadeaux)
// und der Teddy-Menue-Statistik: je Level gesammelte Cadeaux (0xD91FE0, 19 x [gesamt, gesammelt]) und Dunkle Seelen
// (0xD8B940, gleich aufgebaut) sowie die Einzel-Seelen des Seelen-Managers (Getter 0x2E0AA0: +8 Gesamtzahl, [+0x18] Bytes je
// Seele, +0x20 Anzahl; Einsammeln 0x2DF800 setzt das Byte, den Level-Zaehler und +8).
constexpr uintptr_t kSaveMgrSetFlagRva = 0x33EE50;
constexpr uintptr_t kFlagNamesRva = 0xCA2A90;
constexpr uintptr_t kCadeauxLevelsRva = 0xD91FE0;
constexpr uintptr_t kSoulLevelsRva = 0xD8B940;
constexpr uintptr_t kSoulManagerGetRva = 0x2E0AA0;
// Gegenstands-/Waffenbesitz: kexShadowManInventoryLocal (Getter 0x310A60, Objekt 0xF9C380, vt 0x6EE878), 30 Slots ab +8
// (0 = nicht besessen, 1/2 = in der linken/rechten Hand, 3 = besessen). Slot-Definitionen 0xC9AF00, je 32 Byte (+0 Item-Id,
// +8 Name "$i_..."). Aufheben = vt[6] 0x309520 (Slot = 3); Akkumulator, Retraktor und Prisma zaehlen zusaetzlich in +0x84,
// +0x82, +0x80 (Verbrauchsgegenstaende, darum nicht abgeglichen, sonst wuerden sie vervielfaeltigt).
constexpr uintptr_t kItemSlotsRva = 0xF9C380 + 8;
// Gefundene Geheimnisse: Bitmaske GameState+0x48 (gesetzt in 0x2D5C00(bit); das Teddy-Menue testet das Bit je Aufgabe)
constexpr uint32_t kGameStateSecretsOff = 0x48;
constexpr uintptr_t kItemSlotDefsRva = 0xC9AF00;
static bool IsCountedItemSlot(int slot) { return slot == 5 || slot == 20 || slot == 25; }

const char* ItemSlotName(int slot) {
    static char name[48];
    uintptr_t p = 0;
    name[0] = 0;
    if (SafeRead(g_base + kItemSlotDefsRva + static_cast<uintptr_t>(slot) * 32 + 8, &p, 8) && p) SafeRead(p, name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    return name[0] ? name : "?";
}
static const int kProgressInvTypeIds[posecodec::kProgressInvTypes] = {0, 3, 8};
posecodec::ProgressData g_remoteProgress[kMaxRemote];   // je Mitspieler (g_worldLock)
DWORD g_remoteProgressTime[kMaxRemote] = {};             // (g_worldLock)
posecodec::ProgressData g_localProgressOut; // Spiel-Thread legt bereit, Netz-Thread sendet (g_worldLock)

bool ReadFlagsGuarded(uint8_t* out, int* bytes) {
    __try {
        const uintptr_t mgr = g_base + kSaveMgrRva;
        const int len = *reinterpret_cast<int*>(mgr + 0x140B4);
        const uint8_t* bits = *reinterpret_cast<uint8_t**>(mgr + 0x140A8);
        if (len <= 0 || len > posecodec::kMaxFlagBytes || !bits) return false;
        std::memcpy(out, bits, len);
        *bytes = len;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetFlagGuarded(int id) {
    __try {
        reinterpret_cast<void (*)(void*, int)>(g_base + kSaveMgrSetFlagRva)(SaveMgr(), id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Seelen-Manager: Zeiger auf die Bytes je Seele und deren Anzahl (nullptr bei Fehler)
uintptr_t SoulManagerGuarded() {
    __try {
        return reinterpret_cast<uintptr_t (*)()>(g_base + kSoulManagerGetRva)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadSoulsGuarded(uintptr_t mgr, uint8_t* out, int* count) {
    __try {
        const uint8_t* bytes = *reinterpret_cast<uint8_t**>(mgr + 0x18);
        const size_t n = *reinterpret_cast<size_t*>(mgr + 0x20);
        if (!bytes || n == 0 || n > posecodec::kMaxSoulBytes) return false;
        std::memcpy(out, bytes, n);
        *count = static_cast<int>(n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// fehlende Seelen-Bytes setzen und die Gesamtzahl (+8) neu zaehlen; liefert die Zahl neu gesetzter Seelen
int MergeSoulsGuarded(uintptr_t mgr, const uint8_t* remote, int remoteCount) {
    __try {
        uint8_t* bytes = *reinterpret_cast<uint8_t**>(mgr + 0x18);
        const size_t n = *reinterpret_cast<size_t*>(mgr + 0x20);
        if (!bytes || n == 0 || n > posecodec::kMaxSoulBytes) return 0;
        int added = 0, total = 0;
        for (size_t i = 0; i < n; ++i) {
            if (static_cast<int>(i) < remoteCount && remote[i] && !bytes[i]) {
                bytes[i] = 1;
                ++added;
            }
            if (bytes[i]) ++total;
        }
        if (added) *reinterpret_cast<int*>(mgr + 8) = total;
        return added;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 0x459C00(spieler): Gad-Kraefte aus den Flags 0x43..0x46 (Touch/Walk/Swim-Gad, Poigne) neu setzen (so beim Aufheben)
bool RefreshGadPowersGuarded() {
    __try {
        const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
        if (!gp) return false;
        reinterpret_cast<void (*)(uintptr_t)>(g_base + 0x459C00)(gp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* FlagName(int id) {
    static char name[64];
    uintptr_t p = 0;
    name[0] = 0;
    if (SafeRead(g_base + kFlagNamesRva + static_cast<uintptr_t>(id) * 8, &p, 8) && p) SafeRead(p, name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    return name[0] ? name : "?";
}

// Richtung: Der Host schickt seinen ganzen Stand, der Gast nur, was er seit dem Laden dazugewonnen hat (sonst bekaeme der
// Host alles aus einem weiter fortgeschrittenen Spielstand des Gasts). Waffen/Quest-Gegenstaende: der Gast uebernimmt die des
// Hosts nur einmal je Laden; danach hebt jeder sie selbst auf (der Gegenstand liegt ja in jeder Welt noch da).
posecodec::ProgressData g_progressBaseline;        // Gast: Stand direkt nach dem Laden (Spiel-Thread)
bool g_progressBaselineValid = false;              // (Spiel-Thread)
std::atomic<bool> g_progressBaselineReset{true};   // nach Laden / Start aus dem Hauptmenue neu erfassen
bool g_itemsFromHostDone = false;                  // Gast: Gegenstaende des Hosts schon uebernommen (Spiel-Thread)
// Gast: Host, auf dessen Cadeaux/max. Leben schon aufgeholt wurde (einmal je Host und Spielstart, nicht je Laden - sonst
// kaemen abgegebene Cadeaux nach Speichern + Neuladen wieder; Spiel-Thread)
uint64_t g_invCaughtUpHost = 0;

// Gast: nur Zugewinne seit dem Grundstand weitergeben
void MaskToGains(posecodec::ProgressData& d, const posecodec::ProgressData& base) {
    for (int i = 0; i < d.flagBytes && i < base.flagBytes; ++i) d.flags[i] &= static_cast<uint8_t>(~base.flags[i]);
    for (int i = 0; i < d.soulBytes && i < base.soulBytes; ++i)
        if (base.souls[i]) d.souls[i] = 0;
    d.secrets &= ~base.secrets;
    for (int k = 0; k < posecodec::kProgressInvTypes; ++k)
        if (d.inv[k] <= base.inv[k]) d.inv[k] = 0;
    for (int i = 0; i < posecodec::kProgressLevels; ++i) {
        if (d.cadeauxGot[i] <= base.cadeauxGot[i]) d.cadeauxGot[i] = 0;
        if (d.soulsGot[i] <= base.soulsGot[i]) d.soulsGot[i] = 0;
    }
    std::memset(d.items, 0, sizeof(d.items));
}

// besitzt der Spieler einen Gegenstand mit dieser Nummer (Slot-Definition +0 = Nummer, wie QuestObject +0xD0)?
bool OwnsItemGuarded(int itemId) {
    __try {
        const int32_t* slots = reinterpret_cast<const int32_t*>(g_base + kItemSlotsRva);
        for (int i = 0; i < posecodec::kProgressItems; ++i)
            if (slots[i] != 0 && *reinterpret_cast<const int16_t*>(g_base + kItemSlotDefsRva + static_cast<uintptr_t>(i) * 32) == itemId)
                return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Spiel-Thread (etwa jede Sekunde): eigenen Stand bereitlegen, fehlenden Fortschritt des Mitspielers uebernehmen
void ProgressTick() {
    const LevelInfo li = GetLevelInfo(g_base);
    if (!li.inGame) return;
    posecodec::ProgressData mine;
    if (!ReadFlagsGuarded(mine.flags, &mine.flagBytes)) return;
    int* inv = reinterpret_cast<int*>(g_base + kInventoryCountRva);
    for (int k = 0; k < posecodec::kProgressInvTypes; ++k) mine.inv[k] = inv[kProgressInvTypeIds[k]];
    uint8_t* cadeaux = reinterpret_cast<uint8_t*>(g_base + kCadeauxLevelsRva);  // [gesamt, gesammelt] je Level
    uint8_t* souls = reinterpret_cast<uint8_t*>(g_base + kSoulLevelsRva);
    for (int i = 0; i < posecodec::kProgressLevels; ++i) {
        mine.cadeauxGot[i] = cadeaux[i * 2 + 1];
        mine.soulsGot[i] = souls[i * 2 + 1];
    }
    const uintptr_t soulMgr = SoulManagerGuarded();
    if (soulMgr) ReadSoulsGuarded(soulMgr, mine.souls, &mine.soulBytes);
    int32_t* items = reinterpret_cast<int32_t*>(g_base + kItemSlotsRva);
    std::memcpy(mine.items, items, sizeof(mine.items));
    uint32_t* secrets = reinterpret_cast<uint32_t*>(g_base + kGameStateRva + kGameStateSecretsOff);
    mine.secrets = *secrets;
    if (g_progressBaselineReset.exchange(false)) {
        g_progressBaseline = mine;
        g_progressBaselineValid = true;
        g_itemsFromHostDone = false;
        if (!net::IsHost()) Log(L("[Fortschritt] Grundstand nach dem Laden erfasst - an den Host gehen nur neue Fortschritte\n", "[Progress] baseline after loading recorded - only new progress goes to the host\n"));
        return;  // Grundstand unverfaelscht lassen, Uebernahme ab dem naechsten Durchlauf
    }
    posecodec::ProgressData out = mine;
    if (!net::IsHost() && g_progressBaselineValid) MaskToGains(out, g_progressBaseline);
    // Je Durchlauf genau einen Mitspieler uebernehmen (reihum; "mine" ist danach veraltet): der Host nimmt die Zugewinne
    // aller Gaeste, ein Gast nur den Stand des Hosts.
    static int nextPeer = 0;
    posecodec::ProgressData remote;
    bool haveRemote = false;
    AcquireSRWLockExclusive(&g_worldLock);
    g_localProgressOut = out;
    for (int k = 0; k < kMaxRemote && !haveRemote; ++k) {
        const int pi = (nextPeer + k) % kMaxRemote;
        if (!net::IsHost() && pi != HostSlot()) continue;
        if (g_remoteProgress[pi].flagBytes <= 0 || GetTickCount() - g_remoteProgressTime[pi] >= 5000 || !g_peerInGame[pi]) continue;
        haveRemote = true;
        remote = g_remoteProgress[pi];
        nextPeer = pi + 1;
    }
    ReleaseSRWLockExclusive(&g_worldLock);
    if (!haveRemote) return;
    // Story-Flags
    int added = 0;
    bool gadChanged = false;
    const int n = (std::min)(mine.flagBytes, remote.flagBytes) * 8;
    for (int id = 0; id < n && added < 20; ++id) {
        const uint8_t bit = static_cast<uint8_t>(1u << (id & 7));
        if (!(remote.flags[id >> 3] & bit) || (mine.flags[id >> 3] & bit)) continue;
        if (SetFlagGuarded(id)) {
            ++added;
            if (id >= 0x43 && id <= 0x46) gadChanged = true;
            Log(L("[Fortschritt] vom Mitspieler: %s (%d)\n", "[Progress] from other player: %s (%d)\n"), FlagName(id), id);
        }
    }
    if (gadChanged && RefreshGadPowersGuarded()) Log(L("[Fortschritt] Gad-Kraefte neu berechnet\n", "[Progress] Gad powers recalculated\n"));
    // Inventar-Zaehler (coop::AdoptInventoryCount): Schatten-Level immer, Cadeaux/max. Leben nur beim ersten Treffen mit dem Host
    const uint64_t hostId = net::IsHost() ? 0 : net::PeerId(HostSlot());
    const bool catchUp = hostId != 0 && hostId != g_invCaughtUpHost;
    for (int k = 0; k < posecodec::kProgressInvTypes; ++k) {
        if (coop::AdoptInventoryCount(kProgressInvTypeIds[k], catchUp, mine.inv[k], remote.inv[k])) {
            Log(L("[Fortschritt] Inventar Typ %d: %d -> %d (vom Mitspieler)\n", "[Progress] inventory type %d: %d -> %d (from other player)\n"), kProgressInvTypeIds[k], mine.inv[k], remote.inv[k]);
            inv[kProgressInvTypeIds[k]] = remote.inv[k];
        }
    }
    if (catchUp) {
        g_invCaughtUpHost = hostId;
        Log(L("[Fortschritt] Cadeaux/max. Leben einmal auf den Host aufgeholt - ab jetzt zaehlt jeder selbst\n", "[Progress] Cadeaux/max. health caught up to the host once - from now on everyone counts their own\n"));
    }
    // Teddy-Menue: gesammelte Cadeaux/Seelen je Level (nie ueber "gesamt")
    int levels = 0;
    for (int i = 0; i < posecodec::kProgressLevels; ++i) {
        if (remote.cadeauxGot[i] > mine.cadeauxGot[i] && remote.cadeauxGot[i] <= cadeaux[i * 2]) {
            cadeaux[i * 2 + 1] = remote.cadeauxGot[i];
            ++levels;
        }
        if (remote.soulsGot[i] > mine.soulsGot[i] && remote.soulsGot[i] <= souls[i * 2]) {
            souls[i * 2 + 1] = remote.soulsGot[i];
            ++levels;
        }
    }
    // Gegenstaende/Waffen (nur Gast, einmal je Laden): was der Host besitzt (auch in der Hand) und mir fehlt, als
    // "besessen" (3) eintragen
    if (!net::IsHost() && !g_itemsFromHostDone) {
        g_itemsFromHostDone = true;
        for (int i = 0; i < posecodec::kProgressItems; ++i) {
            const int32_t r = remote.items[i];
            if (mine.items[i] != 0 || r < 1 || r > 3 || IsCountedItemSlot(i)) continue;
            items[i] = 3;
            Log(L("[Fortschritt] Gegenstand vom Host: %s (Slot %d)\n", "[Progress] item from host: %s (slot %d)\n"), ItemSlotName(i), i);
        }
    }
    // Geheimnisse (Teddy-Menue "Finde das Geheimnis"): direkt ins Bitfeld, ohne 0x2D5C00 (Meldung + Erfolg ACH_SECRET)
    if (const uint32_t newSecrets = remote.secrets & ~mine.secrets) {
        *secrets = mine.secrets | newSecrets;
        int count = 0;
        for (uint32_t b = newSecrets; b; b &= b - 1) ++count;
        Log(L("[Fortschritt] %d Geheimnis(se) vom Mitspieler (Bits 0x%08X)\n", "[Progress] %d secret(s) from other player (bits 0x%08X)\n"), count, newSecrets);
    }
    if (levels) Log(L("[Fortschritt] Level-Statistik (Cadeaux/Seelen) in %d Eintraegen uebernommen\n", "[Progress] level statistics (Cadeaux/souls) applied in %d entries\n"), levels);
    if (soulMgr && remote.soulBytes > 0) {
        const int gotSouls = MergeSoulsGuarded(soulMgr, remote.souls, remote.soulBytes);
        if (gotSouls) Log(L("[Fortschritt] %d Dunkle Seelen vom Mitspieler uebernommen\n", "[Progress] %d Dark Souls taken over from other player\n"), gotSouls);
    }
}

// Netz-Thread
void SendProgress() {
    posecodec::ProgressData d;
    AcquireSRWLockShared(&g_worldLock);
    d = g_localProgressOut;
    ReleaseSRWLockShared(&g_worldLock);
    if (!d.flagBytes) return;
    uint8_t buf[posecodec::MaxProgressPacketSize()];
    net::Send(buf, posecodec::EncodeProgress(d, buf));
}

// Netz-Thread: false, wenn es kein Fortschritts-Paket war
bool HandleProgressPacket(int from, const uint8_t* data, size_t size) {
    static posecodec::ProgressData d;
    if (!posecodec::DecodeProgress(data, size, d)) return false;
    AcquireSRWLockExclusive(&g_worldLock);
    g_remoteProgress[from] = d;
    g_remoteProgressTime[from] = GetTickCount();
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// ---- Level-Ereignisse (vorerst nur Protokoll) ----
// Ereignisse (levels/<x>/events.evt, shadowmanevents.cpp) sind Zustandsautomaten (+0xA0), die Schalter, Ausloeser und
// Objekte verbinden: verknuepftes Objekt = [[ev+0x10]+0x30], es bekommt sein Signal ueber +0x120 (z.B. Asyl-Tuer: 1 =
// entriegeln). 0x2F3AC0 SetEventState(ev, z) setzt +0xA0 und bei Flag +0xC Bit 4 auch die zweite Tabelle des SaveManagers
// (+0xC098, Eintraege a 0x10: ev+0x8C, +0x90, +0x88, Zustand; Anzahl +0x14098, max 0x800).
constexpr uintptr_t kSetEventStateRva = 0x2F3AC0;
using SetEventStateFn = void (*)(uintptr_t, int);
SetEventStateFn g_setEventStateOrig = nullptr;
int g_eventLogCount = 0;

bool ReadEventInfoGuarded(uintptr_t ev, int* state, int key[3], uint32_t* flags, uintptr_t* obj) {
    __try {
        *state = *reinterpret_cast<int*>(ev + 0xA0);
        key[0] = *reinterpret_cast<int*>(ev + 0x8C);
        key[1] = *reinterpret_cast<int*>(ev + 0x90);
        key[2] = *reinterpret_cast<int*>(ev + 0x88);
        *flags = *reinterpret_cast<uint32_t*>(ev + 0xC);
        const uintptr_t link = *reinterpret_cast<uintptr_t*>(ev + 0x10);
        *obj = link ? *reinterpret_cast<uintptr_t*>(link + 0x30) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Ereignis-Sync: Ereignisse werden ueber Ausloese-Bereiche gestartet. Die Schleife 0x2F0FC0 geht jedes Bild die Liste aller
// Ereignisse durch und ruft fuer jedes 0x2F1120(?, ev) (einziger Aufruf bei 0x2F1065): steht der Spieler im Bereich
// ([[ev+0x18]+0x30], shadEventInfo_s) und ist die Bedingung erfuellt (z.B. Schatten-Level), wird ev auf 1 gesetzt (0x2F12EF,
// Ruecksprung 0x2F12F4) - so oeffnen Sargtore und Schalter-Ereignisse. Wir merken uns dabei jedes Ereignis (Schluessel ->
// Adresse), schicken eigene Starts (0 -> 1 von dort) an den Mitspieler ("SMV1") und starten dasselbe Ereignis bei uns, wenn
// er eines gestartet hat. Die Folgen (Membran schrumpft, Tuer bekommt ihr Signal ...) laufen dann hier genauso ab.
constexpr uintptr_t kEventTriggerRva     = 0x2F1120;
constexpr uintptr_t kEventTriggerCallRva = 0x2F1065;
constexpr uintptr_t kEventTriggeredRet   = 0x2F12F4;  // Ruecksprung des SetEventState(ev, 1) im Ausloese-Pfad
using EventTriggerFn = bool (*)(uintptr_t, uintptr_t);
EventTriggerFn g_eventTriggerOrig = nullptr;
struct EventKey {
    int k[3];
    bool operator<(const EventKey& o) const { return std::memcmp(k, o.k, sizeof(k)) < 0; }
};
std::map<EventKey, uintptr_t> g_eventRegistry;      // Spiel-Thread
std::vector<posecodec::EventInfo> g_localTriggered;  // meine Starts in diesem Level (g_worldLock)
std::vector<posecodec::EventInfo> g_remoteTriggered[kMaxRemote];  // je Mitspieler (g_worldLock)
uint32_t g_remoteEventSession[kMaxRemote] = {};
uint16_t g_remoteTriggeredLevel[kMaxRemote] = {};
bool g_resetAppliedEvents[kMaxRemote] = {};                       // (g_worldLock)
std::map<EventKey, uint32_t> g_appliedEventSeq[kMaxRemote];       // Spiel-Thread
uint32_t g_eventSeq = 0;
bool g_applyingRemoteEvent = false;

bool ReadEventKeyGuarded(uintptr_t ev, EventKey* key, int* state) {
    __try {
        key->k[0] = *reinterpret_cast<int*>(ev + 0x8C);
        key->k[1] = *reinterpret_cast<int*>(ev + 0x90);
        key->k[2] = *reinterpret_cast<int*>(ev + 0x88);
        *state = *reinterpret_cast<int*>(ev + 0xA0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HookedEventTrigger(uintptr_t self, uintptr_t ev) {
    EventKey key;
    int state = 0;
    if (ev && ReadEventKeyGuarded(ev, &key, &state)) {
        auto it = g_eventRegistry.find(key);
        if (it == g_eventRegistry.end()) g_eventRegistry.emplace(key, ev);
        else it->second = ev;
    }
    return g_eventTriggerOrig(self, ev);
}

void HookedSetEventState(uintptr_t ev, int state) {
    int old = 0, key[3] = {};
    uint32_t flags = 0;
    uintptr_t obj = 0;
    const bool ok = ReadEventInfoGuarded(ev, &old, key, &flags, &obj);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
    g_setEventStateOrig(ev, state);
    if (!ok || old == state) return;
    if (g_eventLogCount < 400) {
        ++g_eventLogCount;
        Log(L("[Ereignis] %d/%d/%d: %d -> %d%s, Objekt %s (Aufrufer 0x%llX)%s\n", "[Event] %d/%d/%d: %d -> %d%s, object %s (caller 0x%llX)%s\n"), key[0], key[1], key[2], old, state,
            (flags & 0x10) ? " [gespeichert]" : "", obj ? ClassNameOf(obj).c_str() : "-", static_cast<unsigned long long>(caller),
            g_applyingRemoteEvent ? L(" - vom Mitspieler", " - from other player") : "");
    }
    // eigener Start ueber einen Ausloese-Bereich -> an den Mitspieler
    if (!g_applyingRemoteEvent && old == 0 && state == 1 && caller == kEventTriggeredRet) {
        posecodec::EventInfo e;
        std::memcpy(e.key, key, sizeof(e.key));
        e.seq = ++g_eventSeq;
        AcquireSRWLockExclusive(&g_worldLock);
        if (g_localTriggered.size() >= 256) g_localTriggered.erase(g_localTriggered.begin());
        g_localTriggered.push_back(e);
        ReleaseSRWLockExclusive(&g_worldLock);
    }
}

// Ereignis-Handler pro Typ: Tabelle 0xD8BF30 (38 Eintraege, gefuellt in 0x30540), Verteiler 0x2F0E80 ruft jedes Bild
// handler[ev+0x20](ev). Manche Handler arbeiten mit "dem Spieler" (Sargtor 0x2EFE40 -> 0x2D6820/0x2D6D10 stellen ihn ans
// Bedienfeld und spielen die Animation). Fuer Ereignisse, die der Mitspieler gestartet hat, ist bei uns waehrend des Handlers
// seine Figur der Spieler - sonst wuerde unser eigener Spieler zum Bedienfeld gezogen.
constexpr uintptr_t kEventHandlerTableRva = 0xD8BF30;
constexpr size_t kEventHandlerCount = 38;
using EventHandlerFn = void (*)(uintptr_t);
EventHandlerFn g_eventHandlerOrig[kEventHandlerCount] = {};
bool g_eventHandlersPatched = false;
struct RemoteStartedEvent { DWORD since; int slot; };
std::map<uintptr_t, RemoteStartedEvent> g_remoteStartedEvents;  // Ereignis -> Startzeit + Mitspieler (Spiel-Thread)

// Handler mit der Figur des Mitspielers slot als g_player (und seinem Sektor) aufrufen
bool RunAsRemotePlayerGuarded(uintptr_t ev, EventHandlerFn fn, int slot) {
    uintptr_t* playerSlot = reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    const uintptr_t gp = *playerSlot;
    if (slot < 0 || slot >= kMaxRemote) return false;
    const uintptr_t p2 = g_avatar[slot];
    if (!gp || !p2 || g_avatarOwner[slot] != gp) return false;
    const int32_t sector = AvatarSectorFor(slot, p2);
    if (sector < 0) return false;  // Sektor gehoert nicht zu diesem Level -> wie ohne Mitspieler
    int32_t savedSector = 0;
    __try {
        savedSector = *reinterpret_cast<int32_t*>(p2 + 0xD4);
        *reinterpret_cast<int32_t*>(p2 + 0xD4) = sector;
        *playerSlot = p2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    __try {
        fn(ev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    *playerSlot = gp;
    *reinterpret_cast<int32_t*>(p2 + 0xD4) = savedSector;
    return true;
}

void CallEventHandler(uintptr_t ev, size_t idx) {
    EventHandlerFn orig = g_eventHandlerOrig[idx];
    if (!g_remoteStartedEvents.empty()) {
        auto it = g_remoteStartedEvents.find(ev);
        if (it != g_remoteStartedEvents.end()) {
            EventKey key;
            int state = 0;
            const bool done = !ReadEventKeyGuarded(ev, &key, &state) || state == 2 || state == 0 ||
                              GetTickCount() - it->second.since > 120000;
            const int slot = it->second.slot;
            if (!done && slot >= 0 && slot < kMaxRemote && g_remoteActive[slot] && RunAsRemotePlayerGuarded(ev, orig, slot)) return;
            if (done) g_remoteStartedEvents.erase(it);
        }
    }
    orig(ev);
}

template <size_t I>
void EventHandlerThunk(uintptr_t ev) {
    CallEventHandler(ev, I);
}
template <size_t... I>
constexpr std::array<EventHandlerFn, sizeof...(I)> MakeEventThunks(std::index_sequence<I...>) {
    return {{&EventHandlerThunk<I>...}};
}
const std::array<EventHandlerFn, kEventHandlerCount> kEventThunks = MakeEventThunks(std::make_index_sequence<kEventHandlerCount>{});

// Spiel-Thread: Tabelle umbiegen, sobald das Spiel sie gefuellt hat (Eintraege zeigen in die EXE)
void PatchEventHandlerTable() {
    if (g_eventHandlersPatched) return;
    auto* table = reinterpret_cast<uintptr_t*>(g_base + kEventHandlerTableRva);
    const uintptr_t lo = g_base + 0x1000, hi = g_base + 0x600000;
    for (size_t i = 0; i < kEventHandlerCount; ++i)
        if (table[i] < lo || table[i] >= hi) return;  // noch nicht gefuellt
    DWORD old = 0;
    if (!VirtualProtect(table, kEventHandlerCount * 8, PAGE_READWRITE, &old)) return;
    for (size_t i = 0; i < kEventHandlerCount; ++i) {
        g_eventHandlerOrig[i] = reinterpret_cast<EventHandlerFn>(table[i]);
        table[i] = reinterpret_cast<uintptr_t>(kEventThunks[i]);
    }
    VirtualProtect(table, kEventHandlerCount * 8, old, &old);
    g_eventHandlersPatched = true;
    Log(L("Ereignis-Handler: %zu Typen umgebogen (Mitspieler-Ereignisse laufen mit seiner Figur)\n", "Event handlers: %zu types redirected (other player's events run with their figure)\n"), kEventHandlerCount);
}

bool StartEventGuarded(uintptr_t ev) {
    __try {
        g_setEventStateOrig(ev, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Spiel-Thread (WorldSyncTick): Starts der Mitspieler nachziehen
void ApplyRemoteEventsFrom(int pi);
void ApplyRemoteEvents() {
    if (!g_setEventStateOrig) return;
    for (int pi = 0; pi < kMaxRemote; ++pi) ApplyRemoteEventsFrom(pi);
}

void ApplyRemoteEventsFrom(int pi) {
    std::vector<posecodec::EventInfo> remote;
    AcquireSRWLockExclusive(&g_worldLock);
    if (g_resetAppliedEvents[pi]) {
        g_appliedEventSeq[pi].clear();
        g_resetAppliedEvents[pi] = false;
    }
    if (g_remoteActive[pi] && g_remoteTriggeredLevel[pi] == static_cast<uint16_t>(g_worldLevel)) remote = g_remoteTriggered[pi];
    ReleaseSRWLockExclusive(&g_worldLock);
    for (const auto& e : remote) {
        EventKey key;
        std::memcpy(key.k, e.key, sizeof(key.k));
        auto reg = g_eventRegistry.find(key);
        if (reg == g_eventRegistry.end()) continue;  // (noch) nicht gesehen -> beim naechsten Durchlauf
        uint32_t& applied = g_appliedEventSeq[pi][key];
        if (e.seq <= applied) continue;
        applied = e.seq;
        EventKey check;
        int state = -1;
        if (!ReadEventKeyGuarded(reg->second, &check, &state) || std::memcmp(check.k, key.k, sizeof(key.k)) != 0) continue;
        if (state != 0) continue;  // laeuft hier schon / ist erledigt
        // Hinweis: die Spieler-Sequenz des Sargtors (Handler 0x2EFE40) muss hier mitlaufen - ohne sie geht das Tor nicht auf
        // (getestet). Sie laeuft mit der Figur des Mitspielers; unsere Kamera schaut dabei etwas laenger aufs Tor.
        g_applyingRemoteEvent = true;
        const bool ok = StartEventGuarded(reg->second);
        g_applyingRemoteEvent = false;
        if (ok) g_remoteStartedEvents[reg->second] = RemoteStartedEvent{GetTickCount(), pi};
        Log(L("[Ereignis] Mitspieler %d hat %d/%d/%d ausgeloest - %s\n", "[Event] player %d triggered %d/%d/%d - %s\n"), pi + 1, key.k[0], key.k[1], key.k[2],
            ok ? L("hier auch gestartet", "started here too") : L("Starten fehlgeschlagen (Ausnahme)", "start failed (exception)"));
    }
}

// Netz-Thread
void SendEvents() {
    posecodec::EventInfo list[posecodec::kMaxEvents];
    int num = 0;
    uint16_t level = 0;
    AcquireSRWLockShared(&g_worldLock);
    level = g_localPickupsLevel;
    for (auto it = g_localTriggered.rbegin(); it != g_localTriggered.rend() && num < posecodec::kMaxEvents; ++it) list[num++] = *it;
    ReleaseSRWLockShared(&g_worldLock);
    uint8_t buf[posecodec::MaxEventPacketSize()];
    net::Send(buf, posecodec::EncodeEvents(g_stateSession, level, list, num, buf));
}

// Netz-Thread: false, wenn es kein Ereignis-Paket war
bool HandleEventPacket(int from, const uint8_t* data, size_t size) {
    static posecodec::EventInfo list[posecodec::kMaxEvents];
    uint32_t session = 0;
    uint16_t level = 0;
    int num = 0;
    if (!posecodec::DecodeEvents(data, size, session, level, list, num)) return false;
    AcquireSRWLockExclusive(&g_worldLock);
    if (session != g_remoteEventSession[from]) {
        g_remoteEventSession[from] = session;
        g_resetAppliedEvents[from] = true;
    }
    g_remoteTriggeredLevel[from] = level;
    g_remoteTriggered[from].assign(list, list + num);
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

void HookEvents() {
    // mov [rsp+8],rbx / push rdi / sub rsp,20h / mov edi,edx / mov rbx,rcx
    static const uint8_t expected[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kSetEventStateRva), expected, sizeof(expected)) == 0)
        g_setEventStateOrig = reinterpret_cast<SetEventStateFn>(
            InstallInlineHook(g_base + kSetEventStateRva, sizeof(expected), reinterpret_cast<void*>(HookedSetEventState)));
    // Ausloese-Pruefung: Aufrufstelle umbiegen (Prolog zu kurz fuer einen Inline-Hook); nimmt nur rcx/rdx, liefert al
    g_eventTriggerOrig = reinterpret_cast<EventTriggerFn>(g_base + kEventTriggerRva);
    if (!PatchCallRel32(kEventTriggerCallRva, kEventTriggerRva, reinterpret_cast<void*>(HookedEventTrigger))) g_eventTriggerOrig = nullptr;
    Log(L("Hook Ereignisse: Zustand %s, Ausloeser %s\n", "Hook events: state %s, trigger %s\n"), g_setEventStateOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"), g_eventTriggerOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// Spiel-Thread, alle 100 ms
void DetectLanguageFromGame();
void WorldSyncTick(uintptr_t gp) {
    DetectLanguageFromGame();
    const LevelInfo li = GetLevelInfo(g_base);
    if (gp != g_worldPlayer || li.level != g_worldLevel || !li.inGame) {
        g_trackedPickups.clear();
        for (auto& m : g_remoteEvents) m.clear();
        g_claimedLocal.clear();
        g_trackedContainers.clear();
        g_breakAttempts.clear();
        g_trackedBlocks.clear();
        g_poseBlockTargets.clear();
        for (auto& m : g_appliedStateSeq) m.clear();
        g_pendingRestarts.clear();
        g_trackedEnemies.clear();
        g_hpApplied.clear();
        for (auto& m : g_appliedKillSeq) m.clear();
        for (auto& m : g_appliedDamage) m.clear();
        g_eventRegistry.clear();
        for (auto& m : g_appliedEventSeq) m.clear();
        g_remoteStartedEvents.clear();
        g_snapApplied.clear();
        AcquireSRWLockExclusive(&g_worldLock);
        g_localStates.clear();
        g_localKills.clear();
        g_localTriggered.clear();
        g_localEnemyPos.clear();
        g_localPickups.clear();
        g_localBroken.clear();
        g_localBlocks.clear();
        g_blockTargets.clear();
        g_localPickupsLevel = static_cast<uint16_t>(li.level);
        ReleaseSRWLockExclusive(&g_worldLock);
        g_worldPlayer = gp;
        g_worldLevel = li.level;
        if (!li.inGame) return;
    }
    float me[3] = {};
    SafeRead(gp + 0x20, me, sizeof(me));
    for (auto& kv : g_trackedPickups) kv.second.seen = false;
    for (auto& kv : g_trackedContainers) kv.second.seen = false;
    const int n = SnapshotPoolGuarded();
    static int lastSkipped = 0;  // Diagnose: frueher brach so ein Eintrag den ganzen Schnappschuss ab
    if (g_poolSkipped != lastSkipped) {
        if (g_poolSkipped) Log(L("[Welt] Objekt-Pool: %d Eintraege nicht lesbar - uebersprungen\n", "[World] object pool: %d entries unreadable - skipped\n"), g_poolSkipped);
        lastSkipped = g_poolSkipped;
    }
    const uintptr_t objCb = g_base + kObjectNodeCallbackRva, darkSoulVt = g_base + kDarkSoulVtableRva;
    const uintptr_t objectVt = g_base + kObjectVtableRva;
    const uintptr_t goviVt = g_base + kGoviVtableRva, potVt = g_base + kExplodeObjectVtableRva,
                    calabashObjVt = g_base + kCalabashObjectVtableRva;
    const uintptr_t pushVt = g_base + kPushBlockVtableRva, pushGad3Vt = g_base + kPushGad3BlockVtableRva,
                    pushLavaVt = g_base + kPushLavaBlockVtableRva;
    std::map<uint16_t, uintptr_t> blockIdsThisScan;
    static std::map<uint16_t, bool> blockIdClash;  // gleiche Kennung fuer zwei Bloecke -> diese nie abgleichen
    for (int i = 0; i < n; ++i) {
        const PoolNodeInfo& p = g_poolSnap[i];
        if (p.state == 0x10) continue;  // zum Loeschen markiert
        if (p.vt == pushVt || p.vt == pushGad3Vt || p.vt == pushLavaVt) {  // Schiebeblock (BlockFrameUpdate liest jedes Bild)
            const uint16_t id = coop::BlockId16(EnemyIdGuarded(p.obj));
            const auto other = blockIdsThisScan.find(id);
            if (other != blockIdsThisScan.end() && other->second != p.obj) {
                if (!blockIdClash[id]) Log(L("[Welt] Zwei Schiebebloecke mit Kennung %u - werden nicht abgeglichen\n", "[World] two push blocks with id %u - not synced\n"), id);
                blockIdClash[id] = true;
                g_trackedBlocks.erase(id);
                continue;
            }
            blockIdsThisScan[id] = p.obj;
            if (blockIdClash[id]) continue;
            TrackedBlock& b = g_trackedBlocks[id];
            if (b.obj != p.obj) {
                if (!b.obj)
                    Log(L("[Welt] Schiebeblock Kennung %u erkannt (%s, Pool #%u)\n", "[World] push block id %u found (%s, pool #%u)\n"), id,
                        p.vt == pushVt ? "PushBlock" : p.vt == pushGad3Vt ? "PushGad3Block" : "PushLavaBlock", p.serial);
                b.obj = p.obj;
                b.gad3 = p.vt == pushGad3Vt;
                ReadBlockGuarded(p.obj, b.pos, &b.state);
                float base[3] = {};
                if (SafeRead(p.obj + 0x38, base, sizeof(base)) && coop::BlockNeedsRepair(b.state, b.gad3, b.pos, base)) {
                    const int done = coop::BlockDoneState(b.gad3);
                    if (SetBlockStateGuarded(p.obj, done)) {
                        Log(L("[Welt] Schiebeblock #%u steht verschoben, war aber noch anfassbar (Spielstand mit altem Fehler) - Zustand %d -> %d\n", "[World] push block #%u is moved but was still pushable (savegame with old bug) - state %d -> %d\n"), id, b.state, done);
                        b.state = done;
                    }
                }
            }
            b.seen = true;
            continue;
        }
        if (p.vt == goviVt || p.vt == potVt || p.vt == calabashObjVt) {  // Behaelter (Calabash-Platte zaehlt als zerbrechlich)
            TrackedContainer& c = g_trackedContainers[p.serial];
            if (c.obj != p.obj) {
                c.obj = p.obj; c.node = p.node; c.vt = p.vt; c.kind = p.vt == goviVt ? 1 : 2; c.type = p.type;
                SafeRead(p.obj + 0xD4, &c.sector, 4);
            }
            std::memcpy(c.pos, p.pos, sizeof(c.pos));
            c.seen = true;
            continue;
        }
        // Nur einfache Pickups (Klasse Object) und Dunkle Seelen. Quest-Gegenstaende/Waffen (QuestObject) nicht: ihr
        // Aufheben ist eine eigene Sequenz (Vorfuehrung, Sprache, Kamera-Sektor-Pruefung) - jeder hebt sein Exemplar auf.
        if (!(p.vt == darkSoulVt || (p.vt == objectVt && p.cb == objCb && IsCollectibleType(p.type)))) continue;
        TrackedPickup& t = g_trackedPickups[p.serial];
        if (t.obj != p.obj) {  // neu (oder Platz von einem anderen Objekt wiederverwendet)
            if (t.obj) g_claimedLocal.erase(p.serial);
            t.obj = p.obj; t.node = p.node; t.vt = p.vt; t.type = p.type;
        }
        std::memcpy(t.pos, p.pos, sizeof(t.pos));
        t.seen = true;
    }
    // Levelinhalt neu aufgebaut (alles bekommt neue Pool-Eintraege)? Dann ist nichts zerstoert oder eingesammelt - sonst
    // zerbricht der Mitspieler seine frischen Toepfe/Govis/Platten bzw. sammelt Pickups ein.
    static int lastPoolCount = 0;
    int goneContainers = 0, gonePickups = 0;
    for (const auto& kv : g_trackedContainers) goneContainers += kv.second.seen ? 0 : 1;
    for (const auto& kv : g_trackedPickups) gonePickups += kv.second.seen ? 0 : 1;
    const bool rebuilt = coop::MassVanish(goneContainers, gonePickups, lastPoolCount, n);
    lastPoolCount = n;
    if (rebuilt && goneContainers + gonePickups > 0) {
        Log(L("[Welt] %d Behaelter und %d Pickups auf einmal weg - Levelinhalt neu aufgebaut, nicht gemeldet\n", "[World] %d containers and %d pickups gone at once - level content rebuilt, not reported\n"), goneContainers,
            gonePickups);
        for (auto it = g_trackedContainers.begin(); it != g_trackedContainers.end();) it = it->second.seen ? std::next(it) : g_trackedContainers.erase(it);
        for (auto it = g_trackedPickups.begin(); it != g_trackedPickups.end();) {
            if (!it->second.seen) g_claimedLocal.erase(it->first);
            it = it->second.seen ? std::next(it) : g_trackedPickups.erase(it);
        }
    }
    for (auto it = g_trackedPickups.begin(); it != g_trackedPickups.end();) {
        const TrackedPickup& t = it->second;
        if (t.seen) { ++it; continue; }
        const float dx = t.pos[0] - me[0], dy = t.pos[1] - me[1], dz = t.pos[2] - me[2];
        const bool claimed = g_claimedLocal.erase(it->first) > 0;  // wegen des Mitspielers eingesammelt -> nicht "meins"
        const bool mine = !claimed && dx * dx + dy * dy + dz * dz < kPickupNearMe * kPickupNearMe;
        if (mine) {
            posecodec::PickupInfo info;
            info.serial = static_cast<uint16_t>(it->first);
            info.type = static_cast<int16_t>(t.type);
            std::memcpy(info.pos, t.pos, sizeof(info.pos));
            AcquireSRWLockExclusive(&g_worldLock);
            if (g_localPickups.size() < 1024) g_localPickups.push_back(info);
            ReleaseSRWLockExclusive(&g_worldLock);
            Log(L("[Welt] Du hast %s (#%u) eingesammelt\n", "[World] you collected %s (#%u)\n"), PickupName(t.vt, t.type), it->first);
        }
        it = g_trackedPickups.erase(it);
    }
    // Verschwundene Behaelter merken (egal wer sie zerstoert hat - die Liste ist eine Vereinigung beider Seiten)
    for (auto it = g_trackedContainers.begin(); it != g_trackedContainers.end();) {
        if (it->second.seen) { ++it; continue; }
        posecodec::PickupInfo info;
        info.serial = static_cast<uint16_t>(it->first);
        info.type = static_cast<int16_t>(it->second.kind);
        std::memcpy(info.pos, it->second.pos, sizeof(info.pos));
        AcquireSRWLockExclusive(&g_worldLock);
        if (g_localBroken.size() < 1024) g_localBroken.push_back(info);
        ReleaseSRWLockExclusive(&g_worldLock);
        Log(L("[Welt] %s (#%u) zerstoert\n", "[World] %s (#%u) destroyed\n"), ContainerName(it->second), it->first);
        it = g_trackedContainers.erase(it);
    }
    BreakRemoteBrokenContainers(me);
    ApplyRemotePickups(gp);
    ApplyRemoteStates(n);
    PatchEventHandlerTable();
    ApplyRemoteEvents();
    RunPendingRestarts();
    TrackEnemies(n);
    ApplyRemoteKills();
    ApplyRemoteDamage();
    CollectEnemyPositions(gp);
    ReportIgnoredHits();
    static int reconcileTick = 0;
    if (++reconcileTick >= 10) {  // etwa jede Sekunde
        reconcileTick = 0;
        ReconcileUnlockedObjects(n);
        ApplyHostSnapshot(n);
        ApplyHostTable();
        ApplyHostQuestList(n);
        ProgressTick();
    }
    CollectHostSnapshot(n);
    TouchDoorsForRemote(gp, n);
    for (auto& kv : g_trackedBlocks) {  // nicht mehr im Pool -> nicht mehr anfassen (Version bleibt fuers Wiederfinden)
        if (!kv.second.seen) kv.second.obj = 0;
        kv.second.seen = false;
    }
    // Welt-Liste der Schiebebloecke: ruhende, die schon einmal verschoben wurden (Version > 0), mit aktueller Position.
    // Bloecke in Bewegung gehen mit jeder Pose raus (g_movingBlocks).
    std::vector<posecodec::PickupInfo> blocks;
    for (const auto& kv : g_trackedBlocks) {
        if (!kv.second.obj || !kv.second.version || coop::BlockMoving(kv.second.state, kv.second.gad3)) continue;
        posecodec::PickupInfo info;
        info.serial = static_cast<uint16_t>(kv.first);
        info.type = static_cast<int16_t>(kv.second.version);
        std::memcpy(info.pos, kv.second.pos, sizeof(info.pos));
        blocks.push_back(info);
    }
    AcquireSRWLockExclusive(&g_worldLock);
    g_localBlocks.swap(blocks);
    ReleaseSRWLockExclusive(&g_worldLock);
}

// Netz-Thread: eigene Liste senden (die neuesten zuerst, hoechstens kMaxPickups)
void SendWorldState() {
    posecodec::PickupInfo list[posecodec::kMaxPickups], broken[posecodec::kMaxContainers], blocks[posecodec::kMaxBlocks];
    int num = 0, numC = 0, numB = 0;
    uint16_t level = 0;
    AcquireSRWLockShared(&g_worldLock);
    level = g_localPickupsLevel;
    for (auto it = g_localPickups.rbegin(); it != g_localPickups.rend() && num < posecodec::kMaxPickups; ++it) list[num++] = *it;
    for (auto it = g_localBroken.rbegin(); it != g_localBroken.rend() && numC < posecodec::kMaxContainers; ++it) broken[numC++] = *it;
    for (auto it = g_localBlocks.begin(); it != g_localBlocks.end() && numB < posecodec::kMaxBlocks; ++it) blocks[numB++] = *it;
    ReleaseSRWLockShared(&g_worldLock);
    uint8_t buf[posecodec::MaxWorldPacketSize()];
    const size_t size = posecodec::EncodeWorld(level, list, num, broken, numC, blocks, numB, buf);
    net::Send(buf, size);
}

// Netz-Thread: false, wenn es kein Welt-Paket war
bool HandleWorldPacket(int from, const uint8_t* data, size_t size) {
    static posecodec::PickupInfo list[posecodec::kMaxPickups], broken[posecodec::kMaxContainers], blocks[posecodec::kMaxBlocks];
    uint16_t level = 0;
    int num = 0, numC = 0, numB = 0;
    if (!posecodec::DecodeWorld(data, size, level, list, num, broken, numC, blocks, numB)) return false;
    AcquireSRWLockExclusive(&g_worldLock);
    if (level != g_remotePickupsLevel[from] && level != static_cast<uint16_t>(g_worldLevel)) g_blockTargets.clear();  // Mitspieler wechselte das Level
    g_remotePickupsLevel[from] = level;
    g_remotePickups[from].assign(list, list + num);
    g_remoteBroken[from].assign(broken, broken + numC);
    for (int i = 0; i < numB; ++i) SetBlockTargetLocked(blocks[i].serial, static_cast<uint16_t>(blocks[i].type), blocks[i].pos);
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// Spiel-Thread (aus HookedPlayerBones fuer den echten Spieler)
// ---- Phase 4: Gast folgt dem Host ins Level ----
// Level-Wechsel = GameState+0x34 auf das Ziel-Level setzen (-1 = keiner); 0x2E9763 laedt es nach der Ueberblendung
// (0x2E8A20, danach +0x3C = altes Level). So machen es auch Teddy-Warps (shadowmanteddyhandler.cpp) - kein Cheat-Aufruf.
// Danach setzt sich der Gast an die Stelle des Hosts (Sektor aus seiner Pose).
// Nur EINMAL nach dem Laden eines Spielstands bzw. dem Start aus dem Hauptmenue (g_followArmed); danach darf der Gast
// frei in andere Level gehen, ohne zurueckgezogen zu werden.
constexpr uint32_t kGameStateNextLevelOff = 0x34;
// Teleport aus dem Pausenmenue nutzt denselben Weg mit frei gewaehltem Ziel (g_followTarget, auch fuer den Host).
std::atomic<bool> g_followArmed{false};
std::atomic<int> g_followTarget{-1};  // Platz des Mitspielers, zu dem es geht (-1 = Host)
int PlayingCutsceneOf(uintptr_t mgr);
void FollowHostTick(uintptr_t gp) {
    const int forced = g_followTarget;
    if (!g_followArmed || (net::IsHost() && forced < 0)) return;
    static DWORD differSince = 0, lastRequest = 0, arrivedAt = 0;
    const DWORD now = GetTickCount();
    const LevelInfo li = GetLevelInfo(g_base);
    const int host = forced >= 0 ? forced : HostSlot();
    if (host < 0) return;
    if (forced >= 0 && !net::PeerConnected(forced)) {  // Ziel ist weg
        Log(L("[Folgen] Mitspieler nicht mehr da - Teleport abgebrochen\n", "[Follow] player gone - teleport cancelled\n"));
        g_followArmed = false;
        g_followTarget = -1;
        return;
    }
    if (PlayingCutsceneOf(reinterpret_cast<uintptr_t (*)()>(g_base + kCutsceneMgrRva)()) >= 0) return;  // erst nach der Cutscene
    const int hostLevel = g_peerLevel[host];
    if (!li.inGame || !g_peerFresh[host] || !g_peerInGame[host] || hostLevel <= 0) {
        differSince = 0;
        arrivedAt = 0;
        return;
    }
    if (hostLevel != li.level) {
        arrivedAt = 0;
        if (!differSince) differSince = now;
        int* next = reinterpret_cast<int*>(g_base + kGameStateRva + kGameStateNextLevelOff);
        if (now - differSince > 3000 && now - lastRequest > 20000 && *next == -1) {
            *next = hostLevel;
            lastRequest = now;
            Log(L("[Folgen] %s ist in Level %d (du: %d) - wechsle dorthin\n", "[Follow] %s is in level %d (you: %d) - switching there\n"), forced >= 0 ? L("Mitspieler", "other player") : L("Host", "host"), hostLevel, li.level);
        }
        return;
    }
    differSince = 0;
    // im Level des Hosts: kurz warten, bis Spieler 2 und seine Pose da sind, dann neben den Host setzen
    if (!arrivedAt) arrivedAt = now;
    const uintptr_t hostAvatar = g_avatar[host];
    if (now - arrivedAt < 2000 || !hostAvatar || g_remoteSector[host] < 0) return;
    float pos[3];
    if (SafeRead(hostAvatar + 0x20, pos, sizeof(pos))) {
        TeleportPlayerGuarded(gp, pos, g_remoteSector[host]);
        Log(L("[Folgen] Beim %s angekommen - gesetzt auf %.0f/%.0f/%.0f (Sektor %d), ab jetzt frei\n", "[Follow] arrived at %s - placed at %.0f/%.0f/%.0f (sector %d), free from now on\n"), forced >= 0 ? L("Mitspieler", "other player") : L("Host", "host"),
            pos[0], pos[1], pos[2], g_remoteSector[host]);
    }
    g_followArmed = false;
    g_followTarget = -1;
    arrivedAt = 0;
}

// ---- Boss-Cutscenes ----
// Cutscene-Manager kexShadowManCutsceneMgrLocal (Getter 0x2AAAF0, Objekt 0xF97030, vt 0x6DA290). Start = vt[6] 0x2A4810
// (mgr, nummer, flag): laedt "cutscene/scripts/{}" (Tabelle je Nummer 0x38 Byte), danach mgr+0x88 = 1 (laeuft), +8 = Nummer.
// Die Bosse starten ihre Cutscenes aus ihrem Update (vt[7]): Jack 5, Legion 14, Marco/Milton 1, TFLegion 15, 0x3ACAA0 7.
// Jeder echte Start wird dem Mitspieler gemeldet ("SMX1": Level, Nummer, Ort + Sektor des Ausloesers). Der Mitspieler
// wechselt noetigenfalls ins Level und wird an den Ort gesetzt; die Cutscene loest dort sein eigenes Spiel aus (Boss-KI bzw.
// Ausloeser), denn ein direkter Start ohne deren Vorbereitung laesst den Boss weg (Legion: unsichtbarer Angreifer).
// Echt = vorher lief diese Nummer nicht, danach laeuft sie (manche Ausloeser rufen den Start jedes Bild erneut auf).
// Ausgenommen: Level-Einstiegs-Cutscene (0x2ADE00, Nummer aus GameState+0x44 - sonst zieht jeder Levelwechsel den anderen
// mit), Seilbahn, Asyl-Eingangstuer, Zug, Konsolenbefehle, Spieler-Update. Viele Szenen kommen ueber allgemeine Rueckrufe
// (z.B. Legion: 13 von 0x33CFDF, dann 10 von 0x311B5B); mehrere werden der Reihe nach abgearbeitet.
constexpr uintptr_t kCutsceneMgrVtRva = 0x6DA290;
constexpr int kCutsceneStartIndex = 6;
constexpr uint32_t kCutscenePlayingOff = 0x88;  // byte: 1 = laeuft
constexpr uint32_t kCutsceneIdOff = 0x8;        // aktuelle Nummer
static const uintptr_t kCutsceneNoSyncCallers[] = {0x2ADE50, 0x420F0C, 0x420F27, 0x42CAFD, 0x451D61, 0x467489, 0x46759C, 0x47D149};
static const uintptr_t kCutsceneBossCallers[] = {0x399DDD, 0x39DC89, 0x3A29A0, 0x3A6172, 0x3ACC4D, 0x3B86B6};
using CutsceneStartFn = uint64_t (*)(void*, int, bool);
CutsceneStartFn g_cutsceneStartOrig = nullptr;
uint32_t g_cutSession = 1;
uint32_t g_cutSeq = 0;
std::vector<std::pair<DWORD, posecodec::CutsceneInfo>> g_localCutscenes;  // (g_worldLock) Zeitpunkt + Eintrag
std::vector<posecodec::CutsceneInfo> g_remoteCutIn;                       // (g_worldLock) neu empfangen
std::map<std::pair<int, int>, DWORD> g_cutsceneStartedAt;                 // Spiel-Thread: (Level, Nummer) -> letzter Start hier

// Nummer der laufenden Cutscene im Manager, -1 = keine
int PlayingCutsceneOf(uintptr_t mgr) {
    uint8_t playing = 0;
    int id = -1;
    if (!mgr || !SafeRead(mgr + kCutscenePlayingOff, &playing, 1) || !playing) return -1;
    SafeRead(mgr + kCutsceneIdOff, &id, 4);
    return id;
}

uint64_t HookedCutsceneStart(void* mgr, int id, bool flag) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
    const int before = PlayingCutsceneOf(reinterpret_cast<uintptr_t>(mgr));
    const uint64_t result = g_cutsceneStartOrig(mgr, id, flag);
    if (before == id || PlayingCutsceneOf(reinterpret_cast<uintptr_t>(mgr)) != id) return result;  // kein neuer Start
    bool sync = true, boss = false;
    for (uintptr_t c : kCutsceneNoSyncCallers)
        if (c == ret) sync = false;
    for (uintptr_t c : kCutsceneBossCallers)
        if (c == ret) boss = true;
    const int level = GetLevelInfo(g_base).level;
    Log(L("[Cutscene] Start Nummer %d in Level %d (Flag %d) von 0x%llX%s%s\n", "[Cutscene] start number %d in level %d (flag %d) from 0x%llX%s%s\n"), id, level, flag ? 1 : 0, (unsigned long long)ret,
        boss ? " (Boss)" : "", sync ? L(" - an Mitspieler", " - sent to other players") : L(" - nicht synchronisiert", " - not synchronized"));
    g_cutsceneStartedAt[{level, id}] = GetTickCount();
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (sync && gp && net::IsRunning()) {
        posecodec::CutsceneInfo ci;
        ci.seq = ++g_cutSeq;
        ci.level = static_cast<uint16_t>(level);
        ci.id = static_cast<int16_t>(id);
        ci.flag = flag ? 1 : 0;
        ci.boss = boss ? 1 : 0;
        SafeRead(gp + 0x20, ci.pos, sizeof(ci.pos));
        SafeRead(gp + 0xD4, &ci.sector, sizeof(ci.sector));
        AcquireSRWLockExclusive(&g_worldLock);
        g_localCutscenes.push_back({GetTickCount(), ci});
        if (g_localCutscenes.size() > static_cast<size_t>(posecodec::kMaxCutscenes)) g_localCutscenes.erase(g_localCutscenes.begin());
        ReleaseSRWLockExclusive(&g_worldLock);
    }
    return result;
}

void HookCutscenes() {
    g_cutSession = (GetTickCount() ^ (GetCurrentProcessId() << 12)) | 1;
    g_cutsceneStartOrig = reinterpret_cast<CutsceneStartFn>(
        PatchVtableEntry(kCutsceneMgrVtRva, kCutsceneStartIndex, reinterpret_cast<void*>(HookedCutsceneStart)));
    Log(L("Hook Cutscene-Start: %s\n", "Hook cutscene start: %s\n"), g_cutsceneStartOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// Netz-Thread: eigene Starts der letzten 5 s (mehrfach, gegen Verlust)
void SendCutscenes() {
    posecodec::CutsceneInfo list[posecodec::kMaxCutscenes];
    int num = 0;
    const DWORD now = GetTickCount();
    AcquireSRWLockShared(&g_worldLock);
    for (const auto& e : g_localCutscenes)
        if (now - e.first < 5000 && num < posecodec::kMaxCutscenes) list[num++] = e.second;
    ReleaseSRWLockShared(&g_worldLock);
    if (!num) return;
    uint8_t buf[posecodec::MaxCutscenePacketSize()];
    net::Send(buf, posecodec::EncodeCutscenes(g_cutSession, list, num, buf));
}

// Netz-Thread: false, wenn es kein Cutscene-Paket war
bool HandleCutscenePacket(int from, const uint8_t* data, size_t size) {
    static posecodec::CutsceneInfo list[posecodec::kMaxCutscenes];
    static uint32_t lastSessionArr[kMaxRemote] = {}, lastSeqArr[kMaxRemote] = {};
    uint32_t& lastSession = lastSessionArr[from];
    uint32_t& lastSeq = lastSeqArr[from];
    uint32_t session = 0;
    int num = 0;
    if (!posecodec::DecodeCutscenes(data, size, session, list, num)) return false;
    if (session != lastSession) {
        lastSession = session;
        lastSeq = 0;
    }
    AcquireSRWLockExclusive(&g_worldLock);
    for (int i = 0; i < num; ++i)
        if (list[i].seq > lastSeq) {
            lastSeq = list[i].seq;
            g_remoteCutIn.push_back(list[i]);
        }
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// Spiel-Thread: Cutscene des Mitspielers nachziehen (Level wechseln, hinsetzen, starten)
void CutsceneTick(uintptr_t gp) {
    struct Pending {
        posecodec::CutsceneInfo info;
        DWORD since = 0, arrivedAt = 0, teleportAt = 0;
        bool levelRequested = false, teleported = false;
    };
    static std::deque<Pending> queue;  // der Reihe nach, wie beim Mitspieler (z.B. Legion: 13, dann 10)
    std::vector<posecodec::CutsceneInfo> in;
    AcquireSRWLockExclusive(&g_worldLock);
    in.swap(g_remoteCutIn);
    ReleaseSRWLockExclusive(&g_worldLock);
    const DWORD now = GetTickCount();
    for (const auto& ci : in) {
        const auto it = g_cutsceneStartedAt.find({ci.level, ci.id});
        if (it != g_cutsceneStartedAt.end() && now - it->second < 60000) {
            Log(L("[Cutscene] Mitspieler startete Nummer %d - lief hier gerade selbst\n", "[Cutscene] other player started number %d - was already running here\n"), ci.id);
            continue;
        }
        bool queued = false;
        for (const auto& q : queue)
            if (q.info.id == ci.id && q.info.level == ci.level) queued = true;
        if (queued) continue;
        Pending p;
        p.info = ci;
        p.since = now;
        queue.push_back(p);
        Log(L("[Cutscene] Mitspieler hat Cutscene %d ausgeloest%s (Level %u bei %.0f/%.0f/%.0f) - komme dorthin\n", "[Cutscene] other player triggered cutscene %d%s (level %u at %.0f/%.0f/%.0f) - coming there\n"), ci.id,
            ci.boss ? " (Boss)" : "", ci.level, ci.pos[0], ci.pos[1], ci.pos[2]);
    }
    if (queue.empty()) return;
    Pending& p = queue.front();
    auto next = [&]() {
        queue.pop_front();
        if (!queue.empty()) queue.front().since = now;
    };
    if (now - p.since > 45000) {
        Log(L("[Cutscene] Nummer %d: Zeit abgelaufen, nicht uebernommen\n", "[Cutscene] number %d: timed out, not applied\n"), p.info.id);
        next();
        return;
    }
    const LevelInfo li = GetLevelInfo(g_base);
    if (!li.inGame) return;
    if (li.level != p.info.level) {
        int* nextLevel = reinterpret_cast<int*>(g_base + kGameStateRva + kGameStateNextLevelOff);
        if (!p.levelRequested && *nextLevel == -1) {
            *nextLevel = p.info.level;
            p.levelRequested = true;
            Log(L("[Cutscene] wechsle in Level %u\n", "[Cutscene] switching to level %u\n"), p.info.level);
        }
        p.arrivedAt = 0;
        return;
    }
    const int playing = PlayingCutsceneOf(reinterpret_cast<uintptr_t (*)()>(g_base + kCutsceneMgrRva)());
    const auto self = g_cutsceneStartedAt.find({p.info.level, p.info.id});
    if (playing == p.info.id || (self != g_cutsceneStartedAt.end() && self->second >= p.since)) {
        Log(L("[Cutscene] Nummer %d laeuft jetzt auch hier\n", "[Cutscene] number %d is now running here too\n"), p.info.id);
        next();
        return;
    }
    if (playing >= 0) return;  // erst die laufende Cutscene zu Ende
    if (!p.teleported) {
        if (p.levelRequested) {
            if (!p.arrivedAt) p.arrivedAt = now;
            if (now - p.arrivedAt < 1500) return;
        }
        float me[3] = {};
        SafeRead(gp + 0x20, me, sizeof(me));
        const float dx = me[0] - p.info.pos[0], dy = me[1] - p.info.pos[1], dz = me[2] - p.info.pos[2];
        if (dx * dx + dy * dy + dz * dz > 300.0f * 300.0f && p.info.sector >= 0) {
            TeleportPlayerGuarded(gp, p.info.pos, p.info.sector);
            Log(L("[Cutscene] zum Mitspieler gesetzt (%.0f/%.0f/%.0f, Sektor %d)\n", "[Cutscene] placed at the other player (%.0f/%.0f/%.0f, sector %d)\n"), p.info.pos[0], p.info.pos[1], p.info.pos[2],
                p.info.sector);
        }
        p.teleported = true;
        p.teleportAt = now;
        return;
    }
    if (now - p.teleportAt < 8000) return;  // Zeit fuer den eigenen Ausloeser
    Log(L("[Cutscene] Nummer %d hat hier nicht von selbst begonnen (nur hingesetzt)\n", "[Cutscene] number %d did not start here by itself (only placed)\n"), p.info.id);
    next();
}

// ---- Gemeinsamer Tod ----
// Player::Die 0x46E930(spieler, ?, art): setzt +0x1D65D = 1 (tot; ein zweiter Aufruf tut nichts), GameState+0x58++ (Tode),
// Kamera-Zustand 7 und loest das Skript-Ereignis "OnPlayerDead" aus; danach laeuft der Ablauf des Levels (Welt der Lebenden ->
// Reich der Toten, Bosse mit eigenen Szenen). Aufrufer: Schaden 0x46E030 (Leben [0xDB2200] <= 0), Spieler-Update 0x45DEAD, 0x4882E2.
// Stirbt der eigene Spieler im Hardcore-Lauf oder mit geteilten HP (Balken leer), geht "SMY1" an alle; die anderen sterben
// (Bosskampf allein ist seit 26.10. kein Grund mehr - dort stirbt jeder fuer sich, wie im normalen Spiel)
// ueber dieselbe Funktion (Leben vorher auf 0, am Hook vorbei - kein Echo). Nach dem Wiederaufleben bleibt jeder, wo das
// Spiel ihn hinsetzt (kein Sprung zum Host).
constexpr uintptr_t kPlayerDieRva = 0x46E930;
constexpr uint32_t kPlayerDeadOff = 0x1D65D;    // byte: 1 = tot
constexpr uintptr_t kPlayerLifeRva = 0xDB2200;  // int: Leben (Inventar 0xDB21E0, Typ 1)
constexpr float kBossFightRange = 5000.0f;
using PlayerDieFn = void (*)(void*, uint64_t, uint64_t);
PlayerDieFn g_playerDieOrig = nullptr;
uint32_t g_deathSession = 1;
uint32_t g_deathSeq = 0;
std::vector<std::pair<DWORD, posecodec::DeathInfo>> g_localDeaths;  // (g_worldLock) Zeitpunkt + Eintrag
std::vector<std::pair<int, posecodec::DeathInfo>> g_remoteDeathIn;  // (g_worldLock) Absender + neu empfangener Eintrag
bool g_bossTestOverride = false;  // Debug-Befehl "bosstest": Bosskampf vortaeuschen (Spiel-Thread)
extern DWORD g_hardcoreGameOverAt;
DWORD g_waitRespawnSince = 0;     // Spiel-Thread: nach gemeinsamem Tod auf das Wiederaufleben warten (0 = nicht)
bool g_sawDead = false;

const char* DeathReasonName(coop::DeathReason r) {
    switch (r) {
        case coop::DeathReason::Boss: return L("Bosskampf", "boss fight");
        case coop::DeathReason::SharedHp: return L("geteilte HP", "shared HP");
        case coop::DeathReason::Hardcore: return "Hardcore";
        default: return L("allein", "alone");
    }
}

// Spiel-Thread: lebender Boss in Reichweite (out = Klasse und Entfernung des naechsten)
bool InBossFight(uintptr_t gp, std::string* out = nullptr) {
    if (g_bossTestOverride) {
        if (out) *out = "Test (bosstest)";
        return true;
    }
    float me[3] = {};
    if (!SafeRead(gp + 0x20, me, sizeof(me))) return false;
    float best = 1e30f;
    std::string bestCls;
    for (const auto& kv : g_trackedEnemies) {
        const TrackedEnemy& e = kv.second;
        if (!e.alive) continue;
        const std::string cls = ClassNameOf(e.obj);
        if (!coop::IsBossClass(cls.c_str())) continue;
        const float dx = e.pos[0] - me[0], dy = e.pos[1] - me[1], dz = e.pos[2] - me[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < best) {
            best = d;
            bestCls = cls;
        }
    }
    if (out) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s in %.0f", bestCls.empty() ? L("kein Boss", "no boss") : bestCls.c_str(), bestCls.empty() ? 0.0f : best);
        *out = buf;
    }
    return !bestCls.empty() && best <= kBossFightRange;
}

void BroadcastDeath(coop::DeathReason r) {
    posecodec::DeathInfo d;
    d.seq = ++g_deathSeq;
    d.reason = static_cast<uint8_t>(r);
    d.level = static_cast<uint16_t>(GetLevelInfo(g_base).level);
    AcquireSRWLockExclusive(&g_worldLock);
    g_localDeaths.push_back({GetTickCount(), d});
    if (g_localDeaths.size() > static_cast<size_t>(posecodec::kMaxDeaths)) g_localDeaths.erase(g_localDeaths.begin());
    ReleaseSRWLockExclusive(&g_worldLock);
    Log(L("[Tod] an alle: sterbt mit (%s)\n", "[Death] to everyone: die too (%s)\n"), DeathReasonName(r));
}

uint8_t PlayerDeadFlag(uintptr_t gp) {
    uint8_t dead = 0;
    SafeRead(gp + kPlayerDeadOff, &dead, 1);
    return dead;
}

// Spiel-Thread: eigenen Spieler sterben lassen (am Hook vorbei -> wird nicht weitergemeldet)
bool KillLocalPlayerGuarded(uintptr_t gp) {
    __try {
        *reinterpret_cast<int*>(g_base + kPlayerLifeRva) = 0;
        g_playerDieOrig(reinterpret_cast<void*>(gp), 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Debug-Befehl "sterben": Tod ueber den Hook, als waere er echt
bool DieLikeRealGuarded(uintptr_t gp) {
    __try {
        *reinterpret_cast<int*>(g_base + kPlayerLifeRva) = 0;
        reinterpret_cast<PlayerDieFn>(g_base + kPlayerDieRva)(reinterpret_cast<void*>(gp), 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HookedPlayerDie(void* player, uint64_t a, uint64_t type) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    const bool own = gp && reinterpret_cast<uintptr_t>(player) == gp;
    const uint8_t deadBefore = own ? PlayerDeadFlag(gp) : 1;
    g_playerDieOrig(player, a, type);
    if (!own || deadBefore || !PlayerDeadFlag(gp)) return;  // nur der erste Tod des eigenen Spielers
    const LevelInfo li = GetLevelInfo(g_base);
    if (!li.inGame) return;
    std::string boss;
    InBossFight(gp, &boss);  // nur fuers Log: im Bosskampf stirbt jeder fuer sich (coop::DeathReasonFor)
    const coop::DeathReason r = coop::DeathReasonFor(HardcoreRunOn(), SharedHpOn() && net::IsRunning());
    Log(L("[Tod] Ich bin gestorben (Art %d, Level %d, %s) - %s\n", "[Death] I died (type %d, level %d, %s) - %s\n"), static_cast<int>(type & 0xFF), li.level, boss.c_str(),
        DeathReasonName(r));
    if (r == coop::DeathReason::None) return;
    if (net::IsRunning()) BroadcastDeath(r);
    g_waitRespawnSince = GetTickCount();
    g_sawDead = true;
    if (r == coop::DeathReason::Hardcore) g_hardcoreGameOverAt = GetTickCount();
}

void HookPlayerDie() {
    g_deathSession = (GetTickCount() ^ (GetCurrentProcessId() << 8)) | 1;
    // mov [rsp+8],rbx / mov [rsp+10h],rbp / mov [rsp+18h],rsi
    static const uint8_t expected[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kPlayerDieRva), expected, sizeof(expected)) == 0)
        g_playerDieOrig = reinterpret_cast<PlayerDieFn>(
            InstallInlineHook(g_base + kPlayerDieRva, sizeof(expected), reinterpret_cast<void*>(HookedPlayerDie)));
    Log(L("Hook Spieler-Tod: %s\n", "Hook player death: %s\n"), g_playerDieOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// Netz-Thread: eigene Tode der letzten 5 s (mehrfach, gegen Verlust)
void SendDeaths() {
    posecodec::DeathInfo list[posecodec::kMaxDeaths];
    int num = 0;
    const DWORD now = GetTickCount();
    AcquireSRWLockShared(&g_worldLock);
    for (const auto& e : g_localDeaths)
        if (now - e.first < 5000 && num < posecodec::kMaxDeaths) list[num++] = e.second;
    ReleaseSRWLockShared(&g_worldLock);
    if (!num) return;
    uint8_t buf[posecodec::MaxDeathPacketSize()];
    net::Send(buf, posecodec::EncodeDeaths(g_deathSession, list, num, buf));
}

// Netz-Thread: false, wenn es kein Tod-Paket war
bool HandleDeathPacket(int from, const uint8_t* data, size_t size) {
    static uint32_t lastSessionArr[kMaxRemote] = {}, lastSeqArr[kMaxRemote] = {};
    posecodec::DeathInfo list[posecodec::kMaxDeaths];
    uint32_t session = 0;
    int num = 0;
    if (!posecodec::DecodeDeaths(data, size, session, list, num)) return false;
    if (session != lastSessionArr[from]) {
        lastSessionArr[from] = session;
        lastSeqArr[from] = 0;
    }
    AcquireSRWLockExclusive(&g_worldLock);
    for (int i = 0; i < num; ++i)
        if (list[i].seq > lastSeqArr[from]) {
            lastSeqArr[from] = list[i].seq;
            g_remoteDeathIn.push_back({from, list[i]});
        }
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// Spiel-Thread: Tod eines Mitspielers nachziehen, danach auf das Wiederaufleben warten
void DeathTick(uintptr_t gp) {
    struct Pending {
        int from = -1;
        posecodec::DeathInfo info;
        DWORD since = 0;
    };
    static std::deque<Pending> queue;
    std::vector<std::pair<int, posecodec::DeathInfo>> in;
    AcquireSRWLockExclusive(&g_worldLock);
    in.swap(g_remoteDeathIn);
    ReleaseSRWLockExclusive(&g_worldLock);
    const DWORD now = GetTickCount();
    for (const auto& e : in) {
        char name[64];
        net::PeerName(e.first, name, sizeof(name));
        Log(L("[Tod] %s ist gestorben (%s, Level %u) - alle sterben mit\n", "[Death] %s died (%s, level %u) - everyone dies too\n"), name[0] ? name : L("Mitspieler", "other player"),
            DeathReasonName(static_cast<coop::DeathReason>(e.second.reason)), e.second.level);
        Pending p;
        p.from = e.first;
        p.info = e.second;
        p.since = now;
        queue.push_back(p);
    }
    if (!queue.empty()) {
        Pending& p = queue.front();
        const LevelInfo li = GetLevelInfo(g_base);
        const bool cutscene = PlayingCutsceneOf(reinterpret_cast<uintptr_t (*)()>(g_base + kCutsceneMgrRva)()) >= 0;
        if (now - p.since > 20000) {
            Log(L("[Tod] Mitsterben nicht moeglich (Laden/Cutscene zu lange) - verworfen\n", "[Death] dying along not possible (loading/cutscene too long) - discarded\n"));
            queue.pop_front();
        } else if (li.inGame && !cutscene) {
            if (PlayerDeadFlag(gp)) {
                Log(L("[Tod] war schon tot\n", "[Death] was already dead\n"));
            } else {
                const bool ok = KillLocalPlayerGuarded(gp);
                Log(L("[Tod] mitgestorben%s\n", "[Death] died along%s\n"), ok ? "" : L(" - FEHLER (Ausnahme)", " - ERROR (exception)"));
            }
            g_waitRespawnSince = now;
            g_sawDead = true;
            if (p.info.reason == static_cast<uint8_t>(coop::DeathReason::Hardcore)) g_hardcoreGameOverAt = now;
            queue.pop_front();
            queue.clear();  // mehrere gleichzeitige Tode: einmal sterben genuegt
        }
    }
    // Wiederaufleben: neues Spielerobjekt bzw. Tot-Flag wieder 0, keine Cutscene. Jeder bleibt, wo das Spiel ihn nach dem Tod
    // hinsetzt - auch nach einem Bosskampf (Wunsch des Users; frueher ging der Gast zum Host). Hardcore: es geht zum Titelbild.
    if (g_waitRespawnSince && !g_hardcoreGameOverAt) {
        const bool cutscene = PlayingCutsceneOf(reinterpret_cast<uintptr_t (*)()>(g_base + kCutsceneMgrRva)()) >= 0;
        if (now - g_waitRespawnSince > 180000) {
            g_waitRespawnSince = 0;
        } else if (GetLevelInfo(g_base).inGame && !cutscene && !PlayerDeadFlag(gp) && now - g_waitRespawnSince > 3000) {
            g_waitRespawnSince = 0;
            Log(L("[Tod] wieder da - bleibe, wo das Spiel mich hinsetzt\n", "[Death] back - staying where the game puts me\n"));
        }
    }
}

// ---- Geteilte HP ----
// Leben = Inventar 0xDB21E0 Typ 1 (int [0xDB2200]), Maximum Typ 0 ([0xDB21FC]). Jede eigene Aenderung (Treffer, Heilung) geht
// als Delta an alle ("SMP1", die letzte Sekunde wiederholt); die anderen addieren sie direkt - ohne Wackeln, roten Bildschirm
// und Treffer-Sound. Der Host schickt seinen Wert mit; Gaeste uebernehmen ihn nach 1 s Ruhe (gegen Abweichungen). Tot, Laden,
// anderer Spieler: nichts uebertragen. Ist der Balken leer, stirbt der Getroffene normal und der gemeinsame Tod nimmt alle mit.
constexpr uintptr_t kPlayerMaxLifeRva = 0xDB21FC;
uint32_t g_lifeSession = 1;
uint32_t g_lifeSeq = 0;
std::vector<std::pair<DWORD, posecodec::LifeDelta>> g_localLifeDeltas;  // (g_worldLock) Zeitpunkt + eigene Aenderung
std::vector<int32_t> g_remoteLifeIn;                                     // (g_worldLock) neue Deltas der Mitspieler
int32_t g_hostLifeValue = -1;                                            // (g_worldLock) zuletzt vom Host
DWORD g_hostLifeAt = 0;                                                  // (g_worldLock)
std::atomic<int32_t> g_myLifeForSend{-1};

int ReadLocalLife() {
    int v = -1;
    SafeRead(g_base + kPlayerLifeRva, &v, 4);
    return v;
}

bool SetLocalLifeGuarded(int v) {
    __try {
        *reinterpret_cast<int*>(g_base + kPlayerLifeRva) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Spiel-Thread
void SharedHpTick(uintptr_t gp) {
    static uintptr_t lastGp = 0;
    static int lastLife = -1;
    static DWORD quietUntil = 0, lastLocalChange = 0, lastRemoteDelta = 0;
    const DWORD now = GetTickCount();
    std::vector<int32_t> in;
    AcquireSRWLockExclusive(&g_worldLock);
    in.swap(g_remoteLifeIn);
    const int32_t hostValue = g_hostLifeValue;
    const DWORD hostAt = g_hostLifeAt;
    ReleaseSRWLockExclusive(&g_worldLock);
    const int life = ReadLocalLife();
    if (!SharedHpOn() || !net::IsRunning() || !GetLevelInfo(g_base).inGame || life < 0 || PlayerDeadFlag(gp) || gp != lastGp) {
        lastGp = gp;
        lastLife = life;
        quietUntil = now + 3000;  // nach Laden/Wiederaufleben erst den eigenen Wert annehmen
        g_myLifeForSend = -1;
        return;
    }
    if (now < quietUntil) {
        lastLife = life;
        return;
    }
    if (life != lastLife) {  // eigene Aenderung (Treffer, Heilung)
        posecodec::LifeDelta d;
        d.seq = ++g_lifeSeq;
        d.delta = life - lastLife;
        AcquireSRWLockExclusive(&g_worldLock);
        g_localLifeDeltas.push_back({now, d});
        if (g_localLifeDeltas.size() > static_cast<size_t>(posecodec::kMaxLifeDeltas)) g_localLifeDeltas.erase(g_localLifeDeltas.begin());
        ReleaseSRWLockExclusive(&g_worldLock);
        lastLocalChange = now;
        Log(L("[Geteilte HP] eigene Aenderung %+d -> %d\n", "[Shared HP] own change %+d -> %d\n"), d.delta, life);
    }
    int maxLife = 0;
    SafeRead(g_base + kPlayerMaxLifeRva, &maxLife, 4);
    int v = life;
    for (int32_t d : in) v = coop::ApplyLifeDelta(v, d, maxLife);
    if (!in.empty()) {
        lastRemoteDelta = now;
        Log(L("[Geteilte HP] Mitspieler-Aenderung -> %d\n", "[Shared HP] other player's change -> %d\n"), v);
    }
    if (!net::IsHost() && hostValue >= 0 && in.empty() && now - lastLocalChange > 1000 && now - lastRemoteDelta > 1000 &&
        now - hostAt < 3000 && hostValue != v) {
        v = coop::ApplyLifeDelta(hostValue, 0, maxLife);
        Log(L("[Geteilte HP] Abgleich mit dem Host -> %d\n", "[Shared HP] sync with host -> %d\n"), v);
    }
    if (v != life) SetLocalLifeGuarded(v);
    lastLife = v;
    g_myLifeForSend = v;
}

// Netz-Thread: eigene Aenderungen der letzten Sekunde + (Host) eigener Wert
void SendLife() {
    if (!SharedHpOn()) return;
    posecodec::LifeDelta list[posecodec::kMaxLifeDeltas];
    int num = 0;
    const DWORD now = GetTickCount();
    AcquireSRWLockShared(&g_worldLock);
    for (const auto& e : g_localLifeDeltas)
        if (now - e.first < 1000 && num < posecodec::kMaxLifeDeltas) list[num++] = e.second;
    ReleaseSRWLockShared(&g_worldLock);
    const int32_t value = net::IsHost() ? g_myLifeForSend.load() : -1;
    if (!num && value < 0) return;
    uint8_t buf[posecodec::MaxLifePacketSize()];
    net::Send(buf, posecodec::EncodeLife(g_lifeSession, value, list, num, buf));
}

// Netz-Thread: false, wenn es kein Lebens-Paket war
bool HandleLifePacket(int from, const uint8_t* data, size_t size) {
    static uint32_t lastSessionArr[kMaxRemote] = {}, lastSeqArr[kMaxRemote] = {};
    posecodec::LifeDelta list[posecodec::kMaxLifeDeltas];
    uint32_t session = 0;
    int32_t value = -1;
    int num = 0;
    if (!posecodec::DecodeLife(data, size, session, value, list, num)) return false;
    if (session != lastSessionArr[from]) {
        lastSessionArr[from] = session;
        lastSeqArr[from] = 0;
    }
    AcquireSRWLockExclusive(&g_worldLock);
    for (int i = 0; i < num; ++i)
        if (list[i].seq > lastSeqArr[from]) {
            lastSeqArr[from] = list[i].seq;
            g_remoteLifeIn.push_back(list[i].delta);
        }
    if (FromHost(from) && value >= 0) {
        g_hostLifeValue = value;
        g_hostLifeAt = GetTickCount();
    }
    ReleaseSRWLockExclusive(&g_worldLock);
    return true;
}

// ---- Namensschilder ----
// Kamera: [exe+0xD84128] + 0x40 -> RenderView-Puffer (wie cbuffer RenderView in progs/globals_structs.inc): uClipMatrix +576,
// uViewOrigin +832, Breite/Hoehe +844/+848. Projektion als Zeilenvektor (coop::ProjectToNdc). Menue offen: [0xD838C0] (aktuelles Menue,
// z.B. Pausenmenue 0xCD4DC0) != 0 oder Menue-Stapel [0xCCACEC] > 0.
// Ueber jedem Mitspieler im selben Level: Name, Meter, Pfeilspitze (Farbe nach Spielernummer, kleiner/blasser mit Abstand,
// durch Waende, da nach dem Spielbild gezeichnet). Ausserhalb des Bildes: Pfeil am Rand mit Name + Meter. Anderes Level:
// oben "Name: Levelname" ($m_levelNN, uebersetzt im Spiel-Thread).
constexpr uintptr_t kRenderViewHolderRva = 0xD84128;
constexpr uintptr_t kMenuDepthRva = 0xCCACEC;
constexpr uintptr_t kCurrentMenuRva = 0xD838C0;
constexpr float kHeadHeight = 75.0f;  // ueber dem Ursprung der Figur
SRWLOCK g_levelNameLock = SRWLOCK_INIT;
std::map<int, std::string> g_levelNames;  // (g_levelNameLock) Level -> Name (Latin-1)

const char* TranslateRawGuarded(const char* key) {
    __try {
        const uintptr_t lang = reinterpret_cast<uintptr_t (*)()>(g_base + 0x2E9930)();
        return reinterpret_cast<const char* (*)(uintptr_t, const char*)>(g_base + 0x1CE680)(lang, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Sprache der Mod nach den geladenen Spieltexten: die Schwierigkeit "m_difficulty_hard" heisst nur im deutschen Spiel
// "Der Horror" (englisch "The Horror"). So folgt die Mod auch einer Umstellung in den Optionen. Alle 2 s (Menue- und Welt-Takt).
void DetectLanguageFromGame() {
    static DWORD last = 0;
    static int logged = -1;
    if (GetTickCount() - last < 2000) return;
    last = GetTickCount();
    const char* t = TranslateRawGuarded("$m_difficulty_hard");
    if (!t || !t[0] || t[0] == '$') return;  // Spieltexte noch nicht geladen
    const i18n::Lang lang = std::strcmp(t, "Der Horror") == 0 ? i18n::Lang::De : i18n::Lang::En;
    i18n::SetLang(lang);
    if (logged != static_cast<int>(lang)) {
        logged = static_cast<int>(lang);
        Log(lang == i18n::Lang::De ? "[Sprache] Deutsch (Spieltexte)\n" : "[Language] English (game texts)\n");
    }
}

// Spiel-Thread: Levelnamen der Mitspieler-Level bereitlegen
void UpdateLevelNames() {
    static DWORD last = 0;
    if (GetTickCount() - last < 2000) return;
    last = GetTickCount();
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        const int level = g_peerLevel[pi];
        if (level < 0 || level > 99) continue;
        AcquireSRWLockShared(&g_levelNameLock);
        const bool known = g_levelNames.count(level) != 0;
        ReleaseSRWLockShared(&g_levelNameLock);
        if (known) continue;
        char key[24];
        std::snprintf(key, sizeof(key), "$m_level%02d", level);
        const char* raw = TranslateRawGuarded(key);
        std::string name = raw && raw[0] && raw[0] != '$' ? coop::Utf8ToLatin1(raw) : "Level " + std::to_string(level);
        for (char& ch : name)  // manche Namen sind zweizeilig ("Asyl :\nExperimentierraeume")
            if (ch == '\n' || ch == '\r') ch = ' ';
        while (!name.empty() && static_cast<unsigned char>(name.back()) <= ' ') name.pop_back();
        Log(L("[Namen] Level %d heisst \"%s\"\n", "[Names] level %d is called \"%s\"\n"), level, name.c_str());
        AcquireSRWLockExclusive(&g_levelNameLock);
        g_levelNames[level] = name;
        ReleaseSRWLockExclusive(&g_levelNameLock);
    }
}

void AddLabel(overlay::Frame& f, float x, float y, float scale, uint32_t rgb, float alpha, int align, const std::string& text) {
    if (f.numLabels >= static_cast<int>(sizeof(f.labels) / sizeof(f.labels[0]))) return;
    overlay::Label& l = f.labels[f.numLabels++];
    l.x = x;
    l.y = y;
    l.scale = scale;
    l.rgb = rgb;
    l.alpha = alpha;
    l.align = align;
    std::snprintf(l.text, sizeof(l.text), "%s", text.c_str());
}

void AddArrow(overlay::Frame& f, float x, float y, float angle, float size, uint32_t rgb, float alpha) {
    if (f.numArrows >= static_cast<int>(sizeof(f.arrows) / sizeof(f.arrows[0]))) return;
    overlay::Arrow& a = f.arrows[f.numArrows++];
    a.x = x;
    a.y = y;
    a.angle = angle;
    a.size = size;
    a.rgb = rgb;
    a.alpha = alpha;
}

// Present-Funktion der D3D11-Zeichenflaeche (vtable-Aufruf, 0x2903B0): holt die Swapchain aus dem D3D11-Kontext
// [[0xD83210] + 0x80] und ruft darin IDXGISwapChain::Present (call [rax+40h] bei 0x290435 - diese Stelle biegt SMTrainer um).
// Wir haengen uns an den Anfang und zeichnen die Schilder davor; die dxgi-vtable bleibt unberuehrt (Steam-Overlay).
constexpr uintptr_t kRtPresentRva = 0x2903B0;
constexpr uintptr_t kD3D11ContextRva = 0xD83210;
using RtPresentFn = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
RtPresentFn g_rtPresentOrig = nullptr;

uint64_t HookedRtPresent(void* rt, uint64_t a, uint64_t b, uint64_t c) {
    uintptr_t ctx = 0;
    void* sc = nullptr;
    if (SafeRead(g_base + kD3D11ContextRva, &ctx, 8) && ctx && SafeRead(ctx + 0x80, &sc, 8) && sc) overlay::RenderFrame(sc);
    return g_rtPresentOrig(rt, a, b, c);
}

void HookRtPresent() {
    // mov [rsp+8],rbx / mov [rsp+18h],rsi / push rdi / sub rsp,60h
    static const uint8_t expected[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kRtPresentRva), expected, sizeof(expected)) == 0)
        g_rtPresentOrig = reinterpret_cast<RtPresentFn>(InstallInlineHook(g_base + kRtPresentRva, sizeof(expected), reinterpret_cast<void*>(HookedRtPresent)));
    Log(L("[Namen] Zeichen-Hook vor Present: %s\n", "[Names] draw hook before Present: %s\n"), g_rtPresentOrig ? L("aktiv", "active") : L("FEHLER (andere Spielversion?)", "ERROR (different game version?)"));
}

// Render-Thread (vor Present), je Bild
void OverlayFrame(overlay::Frame& f, float, float) {
    if (!g_localSettings.showNames || !net::IsRunning()) return;
    const LevelInfo li = GetLevelInfo(g_base);
    int menus = 0;
    uintptr_t currentMenu = 0;
    SafeRead(g_base + kMenuDepthRva, &menus, 4);
    SafeRead(g_base + kCurrentMenuRva, &currentMenu, 8);
    if (!li.inGame || menus > 0 || currentMenu) return;
    if (PlayingCutsceneOf(reinterpret_cast<uintptr_t (*)()>(g_base + kCutsceneMgrRva)()) >= 0) return;
    uintptr_t holder = 0, view = 0;
    float clip[16] = {}, me[3] = {};
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (!SafeRead(g_base + kRenderViewHolderRva, &holder, 8) || !holder || !SafeRead(holder + 0x40, &view, 8) || !view ||
        !SafeRead(view + 576, clip, sizeof(clip)) || !gp || !SafeRead(gp + 0x20, me, sizeof(me)))
        return;
    int otherLine = 0;
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        char rawName[64];
        net::PeerName(pi, rawName, sizeof(rawName));
        if (!rawName[0] || !g_peerFresh[pi]) continue;
        const std::string name = coop::Utf8ToLatin1(rawName);
        const uint32_t rgb = coop::PlayerColorRgb(PlayerNumberOfPeer(pi));
        const uintptr_t avatar = g_avatar[pi];
        if (g_peerLevel[pi] != li.level || !g_peerInGame[pi] || !avatar || !g_remoteActive[pi]) {
            if (!g_peerInGame[pi]) continue;
            std::string level = "Level " + std::to_string(g_peerLevel[pi].load());
            AcquireSRWLockShared(&g_levelNameLock);
            const auto it = g_levelNames.find(g_peerLevel[pi]);
            if (it != g_levelNames.end()) level = it->second;
            ReleaseSRWLockShared(&g_levelNameLock);
            AddLabel(f, 0.0f, 0.95f - 0.07f * otherLine++, 0.6f, rgb, 0.9f, 0, name + ": " + level);
            continue;
        }
        float pos[3];
        if (!SafeRead(avatar + 0x20, pos, sizeof(pos))) continue;
        const float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float s = coop::TagScale(dist), a = coop::TagAlpha(dist);
        const std::string meters = std::to_string(coop::Meters(dist)) + " m";
        const float head[3] = {pos[0], pos[1] + kHeadHeight, pos[2]};
        float nx = 0, ny = 0, w = 0;
        const bool front = coop::ProjectToNdc(clip, head, nx, ny, w);
        if (front && std::fabs(nx) <= 1.02f && std::fabs(ny) <= 1.02f) {
            // von unten nach oben: Pfeilspitze am Kopf, darueber Meter, darueber Name (Hoehen in -1..1)
            const float arrowH = 0.052f * s * 1.6f, meterH = 0.084f * 0.7f * s, nameH = 0.084f * s;
            AddArrow(f, nx, ny, -1.5707963f, s, rgb, a);
            AddLabel(f, nx, ny + arrowH + meterH + 0.01f, 0.7f * s, rgb, a, 0, meters);
            AddLabel(f, nx, ny + arrowH + meterH + nameH + 0.02f, s, rgb, a, 0, name);
        } else {
            float ex = 0, ey = 0, ang = 0;
            coop::EdgePoint(nx, ny, !front, 0.06f, ex, ey, ang);
            AddArrow(f, ex, ey, ang, 0.9f, rgb, 0.95f);
            const float lx = ex - std::cos(ang) * 0.10f, ly = ey - std::sin(ang) * 0.10f;
            AddLabel(f, lx, ly + 0.09f, 0.65f, rgb, 0.95f, 0, name);
            AddLabel(f, lx, ly + 0.025f, 0.5f, rgb, 0.95f, 0, meters);
        }
    }
}

// ---- Hardcore ----
// Schwierigkeitsmenue 0xCD4960 (statisch aufgebaut von 0x129A60): "Normal" -> 0x412730, "Der Horror" -> 0x412770 (setzen
// [0xF617BC] = 0/1 und oeffnen die Platzwahl 0xCD4AA0). Angehaengt: "Hardcore" und "Hardcore Der Horror" - sie merken
// g_pendingHardcore und rufen dieselben Rueckrufe. Beim Eintritt ins Spiel ohne geladenen Spielstand (neues Abenteuer) wird
// daraus g_hardcoreRun. Spielstaende: <Saved Games>\Nightdive Studios\Shadowman EX\saves\save_NN.sav (+ .sig), beim Start
// und beim Oeffnen des Lade-Menues in den Speicher gelesen (SaveMgr vt[18] 0x340790) - geladen wird aus diesem Zwischenspeicher
// ueber SaveMgr vt[25] 0x343A60(mgr, platz, 0x19, ?) bzw. vt[26] (Lade-Rueckruf 0x412660(platz)); gespeichert = Aenderungszeit
// der Datei (alle 2 s). smcoop_hardcore.txt im Speicherordner: "<platz> <kennung des Laufs>".
// Tod im Hardcore: alle sterben (SMY1 Grund 3); nach 4 s loescht jeder die Plaetze seines Laufs und geht zum Titelbild
// (0x40BE50 = "Ja" bei "Zurueck zum Titel"); das Hauptmenue zeigt oben einen Hinweis. Probe: smcoop_hardcore_probe.txt im
// Spielordner -> nur protokollieren, nichts loeschen.
constexpr uintptr_t kDifficultyMenuRva = 0xCD4960;
constexpr uintptr_t kDiffNormalRva = 0x412730, kDiffHorrorRva = 0x412770;
constexpr uintptr_t kReturnToTitleRva = 0x40BE50;
constexpr uintptr_t kSaveMgrVtableRva = 0x706810;
constexpr int kMaxSaveSlots = 14;  // SaveMgr+0x50
std::atomic<bool> g_pendingHardcore{false};     // im Schwierigkeitsmenue "Hardcore ..." gewaehlt
std::atomic<uint32_t> g_hardcoreId{0};          // Kennung des laufenden Hardcore-Laufs (0 = keiner)
std::atomic<int> g_lastSaveRead{-1};            // zuletzt zum Lesen geoeffneter Platz
std::atomic<DWORD> g_lastSaveReadAt{0};
std::atomic<DWORD> g_lastPlayerLoadAt{0};       // HookedPlayerLoad: Spielstand geladen
DWORD g_hardcoreGameOverAt = 0;                 // Spiel-Thread: Hardcore-Tod, Abschluss nach 4 s
char g_hardcoreNotice[160] = "";                // Hinweis oben im Hauptmenue
bool g_diffMenuHooked = false;

std::wstring SaveDir() {
    static std::wstring dir;
    if (dir.empty()) {
        PWSTR saved = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)) && saved)
            dir = std::wstring(saved) + L"\\Nightdive Studios\\Shadowman EX\\saves";
        if (saved) CoTaskMemFree(saved);
    }
    return dir;
}

std::wstring SaveSlotPath(int slot, const wchar_t* ext) { return SaveDir() + L"\\" + coop::SaveFileStem(slot) + ext; }

std::string SlotName(int slot) {
    return slot == coop::kQuickSaveSlot ? std::string(L("Schnellspeicher", "quick save")) : L("Platz ", "slot ") + std::to_string(slot + 1);
}

std::map<int, uint32_t> ReadHardcoreFile() {
    std::string text;
    if (FILE* f = _wfopen((SaveDir() + L"\\smcoop_hardcore.txt").c_str(), L"r")) {
        char line[64];
        while (std::fgets(line, sizeof(line), f)) text += line;
        std::fclose(f);
    }
    return coop::ParseHardcoreFile(text);
}

void WriteHardcoreFile(const std::map<int, uint32_t>& m) {
    const std::wstring path = SaveDir() + L"\\smcoop_hardcore.txt";
    if (m.empty()) {
        DeleteFileW(path.c_str());
        return;
    }
    if (FILE* f = _wfopen(path.c_str(), L"w")) {
        std::fputs(coop::FormatHardcoreFile(m).c_str(), f);
        std::fclose(f);
    }
}

uint32_t NewHardcoreId() {
    uint32_t id = 0;
    while (!id) id = (GetTickCount() * 2654435761u) ^ (GetCurrentProcessId() << 16) ^ static_cast<uint32_t>(__rdtsc());
    return id;
}

void SetHardcoreRun(bool on, uint32_t id, const char* why) {
    const bool was = g_hardcoreRun;
    g_hardcoreRun = on;
    g_hardcoreId = on ? id : 0;
    g_hostSettingsDirty = true;
    if (was != on || on) Log(L("[Hardcore] %s: %s%s\n", "[Hardcore] %s: %s%s\n"), why, on ? "HARDCORE" : "normal", on ? "" : "");
}

// --- Welcher Platz wird geladen? (SaveMgr vt[25]/vt[26]) ---
using SaveMgrLoadFn = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
SaveMgrLoadFn g_saveMgrLoadOrig[2] = {};
void NoteSlotLoad(uint64_t slot) {
    g_lastSaveRead = static_cast<int>(slot & 0xFF);
    g_lastSaveReadAt = GetTickCount();
    Log(L("[Hardcore] lade Platz %d\n", "[Hardcore] loading slot %d\n"), g_lastSaveRead.load() + 1);
}
uint64_t HookedSaveMgrLoad0(void* mgr, uint64_t slot, uint64_t a, uint64_t b) {
    NoteSlotLoad(slot);
    return g_saveMgrLoadOrig[0](mgr, slot, a, b);
}
uint64_t HookedSaveMgrLoad1(void* mgr, uint64_t slot, uint64_t a, uint64_t b) {
    NoteSlotLoad(slot);
    return g_saveMgrLoadOrig[1](mgr, slot, a, b);
}

// --- Schwierigkeitsmenue ---
using VoidFn = void (*)();
void DiffNormalClicked() {
    g_pendingHardcore = false;
    reinterpret_cast<VoidFn>(g_base + kDiffNormalRva)();
}
void DiffHorrorClicked() {
    g_pendingHardcore = false;
    reinterpret_cast<VoidFn>(g_base + kDiffHorrorRva)();
}
void DiffHardcoreClicked() {
    g_pendingHardcore = true;
    Log(L("[Hardcore] Schwierigkeit Hardcore gewaehlt\n", "[Hardcore] difficulty Hardcore chosen\n"));
    reinterpret_cast<VoidFn>(g_base + kDiffNormalRva)();
}
void DiffHardcoreHorrorClicked() {
    g_pendingHardcore = true;
    Log(L("[Hardcore] Schwierigkeit Hardcore Der Horror gewaehlt\n", "[Hardcore] difficulty Hardcore The Horror chosen\n"));
    reinterpret_cast<VoidFn>(g_base + kDiffHorrorRva)();
}

bool AppendMenuItemGuarded(uintptr_t def, const char* label, void* submenu, void (*callback)());
bool HookDifficultyMenuGuarded() {
    __try {
        const uintptr_t def = g_base + kDifficultyMenuRva;
        const uint32_t count = *reinterpret_cast<uint32_t*>(def + 0x18 + 0xC);
        auto* items = *reinterpret_cast<uintptr_t**>(def + 0x18);
        if (count != 2 || !items) return false;  // noch nicht aufgebaut
        uintptr_t* cb0 = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(items) + 0x10);
        uintptr_t* cb1 = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(items) + 0x58 + 0x10);
        if (*cb0 != g_base + kDiffNormalRva || *cb1 != g_base + kDiffHorrorRva) return false;
        *cb0 = reinterpret_cast<uintptr_t>(DiffNormalClicked);
        *cb1 = reinterpret_cast<uintptr_t>(DiffHorrorClicked);
        AppendMenuItemGuarded(def, "Hardcore", nullptr, DiffHardcoreClicked);
        AppendMenuItemGuarded(def, L("Hardcore Der Horror", "Hardcore The Horror"), nullptr, DiffHardcoreHorrorClicked);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* PatchVtableEntry(uintptr_t vtRva, int index, void* detour);
void HookHardcore() {
    g_saveMgrLoadOrig[0] = reinterpret_cast<SaveMgrLoadFn>(PatchVtableEntry(kSaveMgrVtableRva, 25, reinterpret_cast<void*>(HookedSaveMgrLoad0)));
    g_saveMgrLoadOrig[1] = reinterpret_cast<SaveMgrLoadFn>(PatchVtableEntry(kSaveMgrVtableRva, 26, reinterpret_cast<void*>(HookedSaveMgrLoad1)));
    g_diffMenuHooked = HookDifficultyMenuGuarded();
    Log(L("Hook Hardcore: Laden %s, Schwierigkeitsmenue %s, Speicherordner %ls\n", "Hook hardcore: loading %s, difficulty menu %s, save folder %ls\n"),
        g_saveMgrLoadOrig[0] && g_saveMgrLoadOrig[1] ? L("aktiv", "active") : L("FEHLER", "ERROR"), g_diffMenuHooked ? L("aktiv", "active") : L("spaeter", "later"), SaveDir().c_str());
}

// HookedPlayerLoad: Spielstand geladen - Hardcore, wenn der gelesene Platz zu einem Lauf gehoert
void HardcoreOnPlayerLoad() {
    const DWORD now = GetTickCount();
    g_lastPlayerLoadAt = now;
    const int slot = g_lastSaveRead;
    if (slot < 0 || now - g_lastSaveReadAt > 15000) return;  // kein Spielstand (z.B. Levelwechsel): Zustand bleibt
    const std::map<int, uint32_t> m = ReadHardcoreFile();
    const auto it = m.find(slot);
    char why[48];
    std::snprintf(why, sizeof(why), L("%s geladen", "%s loaded"), SlotName(slot).c_str());
    SetHardcoreRun(it != m.end(), it != m.end() ? it->second : 0, why);
}

// Eintritt ins Spiel aus dem Hauptmenue: ohne geladenen Spielstand = neues Abenteuer. Das Laden des Spielers folgt erst nach
// dem Eintritt - entschieden wird deshalb 5 s spaeter in HardcoreTick.
std::atomic<DWORD> g_enteredGameAt{0};
void HardcoreOnEnterGame() { g_enteredGameAt = GetTickCount() | 1; }

// Spiel-Thread: Plaetze des Laufs loeschen (bzw. in der Probe nur nennen)
void HardcoreDeleteRun() {
    const uint32_t id = g_hardcoreId;
    std::map<int, uint32_t> m = ReadHardcoreFile();
    const std::vector<int> slots = coop::SlotsOfRun(m, id);
    const bool probe = GetFileAttributesA("smcoop_hardcore_probe.txt") != INVALID_FILE_ATTRIBUTES;
    if (slots.empty()) Log(L("[Hardcore] kein gespeicherter Platz dieses Laufs - nichts zu loeschen\n", "[Hardcore] no saved slot of this run - nothing to delete\n"));
    for (int slot : slots) {
        for (const wchar_t* ext : {L".sav", L".sav.sig"}) {
            const std::wstring path = SaveSlotPath(slot, ext);
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (probe) {
                Log(L("[Hardcore] PROBE - wuerde loeschen: %ls\n", "[Hardcore] DRY RUN - would delete: %ls\n"), path.c_str());
            } else {
                const BOOL ok = DeleteFileW(path.c_str());
                Log(L("[Hardcore] geloescht: %ls%s\n", "[Hardcore] deleted: %ls%s\n"), path.c_str(), ok ? "" : L(" (FEHLER)", " (ERROR)"));
            }
        }
        if (!probe) m.erase(slot);
    }
    if (!probe) WriteHardcoreFile(m);
}

void HardcoreGameOver() {
    Log(L("[Hardcore] Ihr seid gestorben - Spielstand wird geloescht, zurueck zum Titelbild\n", "[Hardcore] you died - adventure is deleted, back to the title screen\n"));
    HardcoreDeleteRun();
    SetHardcoreRun(false, 0, L("Hardcore-Tod", "hardcore death"));
    g_pendingHardcore = false;
    g_waitRespawnSince = 0;
    std::snprintf(g_hardcoreNotice, sizeof(g_hardcoreNotice), L("Hardcore-Tod: Spielstand geloescht", "Hardcore death: adventure deleted"));
    __try {
        reinterpret_cast<VoidFn>(g_base + kReturnToTitleRva)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(L("[Hardcore] Zurueck zum Titel: Ausnahme\n", "[Hardcore] back to title: exception\n"));
    }
}

// Spiel-Thread: Abschluss nach dem Hardcore-Tod, gespeicherte Plaetze verfolgen
void HardcoreTick() {
    const DWORD now = GetTickCount();
    if (!g_diffMenuHooked) {
        static DWORD lastTry = 0;
        if (now - lastTry > 2000) {
            lastTry = now;
            g_diffMenuHooked = HookDifficultyMenuGuarded();
            if (g_diffMenuHooked) Log(L("[Hardcore] Schwierigkeitsmenue erweitert\n", "[Hardcore] difficulty menu extended\n"));
        }
    }
    if (const DWORD entered = g_enteredGameAt; entered && now - entered > 5000) {
        g_enteredGameAt = 0;
        const bool loaded = g_lastPlayerLoadAt && g_lastPlayerLoadAt + 20000 > entered;
        const bool hc = g_pendingHardcore.exchange(false);
        if (!loaded) SetHardcoreRun(hc, hc ? NewHardcoreId() : 0, L("neues Abenteuer", "new adventure"));
    }
    if (g_hardcoreGameOverAt && now - g_hardcoreGameOverAt > 4000) {
        g_hardcoreGameOverAt = 0;
        HardcoreGameOver();
    }
    static DWORD lastScan = 0;
    if (now - lastScan < 2000) return;
    lastScan = now;
    static std::map<int, uint64_t> known;  // Platz -> Aenderungszeit
    static bool first = true;
    for (int i = 0; i <= kMaxSaveSlots; ++i) {
        const int slot = i < kMaxSaveSlots ? i : coop::kQuickSaveSlot;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(SaveSlotPath(slot, L".sav").c_str(), GetFileExInfoStandard, &fa)) {
            known.erase(slot);
            continue;
        }
        const uint64_t t = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        const auto it = known.find(slot);
        const bool changed = !first && (it == known.end() || it->second != t);
        known[slot] = t;
        if (!changed) continue;
        std::map<int, uint32_t> m = ReadHardcoreFile();
        if (HardcoreRunOn()) {
            if (!g_hardcoreId) g_hardcoreId = NewHardcoreId();  // Gast im Hardcore-Lauf des Hosts: eigener Lauf
            m[slot] = g_hardcoreId;
            Log(L("[Hardcore] %s gespeichert - gehoert zum Hardcore-Lauf\n", "[Hardcore] %s saved - belongs to the hardcore run\n"), SlotName(slot).c_str());
        } else if (m.erase(slot)) {
            Log(L("[Hardcore] %s normal ueberschrieben - kein Hardcore mehr\n", "[Hardcore] %s overwritten normally - no longer hardcore\n"), SlotName(slot).c_str());
        }
        WriteHardcoreFile(m);
    }
    first = false;
}

// ---- Koop-Menue im Hauptmenue (Steam-Lobby) ----
// Menue-Beschreibungen liegen statisch in .data (je 0xA0 Byte): +0x00 x/y, +0x10 Titel, +0x18 Eintraege (Zeiger, +0x20
// Kapazitaet, +0x24 Anzahl; je 0x58 Byte: +0x00 Text - "$m_..." wird uebersetzt (0x1CE680), alles andere wortwoertlich
// angezeigt -, +0x08 Untermenue, +0x10 Rueckruf void(), +0x18 -1), +0x28 Flags, +0x30 Skalierung, +0x40 Aufbau(menue),
// +0x50 Aktualisierung(menue). Hauptmenue: 0xCD4C80 (Aufbau 0x40CEC0 -> 0x40C660, Aktualisierung 0x40C5C0); Untermenues
// oeffnet 0x3FD8B0(menue). Wir haengen "Koop-Lobby" vor "Beenden" an; das Untermenue ist eine Kopie der Hauptmenue-
// Beschreibung mit eigener Aufbau-/Aktualisierungs-Funktion.
// Gast in der Lobby: wartet im Hauptmenue; sobald der Host im Spiel ist, startet die Mod ein neues Abenteuer in einem freien
// Speicherplatz (wie "Neues Abenteuer": [0xF617BC] = Schwierigkeit, 0x412800(platz) prueft den Platz und startet 0x2EA070)
// - danach holt "Folgen" ihn zum Host und er uebernimmt dessen Stand.
constexpr uintptr_t kMainMenuDefRva = 0xCD4C80;
constexpr uint32_t kMenuDefSize = 0xA0;
constexpr uint32_t kMenuItemsOff = 0x18, kMenuTitleOff = 0x10, kMenuBuildOff = 0x40, kMenuUpdateOff = 0x50;
constexpr uintptr_t kMenuArrayGrowRva = 0x412F10;      // (array, neue Kapazitaet)
constexpr uintptr_t kNewGameInSlotRva = 0x412800;      // (platz) wie im Speicherplatz-Menue von "Neues Abenteuer"
constexpr uintptr_t kNewGameStartRva = 0x4127B0;      // Start im Platz [0xF617B8] ohne Rueckfrage
constexpr uintptr_t kNewGameSkillRva = 0xF617BC;       // gewaehlte Schwierigkeit (0 normal, 1 schwer)
constexpr uintptr_t kSaveMgrGetRva = 0x347560;
constexpr uintptr_t kSaveSlotInfoRva = 0x33EC50;       // (mgr, platz) -> Info, +0x30 != 0 = belegt
constexpr uint32_t kGameStateSkillOff = 0x50;
struct MenuItem {
    const char* label;
    void* submenu;
    void (*callback)();
    int32_t arg;
    int32_t pad;
    uint8_t rest[0x38];
};
static_assert(sizeof(MenuItem) == 0x58, "MenuItem");
using MenuFn = void (*)(uintptr_t def);
MenuFn g_mainMenuBuildOrig = nullptr;
MenuFn g_mainMenuUpdateOrig = nullptr;
alignas(16) uint8_t g_coopMenuDef[kMenuDefSize];
char g_coopLabels[6][128];
char g_coopStatus[128] = "";  // letzte Meldung des automatischen Starts (Gast)

bool AppendMenuItemGuarded(uintptr_t def, const char* label, void* submenu, void (*callback)()) {
    __try {
        const uintptr_t arr = def + kMenuItemsOff;
        uint32_t* cap = reinterpret_cast<uint32_t*>(arr + 8);
        uint32_t* count = reinterpret_cast<uint32_t*>(arr + 0xC);
        if (*count + 1 >= *cap) reinterpret_cast<void (*)(uintptr_t, uint32_t)>(g_base + kMenuArrayGrowRva)(arr, *cap + 1);
        MenuItem* items = *reinterpret_cast<MenuItem**>(arr);
        if (!items) return false;
        MenuItem& it = items[*count];
        std::memset(&it, 0, sizeof(it));
        it.label = label;
        it.submenu = submenu;
        it.callback = callback;
        it.arg = -1;
        ++*count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Eintrag i und i-1 tauschen (Koop vor "Beenden")
void SwapLastMenuItemsGuarded(uintptr_t def) {
    __try {
        const uintptr_t arr = def + kMenuItemsOff;
        const uint32_t count = *reinterpret_cast<uint32_t*>(arr + 0xC);
        MenuItem* items = *reinterpret_cast<MenuItem**>(arr);
        if (!items || count < 2) return;
        MenuItem tmp = items[count - 1];
        items[count - 1] = items[count - 2];
        items[count - 2] = tmp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void UpdateCoopLabels() {
    const bool lobby = net::LobbyMode();
    if (!lobby) {
        std::snprintf(g_coopLabels[0], sizeof(g_coopLabels[0]), L("Feste Verbindung (smcoop_net.txt)", "Fixed connection (smcoop_net.txt)"));
        std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), "%s", net::IsRunning() ? L("Mitspieler verbunden", "Player connected") : L("kein Mitspieler", "No player connected"));
        std::snprintf(g_coopLabels[2], sizeof(g_coopLabels[2]), L("(Test-Modus)", "(test mode)"));
    }
    char peer[200] = {}, owner[64];
    bool mismatch = false;
    net::LobbyOwnerName(owner, sizeof(owner), &mismatch);
    int peers = 0;
    for (int pi = 0; pi < kMaxRemote; ++pi) {  // alle Mitspieler, durch Komma getrennt
        char name[64];
        net::PeerName(pi, name, sizeof(name));
        if (!name[0]) continue;
        const size_t len = std::strlen(peer);
        std::snprintf(peer + len, sizeof(peer) - len, "%s%s", peers ? ", " : "", name);
        ++peers;
    }
    if (lobby) switch (net::GetLobbyState()) {
        case net::LobbyState::Off:
            std::snprintf(g_coopLabels[0], sizeof(g_coopLabels[0]), L("Lobby erstellen (du wirst Host)", "Create lobby (you are the host)"));
            std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), L("Beitreten: Steam-Einladung annehmen", "Join: accept a Steam invite"));
            std::snprintf(g_coopLabels[2], sizeof(g_coopLabels[2]), L("(noch keine Lobby)", "(no lobby yet)"));
            break;
        case net::LobbyState::Creating:
            std::snprintf(g_coopLabels[0], sizeof(g_coopLabels[0]), L("Lobby wird erstellt ...", "Creating lobby ..."));
            std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), L("bitte warten", "please wait"));
            std::snprintf(g_coopLabels[2], sizeof(g_coopLabels[2]), L("Abbrechen", "Cancel"));
            break;
        case net::LobbyState::Joining:
            std::snprintf(g_coopLabels[0], sizeof(g_coopLabels[0]), L("Trete der Lobby bei ...", "Joining lobby ..."));
            std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), L("bitte warten", "please wait"));
            std::snprintf(g_coopLabels[2], sizeof(g_coopLabels[2]), L("Abbrechen", "Cancel"));
            break;
        case net::LobbyState::Owner:
            std::snprintf(g_coopLabels[0], sizeof(g_coopLabels[0]), L("Freunde einladen (Steam, bis %d Spieler)", "Invite friends (Steam, up to %d players)"), kMaxRemote + 1);
            if (peers) std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), L("Mitspieler (%d/%d): %s", "Players (%d/%d): %s"), peers, kMaxRemote, peer);
            else std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), L("Mitspieler: noch keiner (bis zu %d)", "Players: none yet (up to %d)"), kMaxRemote);
            std::snprintf(g_coopLabels[2], sizeof(g_coopLabels[2]), L("Lobby verlassen", "Leave lobby"));
            break;
        case net::LobbyState::Member:
            std::snprintf(g_coopLabels[0], sizeof(g_coopLabels[0]), L("Lobby von %s", "Lobby of %s"), owner[0] ? owner : "?");
            if (mismatch) std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), L("ACHTUNG: andere Mod-Version als der Host!", "WARNING: different mod version than the host!"));
            else if (g_coopStatus[0]) std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), "%s", g_coopStatus);
            else std::snprintf(g_coopLabels[1], sizeof(g_coopLabels[1]), L("Warte, bis der Host ein Spiel laedt ...", "Waiting for the host to load a game ..."));
            std::snprintf(g_coopLabels[2], sizeof(g_coopLabels[2]), L("Lobby verlassen", "Leave lobby"));
            break;
    }
    // Koop-Extras: Gegner-HP und geteilte HP stellt der Host ein, Namen jeder fuer sich
    const posecodec::HostSettingsInfo h = CurrentHostSettings();
    const char* byHost = IsGuest() ? L(" (stellt der Host ein)", " (set by the host)") : "";
    std::snprintf(g_coopLabels[3], sizeof(g_coopLabels[3]), L("Gegner-HP je Mitspieler: %d %%%s", "Enemy HP per guest: %d %%%s"), h.hpPercent, byHost);
    std::snprintf(g_coopLabels[4], sizeof(g_coopLabels[4]), L("Geteilte HP: %s%s", "Shared HP: %s%s"), h.sharedHp ? L("an", "on") : L("aus", "off"), byHost);
    std::snprintf(g_coopLabels[5], sizeof(g_coopLabels[5]), L("Namen anzeigen: %s", "Show names: %s"), g_localSettings.showNames ? L("an", "on") : L("aus", "off"));
}

void CoopActionClicked() {
    if (!net::LobbyMode()) return;
    const net::LobbyState s = net::GetLobbyState();
    if (s == net::LobbyState::Off) net::LobbyCreate();
    else if (s == net::LobbyState::Owner) net::LobbyInviteDialog();
    UpdateCoopLabels();
}
void OpenAskMenu();
void CoopInfoClicked() { OpenAskMenu(); }
void CoopHpClicked() {
    if (IsGuest()) return;
    g_localSettings.hpPercent = coop::NextHpPercent(g_localSettings.hpPercent);
    SaveSettings();
    UpdateCoopLabels();
}
void CoopSharedClicked() {
    if (IsGuest()) return;
    g_localSettings.sharedHp = !g_localSettings.sharedHp;
    SaveSettings();
    UpdateCoopLabels();
}
void CoopNamesClicked() {
    g_localSettings.showNames = !g_localSettings.showNames;
    SaveSettings();
    UpdateCoopLabels();
}
void CoopLeaveClicked() {
    if (net::LobbyMode()) net::LobbyLeave();
    g_coopStatus[0] = 0;
    UpdateCoopLabels();
}

void BuildCoopMenu(uintptr_t def) {
    UpdateCoopLabels();
    // wie 0x40C660: altes Feld freigeben, neu aufbauen
    __try {
        uint32_t* count = reinterpret_cast<uint32_t*>(def + kMenuItemsOff + 0xC);
        *count = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    AppendMenuItemGuarded(def, g_coopLabels[0], nullptr, CoopActionClicked);
    AppendMenuItemGuarded(def, g_coopLabels[1], nullptr, CoopInfoClicked);
    AppendMenuItemGuarded(def, g_coopLabels[2], nullptr, CoopLeaveClicked);
    AppendMenuItemGuarded(def, g_coopLabels[3], nullptr, CoopHpClicked);
    AppendMenuItemGuarded(def, g_coopLabels[4], nullptr, CoopSharedClicked);
    AppendMenuItemGuarded(def, g_coopLabels[5], nullptr, CoopNamesClicked);
}

void UpdateCoopMenu(uintptr_t) { UpdateCoopLabels(); }

// freier Speicherplatz fuer das Abenteuer des Gasts (-1 = keiner)
int FindFreeSaveSlotGuarded() {
    __try {
        const uintptr_t mgr = reinterpret_cast<uintptr_t (*)()>(g_base + kSaveMgrGetRva)();
        if (!mgr || *reinterpret_cast<uintptr_t*>(mgr) != g_base + kSaveMgrVtableRva) return -1;
        const size_t slots = *reinterpret_cast<size_t*>(mgr + 0x50);
        for (int slot = 0; slot <= 6 && static_cast<size_t>(slot) < slots; ++slot) {
            const uintptr_t info = reinterpret_cast<uintptr_t (*)(uintptr_t, int)>(g_base + kSaveSlotInfoRva)(mgr, slot);
            if (info && !*reinterpret_cast<uint8_t*>(info + 0x30)) return slot;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

// slot < 0: kein freier Platz -> ohne Rueckfrage im zuletzt gewaehlten Platz ([0xF617B8], 0x4127B0 wie nach "Ja" in der
// Ueberschreiben-Frage). Ein neues Abenteuer schreibt selbst nichts (kein Weg von 0x2EA070 zur Speicher-Funktion 0x340790);
// gespeichert wird erst ueber "Spiel speichern" mit Platzwahl.
bool StartNewGameGuarded(int slot, int skill) {
    __try {
        *reinterpret_cast<int*>(g_base + kNewGameSkillRva) = skill;
        if (slot >= 0) reinterpret_cast<void (*)(int)>(g_base + kNewGameInSlotRva)(slot);
        else reinterpret_cast<void (*)()>(g_base + kNewGameStartRva)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Beitrittsfrage fuer Gaeste ----
// Spielt der Host, bekommt ein Gast in der Lobby - in welchem Menue er auch gerade ist - einmal je Spielrunde des Hosts die
// Frage, womit er beitritt: eigenen Spielstand laden (Lade-Liste 0xCD48C0; nach dem Laden geht er einmal zum Host) oder
// neues Abenteuer (wie frueher automatisch). Die Frage ist ein eigenes Menue (Kopie der Hauptmenue-Beschreibung), das ueber
// das gerade offene gelegt wird (0x3FD8B0). "Koop-Lobby -> Host spielt ..." oeffnet sie erneut.
constexpr uintptr_t kLoadMenuDefRva = 0xCD48C0;
constexpr uintptr_t kOpenMenuRva = 0x3FD8B0;
alignas(16) uint8_t g_askMenuDef[kMenuDefSize];
bool g_askMenuReady = false;
bool g_askShown = false;  // in dieser Spielrunde des Hosts schon gefragt (Spiel-Thread)

void OpenMenuGuarded(uintptr_t def) {
    __try {
        reinterpret_cast<void (*)(uintptr_t)>(g_base + kOpenMenuRva)(def);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(L("[Lobby] Menue oeffnen: Ausnahme\n", "[Lobby] opening menu: exception\n"));
    }
}

bool HostPlaying() {
    const int host = HostSlot();
    return host >= 0 && g_peerFresh[host] && g_peerInGame[host];
}

void GuestStartNewGame() {
    const int slot = FindFreeSaveSlotGuarded();
    char value[16] = {};
    const int skill = net::LobbyGetValue("skill", value, sizeof(value)) ? std::atoi(value) : 0;
    std::snprintf(g_coopStatus, sizeof(g_coopStatus), L("Host spielt - starte ...", "Host is playing - starting ..."));
    if (slot >= 0)
        Log(L("[Lobby] Host spielt - starte neues Abenteuer (freier Speicherplatz %d, Schwierigkeit %d) und folge ihm\n", "[Lobby] host is playing - starting a new adventure (free save slot %d, difficulty %d) and following them\n"), slot + 1, skill);
    else
        Log(L("[Lobby] Host spielt - starte neues Abenteuer (kein freier Platz: beim Speichern Platz waehlen, Schwierigkeit %d)\n", "[Lobby] host is playing - starting a new adventure (no free slot: choose a slot when saving, difficulty %d)\n"), skill);
    if (!StartNewGameGuarded(slot, skill)) Log(L("[Lobby] Start fehlgeschlagen (Ausnahme)\n", "[Lobby] start failed (exception)\n"));
}

void AskInfoClicked() {}
void UpdateAskMenu(uintptr_t) {}
void AskLoadClicked() {
    Log(L("[Lobby] Gast waehlt: eigenen Spielstand laden\n", "[Lobby] guest chooses: load own adventure\n"));
    OpenMenuGuarded(g_base + kLoadMenuDefRva);
}
void AskNewClicked() {
    Log(L("[Lobby] Gast waehlt: neues Abenteuer\n", "[Lobby] guest chooses: new adventure\n"));
    GuestStartNewGame();
}

void BuildAskMenu(uintptr_t def) {
    __try {
        *reinterpret_cast<uint32_t*>(def + kMenuItemsOff + 0xC) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    AppendMenuItemGuarded(def, L("Der Host spielt - womit willst du beitreten?", "Host is playing - join with:"), nullptr, AskInfoClicked);
    AppendMenuItemGuarded(def, L("Eigenen Spielstand laden", "Load your own adventure"), nullptr, AskLoadClicked);
    AppendMenuItemGuarded(def, L("Neues Abenteuer", "New Adventure"), nullptr, AskNewClicked);
}

void OpenAskMenu() {
    if (!g_askMenuReady || !HostPlaying() || GetLevelInfo(g_base).inGame) return;
    uintptr_t current = 0;
    SafeRead(g_base + 0xD838C0, &current, 8);
    if (current == reinterpret_cast<uintptr_t>(g_askMenuDef)) return;  // ist schon offen
    OpenMenuGuarded(reinterpret_cast<uintptr_t>(g_askMenuDef));
}

// Spiel-Thread (Hauptmenue-Aktualisierung und Spieler-Update, also in jedem Menue): Gast fragen, sobald der Host spielt
void GuestJoinTick() {
    const bool lobbyGuest = net::LobbyMode() && net::GetLobbyState() == net::LobbyState::Member;
    const bool testAuto = net::FakeLobbyMember();
    if (!(lobbyGuest || testAuto || net::FakeLobbyAsk()) || !net::IsRunning() || GetLevelInfo(g_base).inGame) return;
    if (!HostPlaying()) {
        g_coopStatus[0] = 0;
        g_askShown = false;  // naechste Spielrunde des Hosts fragt wieder
        return;
    }
    char owner[64];
    bool mismatch = false;
    net::LobbyOwnerName(owner, sizeof(owner), &mismatch);
    if (mismatch) return;
    if (testAuto) {  // UDP-Test "autostart": wie frueher gleich ein neues Abenteuer
        static DWORD lastTry = 0;
        if (GetTickCount() - lastTry < 8000) return;
        lastTry = GetTickCount();
        GuestStartNewGame();
        return;
    }
    std::snprintf(g_coopStatus, sizeof(g_coopStatus), L("Host spielt - hier beitreten", "Host is playing - join here"));
    if (g_askShown) return;
    g_askShown = true;
    Log(L("[Lobby] Host spielt - frage, womit du beitrittst\n", "[Lobby] host is playing - asking how you want to join\n"));
    OpenAskMenu();
}

// Host: Schwierigkeit fuer den Gast in der Lobby ablegen (alle paar Sekunden, nur im Spiel)
void HostPublishLobbyValues() {
    if (!net::LobbyMode() || net::GetLobbyState() != net::LobbyState::Owner) return;
    static DWORD last = 0;
    if (GetTickCount() - last < 3000) return;
    last = GetTickCount();
    const LevelInfo li = GetLevelInfo(g_base);
    if (!li.inGame) return;
    char value[16];
    std::snprintf(value, sizeof(value), "%d", *reinterpret_cast<int*>(g_base + kGameStateRva + kGameStateSkillOff));
    net::LobbySetValue("skill", value);
}

// Eintrag ganz nach oben schieben (Hardcore-Hinweis)
void MoveLastMenuItemFirstGuarded(uintptr_t def) {
    __try {
        const uintptr_t arr = def + kMenuItemsOff;
        const uint32_t count = *reinterpret_cast<uint32_t*>(arr + 0xC);
        MenuItem* items = *reinterpret_cast<MenuItem**>(arr);
        if (!items || count < 2) return;
        MenuItem last = items[count - 1];
        std::memmove(items + 1, items, (count - 1) * sizeof(MenuItem));
        items[0] = last;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void HardcoreNoticeClicked() { g_hardcoreNotice[0] = 0; }

void HookedMainMenuBuild(uintptr_t def) {
    g_mainMenuBuildOrig(def);
    if (AppendMenuItemGuarded(def, L("Koop-Lobby", "Co-op Lobby"), g_coopMenuDef, nullptr)) SwapLastMenuItemsGuarded(def);
    if (g_hardcoreNotice[0] && AppendMenuItemGuarded(def, g_hardcoreNotice, nullptr, HardcoreNoticeClicked))
        MoveLastMenuItemFirstGuarded(def);
}

void HookedMainMenuUpdate(uintptr_t def) {
    if (g_mainMenuUpdateOrig) g_mainMenuUpdateOrig(def);
    DetectLanguageFromGame();
    GuestJoinTick();
}

void HookCoopMenu() {
    uintptr_t* build = reinterpret_cast<uintptr_t*>(g_base + kMainMenuDefRva + kMenuBuildOff);
    uintptr_t* update = reinterpret_cast<uintptr_t*>(g_base + kMainMenuDefRva + kMenuUpdateOff);
    if (*build != g_base + 0x40CEC0) {
        Log(L("Hook Koop-Menue: Hauptmenue unerwartet - uebersprungen\n", "Hook co-op menu: main menu unexpected - skipped\n"));
        return;
    }
    std::memcpy(g_coopMenuDef, reinterpret_cast<void*>(g_base + kMainMenuDefRva), kMenuDefSize);
    std::memset(g_coopMenuDef + kMenuItemsOff, 0, 16);  // eigenes Eintrags-Feld
    // Titel leer lassen wie im Hauptmenue (sonst liegt er ueber dem Logo)
    *reinterpret_cast<MenuFn*>(g_coopMenuDef + kMenuBuildOff) = BuildCoopMenu;
    *reinterpret_cast<MenuFn*>(g_coopMenuDef + kMenuUpdateOff) = UpdateCoopMenu;
    std::memcpy(g_askMenuDef, reinterpret_cast<void*>(g_base + kMainMenuDefRva), kMenuDefSize);
    std::memset(g_askMenuDef + kMenuItemsOff, 0, 16);
    *reinterpret_cast<MenuFn*>(g_askMenuDef + kMenuBuildOff) = BuildAskMenu;
    *reinterpret_cast<MenuFn*>(g_askMenuDef + kMenuUpdateOff) = UpdateAskMenu;
    g_askMenuReady = true;
    DWORD old = 0;
    VirtualProtect(build, 0x20, PAGE_READWRITE, &old);
    g_mainMenuBuildOrig = reinterpret_cast<MenuFn>(*build);
    g_mainMenuUpdateOrig = reinterpret_cast<MenuFn>(*update);
    *build = reinterpret_cast<uintptr_t>(HookedMainMenuBuild);
    *update = reinterpret_cast<uintptr_t>(HookedMainMenuUpdate);
    VirtualProtect(build, 0x20, old, &old);
    Log(L("Hook Koop-Menue: aktiv\n", "Hook co-op menu: active\n"));
}

// ---- Keine Spiel-Animationen auf den Mitspieler-Figuren (Kex-Assert "m_nUsed.Get() == 0", lockfreelist.h:259) ----
// Anim::Play 0x2C5E20(anim, nummer, float, flags, byte) holt einen 0x70-Byte-Eintrag aus der lock-free-Liste 0xD8B6B0
// (belegt [0xD8B6B8]); freigegeben wird er erst, wenn das Spiel die Animation dieses Objekts weiterlaufen laesst. Nach dem
// Entladen eines Levels ("OnPostLevelUnload", 0x2E8C48) prueft 0x2CA840, dass keiner mehr belegt ist. Unsere Figuren rufen
// Play beim Einrichten (Init 0x4596B0 -> Haende 0x475400) und beim Umbau des Aussehens (0x459250) auf, und Spiel-Code, der mit
// g_player = Figur laeuft (Ereignisse, Gegner), kann es auch. Bleibt eine Figur danach stehen (Mitspieler im anderen Level,
// Figur nach Levelwechsel verworfen), wird der Eintrag nie zurueckgegeben -> beim naechsten Levelwechsel die Meldung.
// Die Figuren bekommen ihre Bewegung ganz aus den Netz-Posen; Play wird fuer sie (Koerper und Haende, alles im Objekt)
// deshalb uebersprungen. Der Rueckgabewert wird von den Aufrufern nicht benutzt.
constexpr uintptr_t kAnimPlayRva = 0x2C5E20;
constexpr uintptr_t kAnimNodesUsedRva = 0xD8B6B8;
constexpr uintptr_t kPlayerObjectSize = 0x1D680;  // Create 0x458450
using AnimPlayFn = uint64_t (*)(void*, int, float, int, uint8_t);
AnimPlayFn g_animPlayOrig = nullptr;
std::atomic<uintptr_t> g_spawningAvatar{0};  // Figur zwischen Create und Init (noch nicht in g_avatar)
std::atomic<long> g_avatarAnimSkips{0};

bool IsAvatarMemory(uintptr_t a) {
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        const uintptr_t p2 = g_avatar[pi];
        if (p2 && a >= p2 && a < p2 + kPlayerObjectSize) return true;
    }
    const uintptr_t spawning = g_spawningAvatar;
    return spawning && a >= spawning && a < spawning + kPlayerObjectSize;
}

uint64_t HookedAnimPlay(void* anim, int id, float blend, int flags, uint8_t last) {
    if (IsAvatarMemory(reinterpret_cast<uintptr_t>(anim))) {
        const long n = ++g_avatarAnimSkips;
        if (n <= 10 || n % 100 == 0)
            Log(L("[Anim] Figur: Spiel-Animation %d uebersprungen (Aufrufer 0x%llX, %ld mal)\n", "[Anim] figure: game animation %d skipped (caller 0x%llX, %ld times)\n"), id,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base), n);
        return 0;
    }
    return g_animPlayOrig(anim, id, blend, flags, last);
}

void HookAnimPlay() {
    // mov rax,rsp / mov [rax+20h],r9d / mov [rax+10h],edx / push rbp / push rsi / push rdi / push r12
    static const uint8_t expected[15] = {0x48, 0x8B, 0xC4, 0x44, 0x89, 0x48, 0x20, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kAnimPlayRva), expected, sizeof(expected)) == 0)
        g_animPlayOrig = reinterpret_cast<AnimPlayFn>(InstallInlineHook(g_base + kAnimPlayRva, sizeof(expected), reinterpret_cast<void*>(HookedAnimPlay)));
    Log(L("Hook Figur-Animationen: %s\n", "Hook figure animations: %s\n"), g_animPlayOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// ---- Teleport zu Mitspielern (Pausenmenue) ----
// Pausenmenue: Beschreibung 0xCD4DC0, Aufbau +0x40 = 0x40F060 (Fortsetzen, Laden, Speichern, Optionen, Geheimnisse, Zurueck
// zum Titel). Wir haengen "Zu Mitspieler teleportieren" vor dem letzten Eintrag an; das Untermenue (Kopie der Pausenmenue-
// Beschreibung mit eigenem Aufbau) listet die Mitspieler. Klick: alle Menues zu (0x3FC840(0), wie beim Laden), dann wie das
// Folgen zum Host: noetigenfalls Levelwechsel, danach auf seine Position (eigener Teleport, keine Konsole).
constexpr uintptr_t kPauseMenuDefRva = 0xCD4DC0;
constexpr uintptr_t kCloseMenusRva = 0x3FC840;
MenuFn g_pauseMenuBuildOrig = nullptr;
alignas(16) uint8_t g_tpMenuDef[kMenuDefSize];
char g_tpLabels[kMaxRemote + 1][128];

void UpdateTeleportLabels() {
    const LevelInfo li = GetLevelInfo(g_base);
    float me[3] = {};
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (gp) SafeRead(gp + 0x20, me, sizeof(me));
    for (int pi = 0; pi < kMaxRemote; ++pi) {
        char name[64];
        net::PeerName(pi, name, sizeof(name));
        if (!name[0]) {
            std::snprintf(g_tpLabels[pi], sizeof(g_tpLabels[pi]), "-");
        } else if (!g_peerFresh[pi] || !g_peerInGame[pi]) {
            std::snprintf(g_tpLabels[pi], sizeof(g_tpLabels[pi]), L("%s (nicht im Spiel)", "%s (not in game)"), name);
        } else if (g_peerLevel[pi] != li.level) {
            std::string level = "Level " + std::to_string(g_peerLevel[pi].load());
            AcquireSRWLockShared(&g_levelNameLock);
            const auto it = g_levelNames.find(g_peerLevel[pi]);
            if (it != g_levelNames.end()) level = it->second;
            ReleaseSRWLockShared(&g_levelNameLock);
            std::snprintf(g_tpLabels[pi], sizeof(g_tpLabels[pi]), L("%s (anderes Level: %s)", "%s (other level: %s)"), name, level.c_str());
        } else {
            float pos[3] = {};
            const uintptr_t avatar = g_avatar[pi];
            const bool known = avatar && SafeRead(avatar + 0x20, pos, sizeof(pos));
            const float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
            if (known)
                std::snprintf(g_tpLabels[pi], sizeof(g_tpLabels[pi]), "%s (%d m)", name, coop::Meters(std::sqrt(dx * dx + dy * dy + dz * dz)));
            else
                std::snprintf(g_tpLabels[pi], sizeof(g_tpLabels[pi]), "%s", name);
        }
    }
    std::snprintf(g_tpLabels[kMaxRemote], sizeof(g_tpLabels[kMaxRemote]), L("Kein Mitspieler verbunden", "No player connected"));
}

void CloseMenusGuarded() {
    __try {
        reinterpret_cast<void (*)(bool)>(g_base + kCloseMenusRva)(false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(L("[Teleport] Menue schliessen: Ausnahme\n", "[Teleport] closing menu: exception\n"));
    }
}

void TeleportTo(int pi) {
    char name[64];
    net::PeerName(pi, name, sizeof(name));
    if (!name[0] || !g_peerFresh[pi] || !g_peerInGame[pi]) return;  // nicht im Spiel: nichts tun
    Log(L("[Teleport] zu %s (Level %d)\n", "[Teleport] to %s (level %d)\n"), name, g_peerLevel[pi].load());
    g_followTarget = pi;
    g_followArmed = true;
    CloseMenusGuarded();
}
void TeleportClicked0() { TeleportTo(0); }
void TeleportClicked1() { TeleportTo(1); }
void TeleportClicked2() { TeleportTo(2); }
void TeleportNoneClicked() {}
static_assert(kMaxRemote == 3, "Teleport-Rueckrufe je Platz");

void BuildTeleportMenu(uintptr_t def) {
    UpdateTeleportLabels();
    __try {
        *reinterpret_cast<uint32_t*>(def + kMenuItemsOff + 0xC) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    void (*const clicked[kMaxRemote])() = {TeleportClicked0, TeleportClicked1, TeleportClicked2};
    int added = 0;
    for (int pi = 0; pi < kMaxRemote; ++pi)
        if (net::PeerConnected(pi) && AppendMenuItemGuarded(def, g_tpLabels[pi], nullptr, clicked[pi])) ++added;
    if (!added) AppendMenuItemGuarded(def, g_tpLabels[kMaxRemote], nullptr, TeleportNoneClicked);
}

void UpdateTeleportMenu(uintptr_t) { UpdateTeleportLabels(); }

void PauseInviteClicked() { net::LobbyInviteDialog(); }

void HookedPauseMenuBuild(uintptr_t def) {
    g_pauseMenuBuildOrig(def);
    if (net::IsRunning() && AppendMenuItemGuarded(def, L("Zu Mitspieler teleportieren", "Teleport to player"), g_tpMenuDef, nullptr))
        SwapLastMenuItemsGuarded(def);  // vor "Zurueck zum Titel"
    const net::LobbyState ls = net::GetLobbyState();
    if (net::LobbyMode() && (ls == net::LobbyState::Owner || ls == net::LobbyState::Member) &&
        AppendMenuItemGuarded(def, L("Freunde einladen (Steam)", "Invite friends (Steam)"), nullptr, PauseInviteClicked))
        SwapLastMenuItemsGuarded(def);  // z.B. nach einem Absturz beim Gast neu einladen
}

void HookPauseMenu() {
    uintptr_t* build = reinterpret_cast<uintptr_t*>(g_base + kPauseMenuDefRva + kMenuBuildOff);
    if (*build != g_base + 0x40F060) {
        Log(L("Hook Pausenmenue: unerwartet - uebersprungen\n", "Hook pause menu: unexpected - skipped\n"));
        return;
    }
    std::memcpy(g_tpMenuDef, reinterpret_cast<void*>(g_base + kPauseMenuDefRva), kMenuDefSize);
    std::memset(g_tpMenuDef + kMenuItemsOff, 0, 16);  // eigenes Eintrags-Feld
    *reinterpret_cast<MenuFn*>(g_tpMenuDef + kMenuBuildOff) = BuildTeleportMenu;
    *reinterpret_cast<MenuFn*>(g_tpMenuDef + kMenuUpdateOff) = UpdateTeleportMenu;
    DWORD old = 0;
    VirtualProtect(build, 8, PAGE_READWRITE, &old);
    g_pauseMenuBuildOrig = reinterpret_cast<MenuFn>(*build);
    *build = reinterpret_cast<uintptr_t>(HookedPauseMenuBuild);
    VirtualProtect(build, 8, old, &old);
    Log(L("Hook Pausenmenue (Teleport): aktiv\n", "Hook pause menu (teleport): active\n"));
}

void RunGameThreadCommands(uintptr_t gp) {
    HostPublishLobbyValues();
    HardcoreTick();
    GuestJoinTick();
    if (net::IsRunning()) {
        FollowHostTick(gp);
        CutsceneTick(gp);
        DeathTick(gp);
        SharedHpTick(gp);
        UpdateLevelNames();
        static DWORD lastWorld = 0;
        if (GetTickCount() - lastWorld >= 100) {
            lastWorld = GetTickCount();
            WorldSyncTick(gp);
        }
        BlockFrameUpdate();
        EnemyFrameUpdate();
    }
    std::vector<std::string> cmds;
    AcquireSRWLockExclusive(&g_cmdLock);
    cmds.swap(g_cmds);
    ReleaseSRWLockExclusive(&g_cmdLock);
    for (const std::string& c : cmds) {
        unsigned serial = 0;
        float up = 0.0f;
        char arg[32] = {};
        float xyz[3] = {};
        int sec = -1;
        if (std::sscanf(c.c_str(), "face %u", &serial) == 1) {  // Spieler zum Objekt drehen (Blickwinkel +0x18 = -atan2(x, z))
            const uintptr_t node = PoolNodeBySerial(serial);
            uintptr_t obj = 0;
            float pos[3] = {}, me[3] = {};
            if (node && SafeRead(node + 0x30, &obj, 8) && obj && SafeRead(obj + 0x20, pos, sizeof(pos)) && SafeRead(gp + 0x20, me, sizeof(me))) {
                const float yaw = -std::atan2(pos[0] - me[0], pos[2] - me[2]);
                *reinterpret_cast<float*>(gp + 0x18) = yaw;
                Log(L("[Befehl] %s: Blickwinkel %.3f\n", "[Command] %s: view angle %.3f\n"), c.c_str(), yaw);
            } else {
                Log(L("[Befehl] %s: kein Objekt\n", "[Command] %s: no object\n"), c.c_str());
            }
            continue;
        }
        float hp = 0.0f;
        if (std::sscanf(c.c_str(), "sethealth %u %f", &serial, &hp) == 2) {  // Test: Lebenspunkte eines Gegners direkt setzen
            const uintptr_t node = PoolNodeBySerial(serial);
            uintptr_t obj = 0;
            if (node) SafeRead(node + 0x30, &obj, 8);
            bool ok = false;
            if (obj) {
                DWORD old = 0;
                ok = SafeWrite(obj + kAiHealthOff, &hp, 4);
                (void)old;
            }
            Log(L("[Befehl] %s %s\n", "[Command] %s %s\n"), c.c_str(), ok ? L("ausgefuehrt", "done") : L("- Objekt nicht gefunden", "- object not found"));
            continue;
        }
        if (c == "sterben") {  // Test: eigener Tod wie ein echter (ueber den Hook, wird weitergemeldet)
            if (!DieLikeRealGuarded(gp)) Log(L("[Befehl] sterben: Ausnahme\n", "[Command] sterben: exception\n"));
            continue;
        }
        char evClass[48] = {};
        if (std::sscanf(c.c_str(), "ereignisse %47s", evClass) == 1) {  // Test: Ereignisse, deren Objekt-Klasse den Text enthaelt
            for (const auto& kv : g_eventRegistry) {
                int st = 0, key[3] = {};
                uint32_t flags = 0;
                uintptr_t obj = 0;
                if (!ReadEventInfoGuarded(kv.second, &st, key, &flags, &obj) || !obj) continue;
                const std::string cls = ClassNameOf(obj);
                if (cls.find(evClass) == std::string::npos) continue;
                float pos[3] = {};
                SafeRead(obj + 0x20, pos, sizeof(pos));
                Log(L("  %d %d %d: Zustand %d, %s bei %.0f/%.0f/%.0f\n", "  %d %d %d: state %d, %s at %.0f/%.0f/%.0f\n"), key[0], key[1], key[2], st, cls.c_str(), pos[0], pos[1], pos[2]);
            }
            continue;
        }
        int evKey[3] = {};
        if (std::sscanf(c.c_str(), "ausloesen %d %d %d", &evKey[0], &evKey[1], &evKey[2]) == 3) {  // Test: wie Ausloese-Bereich
            EventKey key;
            std::memcpy(key.k, evKey, sizeof(key.k));
            const auto it = g_eventRegistry.find(key);
            if (it == g_eventRegistry.end()) { Log(L("[Befehl] ausloesen: Ereignis unbekannt\n", "[Command] ausloesen: unknown event\n")); continue; }
            const bool ok = StartEventGuarded(it->second);
            posecodec::EventInfo e;
            std::memcpy(e.key, evKey, sizeof(e.key));
            e.seq = ++g_eventSeq;
            AcquireSRWLockExclusive(&g_worldLock);
            g_localTriggered.push_back(e);
            ReleaseSRWLockExclusive(&g_worldLock);
            Log(L("[Befehl] ausloesen %d/%d/%d: %s\n", "[Command] ausloesen %d/%d/%d: %s\n"), evKey[0], evKey[1], evKey[2], ok ? L("gestartet und an Mitspieler", "started and sent to other players") : L("Ausnahme", "exception"));
            continue;
        }
        if (c == "calabash") {  // Test: Calabash aus der linken Hand legen (wie der Animations-Rueckruf beim Werfen)
            Log(L("[Befehl] calabash %s\n", "[Command] calabash %s\n"), PlaceCalabashGuarded() ? L("ausgefuehrt", "done") : L("- Ausnahme", "- exception"));
            continue;
        }
        if (c == "boss" || c == "bosstest") {  // Test: Bosskampf? / Bosskampf vortaeuschen (an/aus)
            if (c == "bosstest") g_bossTestOverride = !g_bossTestOverride;
            std::string which;
            const bool fight = InBossFight(gp, &which);
            Log(L("[Befehl] %s: Bosskampf %s (%s)\n", "[Command] %s: boss fight %s (%s)\n"), c.c_str(), fight ? L("ja", "yes") : L("nein", "no"), which.c_str());
            continue;
        }
        unsigned hcId = 0;
        if (std::sscanf(c.c_str(), "hardcore %u", &hcId) == 1) {  // Test: Hardcore-Lauf mit Kennung setzen (0 = aus)
            SetHardcoreRun(hcId != 0, hcId, "Befehl");
            continue;
        }
        int lifeValue = 0;
        if (std::sscanf(c.c_str(), "leben %d", &lifeValue) == 1) {  // Test: eigenes Leben setzen (wie ein Treffer/Heilung)
            Log(L("[Befehl] leben %d (vorher %d, Maximum %d)\n", "[Command] leben %d (before %d, maximum %d)\n"), lifeValue, ReadLocalLife(),
                *reinterpret_cast<int*>(g_base + kPlayerMaxLifeRva));
            SetLocalLifeGuarded(lifeValue);
            continue;
        }
        if (c == "geteilt") {  // Test: geteilte HP an/aus (nur Host)
            g_localSettings.sharedHp = !g_localSettings.sharedHp;
            SaveSettings();
            Log(L("[Befehl] geteilte HP %s\n", "[Command] shared HP %s\n"), g_localSettings.sharedHp ? L("an", "on") : L("aus", "off"));
            continue;
        }
        int hpv = 0;
        if (std::sscanf(c.c_str(), "gegnerhp %d", &hpv) == 1) {  // Test: Regler Gegner-HP (nur Host)
            g_localSettings.hpPercent = coop::ClampHpPercent(hpv);
            SaveSettings();
            Log(L("[Befehl] Gegner-HP %d %%\n", "[Command] enemy HP %d %%\n"), g_localSettings.hpPercent);
            continue;
        }
        int tpTo = 0;
        if (std::sscanf(c.c_str(), "teleport %d", &tpTo) == 1) {  // Test: wie der Menue-Eintrag (Platz 1..3)
            if (tpTo >= 1 && tpTo <= kMaxRemote) TeleportTo(tpTo - 1);
            continue;
        }
        int levelTo = 0;
        if (std::sscanf(c.c_str(), "levelwechsel %d", &levelTo) == 1) {  // Test: in ein anderes Level wechseln
            *reinterpret_cast<int*>(g_base + kGameStateRva + kGameStateNextLevelOff) = levelTo;
            Log(L("[Befehl] wechsle in Level %d\n", "[Command] switching to level %d\n"), levelTo);
            continue;
        }
        if (c == "einstellungen") {  // Test: Einstellungen und Spielernummern
            LogSettings();
            continue;
        }
        if (c == "figuren") {  // Test: Figuren der Mitspieler (Platz, Name, Position, sichtbar?)
            float me[3] = {};
            SafeRead(gp + 0x20, me, sizeof(me));
            Log(L("[Befehl] figuren: ich bei %.0f/%.0f/%.0f\n", "[Command] figuren: me at %.0f/%.0f/%.0f\n"), me[0], me[1], me[2]);
            for (int pi = 0; pi < kMaxRemote; ++pi) {
                char name[64];
                net::PeerName(pi, name, sizeof(name));
                const uintptr_t a = g_avatar[pi];
                float pos[3] = {};
                if (a) SafeRead(a + 0x20, pos, sizeof(pos));
                Log(L("  Platz %d (%s): Figur 0x%llX bei %.0f/%.0f/%.0f, aktiv %d, Modell passt %d\n", "  Slot %d (%s): figure 0x%llX at %.0f/%.0f/%.0f, active %d, model matches %d\n"), pi + 1, name[0] ? name : "-",
                    (unsigned long long)a, pos[0], pos[1], pos[2], g_remoteActive[pi] ? 1 : 0, g_remoteModelMismatch[pi] ? 0 : 1);
            }
            continue;
        }
        if (c == "mitfahren") {  // Test: Mitfahr-Objekte mit Zustand, Signal, Position und "eigener Spieler drin"
            float me[3] = {};
            SafeRead(gp + 0x20, me, sizeof(me));
            ForEachPoolNode([&](uintptr_t, uintptr_t obj, uint32_t serial) {
                uintptr_t vt = 0;
                if (!SafeRead(obj, &vt, 8)) return;
                for (const RideClass& rc : kRideClasses) {
                    if (vt != g_base + rc.vtRva) continue;
                    int state = 0, signal = 0;
                    float pos[3] = {}, box[6] = {};
                    SafeRead(obj + 0x290, &state, 4);
                    SafeRead(obj + 0x120, &signal, 4);
                    SafeRead(obj + 0x20, pos, sizeof(pos));
                    SafeRead(obj + 0xD8, box, sizeof(box));
                    Log(L("  %s #%u bei %.0f/%.0f/%.0f Zustand %d Signal %d, ich drin %s\n", "  %s #%u at %.0f/%.0f/%.0f state %d signal %d, me inside %s\n"), rc.name, serial, pos[0], pos[1], pos[2],
                        state, signal, coop::RideContains(box, box + 3, me, kRideSide, kRideAbove) ? L("ja", "yes") : L("nein", "no"));
                }
            });
            continue;
        }
        if (c == "tueren") {  // Test: die 8 naechsten Tueren mit Seriennummer, Klasse und Entfernung
            float me[3] = {};
            SafeRead(gp + 0x20, me, sizeof(me));
            std::vector<std::pair<float, std::string>> doors;
            ForEachPoolNode([&](uintptr_t, uintptr_t obj, uint32_t serial) {
                const std::string cls = ClassNameOf(obj);
                if (cls.find("Door") == std::string::npos) return;
                float pos[3] = {};
                if (!SafeRead(obj + 0x20, pos, sizeof(pos))) return;
                const float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
                int sector = -1;
                SafeRead(obj + 0xD4, &sector, 4);
                char line[160];
                std::snprintf(line, sizeof(line), L("#%u %s bei %.0f/%.0f/%.0f (Sektor %d)", "#%u %s at %.0f/%.0f/%.0f (sector %d)"), serial, cls.c_str(), pos[0], pos[1], pos[2], sector);
                doors.push_back({std::sqrt(dx * dx + dy * dy + dz * dz), line});
            });
            std::sort(doors.begin(), doors.end());
            Log(L("[Befehl] tueren: %zu\n", "[Command] tueren: %zu\n"), doors.size());
            for (size_t i = 0; i < doors.size() && i < 8; ++i) Log(L("  %.0f entfernt: %s\n", "  %.0f away: %s\n"), doors[i].first, doors[i].second.c_str());
            continue;
        }
        if (c == "gegner") {  // Test: die 12 naechsten Gegner mit Seriennummer, Art und Lebenspunkten
            float me[3] = {};
            SafeRead(gp + 0x20, me, sizeof(me));
            std::vector<std::pair<float, uint32_t>> byDist;
            for (const auto& kv : g_trackedEnemies) {
                const float dx = kv.second.pos[0] - me[0], dy = kv.second.pos[1] - me[1], dz = kv.second.pos[2] - me[2];
                byDist.push_back({std::sqrt(dx * dx + dy * dy + dz * dz), kv.first});
            }
            std::sort(byDist.begin(), byDist.end());
            Log(L("[Befehl] gegner: %zu verfolgt\n", "[Command] gegner: %zu tracked\n"), byDist.size());
            for (size_t i = 0; i < byDist.size() && i < 12; ++i) {
                const TrackedEnemy& e = g_trackedEnemies[byDist[i].second];
                Log(L("  #%u %s Gruppe %d Kennung %08X, %.0f entfernt, Leben %.1f, %s\n", "  #%u %s group %d id %08X, %.0f away, health %.1f, %s\n"), byDist[i].second, ClassNameOf(e.obj).c_str(),
                    e.type, e.id, byDist[i].first, e.lastHealth, e.alive ? L("lebt", "alive") : L("tot", "dead"));
            }
            continue;
        }
        if (std::sscanf(c.c_str(), "kill %u", &serial) == 1) {  // Test: Gegner toeten (toedlicher gesammelter Schaden)
            const uintptr_t node = PoolNodeBySerial(serial);
            uintptr_t obj = 0;
            if (node) SafeRead(node + 0x30, &obj, 8);
            Log(L("[Befehl] %s %s\n", "[Command] %s %s\n"), c.c_str(), obj && KillEnemyGuarded(obj) ? L("ausgefuehrt", "done") : L("- kein lebender Gegner", "- no living enemy"));
            continue;
        }
        if (std::sscanf(c.c_str(), "zerbrechen %u", &serial) == 1) {  // Test: Topf/Fass zerbrechen wie ein Treffer
            const uintptr_t node = PoolNodeBySerial(serial);
            uintptr_t obj = 0, vt = 0;
            if (node) SafeRead(node + 0x30, &obj, 8);
            if (obj) SafeRead(obj, &vt, 8);
            const bool breakable = vt == g_base + kExplodeObjectVtableRva || vt == g_base + kCalabashObjectVtableRva;
            Log(L("[Befehl] %s %s\n", "[Command] %s %s\n"), c.c_str(), breakable && ExplodePotGuarded(obj) ? L("ausgefuehrt", "done") : L("- kein Topf/keine Platte", "- no pot/plate"));
            continue;
        }
        int value = 0;
        if (std::sscanf(c.c_str(), "tabset %u %d", &serial, &value) == 2) {  // Test: nur die Tabelle, ohne Live-Sync
            const uintptr_t node = PoolNodeBySerial(serial);
            uintptr_t obj = 0;
            int type = 0;
            float pos[3] = {};
            if (node) SafeRead(node + 0x30, &obj, 8);
            const bool ok = obj && SafeRead(obj + 0xD0, &type, 4) && SafeRead(obj + 0x38, pos, 12) && SetStateGuarded(type, pos, value);
            Log(L("[Befehl] %s %s\n", "[Command] %s %s\n"), c.c_str(), ok ? L("ausgefuehrt", "done") : L("- fehlgeschlagen", "- failed"));
            continue;
        }
        if (std::sscanf(c.c_str(), "setstate %u %d", &serial, &value) == 2) {  // Test: Objekt-Zustand ueber die Spiel-Funktion setzen
            const uintptr_t node = PoolNodeBySerial(serial);
            uintptr_t obj = 0;
            if (node) SafeRead(node + 0x30, &obj, 8);
            if (obj) reinterpret_cast<SetObjStateFn>(g_base + kSetObjStateRva)(SaveMgr(), obj, value);
            Log(L("[Befehl] %s %s\n", "[Command] %s %s\n"), c.c_str(), obj ? L("ausgefuehrt", "done") : L("- Objekt nicht gefunden", "- object not found"));
            continue;
        }
        if (std::sscanf(c.c_str(), "tpxyz %f %f %f %d", &xyz[0], &xyz[1], &xyz[2], &sec) == 4) {
            TeleportPlayerGuarded(gp, xyz, sec);
            Log(L("[Befehl] %s ausgefuehrt\n", "[Command] %s done\n"), c.c_str());
            continue;
        }
        const int n = std::sscanf(c.c_str(), "tp %u %f", &serial, &up);
        if (n >= 1) {
            const uintptr_t node = PoolNodeBySerial(serial);
            uintptr_t obj = 0;
            float pos[3] = {};
            int sector = -1;
            if (!node || !SafeRead(node + 0x30, &obj, 8) || !obj || !SafeRead(obj + 0x20, pos, sizeof(pos))) {
                Log(L("[Befehl] %s: kein Objekt\n", "[Command] %s: no object\n"), c.c_str());
                continue;
            }
            SafeRead(obj + 0xD4, &sector, 4);
            pos[1] += up;
            TeleportPlayerGuarded(gp, pos, sector);
            Log(L("[Befehl] %s: zu %s bei %.0f/%.0f/%.0f (Sektor %d)\n", "[Command] %s: to %s at %.0f/%.0f/%.0f (sector %d)\n"), c.c_str(), ClassNameOf(obj).c_str(), pos[0], pos[1], pos[2], sector);
        } else if (std::sscanf(c.c_str(), "watch %31s", arg) == 1) {
            g_worldWatch = std::strcmp(arg, "on") == 0;
            g_watched.clear();
            Log(L("[Befehl] Welt-Beobachter %s\n", "[Command] world watcher %s\n"), g_worldWatch ? L("an", "on") : L("aus", "off"));
        } else {
            Log(L("[Befehl] unbekannt: %s\n", "[Command] unknown: %s\n"), c.c_str());
        }
    }
    static DWORD lastWatch = 0;
    if (g_worldWatch && GetTickCount() - lastWatch >= 250) {
        lastWatch = GetTickCount();
        WorldWatchTick(gp);
    }
}

uint64_t HookedPlayerSave(void* self, void* a, void* b, void* c) {
    LogStreamCall(L("Speichern", "save"), self);
    if (IsOurPlayer2(reinterpret_cast<uintptr_t>(self))) {
        // Absicherung: niemals Spieler 2 in den Spielstand schreiben, sondern den echten Spieler
        void* real = *reinterpret_cast<void**>(g_base + kGPlayerRva);
        Log(L("[Spielstand] WARNUNG: Speichern fuer Spieler 2 abgefangen - speichere stattdessen den echten Spieler\n", "[Savegame] WARNING: saving for player 2 intercepted - saving the real player instead\n"));
        if (real) return g_playerSaveOrig(real, a, b, c);
    }
    return g_playerSaveOrig(self, a, b, c);
}

uint64_t HookedPlayerLoad(void* self, void* a, void* b, void* c) {
    LogStreamCall(L("Laden", "load"), self);
    HardcoreOnPlayerLoad();
    if (!net::IsHost() && !g_followArmed.exchange(true)) Log(L("[Folgen] Spielstand geladen - gehe einmal zum Host\n", "[Follow] adventure loaded - going to the host once\n"));
    g_tableResync = true;
    g_progressBaselineReset = true;
    return g_playerLoadOrig(self, a, b, c);
}

void HookPlayerSaveLoad() {
    auto* vt = reinterpret_cast<uintptr_t*>(g_base + kPlayerVtableRva);
    DWORD old;
    VirtualProtect(vt, sizeof(uintptr_t) * 4, PAGE_READWRITE, &old);
    g_playerSaveOrig = reinterpret_cast<PlayerStreamFn>(vt[2]);
    g_playerLoadOrig = reinterpret_cast<PlayerStreamFn>(vt[3]);
    vt[2] = reinterpret_cast<uintptr_t>(&HookedPlayerSave);
    vt[3] = reinterpret_cast<uintptr_t>(&HookedPlayerLoad);
    VirtualProtect(vt, sizeof(uintptr_t) * 4, old, &old);
    Log(L("Hook Spieler Speichern/Laden: aktiv\n", "Hook player save/load: active\n"));
}

// --- Analyse Waffen/Haende: Hand-Animationskomponenten des Spielers (+0x1D1D0 rechts?, +0x1D410 links?) ---
struct HandDrawInfo {
    std::atomic<uintptr_t> obj{0}, model{0}, ret{0};
    std::atomic<LONG> count{0};
};
HandDrawInfo g_handDraw[2];

void NoteHandDraw(void* obj, void* model, void* anim) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (!gp) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(anim);
    const int h = a == gp + 0x1D1D0 ? 0 : (a == gp + 0x1D410 ? 1 : -1);
    if (h < 0) return;
    g_handDraw[h].obj = reinterpret_cast<uintptr_t>(obj);
    g_handDraw[h].model = reinterpret_cast<uintptr_t>(model);
    g_handDraw[h].ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
    ++g_handDraw[h].count;
}

// --- Analyse Waffen: AddMesh (0x4967B0) zeichnet starre Modelle (u.a. Waffen an Handgelenken) ---
struct MeshDrawNote { std::atomic<uintptr_t> obj{0}, ret{0}; std::atomic<LONG> count{0}; };
constexpr int kMaxMeshNotes = 32;
MeshDrawNote g_meshNotes[kMaxMeshNotes];

// Aufrufer, die Waffen-Objekte (kexShadowManQuestObject) zeichnen
struct CallerNote { std::atomic<uintptr_t> ret{0}; std::atomic<LONG> count{0}; };
CallerNote g_questCallers[16];

uint64_t HookedAddMesh(void* list, void* obj, void* c, void* d) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
    if (obj && reinterpret_cast<uintptr_t>(obj) == g_curHeldObj) g_curHeldDrawn = true;
    if (obj && *reinterpret_cast<uintptr_t*>(obj) == g_base + kQuestObjectVtableRva) {
        NoteItemTemplate(reinterpret_cast<uintptr_t>(obj), false);
        for (auto& n : g_questCallers) {
            uintptr_t cur = n.ret;
            if (cur == ret) { ++n.count; break; }
            if (cur == 0) { uintptr_t e = 0; if (n.ret.compare_exchange_strong(e, ret)) { ++n.count; break; } }
        }
    }
    // Alles notieren, was nahe am Spieler gezeichnet wird (Waffen, Handeffekte, ...)
    bool nearPlayer = false;
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (gp && obj) {
        const float* pp = reinterpret_cast<const float*>(gp + 0x20);
        const float* op = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(obj) + 0x230);
        const float dx = op[0] - pp[0], dy = op[1] - pp[1], dz = op[2] - pp[2];
        nearPlayer = dx * dx + dy * dy + dz * dz < 150.0f * 150.0f;
    }
    if (nearPlayer) {
        const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
        for (auto& n : g_meshNotes) {
            uintptr_t cur = n.obj;
            if (cur == o) { ++n.count; n.ret = ret; break; }
            if (cur == 0) { uintptr_t expected = 0; if (n.obj.compare_exchange_strong(expected, o)) { n.ret = ret; ++n.count; break; } }
        }
    }
    return g_addMeshOrig(list, obj, c, d);
}

void HookAddMesh() {
    static const uint8_t expected[15] = {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0x98};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kAddMeshRva), expected, sizeof(expected)) != 0) {
        Log(L("Hook AddMesh: unerwartete Bytes - uebersprungen\n", "Hook AddMesh: unexpected bytes - skipped\n"));
        return;
    }
    g_addMeshOrig = reinterpret_cast<AddMeshFn>(InstallInlineHook(g_base + kAddMeshRva, sizeof(expected), reinterpret_cast<void*>(HookedAddMesh)));
    Log(L("Hook AddMesh: %s\n", "Hook AddMesh: %s\n"), g_addMeshOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

// Analyse Waffen: animierte Modelle (AddSkin) in Spielernaehe, die nicht der Spieler selbst sind
struct SkinNote { std::atomic<uintptr_t> obj{0}, model{0}, anim{0}, ret{0}; std::atomic<LONG> count{0}; };
SkinNote g_skinNotes[16];

void NoteSkinNearPlayer(void* obj, void* model, void* anim) {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    if (!gp || !o || o == gp || IsOurPlayer2(o) || !anim) return;
    const float* pp = reinterpret_cast<const float*>(gp + 0x20);
    const float* ap = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(anim) + 0x1A0);
    const float dx = ap[0] - pp[0], dy = ap[1] - pp[1], dz = ap[2] - pp[2];
    if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f) return;
    for (auto& n : g_skinNotes) {
        uintptr_t cur = n.obj;
        if (cur == o) { ++n.count; return; }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (n.obj.compare_exchange_strong(expected, o)) {
                n.model = reinterpret_cast<uintptr_t>(model);
                n.anim = reinterpret_cast<uintptr_t>(anim);
                n.ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
                ++n.count;
                return;
            }
        }
    }
}

void HookedAddSkin(void* list, void* obj, void* model, void* anim) {
    NoteHandDraw(obj, model, anim);
    NoteSkinNearPlayer(obj, model, anim);
    // Zweiten Spieler zeichnen bzw. auf Wunsch erzeugen (nur beim Hauptansicht-Aufruf fuer den echten Spieler)
    {
        const uintptr_t o0 = reinterpret_cast<uintptr_t>(obj);
        const uintptr_t ret0 = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
        const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
        if (o0 && o0 == gp) {
            // g_spawnRequest: 1 + Platz = angefordert, -1 = wird gerade erzeugt (sonst fordert der Netz-Thread in der
            // Zwischenzeit, solange die Figur noch 0 ist, ein zweites Mal an -> zwei Figuren pro Levelwechsel)
            const int requested = g_spawnRequest;
            int expected = requested;
            if (ret0 == 0x4A0985 && requested >= 1 && requested <= kMaxRemote && g_spawnRequest.compare_exchange_strong(expected, -1)) {
                const int pi = requested - 1;
                uintptr_t p2 = 0;
                const bool ok = SpawnPlayer2Guarded(o0, &p2);
                if (ok) {
                    g_avatarOwner[pi] = o0;
                    g_avatar[pi] = p2;
                    const int idx = g_numPlayer2.fetch_add(1);
                    if (idx < kMaxPlayer2) g_allPlayer2[idx] = p2;
                    const int fixedEntries = RestorePoolPlayerEntries(o0);
                    Log(L("\n[Spieler2] Figur fuer Mitspieler %d erzeugt: 0x%llX (%d Pool-Eintrag/Eintraege zurueck auf den echten Spieler)\n", "\n[Player2] figure for player %d created: 0x%llX (%d pool entry/entries back to the real player)\n"),
                        pi + 1, (unsigned long long)p2, fixedEntries);
                } else {
                    Log(L("\n[Spieler2] Erzeugen FEHLGESCHLAGEN (Ausnahme)\n", "\n[Player2] creation FAILED (exception)\n"));
                }
                g_spawnRequest = 0;
            }
            bool any = false;
            for (int pi = 0; pi < kMaxRemote; ++pi) {
                if (g_avatar[pi] && g_avatarOwner[pi] != o0) {
                    // Spielstand/Level wurde neu geladen: die Figur haengt an alten Ressourcen -> nicht mehr benutzen.
                    // (Speicher wird bewusst nicht freigegeben, solange wir die Aufraeum-Funktion nicht kennen.)
                    Log(L("\n[Spieler2] Spieler wurde neu erstellt (Laden) - Figur %d abgeschaltet, wird neu erzeugt\n", "\n[Player2] player was recreated (loading) - figure %d disabled, will be recreated\n"), pi + 1);
                    g_avatar[pi] = 0;
                }
                if (g_avatar[pi]) any = true;
            }
            if (any) {
                // Position, Animation und Knochen der Figuren werden in HookedPlayerBones aktualisiert.
                const uintptr_t first = g_avatar[0];
                if (first && ret0 == 0x4A0985 && g_traceRequest.exchange(0)) {  // Analyse: Weg durch UpdateBones (0x2C7180)
                    auto updBones = reinterpret_cast<void (*)(void*)>(g_base + 0x2C7180);
                    TracedCall(updBones, reinterpret_cast<void*>(o0 + 0x368), g_base + 0x2C7180, g_base + 0x2C7E00);
                    LogTrace("Spieler1");
                    TracedCall(updBones, reinterpret_cast<void*>(first + 0x368), g_base + 0x2C7180, g_base + 0x2C7E00);
                    LogTrace("Spieler2");
                }
                g_addSkinOrig(list, obj, model, anim);
                for (int pi = 0; pi < kMaxRemote; ++pi) {
                    const uintptr_t p2 = g_avatar[pi];
                    if (!p2) continue;
                    // im Netzwerk-Modus nur, solange Daten kommen und das Modell passt
                    const bool visible = net::IsRunning() ? (g_remoteActive[pi] && !g_remoteModelMismatch[pi]) : g_localTestFigure.load();
                    const bool bodyVisible = !net::IsRunning() || !(g_remoteLooks[pi] & kLookInvisibleBit);  // Mitspieler "Unsichtbar"
                    if (visible && bodyVisible && !DrawPlayer2Guarded(list, p2)) {
                        Log(L("[Spieler2] Zeichnen fehlgeschlagen - Figur %d abgeschaltet\n", "[Player2] drawing failed - figure %d disabled\n"), pi + 1);
                        g_avatar[pi] = 0;
                    } else if (visible && ret0 == 0x4A0985 && net::IsRunning()) {
                        DrawPlayer2Items(pi, p2);  // Waffen in seinen Haenden
                    }
                }
                InterlockedIncrement64(&g_skinCallsPlayer);
                return;
            }
        }
    }
    // Spieler am vtable-Zeiger erkennen (sicher, auch wenn der Render-Thread aufruft)
    const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    if (!o || *reinterpret_cast<uintptr_t*>(o) != g_base + kPlayerVtableRva) {
        InterlockedIncrement64(&g_skinCallsOther);
        g_addSkinOrig(list, obj, model, anim);
        return;
    }
    InterlockedIncrement64(&g_skinCallsPlayer);
    {   // Aufrufer merken (Ruecksprungadresse als RVA), fuer die Analyse
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
        for (int i = 0; i < kMaxCallers; ++i) {
            uintptr_t cur = g_callerRva[i];
            if (cur == ret) { InterlockedIncrement64(&g_callerCount[i]); break; }
            if (cur == 0 && InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&g_callerRva[i]), ret, 0) == 0) {
                InterlockedIncrement64(&g_callerCount[i]);
                break;
            }
        }
    }
    g_lastPlayerAnim = reinterpret_cast<uintptr_t>(anim);
    g_lastPlayerModel = reinterpret_cast<uintptr_t>(model);
    const GhostConfig& c = g_ghostCfg[g_ghostCfgIdx.load()];
    if (!g_ghostOn) { g_addSkinOrig(list, obj, model, anim); return; }
    if (c.mode == 3) return;  // Test: Spieler gar nicht eintragen
    if (c.mode == 6) {        // Geist ueber eine eigene, dauerhafte Kopie der Animationskomponente
        g_addSkinOrig(list, obj, model, anim);
        const uintptr_t a6 = reinterpret_cast<uintptr_t>(anim);
        const float* srcBones = *reinterpret_cast<float**>(a6 + 0x180);
        const size_t nb = *reinterpret_cast<size_t*>(a6 + 0x188);
        if (!srcBones || nb == 0 || nb > 128) return;
        constexpr size_t kAnimCopy = 0x1000;
        alignas(16) static uint8_t fakeAnim[kAnimCopy];
        alignas(16) static float fakeBones[128 * 16];
        std::memcpy(fakeAnim, anim, kAnimCopy);
        std::memcpy(fakeBones, srcBones, nb * 16 * sizeof(float));
        for (size_t b = 0; b < nb; ++b)
            for (int i = 0; i < 3; ++i) { fakeBones[b * 16 + 4 + i] += c.offset[i]; fakeBones[b * 16 + 12 + i] += c.offset[i]; }
        *reinterpret_cast<float**>(fakeAnim + 0x180) = fakeBones;
        g_addSkinOrig(list, obj, model, fakeAnim);
        return;
    }
    if (c.mode == 5) {        // Test: alle Knochen-Positionen waehrend des Aufrufs auf 0 setzen
        const uintptr_t a5 = reinterpret_cast<uintptr_t>(anim);
        float* bn = *reinterpret_cast<float**>(a5 + 0x180);
        const size_t nb = *reinterpret_cast<size_t*>(a5 + 0x188);
        if (!bn || nb == 0 || nb > 256) { g_addSkinOrig(list, obj, model, anim); return; }
        std::vector<float> keep(bn, bn + nb * 16);
        for (size_t b = 0; b < nb; ++b)
            for (int i = 0; i < 3; ++i) { bn[b * 16 + 4 + i] = 0.0f; bn[b * 16 + 12 + i] = 0.0f; }
        g_addSkinOrig(list, obj, model, anim);
        std::memcpy(bn, keep.data(), keep.size() * sizeof(float));
        // Angehaengten Listeneintrag pruefen: [list]=Daten, [list+0xC]=Anzahl, Eintrag 0xB0 Bytes, Knochenzeiger bei +0x80
        static DWORD lastDbg5 = 0;
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base == 0x4A0985 && GetTickCount() - lastDbg5 > 3000) {
            lastDbg5 = GetTickCount();
            const uintptr_t l = reinterpret_cast<uintptr_t>(list);
            uintptr_t data = 0;
            uint32_t cnt = 0;
            SafeRead(l, &data, 8);
            SafeRead(l + 0xC, &cnt, 4);
            uintptr_t bptr = 0, bnum = 0;
            float bone0[8] = {};
            if (data && cnt) {
                const uintptr_t e = data + (cnt - 1) * 0xB0;
                SafeRead(e + 0x80, &bptr, 8);
                SafeRead(e + 0x88, &bnum, 8);
                if (bptr) SafeRead(bptr, bone0, sizeof(bone0));
            }
            Log(L("[Liste] ret=0x%llX list=0x%llX Eintraege=%u Knochenzeiger=0x%llX Anzahl=%llu Knochen0=[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f]\n", "[List] ret=0x%llX list=0x%llX entries=%u bone pointer=0x%llX count=%llu bone0=[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f]\n"),
                (unsigned long long)(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base), (unsigned long long)l, cnt,
                (unsigned long long)bptr, (unsigned long long)(bnum & 0xFFFFFFFF), bone0[0], bone0[1], bone0[2], bone0[3],
                bone0[4], bone0[5], bone0[6]);
        }
        return;
    }
    if (c.mode == 0) g_addSkinOrig(list, obj, model, anim);
    if (c.mode == 4) {  // nur Knochen verschieben, nur in der Hauptansicht, ohne Zuruecksetzen
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base != 0x4A0985) { g_addSkinOrig(list, obj, model, anim); return; }
    }
    float save[32][3];
    for (int k = 0; k < c.numPos; ++k) {
        float* p = reinterpret_cast<float*>(o + c.pos[k]);
        std::memcpy(save[k], p, sizeof(save[k]));
        for (int i = 0; i < 3; ++i) p[i] += c.offset[i];
    }
    // Knochen liegen in Weltkoordinaten: anim+0x180 = Array, anim+0x188 = Anzahl.
    // Jeder Eintrag (0x40 Bytes): Quaternion, Position (+0x10), Quaternion (Vorframe), Position (+0x30).
    const uintptr_t a = reinterpret_cast<uintptr_t>(anim);
    float* bones = *reinterpret_cast<float**>(a + 0x180);
    const size_t numBones = *reinterpret_cast<size_t*>(a + 0x188);
    const bool shiftBones = bones && numBones > 0 && numBones <= 256;
    float dbgBefore[3] = {}, dbgShifted[3] = {}, dbgAfter[3] = {};
    if (shiftBones) std::memcpy(dbgBefore, bones + 4, sizeof(dbgBefore));
    if (shiftBones)
        for (size_t b = 0; b < numBones; ++b)
            for (int i = 0; i < 3; ++i) { bones[b * 16 + 4 + i] += c.offset[i]; bones[b * 16 + 12 + i] += c.offset[i]; }
    if (shiftBones) std::memcpy(dbgShifted, bones + 4, sizeof(dbgShifted));
    g_addSkinOrig(list, obj, model, anim);
    if (shiftBones) std::memcpy(dbgAfter, bones + 4, sizeof(dbgAfter));
    static DWORD lastDbg = 0;
    if (shiftBones && GetTickCount() - lastDbg > 3000) {
        lastDbg = GetTickCount();
        Log(L("[Geist-Debug] ret=0x%llX Knochen0: vorher %.1f/%.1f/%.1f  verschoben %.1f/%.1f/%.1f  nach Aufruf %.1f/%.1f/%.1f\n", "[Ghost debug] ret=0x%llX bone0: before %.1f/%.1f/%.1f  moved %.1f/%.1f/%.1f  after call %.1f/%.1f/%.1f\n"),
            (unsigned long long)(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base),
            dbgBefore[0], dbgBefore[1], dbgBefore[2], dbgShifted[0], dbgShifted[1], dbgShifted[2], dbgAfter[0], dbgAfter[1], dbgAfter[2]);
    }
    if (c.mode == 4) return;  // Test: Verschiebung nicht zuruecknehmen
    if (shiftBones)
        for (size_t b = 0; b < numBones; ++b)
            for (int i = 0; i < 3; ++i) { bones[b * 16 + 4 + i] -= c.offset[i]; bones[b * 16 + 12 + i] -= c.offset[i]; }
    for (int k = c.numPos - 1; k >= 0; --k)
        std::memcpy(reinterpret_cast<float*>(o + c.pos[k]), save[k], sizeof(save[k]));
    if (c.mode == 2) g_addSkinOrig(list, obj, model, anim);
}

// Einfacher Inline-Hook: die ersten Bytes in einen Trampolin-Puffer kopieren und durch
// einen absoluten Sprung (jmp [rip+0]; dq ziel) auf unsere Funktion ersetzen.
void* InstallInlineHook(uintptr_t target, size_t prologueLen, void* detour) {
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    std::memcpy(tramp, reinterpret_cast<void*>(target), prologueLen);
    uint8_t* p = tramp + prologueLen;
    const uint64_t back = target + prologueLen;
    *p++ = 0xFF; *p++ = 0x25; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; std::memcpy(p, &back, 8);
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(target), prologueLen, PAGE_EXECUTE_READWRITE, &old);
    auto* t = reinterpret_cast<uint8_t*>(target);
    const uint64_t dst = reinterpret_cast<uint64_t>(detour);
    t[0] = 0xFF; t[1] = 0x25; t[2] = 0; t[3] = 0; t[4] = 0; t[5] = 0; std::memcpy(t + 6, &dst, 8);
    for (size_t i = 14; i < prologueLen; ++i) t[i] = 0x90;  // nop
    VirtualProtect(reinterpret_cast<void*>(target), prologueLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), prologueLen);
    return tramp;
}

void HookAddSkin() {
    static const uint8_t expected[kAddSkinPrologue] = {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41,
                                                       0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0x80};
    if (std::memcmp(reinterpret_cast<void*>(g_base + kAddSkinRva), expected, kAddSkinPrologue) != 0) {
        Log(L("Hook AddSkin: unerwartete Bytes - uebersprungen (andere Spielversion?)\n", "Hook AddSkin: unexpected bytes - skipped (different game version?)\n"));
        return;
    }
    g_addSkinOrig = reinterpret_cast<AddSkinFn>(InstallInlineHook(g_base + kAddSkinRva, kAddSkinPrologue, HookedAddSkin));
    Log(L("Hook AddSkin: %s\n", "Hook AddSkin: %s\n"), g_addSkinOrig ? L("aktiv (Geist an, F9 schaltet um)", "active (ghost on, F9 toggles)") : L("FEHLER", "ERROR"));
}

// --- Analyse-Werkzeug: Hardware-Watchpoint (Debug-Register DR0) ---
// Meldet, welcher Code eine Adresse beschreibt, inkl. Aufrufkette. Ergebnisse werden im Ausnahme-Handler
// nur gesammelt und spaeter vom Probe-Thread geloggt (kein Logging im Handler -> keine Sperren).
struct WatchHit {
    uintptr_t rip;
    int numFrames;
    uintptr_t frames[16];
};
constexpr int kMaxWatchHits = 4;
WatchHit g_watchHits[kMaxWatchHits];
volatile LONG g_watchHitCount = 0;
volatile LONG g_watchTotal = 0;
std::atomic<bool> g_watchActive{false};

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    const LONG total = InterlockedIncrement(&g_watchTotal);
    const LONG idx = InterlockedIncrement(&g_watchHitCount) - 1;
    if (idx < kMaxWatchHits) {
        WatchHit& h = g_watchHits[idx];
        h.rip = ep->ContextRecord->Rip;
        h.numFrames = 0;
        CONTEXT ctx = *ep->ContextRecord;   // Aufrufkette per Unwind ab dem Fehler-Kontext
        for (int i = 0; i < 16 && ctx.Rip; ++i) {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {
                // Blattfunktion ohne Unwind-Info: Ruecksprungadresse liegt direkt auf dem Stack
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
                if (ctx.Rip) h.frames[h.numFrames++] = ctx.Rip;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            if (ctx.Rip) h.frames[h.numFrames++] = ctx.Rip;
        }
    }
    if (total >= 50) ep->ContextRecord->Dr7 &= ~0xFULL;  // nach 50 Treffern in diesem Thread abschalten
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DR0 in allen Threads ausser dem eigenen setzen (addr = 0 -> abschalten)
void SetWatchAllThreads(uintptr_t addr) {
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) {
            c.Dr0 = addr;
            c.Dr6 = 0;
            // L0 = 1, RW0 = 01 (Schreiben), LEN0 = 11 (4 Bytes)
            c.Dr7 = addr ? ((c.Dr7 & ~0xF0003ULL) | 0x1ULL | (0x1ULL << 16) | (0x3ULL << 18)) : (c.Dr7 & ~0xF0003ULL);
            SetThreadContext(th, &c);
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
}

DWORD g_watchStart = 0;

void StartWatch(uintptr_t addr, const char* what) {
    static void* veh = AddVectoredExceptionHandler(1, WatchHandler);
    (void)veh;
    g_watchHitCount = 0;
    g_watchTotal = 0;
    g_watchStart = GetTickCount();
    SetWatchAllThreads(addr);
    g_watchActive = true;
    Log(L("\n[Watch] Beobachte Schreibzugriffe auf 0x%llX (%s)\n", "\n[Watch] watching writes to 0x%llX (%s)\n"), (unsigned long long)addr, what);
}

void PollWatch() {
    if (!g_watchActive) return;
    if (g_watchHitCount < kMaxWatchHits && GetTickCount() - g_watchStart < 3000) return;
    SetWatchAllThreads(0);
    g_watchActive = false;
    const LONG n = g_watchHitCount < kMaxWatchHits ? g_watchHitCount : kMaxWatchHits;
    Log(L("[Watch] %ld Treffer insgesamt\n", "[Watch] %ld hits total\n"), (long)g_watchTotal);
    for (LONG i = 0; i < n; ++i) {
        const WatchHit& h = g_watchHits[i];
        Log(L("  Treffer %ld: Code 0x%llX  Aufrufkette:", "  Hit %ld: code 0x%llX  call chain:"), (long)i, (unsigned long long)(h.rip - g_base));
        for (int f = 0; f < h.numFrames; ++f) Log(L(" 0x%llX", " 0x%llX"), (unsigned long long)(h.frames[f] - g_base));
        Log(L("\n", "\n"));
    }
}

// Eigener Thread nur fuer die Tasten, damit waehrend einer laengeren Suche keine Druecke verloren gehen.
constexpr int kNumKeys = 4;
constexpr int kKeys[kNumKeys] = {VK_F6, VK_F8, VK_F9, VK_F11};
std::atomic<int> g_pending[kNumKeys];

// Nur im Entwickler-Build (build.ps1 -Dev): GetAsyncKeyState allein reicht manchen Virenscannern fuer "Keylogger"
#ifdef SMCOOP_DEV
DWORD WINAPI KeyThread(LPVOID) {
    bool prev[kNumKeys] = {};
    for (;;) {
        for (int i = 0; i < kNumKeys; ++i) {
            const SHORT st = GetAsyncKeyState(kKeys[i]);
            const bool down = (st & 0x8000) != 0;
            if ((down && !prev[i]) || (!down && (st & 1))) g_pending[i] = 1;  // Bit 0: kurz gedrueckt seit letzter Abfrage
            prev[i] = down;
        }
        Sleep(10);
    }
    return 0;
}
#endif

// Hand-Komponenten des echten Spielers ins Log (Analyse Waffen)
void LogHands() {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (!gp) return;
    auto rtti = [](uintptr_t objAddr) -> std::string {
        uintptr_t vt = 0, col = 0;
        uint32_t td = 0;
        char name[96] = {};
        if (!objAddr || !SafeRead(objAddr, &vt, 8) || !SafeRead(vt - 8, &col, 8) || !SafeRead(col + 0x0C, &td, 4) ||
            !SafeRead(g_base + td + 0x10, name, sizeof(name) - 1))
            return "?";
        return name;
    };
    for (auto& n : g_meshNotes) {
        const uintptr_t o = n.obj;
        if (!o) break;
        float m[16] = {};
        uintptr_t geom = 0;
        SafeRead(o + 0x200, m, sizeof(m));
        SafeRead(o + 0x118, &geom, 8);
        Log(L("[Waffe?] obj=0x%llX (%s) Aufrufer 0x%llX, %ld mal, Pos %.1f/%.1f/%.1f, Geometrie 0x%llX\n", "[Weapon?] obj=0x%llX (%s) caller 0x%llX, %ld times, pos %.1f/%.1f/%.1f, geometry 0x%llX\n"), (unsigned long long)o,
            rtti(o).c_str(), (unsigned long long)n.ret.load(), (long)n.count.load(), m[12], m[13], m[14], (unsigned long long)geom);
    }
    for (auto& n : g_questCallers) {
        if (!n.ret) break;
        Log(L("[Waffen-Zeichner] Aufrufer 0x%llX: %ld mal\n", "[Weapon drawer] caller 0x%llX: %ld times\n"), (unsigned long long)n.ret.load(), (long)n.count.load());
    }
    for (auto& n : g_skinNotes) {
        const uintptr_t o = n.obj;
        if (!o) break;
        Log(L("[Waffe-Skin?] obj=0x%llX (%s) anim=0x%llX (obj%+lld, Spieler%+lld) model=0x%llX (%s) Aufrufer 0x%llX, %ld mal\n", "[Weapon skin?] obj=0x%llX (%s) anim=0x%llX (obj%+lld, player%+lld) model=0x%llX (%s) caller 0x%llX, %ld times\n"),
            (unsigned long long)o, rtti(o).c_str(), (unsigned long long)n.anim.load(), (long long)(n.anim - o), (long long)(n.anim - gp),
            (unsigned long long)n.model.load(), rtti(n.model).c_str(), (unsigned long long)n.ret.load(), (long)n.count.load());
    }
    const uint32_t offs[2] = {0x1D1D0, 0x1D410};
    for (int h = 0; h < 2; ++h) {
        const uintptr_t a = gp + offs[h];
        uintptr_t owner = 0, bones = 0, nBones = 0, c8 = 0, e0 = 0;
        uint16_t slot = 0;
        SafeRead(a + 0x120, &owner, 8);
        SafeRead(a + 0x180, &bones, 8);
        SafeRead(a + 0x188, &nBones, 8);
        SafeRead(a + 0xC8, &c8, 8);
        SafeRead(a + 0xE0, &e0, 8);
        SafeRead(a + 0x198, &slot, 2);
        Log(L("[Hand %d] anim=Spieler+0x%X  Besitzer=0x%llX (%s)  Knochen=%llu (A %llu + B %llu)  GPU-Platz=%u\n", "[Hand %d] anim=player+0x%X  owner=0x%llX (%s)  bones=%llu (A %llu + B %llu)  GPU slot=%u\n"), h, offs[h],
            (unsigned long long)owner, owner == gp ? L("Spieler", "player") : rtti(owner).c_str(), (unsigned long long)nBones,
            (unsigned long long)c8, (unsigned long long)e0, slot);
        Log(L("         gezeichnet: %ld mal, zuletzt obj=0x%llX (%s) model=0x%llX (%s) Aufrufer 0x%llX\n", "         drawn: %ld times, last obj=0x%llX (%s) model=0x%llX (%s) caller 0x%llX\n"), (long)g_handDraw[h].count.load(),
            (unsigned long long)g_handDraw[h].obj.load(), g_handDraw[h].obj == gp ? L("Spieler", "player") : rtti(g_handDraw[h].obj).c_str(),
            (unsigned long long)g_handDraw[h].model.load(), rtti(g_handDraw[h].model).c_str(), (unsigned long long)g_handDraw[h].ret.load());
    }
}

// Analyse Waffen: Aenderungen an den Hand-Objekten des Spielers automatisch protokollieren
[[maybe_unused]] void WatchHandWeapons() {
    const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
    if (!gp) return;
    struct State { uintptr_t objL, objR, geomL, geomR; uint32_t ids; uint32_t form; };
    static State last{};
    State s{};
    SafeRead(gp + 0x1D618, &s.objL, 8);
    SafeRead(gp + 0x1D620, &s.objR, 8);
    if (s.objL) SafeRead(s.objL + 0x118, &s.geomL, 8);
    if (s.objR) SafeRead(s.objR + 0x118, &s.geomR, 8);
    SafeRead(gp + 0x1D628, &s.ids, 4);
    SafeRead(gp + 0x1D184, &s.form, 4);
    // Geometrie wechselt bei Effekt-Objekten jedes Frame (Daumenkino) -> nur Objekte/Kennungen/Form vergleichen
    if (s.objL != last.objL || s.objR != last.objR || s.ids != last.ids || s.form != last.form) {
        Log(L("[Haende] Form(+0x1D184)=%u  Kennungen(+0x1D628)=%04X/%04X | links obj 0x%llX geom 0x%llX | rechts obj 0x%llX geom 0x%llX\n", "[Hands] form(+0x1D184)=%u  ids(+0x1D628)=%04X/%04X | left obj 0x%llX geom 0x%llX | right obj 0x%llX geom 0x%llX\n"),
            s.form, s.ids & 0xFFFF, s.ids >> 16, (unsigned long long)s.objL, (unsigned long long)s.geomL, (unsigned long long)s.objR,
            (unsigned long long)s.geomR);
        last = s;
    }
}

// Eigener Thread nur fuers Netzwerk: nichts darin darf lange dauern oder blockieren
DWORD WINAPI NetThread(LPVOID) {
    for (;;) {
        if (net::SteamWorkBegin()) {  // nach SteamAPI_Shutdown (Spiel endet) nichts mehr senden/empfangen
            static DWORD lastLobby = 0;
            if (GetTickCount() - lastLobby >= 200) {  // Steam-Lobby: Ergebnisse abholen, Mitspieler nachfuehren
                lastLobby = GetTickCount();
                net::LobbyTick();
            }
            PumpNetwork();
            net::SteamWorkEnd();
        }
        Sleep(2);
    }
    return 0;
}

// ---- Steam beenden ----
// Beim Beenden ruft das Spiel SteamAPI_Shutdown (Aufruf bei 0x2B92DA); danach ist steamclient64.dll entladen. Rief der
// Netz-Thread gerade (oder danach) Steam auf, sprang er in entladenen Code -> Absturz (Windows-Bericht 23.10. 17:01
// "crashhandler64.dll_unloaded", 21.10. "steamclient64.dll_unloaded"; Netz-Thread in ReceiveMessagesOnChannel).
// Import-Eintrag umbiegen: erst den Netz-Thread aus Steam herauskommen lassen und sperren, dann das Original.
using SteamShutdownFn = void (*)();
SteamShutdownFn g_steamShutdownOrig = nullptr;

void HookedSteamShutdown() {
    net::SteamShutdown();
    Log(L("[Steam] Spiel beendet Steam - Netz gestoppt\n", "[Steam] game shuts down Steam - network stopped\n"));
    if (g_steamShutdownOrig) g_steamShutdownOrig();
}

// Import-Adress-Eintrag dll!fn des Spiels (nullptr = nicht gefunden)
void** FindImportSlot(const char* dll, const char* fn) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(g_base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(g_base + d->Name), dll) != 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(g_base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(g_base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), fn) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

void HookSteamShutdown() {
    if (void** slot = FindImportSlot("steam_api64.dll", "SteamAPI_Shutdown")) {
        DWORD old = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            g_steamShutdownOrig = reinterpret_cast<SteamShutdownFn>(*slot);
            *slot = reinterpret_cast<void*>(HookedSteamShutdown);
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
    }
    Log(L("Hook Steam-Ende: %s\n", "Hook Steam shutdown: %s\n"), g_steamShutdownOrig ? L("aktiv", "active") : L("FEHLER", "ERROR"));
}

DWORD WINAPI ProbeThread(LPVOID) {
    AllocConsole();
    SetConsoleTitleA("SMCoop");
    std::freopen("CONOUT$", "w", stdout);
    DisableConsoleQuickEdit();
    // Vorheriges Protokoll behalten: nach einem Absturz startet das Spiel neu und ueberschrieb es sonst. Laeuft schon eine
    // Instanz (Test auf einem PC), ist die Datei offen, das Umbenennen scheitert und sie bleibt, wie sie ist.
    MoveFileExA("smcoop_log.txt", "smcoop_log_prev.txt", MOVEFILE_REPLACE_EXISTING);
    // Exklusiv oeffnen; laeuft schon eine zweite Spiel-Instanz (Test auf einem PC), eigene Datei nehmen
    g_log = _fsopen("smcoop_log.txt", "w", _SH_DENYWR);
    for (int i = 1; !g_log && i < 4; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "smcoop_log%d.txt", i + 1);
        g_log = _fsopen(name, "w", _SH_DENYWR);
        g_instance = i;
    }
    DetectLanguageFromConfig();
    StartLogger();
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));

    Log(L("=== SMCoop - Build %s %s ===\n", "=== SMCoop - Build %s %s ===\n"), __DATE__, __TIME__);
    Log(L("Spiel-Basisadresse: 0x%llX\n", "Game base address: 0x%llX\n"), (unsigned long long)g_base);
    const bool okPlayer = VerifyVtable(kPlayerVtableRva, ".?AVkexShadowManPlayer@@");
    const bool okGame   = VerifyVtable(kGameInstanceVtableRva, ".?AVkexShadowManGameInstance@@");
    Log(L("vtable kexShadowManPlayer:       %s\n", "vtable kexShadowManPlayer:       %s\n"), okPlayer ? "OK" : "FALSCH (andere Spielversion?)");
    Log(L("vtable kexShadowManGameInstance: %s\n", "vtable kexShadowManGameInstance: %s\n"), okGame ? "OK" : "FALSCH (andere Spielversion?)");
    if (okPlayer) { HookPlayerVtable(); HookPlayerDtor(); HookPlayerSaveLoad(); HookAddSkin(); HookAddMesh(); HookHeldItem(); HookSpawnProjectile(); HookSounds(); HookObjectStates(); HookProximityDoors(); HookEvents(); HookAiUpdate(); HookPlayerDamage(); HookPlayerBones(); HookOnGameLoad(); HookLooks(); HookCutscenes(); HookCoopMenu(); LoadSettings(); HookPlayerDie(); HookHardcore(); HookPauseMenu(); HookAnimPlay(); HookRideUpdates(); HookSteamShutdown(); g_lifeSession = (GetTickCount() ^ (GetCurrentProcessId() << 4)) | 1;
                    if (overlay::Install("ShadowManEX01.kpf", OverlayFrame, Log)) HookRtPresent(); }
#ifdef SMCOOP_DEV
    Log(L("\nAnalyse-Tasten: F6 Schnappschuss | F8 Analyse ins Log | F9 Spieler 2 (Test) | F11 Trace  (F5/F7 = Schnellspeichern/-laden des Spiels)\n", "\nAnalysis keys: F6 snapshot | F8 analysis to log | F9 player 2 (test) | F11 trace  (F5/F7 = game quick save/load)\n"));
    if (HANDLE t = CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr)) CloseHandle(t);
#endif

    uintptr_t lastPlayer = 0;
    for (;;) {
        // Spieler direkt aus der globalen Variable g_player lesen (keine Speichersuche mehr)
        const uintptr_t gp = *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
        if (gp != lastPlayer && !IsOurPlayer2(gp) && (gp == 0 || PlayerStillValid(gp))) {  // Gegner-Update tauscht g_player kurz gegen Spieler 2
            if (gp) {
                Log(lastPlayer ? L("\n[Auto] Spielerobjekt wurde neu erstellt.\n", "\n[Auto] player object was recreated.\n") : L("\n[Auto] Spieler gefunden.\n", "\n[Auto] player found.\n"));
                g_players.assign(1, gp);
                Log(L("  Spieler bei 0x%llX\n", "  Player at 0x%llX\n"), (unsigned long long)gp);
                CmdSnapshot();
                static bool netStarted = false;  // Lobby-Modus: IsRunning() bleibt ohne Mitspieler false
                if (!lastPlayer && !net::IsRunning() && !netStarted) {  // Steam ist jetzt sicher initialisiert
                    net::LogSteamInfo(Log);
                    if (net::Start(Log)) {
                        netStarted = true;
                        net::LobbyInit();
                        if (HANDLE t = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr)) CloseHandle(t);
                    }
                }
            } else {
                Log(L("\n[Auto] Spielerobjekt verschwunden (Menue/Ladebildschirm?).\n", "\n[Auto] player object gone (menu/loading screen?).\n"));
                g_players.clear();
                // Figuren haengen am alten Level -> nicht mehr benutzen (zweite Sicherung neben HookedPlayerDelete; das Objekt
                // selbst gibt die Mod nie frei, ein Zugriff, der gerade laeuft, bleibt also gueltig)
                for (int pi = 0; pi < kMaxRemote; ++pi)
                    if (g_avatar[pi].exchange(0)) Log(L("[Spieler2] Figur %d verworfen (Ladebildschirm)\n", "[Player2] figure %d dropped (loading screen)\n"), pi + 1);
            }
            lastPlayer = gp;
        }
        for (int i = 0; i < kNumKeys; ++i) {
            if (g_pending[i].exchange(0) == 0) continue;
            switch (kKeys[i]) {
                case VK_F6: Log(L("\n[F6]\n", "\n[F6]\n")); CmdSnapshot(); Signal(1000, 150); break;
                case VK_F8:
                    CmdDiff();
                    LogPlayerAnim();
                    LogCallers();
                    LogBonesOf("Spieler1", *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva));
                    LogBonesOf("Spieler2", g_avatar[0]);
                    LogHands();
                    break;
                case VK_F9:
                    if (g_avatar[0]) { Log(L("\n[F9] Spieler 2 existiert schon (0x%llX)\n", "\n[F9] player 2 already exists (0x%llX)\n"), (unsigned long long)g_avatar[0].load()); Signal(500, 120); }
                    else { g_localTestFigure = !net::IsRunning(); g_spawnRequest = 1; Log(L("\n[F9] Spieler 2 wird beim naechsten Frame erzeugt...\n", "\n[F9] player 2 will be created next frame...\n")); Signal(1200, 120); }
                    break;
                case VK_F11: {  // Waffe in der Hand? -> Watchpoint auf deren Position; mit Spieler 2: Tracer; sonst Knochen
                    uintptr_t quest = 0;
                    for (auto& n : g_meshNotes) {
                        const uintptr_t o = n.obj;
                        uintptr_t vt = 0;
                        if (o && SafeRead(o, &vt, 8) && vt == g_base + kQuestObjectVtableRva) quest = o;
                    }
                    if (quest) { StartWatch(quest + 0x230, "Position der Waffe in der Hand (+0x230)"); break; }
                    if (g_avatar[0]) { g_traceRequest = 1; Log(L("\n[F11] Trace angefordert\n", "\n[F11] trace requested\n")); break; }
                    const uintptr_t p1 = g_avatar[0] ? g_avatar[0].load() : *reinterpret_cast<uintptr_t*>(g_base + kGPlayerRva);
                    uintptr_t bones = 0;
                    if (p1) SafeRead(p1 + 0x368 + 0x180, &bones, 8);
                    if (bones) StartWatch(bones + 0x10, "Knochen 0, Position X");
                    else Log(L("\n[F11] Keine Knochendaten\n", "\n[F11] no bone data\n"));
                    break;
                }
            }
        }
        if (g_watch) PrintWatch();
        PollWatch();
        // WatchHandWeapons(); (Analyse der Hand-Objekte, fuellte das Log bei jedem Waffenwechsel)
        static DWORD lastCfg = 0;
        if (GetTickCount() - lastCfg > 1000) { LoadGhostConfig(); ReadCommandFile(); lastCfg = GetTickCount(); }
        {   // Levelwechsel protokollieren
            static LevelInfo lastLevel{-999, false};
            const LevelInfo li = GetLevelInfo(g_base);
            if (li.level != lastLevel.level || li.inGame != lastLevel.inGame) {
                Log(L("\n[Level] Level %d, %s (Animations-Eintraege belegt %d)\n", "\n[Level] level %d, %s (animation entries in use %d)\n"), li.level, li.inGame ? L("im Spiel", "in game") : L("Hauptmenue", "main menu"),
                    *reinterpret_cast<int*>(g_base + 0xD8B6B8));
                if (li.inGame && !lastLevel.inGame) {
                    HardcoreOnEnterGame();
                    g_tableResync = true;
                    g_progressBaselineReset = true;
                    if (!net::IsHost() && !g_followArmed.exchange(true)) Log(L("[Folgen] aus dem Hauptmenue gestartet - gehe einmal zum Host\n", "[Follow] started from the main menu - going to the host once\n"));
                }
                if (li.inGame) {
                    const CheatState cs = GetCheatState(g_base);
                    Log(L("[Erfolge] %s (Abenteuer-Flags 0x%02X, Sitzung %s)\n", "[Achievements] %s (adventure flags 0x%02X, session %s)\n"),
                        CheatsUsed(g_base) ? L("GESPERRT - Cheats benutzt", "LOCKED - cheats used") : L("aktiv (keine Cheats)", "active (no cheats)"), cs.adventure,
                        cs.session ? "Cheats" : L("sauber", "clean"));
                }
                lastLevel = li;
            }
            static bool lastCheats = false;
            const bool cheats = li.inGame && CheatsUsed(g_base);
            if (cheats && !lastCheats && li.level == lastLevel.level)
                Log(L("\n[Erfolge] ACHTUNG: Das Cheat-Flag wurde gerade gesetzt - Erfolge sind fuer dieses Abenteuer gesperrt!\n", "\n[Achievements] WARNING: the cheat flag was just set - achievements are locked for this adventure!\n"));
            lastCheats = cheats;
        }
        static DWORD lastNetLog = 0;
        if (net::IsRunning() && GetTickCount() - lastNetLog > 10000) {
            lastNetLog = GetTickCount();
            Log(L("[Netz] gesendet %lld, empfangen %lld, Mitspieler %s\n", "[Net] sent %lld, received %lld, players %s\n"), (long long)g_netSent, (long long)g_netRecv,
                AnyRemoteActive() ? L("aktiv", "active") : "-");
        }
        Sleep(5);
    }
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        if (HANDLE t = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
