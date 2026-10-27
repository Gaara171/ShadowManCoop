// Transporte: UDP (Winsock) und Steam-P2P ueber ISteamNetworkingMessages (flache C-API aus steam_api64.dll,
// die das Spiel ohnehin laedt und initialisiert). Bis zu kMaxPeers Mitspieler: jedes Paket geht an alle (Masche),
// empfangene Pakete tragen den Platz des Absenders (0..kMaxPeers-1, fest, solange er da ist).
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include "coop_logic.h"
#include "i18n.h"
#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace net {
namespace {

enum class Mode { None, Udp, Steam };
Mode g_mode = Mode::None;

// ---------------- Steam (Deklarationen) ----------------
// Minimale Deklarationen der flachen Steam-API (steam_api_flat.h, SDK ~1.53)
struct SteamNetworkingIdentity { uint8_t raw[136]; };
struct SteamNetworkingMessage {
    void* m_pData;
    int m_cbSize;
    uint32_t m_conn;
    SteamNetworkingIdentity m_identityPeer;  // Absender
    // weitere Felder werden nicht gebraucht
};

using FnGetMessages = void* (*)();
using FnSend = int (*)(void* self, const SteamNetworkingIdentity& to, const void* data, uint32_t size, int flags, int channel);
using FnReceive = int (*)(void* self, int channel, SteamNetworkingMessage** out, int max);
using FnRelease = void (*)(SteamNetworkingMessage* msg);
using FnAccept = bool (*)(void* self, const SteamNetworkingIdentity& who);
using FnIdentityClear = void (*)(SteamNetworkingIdentity* self);
using FnIdentitySetId = void (*)(SteamNetworkingIdentity* self, uint64_t steamId);
using FnIdentityGetId = uint64_t (*)(const SteamNetworkingIdentity* self);
using FnGetUser = void* (*)();
using FnGetSteamId = uint64_t (*)(void* self);
using FnGetFriends = void* (*)();
using FnFriendCount = int (*)(void* self, int flags);
using FnFriendByIndex = uint64_t (*)(void* self, int index, int flags);
using FnFriendName = const char* (*)(void* self, uint64_t steamId);

constexpr int kSendUnreliableNoNagle = 1;
constexpr int kChannel = 7;  // beliebig, aber fest

HMODULE g_steam = nullptr;
void* g_messages = nullptr;
FnSend g_send = nullptr;
FnReceive g_receive = nullptr;
FnRelease g_release = nullptr;
FnAccept g_accept = nullptr;
FnIdentityClear g_idClear = nullptr;
FnIdentitySetId g_idSet = nullptr;
FnIdentityGetId g_idGet = nullptr;
DWORD g_lastAccept = 0;
SRWLOCK g_steamGate = SRWLOCK_INIT;     // Netz-Thread haelt ihn geteilt, solange er Steam aufruft; SteamShutdown exklusiv
std::atomic<bool> g_steamDown{false};  // Spiel hat Steam beendet

template <typename T>
T SteamFn(const char* name) {
    if (!g_steam) g_steam = GetModuleHandleA("steam_api64.dll");
    return g_steam ? reinterpret_cast<T>(GetProcAddress(g_steam, name)) : nullptr;
}

// ---------------- Mitspieler-Plaetze ----------------
struct PeerSlot {
    bool used = false;
    uint64_t steamId = 0;          // Steam
    SteamNetworkingIdentity id{};  // Steam (fuer Senden/Annehmen)
    uint16_t udpPort = 0;          // UDP (Test auf einem PC)
    char name[64] = {};
};
SRWLOCK g_peerLock = SRWLOCK_INIT;  // g_peers (Netz-Thread liest, Lobby-Rueckrufe schreiben)
PeerSlot g_peers[kMaxPeers];
std::atomic<int> g_hostPeer{-1};    // Platz des Hosts (-1 = ich bin Host oder keiner)
std::atomic<bool> g_running{false};
std::atomic<bool> g_isHost{false};
std::atomic<uint64_t> g_localId{0};  // eigene Kennung (Steam-ID; UDP-Test: Instanznummer)
LogFn g_log = nullptr;
bool g_lobbyMode = false;
bool g_fakeMember = false;
bool g_fakeAsk = false;

// ---------------- UDP ----------------
SOCKET g_sock = INVALID_SOCKET;
in_addr g_udpPeerAddr = {};

bool BindUdp(int localPort, bool loopback, LogFn log, bool quiet) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { log(L("[Netz] WSAStartup fehlgeschlagen\n", "[Net] WSAStartup failed\n")); return false; }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { log(L("[Netz] socket() fehlgeschlagen\n", "[Net] socket() failed\n")); return false; }
    u_long nonBlocking = 1;
    ioctlsocket(g_sock, FIONBIO, &nonBlocking);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<u_short>(localPort));
    // Test auf einem PC: nur lokal lauschen (vermeidet die Nachfrage der Windows-Firewall)
    local.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        if (!quiet) log(L("[Netz] Port %d belegt oder gesperrt (Fehler %d)\n", "[Net] port %d in use or blocked (error %d)\n"), localPort, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    return true;
}

bool StartUdp(int localPort, const char* peerIp, int peerPort, LogFn log) {
    if (!BindUdp(localPort, std::strcmp(peerIp, "127.0.0.1") == 0, log, false)) return false;
    inet_pton(AF_INET, peerIp, &g_udpPeerAddr);
    g_peers[0].used = true;
    g_peers[0].udpPort = static_cast<uint16_t>(peerPort);
    std::snprintf(g_peers[0].name, sizeof(g_peers[0].name), "Port %d", peerPort);
    log(L("[Netz] UDP aktiv: lausche auf Port %d, sende an %s:%d\n", "[Net] UDP active: listening on port %d, sending to %s:%d\n"), localPort, peerIp, peerPort);
    return true;
}

// ---------------- Steam ----------------
uint64_t OwnSteamId();
bool StartSteam(uint64_t peer, LogFn log) {
    auto getMessages = SteamFn<FnGetMessages>("SteamAPI_SteamNetworkingMessages_v002");
    g_idClear = SteamFn<FnIdentityClear>("SteamAPI_SteamNetworkingIdentity_Clear");
    g_idSet = SteamFn<FnIdentitySetId>("SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    g_idGet = SteamFn<FnIdentityGetId>("SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    g_send = SteamFn<FnSend>("SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    g_receive = SteamFn<FnReceive>("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    g_release = SteamFn<FnRelease>("SteamAPI_SteamNetworkingMessage_t_Release");
    g_accept = SteamFn<FnAccept>("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    if (!getMessages || !g_idClear || !g_idSet || !g_idGet || !g_send || !g_receive || !g_release || !g_accept) {
        log(L("[Netz] Steam-Funktionen nicht gefunden\n", "[Net] Steam functions not found\n"));
        return false;
    }
    g_messages = getMessages();
    if (!g_messages) { log(L("[Netz] SteamNetworkingMessages nicht verfuegbar (Steam laeuft nicht?)\n", "[Net] SteamNetworkingMessages not available (Steam not running?)\n")); return false; }
    g_localId = OwnSteamId();
    if (peer) {
        g_peers[0].used = true;
        g_peers[0].steamId = peer;
        g_idClear(&g_peers[0].id);
        g_idSet(&g_peers[0].id, peer);
        log(L("[Netz] Steam-P2P aktiv: Mitspieler %llu (Kanal %d)\n", "[Net] Steam P2P active: player %llu (channel %d)\n"), static_cast<unsigned long long>(peer), kChannel);
    } else {
        log(L("[Netz] Steam-P2P bereit (Kanal %d), Mitspieler kommen aus der Lobby\n", "[Net] Steam P2P ready (channel %d), players come from the lobby\n"), kChannel);
    }
    return true;
}

uint64_t OwnSteamId() {
    auto getUser = SteamFn<FnGetUser>("SteamAPI_SteamUser_v021");
    auto getId = SteamFn<FnGetSteamId>("SteamAPI_ISteamUser_GetSteamID");
    void* user = getUser ? getUser() : nullptr;
    return user && getId ? getId(user) : 0;
}

// Lobby: Plaetze an die aktuelle Mitgliederliste anpassen (bleibende behalten ihren Platz). host = Steam-ID des Hosts.
void SetSteamPeers(const uint64_t* ids, const char (*names)[64], int count, uint64_t host, bool selfHost) {
    AcquireSRWLockExclusive(&g_peerLock);
    for (PeerSlot& s : g_peers) {  // Gegangene freigeben
        if (!s.used) continue;
        bool still = false;
        for (int i = 0; i < count; ++i)
            if (ids[i] == s.steamId) still = true;
        if (!still) s = PeerSlot();
    }
    for (int i = 0; i < count; ++i) {  // Neue auf freie Plaetze, Namen auffrischen
        int slot = -1;
        for (int k = 0; k < kMaxPeers; ++k)
            if (g_peers[k].used && g_peers[k].steamId == ids[i]) slot = k;
        if (slot < 0) {
            for (int k = 0; k < kMaxPeers && slot < 0; ++k)
                if (!g_peers[k].used) slot = k;
            if (slot < 0) continue;
            g_peers[slot].used = true;
            g_peers[slot].steamId = ids[i];
            if (g_idClear && g_idSet) {
                g_idClear(&g_peers[slot].id);
                g_idSet(&g_peers[slot].id, ids[i]);
            }
        }
        std::snprintf(g_peers[slot].name, sizeof(g_peers[slot].name), "%s", names[i]);
    }
    int hostSlot = -1;
    bool any = false;
    for (int k = 0; k < kMaxPeers; ++k) {
        if (!g_peers[k].used) continue;
        any = true;
        if (g_peers[k].steamId == host) hostSlot = k;
    }
    g_hostPeer = selfHost ? -1 : hostSlot;
    g_isHost = selfHost;
    g_running = any && g_messages != nullptr;
    ReleaseSRWLockExclusive(&g_peerLock);
}

}  // namespace

bool Start(LogFn log) {
    if (g_running || g_lobbyMode) return true;
    g_log = log;
    FILE* f = std::fopen("smcoop_net.txt", "r");
    char line[256] = "lobby";  // ohne Datei: Steam-Lobby aus dem Hauptmenue
    bool ok = false;
    while (!f || std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (std::strncmp(line, "lobby", 5) == 0) {
            if (!StartSteam(0, log)) break;
            g_mode = Mode::Steam;
            g_lobbyMode = true;
            log(L("[Netz] Lobby-Modus: Mitspieler kommen ueber die Koop-Lobby (Hauptmenue -> Koop-Lobby)\n", "[Net] lobby mode: players join via the co-op lobby (main menu -> Co-op Lobby)\n"));
            if (f) std::fclose(f);
            return true;  // Netz-Thread laeuft, IsRunning() erst mit Mitspieler
        }
        int localPort = 0, peerPort = 0, count = 0;
        char ip[64] = {};
        unsigned long long peer = 0;
        // Rolle ausdruecklich festlegen: "host" oder "gast" irgendwo in der Zeile
        const bool forceHost = std::strstr(line, " host") != nullptr;
        const bool forceGuest = std::strstr(line, " gast") != nullptr;
        const bool autostart = std::strstr(line, "autostart") != nullptr;
        const bool ask = std::strstr(line, "frage") != nullptr;
        if (std::sscanf(line, "udpmesh %d %d", &localPort, &count) == 2 && count >= 2 && count <= kMaxPeers + 1) {
            // Test mit bis zu 4 Spiel-Instanzen auf einem PC: jede nimmt den ersten freien Port ab localPort,
            // die erste (Port localPort) ist Host
            int own = -1;
            for (int k = 0; k < count && own < 0; ++k)
                if (BindUdp(localPort + k, true, log, true)) own = k;
            if (own >= 0) {
                inet_pton(AF_INET, "127.0.0.1", &g_udpPeerAddr);
                int slot = 0;
                for (int k = 0; k < count; ++k) {
                    if (k == own) continue;
                    g_peers[slot].used = true;
                    g_peers[slot].udpPort = static_cast<uint16_t>(localPort + k);
                    std::snprintf(g_peers[slot].name, sizeof(g_peers[slot].name), L("Instanz %d", "Instance %d"), k + 1);
                    // Test/Bilder: "names A,B,C" in der Zeile gibt den Instanzen Namen (sonst "Instanz n")
                    if (const char* nm = std::strstr(line, " names ")) {
                        nm += 7;
                        for (int skip = 0; skip < k && nm; ++skip)
                            if ((nm = std::strchr(nm, ',')) != nullptr) ++nm;
                        const size_t len = nm ? std::strcspn(nm, ", \r\n") : 0;
                        if (len > 0 && len < sizeof(g_peers[slot].name))
                            std::snprintf(g_peers[slot].name, sizeof(g_peers[slot].name), "%.*s", static_cast<int>(len), nm);
                    }
                    g_peers[slot].steamId = static_cast<uint64_t>(k + 1);  // Kennung fuer die Spielernummer
                    if (k == 0) g_hostPeer = slot;
                    ++slot;
                }
                g_isHost = own == 0;
                g_localId = static_cast<uint64_t>(own + 1);
                ok = true;
                g_mode = Mode::Udp;
                g_fakeMember = autostart && own != 0;
                g_fakeAsk = ask && own != 0;
                log(L("[Netz] UDP-Masche: Instanz %d von %d (Port %d)\n", "[Net] UDP mesh: instance %d of %d (port %d)\n"), own + 1, count, localPort + own);
            }
        } else if (std::sscanf(line, "udppair %d %d", &localPort, &peerPort) == 2) {
            // Test mit zwei Spiel-Instanzen auf einem PC: die erste nimmt Port A (= Host), die zweite die Gegenrichtung
            if (StartUdp(localPort, "127.0.0.1", peerPort, log)) {
                ok = true;
                g_isHost = true;
            } else {
                ok = StartUdp(peerPort, "127.0.0.1", localPort, log);
                g_isHost = false;
                g_hostPeer = 0;
            }
            if (ok) g_mode = Mode::Udp;
            g_peers[0].steamId = g_isHost ? 2 : 1;
            g_localId = g_isHost ? 1 : 2;
            g_fakeMember = ok && !g_isHost && autostart;
        } else if (std::sscanf(line, "udp %d %63s %d", &localPort, ip, &peerPort) == 3) {
            ok = StartUdp(localPort, ip, peerPort, log);
            g_isHost = forceHost;
            if (!forceHost) g_hostPeer = 0;
            g_peers[0].steamId = forceHost ? 2 : 1;
            g_localId = forceHost ? 1 : 2;
            if (ok) g_mode = Mode::Udp;
        } else if (std::sscanf(line, "steam %llu", &peer) == 1) {
            ok = StartSteam(peer, log);
            if (ok) g_mode = Mode::Steam;
            // ohne Angabe: die kleinere Steam-ID ist Host (auf beiden Seiten gleich entschieden)
            const uint64_t own = OwnSteamId();
            g_isHost = forceHost ? true : forceGuest ? false : (own != 0 && own < peer);
            if (!g_isHost) g_hostPeer = 0;
        } else {
            log(L("[Netz] Unbekannte Zeile in smcoop_net.txt: %s", "[Net] unknown line in smcoop_net.txt: %s"), line);
        }
        break;
    }
    if (f) std::fclose(f);
    g_running = ok;
    if (ok) log(L("[Netz] Rolle: %s\n", "[Net] role: %s\n"), g_isHost ? L("HOST (deine Welt gilt)", "HOST (your world counts)") : L("GAST (uebernimmt die Welt des Hosts)", "GUEST (takes over the host's world)"));
    return ok;
}

bool IsRunning() { return g_running; }
bool IsHost() { return g_isHost; }
int HostPeer() { return g_hostPeer; }
uint64_t LocalId() { return g_localId; }

uint64_t PeerId(int i) {
    if (i < 0 || i >= kMaxPeers) return 0;
    AcquireSRWLockShared(&g_peerLock);
    const uint64_t id = g_peers[i].used ? g_peers[i].steamId : 0;
    ReleaseSRWLockShared(&g_peerLock);
    return id;
}

bool PeerConnected(int i) {
    if (i < 0 || i >= kMaxPeers) return false;
    AcquireSRWLockShared(&g_peerLock);
    const bool used = g_peers[i].used;
    ReleaseSRWLockShared(&g_peerLock);
    return used;
}

void PeerName(int i, char* out, size_t size) {
    out[0] = 0;
    if (i < 0 || i >= kMaxPeers) return;
    AcquireSRWLockShared(&g_peerLock);
    if (g_peers[i].used) std::snprintf(out, size, "%s", g_peers[i].name);
    ReleaseSRWLockShared(&g_peerLock);
}

void Send(const void* data, size_t size) {
    AcquireSRWLockShared(&g_peerLock);
    for (const PeerSlot& s : g_peers) {
        if (!s.used) continue;
        if (g_mode == Mode::Udp && g_sock != INVALID_SOCKET) {
            sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_port = htons(s.udpPort);
            to.sin_addr = g_udpPeerAddr;
            sendto(g_sock, static_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        } else if (g_mode == Mode::Steam && g_messages && !g_steamDown) {
            g_send(g_messages, s.id, data, static_cast<uint32_t>(size), kSendUnreliableNoNagle, kChannel);
        }
    }
    ReleaseSRWLockShared(&g_peerLock);
}

size_t Receive(void* buffer, size_t capacity, int* from) {
    *from = -1;
    if (g_mode == Mode::Udp && g_sock != INVALID_SOCKET) {
        for (;;) {
            sockaddr_in src = {};
            int len = sizeof(src);
            const int n = recvfrom(g_sock, static_cast<char*>(buffer), static_cast<int>(capacity), 0, reinterpret_cast<sockaddr*>(&src), &len);
            if (n <= 0) return 0;
            AcquireSRWLockShared(&g_peerLock);
            for (int k = 0; k < kMaxPeers; ++k)
                if (g_peers[k].used && g_peers[k].udpPort == ntohs(src.sin_port)) *from = k;
            ReleaseSRWLockShared(&g_peerLock);
            if (*from >= 0) return static_cast<size_t>(n);  // unbekannter Absender: verwerfen
        }
    }
    if (g_mode == Mode::Steam && g_messages && !g_steamDown) {
        // Sitzungen der Mitspieler regelmaessig ausdruecklich annehmen (sonst verwirft Steam ihre Pakete,
        // solange wir ihnen selbst noch nichts geschickt haben)
        if (GetTickCount() - g_lastAccept > 1000) {
            AcquireSRWLockShared(&g_peerLock);
            for (const PeerSlot& s : g_peers)
                if (s.used) g_accept(g_messages, s.id);
            ReleaseSRWLockShared(&g_peerLock);
            g_lastAccept = GetTickCount();
        }
        for (;;) {
            SteamNetworkingMessage* msg = nullptr;
            if (g_receive(g_messages, kChannel, &msg, 1) != 1 || !msg) return 0;
            const uint64_t sender = g_idGet ? g_idGet(&msg->m_identityPeer) : 0;
            AcquireSRWLockShared(&g_peerLock);
            for (int k = 0; k < kMaxPeers; ++k)
                if (g_peers[k].used && g_peers[k].steamId == sender) *from = k;
            ReleaseSRWLockShared(&g_peerLock);
            size_t n = 0;
            if (*from >= 0 && msg->m_cbSize > 0 && static_cast<size_t>(msg->m_cbSize) <= capacity) {
                n = static_cast<size_t>(msg->m_cbSize);
                std::memcpy(buffer, msg->m_pData, n);
            }
            g_release(msg);
            if (n) return n;  // unbekannter Absender / leer: naechste Nachricht
        }
    }
    return 0;
}

void SteamShutdown() {
    AcquireSRWLockExclusive(&g_steamGate);
    g_steamDown = true;
    ReleaseSRWLockExclusive(&g_steamGate);
}

bool SteamWorkBegin() {
    AcquireSRWLockShared(&g_steamGate);
    if (!g_steamDown) return true;
    ReleaseSRWLockShared(&g_steamGate);
    return false;
}

void SteamWorkEnd() { ReleaseSRWLockShared(&g_steamGate); }

void LogSteamInfo(LogFn log) {
    auto getUser = SteamFn<FnGetUser>("SteamAPI_SteamUser_v021");
    auto getId = SteamFn<FnGetSteamId>("SteamAPI_ISteamUser_GetSteamID");
    if (!getUser || !getId) { log(L("[Steam] Funktionen nicht gefunden\n", "[Steam] functions not found\n")); return; }
    void* user = getUser();
    if (!user) { log(L("[Steam] nicht initialisiert\n", "[Steam] not initialized\n")); return; }
    log(L("[Steam] Deine Steam-ID: %llu\n", "[Steam] your Steam ID: %llu\n"), static_cast<unsigned long long>(getId(user)));
}

// ---------------- Steam-Lobby ----------------
// Flache API: ISteamMatchmaking (Lobby), ISteamFriends (Overlay-Einladung, Namen), ISteamUtils (Ergebnis von
// CreateLobby abfragen). Rueckrufe (Einladung angenommen, Lobby betreten, Mitglied kam/ging) meldet Steam ueber
// SteamAPI_RegisterCallback; das Spiel ruft SteamAPI_RunCallbacks jedes Bild (Spiel-Thread).
namespace {

using FnIface = void* (*)();
using FnCreateLobby = uint64_t (*)(void*, int type, int maxMembers);
using FnJoinLobby = uint64_t (*)(void*, uint64_t lobby);
using FnLeaveLobby = void (*)(void*, uint64_t lobby);
using FnNumMembers = int (*)(void*, uint64_t lobby);
using FnMemberByIndex = uint64_t (*)(void*, uint64_t lobby, int index);
using FnLobbyOwner = uint64_t (*)(void*, uint64_t lobby);
using FnSetLobbyData = bool (*)(void*, uint64_t lobby, const char* key, const char* value);
using FnGetLobbyData = const char* (*)(void*, uint64_t lobby, const char* key);
using FnInviteDialog = void (*)(void*, uint64_t lobby);
using FnSetRichPresence = bool (*)(void*, const char* key, const char* value);
using FnCallCompleted = bool (*)(void*, uint64_t call, bool* failed);
using FnCallResult = bool (*)(void*, uint64_t call, void* out, int size, int expected, bool* failed);
using FnRegisterCallback = void (*)(void* callback, int id);

constexpr int kLobbyTypeFriendsOnly = 1;
constexpr int kCbGameLobbyJoinRequested = 333;  // k_iSteamFriendsCallbacks + 33
constexpr int kCbGameRichPresenceJoinRequested = 337;  // "Spiel beitreten" in der Freundesliste bei laufendem Spiel
constexpr int kCbLobbyEnter = 504;              // k_iSteamMatchmakingCallbacks + 4
constexpr int kCbLobbyChatUpdate = 506;
constexpr int kCbLobbyCreated = 513;
const char* const kModVersion = __DATE__ " " __TIME__;  // alle brauchen dieselbe Mod-Version

#pragma pack(push, 8)  // wie steam_api.h unter Windows
struct GameLobbyJoinRequested { uint64_t lobby; uint64_t fromFriend; };
struct GameRichPresenceJoinRequested { uint64_t fromFriend; char connect[256]; };
struct LobbyEnter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };
struct LobbyChatUpdate { uint64_t lobby; uint64_t userChanged; uint64_t makingChange; uint32_t change; };
struct LobbyCreated { int result; uint64_t lobby; };
#pragma pack(pop)

// gleiche Deklaration wie CCallbackBase im Steamworks-SDK (MSVC legt die vtable dann genauso an)
class SteamCallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    uint8_t m_nCallbackFlags = 0;
    int m_iCallback = 0;
};

template <typename T>
class SteamCallback : public SteamCallbackBase {
public:
    using Fn = void (*)(const T&);
    explicit SteamCallback(Fn fn) : m_fn(fn) {}
    void Run(void* param) override { m_fn(*static_cast<const T*>(param)); }
    void Run(void* param, bool, uint64_t) override { m_fn(*static_cast<const T*>(param)); }
    int GetCallbackSizeBytes() override { return sizeof(T); }
    Fn m_fn;
};

SRWLOCK g_lobbyLock = SRWLOCK_INIT;
LobbyState g_lobbyState = LobbyState::Off;  // (g_lobbyLock)
uint64_t g_lobbyId = 0;                     // (g_lobbyLock)
uint64_t g_lobbyCall = 0;                   // laufender CreateLobby-Aufruf (g_lobbyLock)
char g_lobbyOwnerName[64] = {};             // (g_lobbyLock)
bool g_lobbyVersionMismatch = false;        // (g_lobbyLock)
uint64_t g_lobbyMembersKey = 0;             // fuer "hat sich etwas geaendert" (g_lobbyLock)
std::atomic<bool> g_lobbyDirty{false};      // Mitglieder neu einlesen

// nullptr nach SteamShutdown: alle Lobby-Aufrufe fallen dann weg
void* Matchmaking() {
    static auto get = SteamFn<FnIface>("SteamAPI_SteamMatchmaking_v009");
    return get && !g_steamDown ? get() : nullptr;
}
void* Friends() {
    static auto get = SteamFn<FnIface>("SteamAPI_SteamFriends_v017");
    return get && !g_steamDown ? get() : nullptr;
}
void* Utils() {
    static auto get = SteamFn<FnIface>("SteamAPI_SteamUtils_v010");
    return get && !g_steamDown ? get() : nullptr;
}

void FriendName(uint64_t id, char* out, size_t size) {
    static auto name = SteamFn<FnFriendName>("SteamAPI_ISteamFriends_GetFriendPersonaName");
    void* friends = Friends();
    const char* n = friends && name && id ? name(friends, id) : "";
    std::snprintf(out, size, "%s", n ? n : "");
}

void SetRichPresenceConnect(uint64_t lobby) {
    static auto set = SteamFn<FnSetRichPresence>("SteamAPI_ISteamFriends_SetRichPresence");
    void* friends = Friends();
    if (!set || !friends) return;
    char value[64];
    std::snprintf(value, sizeof(value), "+connect_lobby %llu", static_cast<unsigned long long>(lobby));
    set(friends, "connect", lobby ? value : "");  // "Spiel beitreten" in der Steam-Freundesliste
}

// Mitspieler = alle anderen Mitglieder; Plaetze, Rolle und Host nachfuehren
void RefreshLobbyMembers() {
    static auto num = SteamFn<FnNumMembers>("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    static auto member = SteamFn<FnMemberByIndex>("SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    static auto owner = SteamFn<FnLobbyOwner>("SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    static auto getData = SteamFn<FnGetLobbyData>("SteamAPI_ISteamMatchmaking_GetLobbyData");
    void* mm = Matchmaking();
    AcquireSRWLockShared(&g_lobbyLock);
    const uint64_t lobby = g_lobbyId;
    const LobbyState state = g_lobbyState;
    ReleaseSRWLockShared(&g_lobbyLock);
    if (!mm || !num || !member || !owner || !lobby || (state != LobbyState::Owner && state != LobbyState::Member)) return;
    const uint64_t own = OwnSteamId();
    const uint64_t ownerId = owner(mm, lobby);
    uint64_t ids[kMaxPeers] = {};
    char names[kMaxPeers][64] = {};
    int count = 0;
    uint64_t key = ownerId;
    const int n = num(mm, lobby);
    for (int i = 0; i < n && count < kMaxPeers; ++i) {
        const uint64_t m = member(mm, lobby, i);
        if (!m || m == own) continue;
        ids[count] = m;
        FriendName(m, names[count], sizeof(names[count]));
        key = key * 1000003u ^ m;
        ++count;
    }
    const char* ver = getData ? getData(mm, lobby, "smcoop") : "";
    char ownerName[64];
    FriendName(ownerId, ownerName, sizeof(ownerName));
    AcquireSRWLockExclusive(&g_lobbyLock);
    const bool changed = key != g_lobbyMembersKey;
    g_lobbyMembersKey = key;
    g_lobbyState = ownerId == own ? LobbyState::Owner : LobbyState::Member;
    std::snprintf(g_lobbyOwnerName, sizeof(g_lobbyOwnerName), "%s", ownerName);
    g_lobbyVersionMismatch = ownerId != own && ver && ver[0] && std::strcmp(ver, kModVersion) != 0;
    ReleaseSRWLockExclusive(&g_lobbyLock);
    if (!changed) return;
    SetSteamPeers(ids, names, count, ownerId, ownerId == own);
    if (g_log) {
        g_log(L("[Lobby] %d Mitspieler, du bist %s:", "[Lobby] %d players, you are %s:"), count, ownerId == own ? "HOST" : L("GAST", "GUEST"));
        for (int i = 0; i < count; ++i) g_log(L(" %s%s", " %s%s"), names[i], ids[i] == ownerId ? " (Host)" : "");
        g_log(L("\n", "\n"));
    }
}

void OnJoinRequested(const GameLobbyJoinRequested& e) {
    if (g_log) g_log(L("[Lobby] Einladung angenommen - trete Lobby %llu bei\n", "[Lobby] invite accepted - joining lobby %llu\n"), static_cast<unsigned long long>(e.lobby));
    LobbyLeave();
    static auto join = SteamFn<FnJoinLobby>("SteamAPI_ISteamMatchmaking_JoinLobby");
    void* mm = Matchmaking();
    if (!mm || !join) return;
    join(mm, e.lobby);
    AcquireSRWLockExclusive(&g_lobbyLock);
    g_lobbyState = LobbyState::Joining;
    ReleaseSRWLockExclusive(&g_lobbyLock);
}

// "Spiel beitreten" beim Host in der Freundesliste, waehrend das Spiel schon laeuft (z.B. nach einem Absturz neu gestartet):
// Steam schickt dann den Rich-Presence-Text "+connect_lobby <id>" statt einer Lobby-Einladung
void OnRichPresenceJoinRequested(const GameRichPresenceJoinRequested& e) {
    char connect[sizeof(e.connect) + 1] = {};
    std::memcpy(connect, e.connect, sizeof(e.connect));
    const uint64_t lobby = coop::ParseConnectLobby(connect);
    if (g_log) g_log(L("[Lobby] Beitritt ueber die Freundesliste (%s)\n", "[Lobby] joining via the friends list (%s)\n"), lobby ? L("Lobby gefunden", "lobby found") : L("ohne Lobby", "without lobby"));
    if (!lobby) return;
    GameLobbyJoinRequested j{lobby, e.fromFriend};
    OnJoinRequested(j);
}

void OnLobbyEnter(const LobbyEnter& e) {
    if (e.response != 1) {  // k_EChatRoomEnterResponseSuccess (sonst z.B. voll)
        if (g_log) g_log(L("[Lobby] Beitreten fehlgeschlagen (Antwort %u, z.B. Lobby voll)\n", "[Lobby] joining failed (response %u, e.g. lobby full)\n"), e.response);
        AcquireSRWLockExclusive(&g_lobbyLock);
        g_lobbyState = LobbyState::Off;
        ReleaseSRWLockExclusive(&g_lobbyLock);
        return;
    }
    AcquireSRWLockExclusive(&g_lobbyLock);
    g_lobbyId = e.lobby;
    g_lobbyState = LobbyState::Member;  // RefreshLobbyMembers entscheidet Besitzer/Mitglied
    g_lobbyMembersKey = 0;
    ReleaseSRWLockExclusive(&g_lobbyLock);
    SetRichPresenceConnect(e.lobby);
    if (g_log) g_log(L("[Lobby] Lobby %llu betreten\n", "[Lobby] entered lobby %llu\n"), static_cast<unsigned long long>(e.lobby));
    g_lobbyDirty = true;
}

void OnLobbyChatUpdate(const LobbyChatUpdate& e) {
    if (g_log) g_log(L("[Lobby] Mitglied %llu: %s\n", "[Lobby] member %llu: %s\n"), static_cast<unsigned long long>(e.userChanged),
                     (e.change & 1) ? "beigetreten" : "gegangen");
    g_lobbyDirty = true;
}

SteamCallback<GameLobbyJoinRequested> g_cbJoinRequested(OnJoinRequested);
SteamCallback<GameRichPresenceJoinRequested> g_cbRichPresenceJoin(OnRichPresenceJoinRequested);
SteamCallback<LobbyEnter> g_cbLobbyEnter(OnLobbyEnter);
SteamCallback<LobbyChatUpdate> g_cbLobbyChatUpdate(OnLobbyChatUpdate);

}  // namespace

bool LobbyMode() { return g_lobbyMode; }
bool FakeLobbyMember() { return g_fakeMember; }
bool FakeLobbyAsk() { return g_fakeAsk; }

void LobbyInit() {
    static bool done = false;
    if (done || !g_lobbyMode) return;
    done = true;
    auto reg = SteamFn<FnRegisterCallback>("SteamAPI_RegisterCallback");
    if (!reg || !Matchmaking()) {
        if (g_log) g_log(L("[Lobby] Steam-Lobby nicht verfuegbar\n", "[Lobby] Steam lobby not available\n"));
        return;
    }
    reg(&g_cbJoinRequested, kCbGameLobbyJoinRequested);
    reg(&g_cbRichPresenceJoin, kCbGameRichPresenceJoinRequested);
    reg(&g_cbLobbyEnter, kCbLobbyEnter);
    reg(&g_cbLobbyChatUpdate, kCbLobbyChatUpdate);
    if (g_log) g_log(L("[Lobby] bereit - Koop-Lobby im Hauptmenue erstellen oder eine Steam-Einladung annehmen\n", "[Lobby] ready - create a co-op lobby in the main menu or accept a Steam invite\n"));
    // Steam hat das Spiel fuer eine Einladung gestartet: "+connect_lobby <id>" in der Befehlszeile
    if (const uint64_t lobby = coop::ParseConnectLobby(GetCommandLineA())) {
        GameLobbyJoinRequested e{lobby, 0};
        OnJoinRequested(e);
    }
}

void LobbyTick() {
    if (!g_lobbyMode) return;
    static auto completed = SteamFn<FnCallCompleted>("SteamAPI_ISteamUtils_IsAPICallCompleted");
    static auto result = SteamFn<FnCallResult>("SteamAPI_ISteamUtils_GetAPICallResult");
    static auto setData = SteamFn<FnSetLobbyData>("SteamAPI_ISteamMatchmaking_SetLobbyData");
    AcquireSRWLockShared(&g_lobbyLock);
    const uint64_t call = g_lobbyCall;
    ReleaseSRWLockShared(&g_lobbyLock);
    if (call && completed && result && Utils()) {
        bool failed = false;
        if (completed(Utils(), call, &failed)) {
            LobbyCreated r{};
            const bool ok = result(Utils(), call, &r, sizeof(r), kCbLobbyCreated, &failed) && !failed && r.result == 1;
            AcquireSRWLockExclusive(&g_lobbyLock);
            g_lobbyCall = 0;
            if (ok) {
                g_lobbyId = r.lobby;
                g_lobbyState = LobbyState::Owner;
                g_lobbyMembersKey = 0;
            } else {
                g_lobbyState = LobbyState::Off;
            }
            ReleaseSRWLockExclusive(&g_lobbyLock);
            if (ok && setData && Matchmaking()) setData(Matchmaking(), r.lobby, "smcoop", kModVersion);
            if (ok) SetRichPresenceConnect(r.lobby);
            if (g_log) {
                if (ok) g_log(L("[Lobby] erstellt (%llu, bis %d Spieler) - du bist Host, jetzt Freunde einladen\n", "[Lobby] created (%llu, up to %d players) - you are the host, now invite friends\n"),
                              static_cast<unsigned long long>(r.lobby), kMaxPeers + 1);
                else g_log(L("[Lobby] Erstellen fehlgeschlagen (Ergebnis %d)\n", "[Lobby] creating failed (result %d)\n"), r.result);
            }
            g_lobbyDirty = true;
        }
    }
    static DWORD lastRefresh = 0;
    if (g_lobbyDirty.exchange(false) || GetTickCount() - lastRefresh > 3000) {
        lastRefresh = GetTickCount();
        RefreshLobbyMembers();
    }
}

void LobbyCreate() {
    static auto create = SteamFn<FnCreateLobby>("SteamAPI_ISteamMatchmaking_CreateLobby");
    void* mm = Matchmaking();
    if (!g_lobbyMode || !mm || !create) return;
    AcquireSRWLockExclusive(&g_lobbyLock);
    const bool busy = g_lobbyState != LobbyState::Off;
    if (!busy) g_lobbyState = LobbyState::Creating;
    ReleaseSRWLockExclusive(&g_lobbyLock);
    if (busy) return;
    const uint64_t call = create(mm, kLobbyTypeFriendsOnly, kMaxPeers + 1);
    AcquireSRWLockExclusive(&g_lobbyLock);
    g_lobbyCall = call;
    if (!call) g_lobbyState = LobbyState::Off;
    ReleaseSRWLockExclusive(&g_lobbyLock);
    if (g_log) g_log(L("[Lobby] wird erstellt ...\n", "[Lobby] creating ...\n"));
}

void LobbyInviteDialog() {
    static auto invite = SteamFn<FnInviteDialog>("SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    AcquireSRWLockShared(&g_lobbyLock);
    const uint64_t lobby = g_lobbyId;
    ReleaseSRWLockShared(&g_lobbyLock);
    if (invite && Friends() && lobby) invite(Friends(), lobby);
}

void LobbyLeave() {
    static auto leave = SteamFn<FnLeaveLobby>("SteamAPI_ISteamMatchmaking_LeaveLobby");
    AcquireSRWLockExclusive(&g_lobbyLock);
    const uint64_t lobby = g_lobbyId;
    g_lobbyId = 0;
    g_lobbyCall = 0;
    g_lobbyOwnerName[0] = 0;
    g_lobbyVersionMismatch = false;
    g_lobbyMembersKey = 0;
    const bool was = g_lobbyState != LobbyState::Off;
    g_lobbyState = LobbyState::Off;
    ReleaseSRWLockExclusive(&g_lobbyLock);
    if (!was) return;
    if (lobby && leave && Matchmaking()) leave(Matchmaking(), lobby);
    SetRichPresenceConnect(0);
    SetSteamPeers(nullptr, nullptr, 0, 0, false);
    if (g_log) g_log(L("[Lobby] verlassen\n", "[Lobby] left\n"));
}

LobbyState GetLobbyState() {
    AcquireSRWLockShared(&g_lobbyLock);
    const LobbyState s = g_lobbyState;
    ReleaseSRWLockShared(&g_lobbyLock);
    return s;
}

void LobbyOwnerName(char* owner, size_t ownerSize, bool* versionMismatch) {
    AcquireSRWLockShared(&g_lobbyLock);
    std::snprintf(owner, ownerSize, "%s", g_lobbyOwnerName);
    *versionMismatch = g_lobbyVersionMismatch;
    ReleaseSRWLockShared(&g_lobbyLock);
}

void LobbySetValue(const char* key, const char* value) {
    static auto setData = SteamFn<FnSetLobbyData>("SteamAPI_ISteamMatchmaking_SetLobbyData");
    AcquireSRWLockShared(&g_lobbyLock);
    const uint64_t lobby = g_lobbyState == LobbyState::Owner ? g_lobbyId : 0;
    ReleaseSRWLockShared(&g_lobbyLock);
    if (lobby && setData && Matchmaking()) setData(Matchmaking(), lobby, key, value);
}

bool LobbyGetValue(const char* key, char* out, size_t size) {
    static auto getData = SteamFn<FnGetLobbyData>("SteamAPI_ISteamMatchmaking_GetLobbyData");
    AcquireSRWLockShared(&g_lobbyLock);
    const uint64_t lobby = g_lobbyId;
    ReleaseSRWLockShared(&g_lobbyLock);
    const char* v = lobby && getData && Matchmaking() ? getData(Matchmaking(), lobby, key) : nullptr;
    std::snprintf(out, size, "%s", v ? v : "");
    return v && v[0];
}

}  // namespace net
