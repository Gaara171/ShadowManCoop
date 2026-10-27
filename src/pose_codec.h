// Kompaktes Netzwerkformat fuer eine Spieler-Pose (Weltlage + fertige Knochen).
//
// Eine Pose im Spiel besteht aus der Weltmatrix des Spielers (+0x200) und bis zu 128 Knochen zu je
// 16 floats (Quaternion, Weltposition, Vorframe-Quaternion, Vorframe-Position). Uebertragen wird nur der
// aktuelle Teil; "Vorframe" baut der Empfaenger aus der zuletzt angewendeten Pose.
//
// Paket (little endian):
//   PacketHeader | 3 floats Wurzelposition | 4 x int16 Wurzel-Quaternion | numBones x PackedBone
//   [optional: 'K' | Anzahl Haende | Anzahl x PackedHand]   (aeltere Empfaenger ignorieren Anhaenge)
//   [optional: 'S' | Anzahl Schuesse | Anzahl x PackedShot] (die letzten Schuesse, mehrfach gesendet gegen Verlust)
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace posecodec {

constexpr uint32_t kMagic = 0x32434D53;  // "SMC2" (Version 2: Level im Header)
constexpr int kMaxBones = 128;
constexpr float kBonePosScale = 16.0f;   // Knochenposition relativ zur Wurzel in 1/16 Einheiten (+-2048)

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint32_t seq;
    uint32_t timeMs;     // Sendezeit (Uhr des Senders)
    uint8_t numBones;
    uint8_t flags;       // Bit 0: Sender ist im Spiel (nicht im Hauptmenue)
    uint16_t level;      // Level-Nummer des Senders
};
constexpr uint8_t kFlagInGame = 0x01;
struct PackedBone {
    int16_t q[4];
    int16_t p[3];
};
// Gegenstand in einer Hand: Typ (Knoten +0x20, -1 = leer), Lage relativ zum Handknochen
// (3x4, Zeilen X/Y/Z-Achse und Position, im Koordinatensystem des Knochens) und Aufladung (Shadowgun, 0 = keine)
struct PackedHand {
    int16_t type;
    float rel[12];
    float charge;
};
// Ein abgefeuertes Geschoss (Argumente der Spielfunktion 0x25EF10), Positionen in Weltkoordinaten
struct PackedShot {
    uint16_t seq;        // fortlaufend, zum Aussortieren doppelt empfangener Schuesse
    uint32_t timeMs;     // Zeitpunkt (Uhr des Senders)
    int16_t type;        // Geschoss-Typ (Tabelle 0xC8E260 + Typ * 0x40)
    int16_t sector;
    float pos[3];
    float dir[3];
    int32_t power;       // Ladung beim Schuss (global 0xDB220C, Shadowgun-Staerke)
};
#pragma pack(pop)

// Schiebeblock, den der Sender gerade bewegt (mit jeder Pose, damit er beim anderen fluessig mitlaeuft); 16 Bytes ohne Luecken
struct PackedBlock {
    uint16_t serial;
    uint16_t version;
    float pos[3];
};
static_assert(sizeof(PackedBlock) == 16, "PackedBlock");

constexpr uint8_t kHandsTag = 'K';
constexpr uint8_t kShotsTag = 'S';
constexpr uint8_t kBlocksTag = 'B';
constexpr uint8_t kSectorTag = 'Z';  // Sektor des Spielers (+0xD4), 1 Eintrag int32
constexpr uint8_t kLookTag = 'F';    // Aussehen: Form (+0x1D184) int32 + Geheimnis-Bits uint32, 1 Eintrag
constexpr int kNumHands = 2;
constexpr int kMaxShots = 8;
constexpr int kMaxMovingBlocks = 4;
struct BlockState {
    uint16_t serial = 0;
    uint16_t version = 0;
    float pos[3] = {};
};
struct HandItem {
    int16_t type = -1;
    float rel[12] = {};
    float charge = 0.0f;
};
struct ShotEvent {
    uint16_t seq = 0;
    uint32_t timeMs = 0;
    int16_t type = 0;
    int16_t sector = 0;
    float pos[3] = {};
    float dir[3] = {};
    int32_t power = 0;
};

constexpr size_t MaxPacketSize() {
    return sizeof(PacketHeader) + 12 + 8 + kMaxBones * sizeof(PackedBone) + 2 + kNumHands * sizeof(PackedHand) + 2 +
           kMaxShots * sizeof(PackedShot) + 2 + kMaxMovingBlocks * sizeof(PackedBlock) + 2 + 4 + 2 + 8;
}

// Entpackte Pose: nur aktueller Teil
struct NetPose {
    uint32_t seq = 0;
    uint32_t timeMs = 0;
    uint32_t numBones = 0;
    uint16_t level = 0;
    bool inGame = false;
    float rootPos[3] = {};
    float rootRot[4] = {0, 0, 0, 1};       // Quaternion der Weltmatrix
    float boneRot[kMaxBones][4] = {};
    float bonePos[kMaxBones][3] = {};
    HandItem hands[kNumHands];
    ShotEvent shots[kMaxShots];
    int numShots = 0;
    BlockState blocks[kMaxMovingBlocks];
    int numBlocks = 0;
    int32_t sector = -1;  // -1 = nicht mitgeschickt
    int32_t form = -1;    // -1 = nicht mitgeschickt
    uint32_t looks = 0;   // Bits der Aussehens-Geheimnisse
};

inline int16_t PackUnit(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}
inline float UnpackUnit(int16_t v) { return static_cast<float>(v) / 32767.0f; }

inline int16_t PackPos(float v) {
    float s = v * kBonePosScale;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32767.0f) s = -32767.0f;
    return static_cast<int16_t>(std::lround(s));
}

// Rotationsteil einer zeilenweisen 4x4-Matrix (Zeilen = Achsen) -> Quaternion (x, y, z, w)
inline void MatrixToQuat(const float* m, float q[4]) {
    const float m00 = m[0], m01 = m[1], m02 = m[2];
    const float m10 = m[4], m11 = m[5], m12 = m[6];
    const float m20 = m[8], m21 = m[9], m22 = m[10];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (m12 - m21) / s; q[1] = (m20 - m02) / s; q[2] = (m01 - m10) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m12 - m21) / s; q[0] = 0.25f * s; q[1] = (m10 + m01) / s; q[2] = (m20 + m02) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m20 - m02) / s; q[0] = (m10 + m01) / s; q[1] = 0.25f * s; q[2] = (m21 + m12) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m01 - m10) / s; q[0] = (m20 + m02) / s; q[1] = (m21 + m12) / s; q[2] = 0.25f * s;
    }
}

inline void QuatToMatrix(const float q[4], float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + w * z);     m[2] = 2 * (x * z - w * y);     m[3] = 0;
    m[4] = 2 * (x * y - w * z);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + w * x);     m[7] = 0;
    m[8] = 2 * (x * z + w * y);     m[9] = 2 * (y * z - w * x);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
}

// world: 16 floats (+0x200), bones: numBones x 16 floats (Spielformat), hands: kNumHands Eintraege oder nullptr,
// shots: bis zu kMaxShots Schuesse. Gibt Paketgroesse zurueck.
inline size_t Encode(uint32_t seq, uint32_t timeMs, uint16_t level, bool inGame, const float* world, const float* bones,
                     uint32_t numBones, const HandItem* hands, const ShotEvent* shots, int numShots, const BlockState* blocks,
                     int numBlocks, int32_t sector, int32_t form, uint32_t looks, uint8_t* out) {
    if (numBones > kMaxBones) numBones = kMaxBones;
    PacketHeader h{kMagic, seq, timeMs, static_cast<uint8_t>(numBones), static_cast<uint8_t>(inGame ? kFlagInGame : 0), level};
    uint8_t* p = out;
    std::memcpy(p, &h, sizeof(h)); p += sizeof(h);
    const float root[3] = {world[12], world[13], world[14]};
    std::memcpy(p, root, 12); p += 12;
    float rq[4];
    MatrixToQuat(world, rq);
    int16_t prq[4] = {PackUnit(rq[0]), PackUnit(rq[1]), PackUnit(rq[2]), PackUnit(rq[3])};
    std::memcpy(p, prq, 8); p += 8;
    for (uint32_t b = 0; b < numBones; ++b) {
        const float* src = bones + b * 16;
        PackedBone pb;
        for (int i = 0; i < 4; ++i) pb.q[i] = PackUnit(src[i]);
        for (int i = 0; i < 3; ++i) pb.p[i] = PackPos(src[4 + i] - root[i]);
        std::memcpy(p, &pb, sizeof(pb)); p += sizeof(pb);
    }
    if (hands) {
        *p++ = kHandsTag;
        *p++ = static_cast<uint8_t>(kNumHands);
        for (int k = 0; k < kNumHands; ++k) {
            PackedHand ph;
            ph.type = hands[k].type;
            std::memcpy(ph.rel, hands[k].rel, sizeof(ph.rel));
            ph.charge = hands[k].charge;
            std::memcpy(p, &ph, sizeof(ph)); p += sizeof(ph);
        }
    }
    if (shots && numShots > 0) {
        if (numShots > kMaxShots) numShots = kMaxShots;
        *p++ = kShotsTag;
        *p++ = static_cast<uint8_t>(numShots);
        for (int k = 0; k < numShots; ++k) {
            PackedShot ps;
            ps.seq = shots[k].seq;
            ps.timeMs = shots[k].timeMs;
            ps.type = shots[k].type;
            ps.sector = shots[k].sector;
            std::memcpy(ps.pos, shots[k].pos, sizeof(ps.pos));
            std::memcpy(ps.dir, shots[k].dir, sizeof(ps.dir));
            ps.power = shots[k].power;
            std::memcpy(p, &ps, sizeof(ps)); p += sizeof(ps);
        }
    }
    if (blocks && numBlocks > 0) {
        if (numBlocks > kMaxMovingBlocks) numBlocks = kMaxMovingBlocks;
        *p++ = kBlocksTag;
        *p++ = static_cast<uint8_t>(numBlocks);
        for (int k = 0; k < numBlocks; ++k) {
            PackedBlock pb{blocks[k].serial, blocks[k].version, {blocks[k].pos[0], blocks[k].pos[1], blocks[k].pos[2]}};
            std::memcpy(p, &pb, sizeof(pb)); p += sizeof(pb);
        }
    }
    if (sector >= 0) {
        *p++ = kSectorTag;
        *p++ = 1;
        std::memcpy(p, &sector, 4); p += 4;
    }
    if (form >= 0) {
        *p++ = kLookTag;
        *p++ = 1;
        std::memcpy(p, &form, 4); p += 4;
        std::memcpy(p, &looks, 4); p += 4;
    }
    return static_cast<size_t>(p - out);
}

inline bool Decode(const uint8_t* data, size_t size, NetPose& out) {
    if (size < sizeof(PacketHeader) + 20) return false;
    PacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.numBones > kMaxBones) return false;
    if (size < sizeof(PacketHeader) + 20 + h.numBones * sizeof(PackedBone)) return false;
    const uint8_t* p = data + sizeof(h);
    out.seq = h.seq;
    out.timeMs = h.timeMs;
    out.numBones = h.numBones;
    out.level = h.level;
    out.inGame = (h.flags & kFlagInGame) != 0;
    std::memcpy(out.rootPos, p, 12); p += 12;
    int16_t prq[4];
    std::memcpy(prq, p, 8); p += 8;
    for (int i = 0; i < 4; ++i) out.rootRot[i] = UnpackUnit(prq[i]);
    for (uint32_t b = 0; b < h.numBones; ++b) {
        PackedBone pb;
        std::memcpy(&pb, p, sizeof(pb)); p += sizeof(pb);
        for (int i = 0; i < 4; ++i) out.boneRot[b][i] = UnpackUnit(pb.q[i]);
        for (int i = 0; i < 3; ++i) out.bonePos[b][i] = out.rootPos[i] + pb.p[i] / kBonePosScale;
    }
    for (auto& hi : out.hands) hi = HandItem{};
    out.numShots = 0;
    out.numBlocks = 0;
    out.sector = -1;
    out.form = -1;
    out.looks = 0;
    // Anhaenge: Kennung, Anzahl, Eintraege. Bei unbekannter Kennung oder zu kurzem Rest aufhoeren.
    const uint8_t* end = data + size;
    while (end - p >= 2) {
        const uint8_t tag = p[0];
        const int count = p[1];
        const size_t entry = tag == kHandsTag    ? sizeof(PackedHand)
                             : tag == kShotsTag  ? sizeof(PackedShot)
                             : tag == kBlocksTag ? sizeof(PackedBlock)
                             : tag == kSectorTag ? 4
                             : tag == kLookTag   ? 8
                                                 : 0;
        if (entry == 0 || static_cast<size_t>(end - p) < 2 + count * entry) break;
        p += 2;
        for (int k = 0; k < count; ++k, p += entry) {
            if (tag == kHandsTag && k < kNumHands) {
                PackedHand ph;
                std::memcpy(&ph, p, sizeof(ph));
                out.hands[k].type = ph.type;
                std::memcpy(out.hands[k].rel, ph.rel, sizeof(ph.rel));
                out.hands[k].charge = ph.charge;
            } else if (tag == kShotsTag && out.numShots < kMaxShots) {
                PackedShot ps;
                std::memcpy(&ps, p, sizeof(ps));
                ShotEvent& s = out.shots[out.numShots++];
                s.seq = ps.seq;
                s.timeMs = ps.timeMs;
                s.type = ps.type;
                s.sector = ps.sector;
                std::memcpy(s.pos, ps.pos, sizeof(s.pos));
                std::memcpy(s.dir, ps.dir, sizeof(s.dir));
                s.power = ps.power;
            } else if (tag == kBlocksTag && out.numBlocks < kMaxMovingBlocks) {
                PackedBlock pb;
                std::memcpy(&pb, p, sizeof(pb));
                BlockState& b = out.blocks[out.numBlocks++];
                b.serial = pb.serial;
                b.version = pb.version;
                std::memcpy(b.pos, pb.pos, sizeof(b.pos));
            } else if (tag == kSectorTag && k == 0) {
                std::memcpy(&out.sector, p, 4);
            } else if (tag == kLookTag && k == 0) {
                std::memcpy(&out.form, p, 4);
                std::memcpy(&out.looks, p + 4, 4);
            }
        }
    }
    return true;
}

// ---- Ausgeloeste Level-Ereignisse ("SMV1"): Schluessel (ev+0x8C, +0x90, +0x88) + Folgenummer je Sitzung ----
constexpr uint32_t kEventMagic = 0x31564D53;  // "SMV1"
constexpr int kMaxEvents = 64;
#pragma pack(push, 1)
struct EventHeader {
    uint32_t magic;
    uint32_t session;
    uint16_t level;
    uint16_t num;
};
struct EventInfo {
    int32_t key[3] = {};
    uint32_t seq = 0;
};
#pragma pack(pop)
static_assert(sizeof(EventInfo) == 16, "EventInfo");
constexpr size_t MaxEventPacketSize() { return sizeof(EventHeader) + kMaxEvents * sizeof(EventInfo); }

inline size_t EncodeEvents(uint32_t session, uint16_t level, const EventInfo* list, int num, uint8_t* out) {
    if (num > kMaxEvents) num = kMaxEvents;
    EventHeader h{kEventMagic, session, level, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, num * sizeof(EventInfo));
    return sizeof(h) + num * sizeof(EventInfo);
}

inline bool DecodeEvents(const uint8_t* data, size_t size, uint32_t& session, uint16_t& level, EventInfo* list, int& num) {
    if (size < sizeof(EventHeader)) return false;
    EventHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kEventMagic || h.num > kMaxEvents || size < sizeof(h) + h.num * sizeof(EventInfo)) return false;
    session = h.session;
    level = h.level;
    num = h.num;
    std::memcpy(list, data + sizeof(h), num * sizeof(EventInfo));
    return true;
}

// ---- Fortschritt ("SMF1"): Fortschritts-Flags (Bitfeld des SaveManagers) + einige Inventar-Zaehler ----
// Version 3 ("SMF3"): dazu je Level gesammelte Cadeaux und Dunkle Seelen (19 Level), die Einzel-Seelen-Bytes und die
// Waffen-/Gegenstands-Slots von kexShadowManInventoryLocal (30 Werte: 0 = nicht besessen, 1/2 = in der Hand, 3 = besessen)
// Version 4 ("SMF4"): dazu die gefundenen Geheimnisse (Bitmaske GameState+0x48)
constexpr uint32_t kProgressMagic = 0x34464D53;  // "SMF4"
constexpr int kMaxFlagBytes = 256;
constexpr int kProgressInvTypes = 3;               // Inventar-Typen 0 (max. Leben), 3 (Schatten-Level), 8 (Cadeaux)
constexpr int kProgressLevels = 19;                // Level-Zaehler-Tabellen (Index = Level % 19)
constexpr int kMaxSoulBytes = 256;
constexpr int kProgressItems = 30;
struct ProgressData {
    uint32_t secrets = 0;
    int32_t inv[kProgressInvTypes] = {};
    int32_t items[kProgressItems] = {};
    uint8_t cadeauxGot[kProgressLevels] = {};
    uint8_t soulsGot[kProgressLevels] = {};
    int flagBytes = 0;
    uint8_t flags[kMaxFlagBytes] = {};
    int soulBytes = 0;
    uint8_t souls[kMaxSoulBytes] = {};
};
#pragma pack(push, 1)
struct ProgressHeader {
    uint32_t magic;
    uint16_t flagBytes;
    uint16_t soulBytes;
    uint32_t secrets;
    int32_t inv[kProgressInvTypes];
    int32_t items[kProgressItems];
    uint8_t cadeauxGot[kProgressLevels];
    uint8_t soulsGot[kProgressLevels];
};
#pragma pack(pop)
constexpr size_t MaxProgressPacketSize() { return sizeof(ProgressHeader) + kMaxFlagBytes + kMaxSoulBytes; }

inline size_t EncodeProgress(const ProgressData& d, uint8_t* out) {
    const int fb = d.flagBytes > kMaxFlagBytes ? kMaxFlagBytes : d.flagBytes;
    const int sb = d.soulBytes > kMaxSoulBytes ? kMaxSoulBytes : d.soulBytes;
    ProgressHeader h{};
    h.magic = kProgressMagic;
    h.flagBytes = static_cast<uint16_t>(fb);
    h.soulBytes = static_cast<uint16_t>(sb);
    h.secrets = d.secrets;
    std::memcpy(h.inv, d.inv, sizeof(h.inv));
    std::memcpy(h.items, d.items, sizeof(h.items));
    std::memcpy(h.cadeauxGot, d.cadeauxGot, sizeof(h.cadeauxGot));
    std::memcpy(h.soulsGot, d.soulsGot, sizeof(h.soulsGot));
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), d.flags, fb);
    std::memcpy(out + sizeof(h) + fb, d.souls, sb);
    return sizeof(h) + fb + sb;
}

inline bool DecodeProgress(const uint8_t* data, size_t size, ProgressData& d) {
    if (size < sizeof(ProgressHeader)) return false;
    ProgressHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kProgressMagic || h.flagBytes > kMaxFlagBytes || h.soulBytes > kMaxSoulBytes ||
        size < sizeof(h) + h.flagBytes + h.soulBytes)
        return false;
    d.secrets = h.secrets;
    std::memcpy(d.inv, h.inv, sizeof(d.inv));
    std::memcpy(d.items, h.items, sizeof(d.items));
    std::memcpy(d.cadeauxGot, h.cadeauxGot, sizeof(d.cadeauxGot));
    std::memcpy(d.soulsGot, h.soulsGot, sizeof(d.soulsGot));
    d.flagBytes = h.flagBytes;
    std::memcpy(d.flags, data + sizeof(h), h.flagBytes);
    d.soulBytes = h.soulBytes;
    std::memcpy(d.souls, data + sizeof(h) + h.flagBytes, h.soulBytes);
    return true;
}

// ---- Quest-Gegenstaende des Hosts im aktuellen Level ("SMQ1"): Art (+0xD0) + Ort (+0x38) ----
constexpr uint32_t kQuestMagic = 0x31514D53;  // "SMQ1"
constexpr int kMaxQuestItems = 64;
#pragma pack(push, 1)
struct QuestHeader {
    uint32_t magic;
    uint16_t level;
    uint16_t num;
};
struct QuestItem {
    int32_t type = 0;
    float pos[3] = {};
};
#pragma pack(pop)
static_assert(sizeof(QuestItem) == 16, "QuestItem");
constexpr size_t MaxQuestPacketSize() { return sizeof(QuestHeader) + kMaxQuestItems * sizeof(QuestItem); }

inline size_t EncodeQuests(uint16_t level, const QuestItem* list, int num, uint8_t* out) {
    if (num > kMaxQuestItems) num = kMaxQuestItems;
    QuestHeader h{kQuestMagic, level, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, num * sizeof(QuestItem));
    return sizeof(h) + num * sizeof(QuestItem);
}

inline bool DecodeQuests(const uint8_t* data, size_t size, uint16_t& level, QuestItem* list, int& num) {
    if (size < sizeof(QuestHeader)) return false;
    QuestHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kQuestMagic || h.num > kMaxQuestItems || size < sizeof(h) + h.num * sizeof(QuestItem)) return false;
    level = h.level;
    num = h.num;
    std::memcpy(list, data + sizeof(h), num * sizeof(QuestItem));
    return true;
}

// ---- Welt-Stand des Hosts ("SMH1"): seine Zustandstabelle fuer die aktuelle Level-Gruppe, in Stuecken ----
constexpr uint32_t kSnapMagic = 0x31484D53;  // "SMH1"
constexpr int kSnapPerChunk = 64;
#pragma pack(push, 1)
struct SnapHeader {
    uint32_t magic;
    uint16_t level;
    int16_t group;     // Level-Gruppe der Eintraege (Tabelle +0xC)
    uint16_t chunk;    // Stueck-Nummer
    uint16_t chunks;   // Anzahl Stuecke
    uint16_t num;      // Eintraege in diesem Stueck
    uint16_t total;    // Eintraege insgesamt
};
struct SnapEntry {
    int32_t type = 0;
    int32_t value = 0;
    float pos[3] = {};
};
#pragma pack(pop)
static_assert(sizeof(SnapEntry) == 20, "SnapEntry");
constexpr size_t MaxSnapPacketSize() { return sizeof(SnapHeader) + kSnapPerChunk * sizeof(SnapEntry); }

inline size_t EncodeSnap(const SnapHeader& hdr, const SnapEntry* list, uint8_t* out) {
    SnapHeader h = hdr;
    h.magic = kSnapMagic;
    if (h.num > kSnapPerChunk) h.num = kSnapPerChunk;
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, h.num * sizeof(SnapEntry));
    return sizeof(h) + h.num * sizeof(SnapEntry);
}

inline bool DecodeSnap(const uint8_t* data, size_t size, SnapHeader& h, SnapEntry* list) {
    if (size < sizeof(SnapHeader)) return false;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kSnapMagic || h.num > kSnapPerChunk || size < sizeof(h) + h.num * sizeof(SnapEntry)) return false;
    std::memcpy(list, data + sizeof(h), h.num * sizeof(SnapEntry));
    return true;
}

// ---- Ganze Zustandstabelle des Hosts ("SMT1", alle Level-Gruppen, alle 10 s, in Stuecken) ----
// Das Teddy-Menue liest offene Aufgaben (z.B. Sargtore) je Level aus der Tabelle; andere Level sind so beim Betreten schon
// im Stand des Hosts. Kopf wie SnapHeader (group = aktuelle Gruppe des Hosts).
constexpr uint32_t kTableMagic = 0x31544D53;  // "SMT1"
constexpr int kTablePerChunk = 48;
#pragma pack(push, 1)
struct TableEntry {
    int32_t group = 0;
    int32_t type = 0;
    int32_t value = 0;
    float pos[3] = {};
};
#pragma pack(pop)
static_assert(sizeof(TableEntry) == 24, "TableEntry");
constexpr size_t MaxTablePacketSize() { return sizeof(SnapHeader) + kTablePerChunk * sizeof(TableEntry); }

inline size_t EncodeTable(const SnapHeader& hdr, const TableEntry* list, uint8_t* out) {
    SnapHeader h = hdr;
    h.magic = kTableMagic;
    if (h.num > kTablePerChunk) h.num = kTablePerChunk;
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, h.num * sizeof(TableEntry));
    return sizeof(h) + h.num * sizeof(TableEntry);
}

inline bool DecodeTable(const uint8_t* data, size_t size, SnapHeader& h, TableEntry* list) {
    if (size < sizeof(SnapHeader)) return false;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kTableMagic || h.num > kTablePerChunk || size < sizeof(h) + h.num * sizeof(TableEntry)) return false;
    std::memcpy(list, data + sizeof(h), h.num * sizeof(TableEntry));
    return true;
}

// ---- Gestartete Cutscenes ("SMX1"): Nummer + Ort/Sektor/Level des Ausloesers, mehrfach gesendet gegen Verlust ----
constexpr uint32_t kCutsceneMagic = 0x31584D53;  // "SMX1"
constexpr int kMaxCutscenes = 4;
#pragma pack(push, 1)
struct CutsceneHeader {
    uint32_t magic;
    uint32_t session;
    uint16_t num;
};
struct CutsceneInfo {
    uint32_t seq = 0;
    uint16_t level = 0;
    int16_t id = -1;
    uint8_t flag = 0;
    uint8_t boss = 0;      // 1 = von einem Boss ausgeloest (dann auch Levelwechsel)
    uint8_t pad[2] = {};
    int32_t sector = -1;
    float pos[3] = {};
};
#pragma pack(pop)
static_assert(sizeof(CutsceneInfo) == 28, "CutsceneInfo");
constexpr size_t MaxCutscenePacketSize() { return sizeof(CutsceneHeader) + kMaxCutscenes * sizeof(CutsceneInfo); }

inline size_t EncodeCutscenes(uint32_t session, const CutsceneInfo* list, int num, uint8_t* out) {
    if (num > kMaxCutscenes) num = kMaxCutscenes;
    CutsceneHeader h{kCutsceneMagic, session, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, num * sizeof(CutsceneInfo));
    return sizeof(h) + num * sizeof(CutsceneInfo);
}

inline bool DecodeCutscenes(const uint8_t* data, size_t size, uint32_t& session, CutsceneInfo* list, int& num) {
    if (size < sizeof(CutsceneHeader)) return false;
    CutsceneHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kCutsceneMagic || h.num > kMaxCutscenes || size < sizeof(h) + h.num * sizeof(CutsceneInfo)) return false;
    session = h.session;
    num = h.num;
    std::memcpy(list, data + sizeof(h), num * sizeof(CutsceneInfo));
    return true;
}

// ---- Gegner-Positionen vom Host ("SME2"), etwa 10x pro Sekunde ----
// Gegner werden ueberall ueber ihre Kennung angesprochen (serial = Hash aus Gruppe +0xD0 und Grundposition +0x38): die
// Pool-Seriennummern unterscheiden sich zwischen den PCs, sobald einer das Level anders betreten hat (Spielstand / Levelwechsel).
constexpr uint32_t kEnemyMagic = 0x32454D53;  // "SME2"
constexpr int kMaxEnemyPos = 48;
#pragma pack(push, 1)
struct EnemyHeader {
    uint32_t magic;
    uint16_t level;
    uint16_t num;
};
struct EnemyPos {
    uint32_t serial = 0;
    float pos[3] = {};
    float yaw = 0.0f;  // +0x18
};
#pragma pack(pop)
static_assert(sizeof(EnemyPos) == 20, "EnemyPos");
constexpr size_t MaxEnemyPacketSize() { return sizeof(EnemyHeader) + kMaxEnemyPos * sizeof(EnemyPos); }

inline size_t EncodeEnemies(uint16_t level, const EnemyPos* list, int num, uint8_t* out) {
    if (num > kMaxEnemyPos) num = kMaxEnemyPos;
    EnemyHeader h{kEnemyMagic, level, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, num * sizeof(EnemyPos));
    return sizeof(h) + num * sizeof(EnemyPos);
}

inline bool DecodeEnemies(const uint8_t* data, size_t size, uint16_t& level, EnemyPos* list, int& num) {
    if (size < sizeof(EnemyHeader)) return false;
    EnemyHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kEnemyMagic || h.num > kMaxEnemyPos || size < sizeof(h) + h.num * sizeof(EnemyPos)) return false;
    level = h.level;
    num = h.num;
    std::memcpy(list, data + sizeof(h), num * sizeof(EnemyPos));
    return true;
}

// ---- Welt-Paket "SMW1": Liste der im aktuellen Level eingesammelten Pickups (jede Sekunde komplett gesendet) ----
// Version 3 ("SMW3"): Pickups, zerstoerte Behaelter (Govis, Toepfe) und verschobene Schiebebloecke
// (Block: serial = Seriennummer, type = Version, pos = aktuelle Position).
constexpr uint32_t kWorldMagic = 0x33574D53;  // "SMW3"
constexpr int kMaxPickups = 64;
constexpr int kMaxContainers = 64;
constexpr int kMaxBlocks = 32;
#pragma pack(push, 1)
struct WorldHeader {
    uint32_t magic;
    uint16_t level;
    uint16_t numPickups;
    uint16_t numContainers;
    uint16_t numBlocks;
};
struct PackedPickup {
    uint16_t serial;     // Pool-Seriennummer des Objekts
    int16_t type;        // Pickup: Gegenstands-Typ (Pool-Eintrag +0x20); Behaelter: Art (1 Govi, 2 Topf)
    float pos[3];        // letzte bekannte Position (zur Kontrolle)
};
#pragma pack(pop)
struct PickupInfo {
    uint16_t serial = 0;
    int16_t type = 0;
    float pos[3] = {};
};
constexpr size_t MaxWorldPacketSize() {
    return sizeof(WorldHeader) + (kMaxPickups + kMaxContainers + kMaxBlocks) * sizeof(PackedPickup);
}

// Listen: die neuesten zuerst uebergeben (bei mehr als dem Maximum fallen die aeltesten weg)
inline size_t EncodeWorld(uint16_t level, const PickupInfo* pickups, int num, const PickupInfo* containers, int numC,
                          const PickupInfo* blocks, int numB, uint8_t* out) {
    if (num > kMaxPickups) num = kMaxPickups;
    if (numC > kMaxContainers) numC = kMaxContainers;
    if (numB > kMaxBlocks) numB = kMaxBlocks;
    WorldHeader h{kWorldMagic, level, static_cast<uint16_t>(num), static_cast<uint16_t>(numC), static_cast<uint16_t>(numB)};
    std::memcpy(out, &h, sizeof(h));
    uint8_t* p = out + sizeof(h);
    auto put = [&p](const PickupInfo& i) {
        PackedPickup pp{i.serial, i.type, {i.pos[0], i.pos[1], i.pos[2]}};
        std::memcpy(p, &pp, sizeof(pp));
        p += sizeof(pp);
    };
    for (int i = 0; i < num; ++i) put(pickups[i]);
    for (int i = 0; i < numC; ++i) put(containers[i]);
    for (int i = 0; i < numB; ++i) put(blocks[i]);
    return static_cast<size_t>(p - out);
}

// false, wenn es kein Welt-Paket ist
inline bool DecodeWorld(const uint8_t* data, size_t size, uint16_t& level, PickupInfo* pickups, int& num, PickupInfo* containers,
                        int& numC, PickupInfo* blocks, int& numB) {
    if (size < sizeof(WorldHeader)) return false;
    WorldHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kWorldMagic || h.numPickups > kMaxPickups || h.numContainers > kMaxContainers || h.numBlocks > kMaxBlocks ||
        size < sizeof(h) + (h.numPickups + h.numContainers + h.numBlocks) * sizeof(PackedPickup))
        return false;
    level = h.level;
    num = h.numPickups;
    numC = h.numContainers;
    numB = h.numBlocks;
    const uint8_t* p = data + sizeof(h);
    auto get = [&p](PickupInfo& i) {
        PackedPickup pp;
        std::memcpy(&pp, p, sizeof(pp));
        p += sizeof(pp);
        i.serial = pp.serial;
        i.type = pp.type;
        std::memcpy(i.pos, pp.pos, sizeof(pp.pos));
    };
    for (int i = 0; i < num; ++i) get(pickups[i]);
    for (int i = 0; i < numC; ++i) get(containers[i]);
    for (int i = 0; i < numB; ++i) get(blocks[i]);
    return true;
}

// Objekt-Zustaende ("SMS1"): Aenderungen an der Zustandstabelle des SaveManagers (Tuer offen, Schloss offen, ...).
// Schluessel = Art (Objekt +0xD0) + Ort (Objekt +0x38, auf beiden PCs bitgleich). seq zaehlt je Sitzung hoch.
constexpr uint32_t kStateMagic = 0x31534D53;  // "SMS1"
constexpr int kMaxStates = 64;
#pragma pack(push, 1)
struct StateHeader {
    uint32_t magic;
    uint32_t session;  // zufaellig je Spielstart: neue Sitzung -> Empfaenger vergisst alte Folgenummern
    uint16_t level;
    uint16_t num;
};
struct ObjState {
    int32_t type = 0;
    int32_t value = 0;
    uint32_t seq = 0;
    float pos[3] = {};
};
#pragma pack(pop)
static_assert(sizeof(ObjState) == 24, "ObjState");
constexpr size_t MaxStatePacketSize() { return sizeof(StateHeader) + kMaxStates * sizeof(ObjState); }

inline size_t EncodeStates(uint32_t session, uint16_t level, const ObjState* states, int num, uint8_t* out) {
    if (num > kMaxStates) num = kMaxStates;
    StateHeader h{kStateMagic, session, level, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), states, num * sizeof(ObjState));
    return sizeof(h) + num * sizeof(ObjState);
}

// false, wenn es kein Zustands-Paket ist
inline bool DecodeStates(const uint8_t* data, size_t size, uint32_t& session, uint16_t& level, ObjState* states, int& num) {
    if (size < sizeof(StateHeader)) return false;
    StateHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kStateMagic || h.num > kMaxStates || size < sizeof(h) + h.num * sizeof(ObjState)) return false;
    session = h.session;
    level = h.level;
    num = h.num;
    std::memcpy(states, data + sizeof(h), num * sizeof(ObjState));
    return true;
}

// Getoetete Gegner ("SMK2"): Kennung (siehe "SME2"), Folgenummer je Sitzung (jeder Tod genau einmal), letzte Position,
// type = vtable-RVA der Klasse (Gegenprobe)
constexpr uint32_t kKillMagic = 0x324B4D53;  // "SMK2"
constexpr int kMaxKills = 64;
#pragma pack(push, 1)
struct KillHeader {
    uint32_t magic;
    uint32_t session;
    uint16_t level;
    uint16_t num;
};
struct KillInfo {
    uint32_t serial = 0;
    uint32_t seq = 0;
    float pos[3] = {};
    int32_t type = 0;
};
#pragma pack(pop)
static_assert(sizeof(KillInfo) == 24, "KillInfo");
constexpr size_t MaxKillPacketSize() { return sizeof(KillHeader) + kMaxKills * sizeof(KillInfo); }

inline size_t EncodeKills(uint32_t session, uint16_t level, const KillInfo* kills, int num, uint8_t* out) {
    if (num > kMaxKills) num = kMaxKills;
    KillHeader h{kKillMagic, session, level, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), kills, num * sizeof(KillInfo));
    return sizeof(h) + num * sizeof(KillInfo);
}

// false, wenn es kein Gegner-Paket ist
inline bool DecodeKills(const uint8_t* data, size_t size, uint32_t& session, uint16_t& level, KillInfo* kills, int& num) {
    if (size < sizeof(KillHeader)) return false;
    KillHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kKillMagic || h.num > kMaxKills || size < sizeof(h) + h.num * sizeof(KillInfo)) return false;
    session = h.session;
    level = h.level;
    num = h.num;
    std::memcpy(kills, data + sizeof(h), num * sizeof(KillInfo));
    return true;
}

// Schaden an Gegnern ("SMD1"): je Gegner (Kennung, type = vtable-RVA) die SUMME des eigenen Schadens in diesem Level (steigt
// nur) -> der Empfaenger zieht die Differenz zur zuletzt uebernommenen Summe ab; verlorene Pakete schaden so nicht.
constexpr uint32_t kDamageMagic = 0x31444D53;  // "SMD1"
constexpr int kMaxDamage = 40;
#pragma pack(push, 1)
struct DamageInfo {
    uint32_t serial = 0;
    int32_t type = 0;
    float total = 0.0f;
    float pos[3] = {};
};
#pragma pack(pop)
static_assert(sizeof(DamageInfo) == 24, "DamageInfo");
constexpr size_t MaxDamagePacketSize() { return sizeof(KillHeader) + kMaxDamage * sizeof(DamageInfo); }

inline size_t EncodeDamage(uint32_t session, uint16_t level, const DamageInfo* list, int num, uint8_t* out) {
    if (num > kMaxDamage) num = kMaxDamage;
    KillHeader h{kDamageMagic, session, level, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, num * sizeof(DamageInfo));
    return sizeof(h) + num * sizeof(DamageInfo);
}

inline bool DecodeDamage(const uint8_t* data, size_t size, uint32_t& session, uint16_t& level, DamageInfo* list, int& num) {
    if (size < sizeof(KillHeader)) return false;
    KillHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kDamageMagic || h.num > kMaxDamage || size < sizeof(h) + h.num * sizeof(DamageInfo)) return false;
    session = h.session;
    level = h.level;
    num = h.num;
    std::memcpy(list, data + sizeof(h), num * sizeof(DamageInfo));
    return true;
}

// ---- Host-Einstellungen + Spielernummern ("SMO1"), alle 2 s vom Host ----
// roster[i] = Steam-ID (udpmesh: Instanznummer) von Spieler i+1; 0 = frei. Host ist immer roster[0].
constexpr uint32_t kHostSettingsMagic = 0x314F4D53;  // "SMO1"
#pragma pack(push, 1)
struct HostSettingsInfo {
    uint32_t seq = 0;
    uint8_t hpPercent = 50;
    uint8_t sharedHp = 0;
    uint8_t hardcore = 0;
    uint8_t count = 0;  // belegte Plaetze im roster
    uint64_t roster[4] = {};
};
#pragma pack(pop)
constexpr size_t MaxHostSettingsPacketSize() { return 4 + sizeof(HostSettingsInfo); }

inline size_t EncodeHostSettings(const HostSettingsInfo& info, uint8_t* out) {
    std::memcpy(out, &kHostSettingsMagic, 4);
    std::memcpy(out + 4, &info, sizeof(info));
    return MaxHostSettingsPacketSize();
}

inline bool DecodeHostSettings(const uint8_t* data, size_t size, HostSettingsInfo& out) {
    uint32_t magic = 0;
    if (size < MaxHostSettingsPacketSize()) return false;
    std::memcpy(&magic, data, 4);
    if (magic != kHostSettingsMagic) return false;
    std::memcpy(&out, data + 4, sizeof(out));
    return true;
}

// ---- Gemeinsamer Tod ("SMY1"): Grund 1 Boss, 2 geteilte HP, 3 Hardcore; 5 s lang wiederholt gegen Verlust ----
constexpr uint32_t kDeathMagic = 0x31594D53;  // "SMY1"
constexpr int kMaxDeaths = 4;
#pragma pack(push, 1)
struct DeathInfo {
    uint32_t seq = 0;
    uint8_t reason = 0;
    uint8_t pad = 0;
    uint16_t level = 0;
};
#pragma pack(pop)
constexpr size_t MaxDeathPacketSize() { return sizeof(CutsceneHeader) + kMaxDeaths * sizeof(DeathInfo); }

inline size_t EncodeDeaths(uint32_t session, const DeathInfo* list, int num, uint8_t* out) {
    if (num > kMaxDeaths) num = kMaxDeaths;
    CutsceneHeader h{kDeathMagic, session, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, num * sizeof(DeathInfo));
    return sizeof(h) + num * sizeof(DeathInfo);
}

inline bool DecodeDeaths(const uint8_t* data, size_t size, uint32_t& session, DeathInfo* list, int& num) {
    if (size < sizeof(CutsceneHeader)) return false;
    CutsceneHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kDeathMagic || h.num > kMaxDeaths || size < sizeof(h) + h.num * sizeof(DeathInfo)) return false;
    session = h.session;
    num = h.num;
    std::memcpy(list, data + sizeof(h), num * sizeof(DeathInfo));
    return true;
}

// ---- Geteilte HP ("SMP1"): eigene Lebensaenderungen als Deltas (die letzten 1 s, wiederholt), dazu der eigene Wert
// (der Host-Wert gleicht Abweichungen aus; -1 = keiner) ----
constexpr uint32_t kLifeMagic = 0x31504D53;  // "SMP1"
constexpr int kMaxLifeDeltas = 16;
#pragma pack(push, 1)
struct LifeHeader {
    uint32_t magic;
    uint32_t session;
    int32_t value;
    uint16_t num;
};
struct LifeDelta {
    uint32_t seq = 0;
    int32_t delta = 0;
};
#pragma pack(pop)
constexpr size_t MaxLifePacketSize() { return sizeof(LifeHeader) + kMaxLifeDeltas * sizeof(LifeDelta); }

inline size_t EncodeLife(uint32_t session, int32_t value, const LifeDelta* list, int num, uint8_t* out) {
    if (num > kMaxLifeDeltas) num = kMaxLifeDeltas;
    LifeHeader h{kLifeMagic, session, value, static_cast<uint16_t>(num)};
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), list, num * sizeof(LifeDelta));
    return sizeof(h) + num * sizeof(LifeDelta);
}

inline bool DecodeLife(const uint8_t* data, size_t size, uint32_t& session, int32_t& value, LifeDelta* list, int& num) {
    if (size < sizeof(LifeHeader)) return false;
    LifeHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kLifeMagic || h.num > kMaxLifeDeltas || size < sizeof(h) + h.num * sizeof(LifeDelta)) return false;
    session = h.session;
    value = h.value;
    num = h.num;
    std::memcpy(list, data + sizeof(h), num * sizeof(LifeDelta));
    return true;
}

// Interpolation zwischen zwei Posen (t = 0..1); Quaternionen per normalisiertem lerp (kuerzester Weg)
inline void Lerp(const NetPose& a, const NetPose& b, float t, NetPose& out) {
    out = b;
    // Bewegte Schiebebloecke: gleiche Seriennummer und Version in beiden -> Position dazwischen
    for (int k = 0; k < b.numBlocks; ++k)
        for (int j = 0; j < a.numBlocks; ++j)
            if (a.blocks[j].serial == b.blocks[k].serial && a.blocks[j].version == b.blocks[k].version)
                for (int i = 0; i < 3; ++i) out.blocks[k].pos[i] = a.blocks[j].pos[i] + (b.blocks[k].pos[i] - a.blocks[j].pos[i]) * t;
    if (a.numBones != b.numBones) return;
    auto nlerp = [t](const float* qa, const float* qb, float* q) {
        const float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
        const float s = dot < 0 ? -1.0f : 1.0f;
        float len = 0;
        for (int i = 0; i < 4; ++i) { q[i] = qa[i] + (qb[i] * s - qa[i]) * t; len += q[i] * q[i]; }
        len = std::sqrt(len);
        if (len > 1e-6f) for (int i = 0; i < 4; ++i) q[i] /= len;
    };
    for (int i = 0; i < 3; ++i) out.rootPos[i] = a.rootPos[i] + (b.rootPos[i] - a.rootPos[i]) * t;
    nlerp(a.rootRot, b.rootRot, out.rootRot);
    for (uint32_t k = 0; k < b.numBones; ++k) {
        nlerp(a.boneRot[k], b.boneRot[k], out.boneRot[k]);
        for (int i = 0; i < 3; ++i) out.bonePos[k][i] = a.bonePos[k][i] + (b.bonePos[k][i] - a.bonePos[k][i]) * t;
    }
}

}  // namespace posecodec
