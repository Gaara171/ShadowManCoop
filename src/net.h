// Netzwerkschicht des Co-op-Mods: verschickt Posen und empfaengt die des Mitspielers.
//
// Konfiguration in smcoop_net.txt (im Spielordner), eine Zeile:
//   lobby                                     Steam-Lobby aus dem Hauptmenue (Standard, auch wenn die Datei fehlt)
//   udp <eigenerPort> <peerIP> <peerPort>     z.B. "udp 27700 127.0.0.1 27700" (Test auf einem PC: Echo an sich selbst)
//   steam <SteamID64 des Mitspielers>         Steam-P2P (keine Portfreigabe noetig)
//   udppair <PortA> <PortB>                   zwei Instanzen auf einem PC (die erste ist Host)
//   udpmesh <Port> <Anzahl>                   bis zu 4 Instanzen auf einem PC (Ports Port..Port+Anzahl-1, die erste ist Host)
// Optional am Zeilenende "host" oder "gast" (sonst: Steam = kleinere Steam-ID ist Host, udp = Gast).
// Bei udppair/udpmesh zusaetzlich "autostart": Gaeste verhalten sich wie Lobby-Mitglieder (Test des automatischen Starts).
#pragma once
#include <cstddef>
#include <cstdint>

namespace net {

// bis zu 3 Mitspieler (4 Spieler); jeder Mitspieler hat einen festen Platz 0..kMaxPeers-1, solange er da ist
constexpr int kMaxPeers = 3;

using LogFn = void (*)(const char* fmt, ...);

// Liest smcoop_net.txt und startet den Transport. Gibt false zurueck, wenn nichts konfiguriert ist.
bool Start(LogFn log);
bool IsRunning();
// Host = seine Welt gilt (Gast uebernimmt Welt-Zustand, folgt ins Level, Gegner-Positionen kommen vom Host)
bool IsHost();
// Platz des Hosts unter den Mitspielern (-1 = ich bin Host oder keiner da)
int HostPeer();
bool PeerConnected(int peer);
uint64_t LocalId();         // eigene Kennung: Steam-ID (UDP-Test: Instanznummer 1..4)
uint64_t PeerId(int peer);  // Kennung des Mitspielers auf Platz peer, 0 = frei
void PeerName(int peer, char* out, size_t size);
// Unzuverlaessig an alle Mitspieler senden (alte Posen sind wertlos, also kein Wiederholen)
void Send(const void* data, size_t size);
// Ein empfangenes Paket abholen; gibt die Groesse zurueck oder 0, wenn nichts da ist; *from = Platz des Absenders
size_t Receive(void* buffer, size_t capacity, int* from);
// Eigene Steam-ID und Freundesliste ins Log schreiben (hilft beim Ausfuellen von smcoop_net.txt)
void LogSteamInfo(LogFn log);
// Das Spiel beendet Steam (SteamAPI_Shutdown, danach ist steamclient64.dll entladen): wartet, bis der Netz-Thread aus seinem
// Steam-Aufruf heraus ist; danach ruft die Mod Steam nicht mehr auf
void SteamShutdown();
// Netz-Thread: um jeden Durchlauf mit Steam-Aufrufen; false = Steam ist schon beendet (dann nichts tun, kein SteamWorkEnd)
bool SteamWorkBegin();
void SteamWorkEnd();

// ---- Steam-Lobby ("lobby" in smcoop_net.txt) ----
// Wer die Lobby erstellt, ist Host; Mitspieler sind alle anderen Mitglieder (bis zu 3). Einladen ueber das Steam-Overlay, beitreten
// ueber die Einladung (auch wenn Steam das Spiel dafuer erst startet).
enum class LobbyState { Off, Creating, Joining, Owner, Member };
bool LobbyMode();
// einmal nach Start(), wenn Steam laeuft: Rueckrufe anmelden, Startparameter +connect_lobby auswerten
void LobbyInit();
// regelmaessig (Netz-Thread): Ergebnisse von Steam abholen, Mitspieler/Rolle nachfuehren
void LobbyTick();
void LobbyCreate();
void LobbyInviteDialog();
void LobbyLeave();
LobbyState GetLobbyState();
// Name des Hosts ("" = keiner), true wenn der Host eine andere Mod-Version hat (Mitspieler: PeerName)
void LobbyOwnerName(char* owner, size_t ownerSize, bool* versionMismatch);
// Host: Wert in der Lobby ablegen / Gast: lesen (z.B. Schwierigkeit); "" = nicht gesetzt
void LobbySetValue(const char* key, const char* value);
bool LobbyGetValue(const char* key, char* out, size_t size);
// Test (udppair ... autostart): Gast-Instanz gilt als Lobby-Mitglied
bool FakeLobbyMember();
bool FakeLobbyAsk();  // UDP-Test "frage": Gast wird wie in der Lobby gefragt, womit er beitritt

}  // namespace net
