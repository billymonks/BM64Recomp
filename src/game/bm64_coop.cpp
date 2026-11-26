#include "bm64_coop.h"

#include "bm64_debug.h"
#include "bm64_session.h"
#include "librecomp/helpers.hpp"
#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

struct Session {
    bool on = false;
    bool unlimited = false;
    int players = 1;
    int local_player = 0;
    int overlay = -1;
    int override_slot = -1;
    bool probed = false;
};

Session& sess() {
    static Session s;
    return s;
}

// Written by the input thread (F9), read by the game thread when it renders.
std::atomic<int> g_view_player{ -1 };

int32_t word(uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<int32_t*>(rdram + (addr - 0x80000000u));
}

void set_word(uint8_t* rdram, uint32_t addr, int32_t value) {
    *reinterpret_cast<int32_t*>(rdram + (addr - 0x80000000u)) = value;
}

// Overlay id (gOverlayID) to bank and sub-overlay, both 1-based. False for "main" and unknown ids.
bool overlay_bank(int id, int& bank, int& sub) {
    static const int kBanks[] = { 4, 6, 7, 10, 7, 15, 10, 13, 6, 2, 2, 2, 1, 10 };
    int remaining = id - 1;
    for (int b = 0; b < 14 && remaining >= 0; b++) {
        if (remaining < kBanks[b]) {
            bank = b + 1;
            sub = remaining + 1;
            return true;
        }
        remaining -= kBanks[b];
    }
    return false;
}

bool rival_map(int id) {
    int b = 0, sub = 0;
    if (!overlay_bank(id, b, sub)) {
        return false;
    }
    return (b == 4 && sub == 10) || (b == 5 && sub == 7) || (b == 6 && sub == 15) ||
        (b == 8 && sub == 13) || (b == 9 && sub == 5) || (b == 9 && sub == 6);
}

// Banks 4-9 are the worlds and 10-12 the large boss maps. Battle arenas (bank 14), menus and
// sequences are not story stages, so the co-op rules leave them as the original game plays them.
bool story_map(int id) {
    int b = 0, sub = 0;
    return overlay_bank(id, b, sub) && b >= 4 && b <= 12;
}

// Bit 2 of the game flags at D_802AC610 is set by func_80297ADC, which starts the attract
// sequence (intro and demo stages played from recorded input), and by arena setup. Pressing Start
// returns to the title, which resets the flags.
bool attract_running(uint8_t* rdram) {
    return (word(rdram, 0x802AC610u) & 0x4) != 0;
}

bool story(uint8_t* rdram) {
    return sess().on && story_map(sess().overlay) && !attract_running(rdram);
}

float f32_at(uint8_t* rdram, uint32_t addr) {
    return std::bit_cast<float>(static_cast<uint32_t>(word(rdram, addr)));
}

bool obj_pos(uint8_t* rdram, uint32_t obj, float& x, float& y, float& z) {
    if ((obj & 0xFF000000u) != 0x80000000u) {
        return false;
    }
    uint32_t body = static_cast<uint32_t>(word(rdram, obj + 0x40));
    if ((body & 0xFF000000u) != 0x80000000u) {
        return false;
    }
    x = f32_at(rdram, body + 0x10);
    y = f32_at(rdram, body + 0x14);
    z = f32_at(rdram, body + 0x18);
    return true;
}

bool body_pos(uint8_t* rdram, uint32_t slot, float& x, float& y, float& z) {
    return obj_pos(rdram, static_cast<uint32_t>(word(rdram, slot + 0x28)), x, y, z);
}

bool living(uint8_t* rdram, int slot) {
    if (slot < 0 || slot >= bm64::coop::kPlayerSlots) {
        return false;
    }
    int control = word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride + 0x24);
    return control == 1 || control == 2;
}

// Category of the process that is running. Overlay entry points share addresses, so the key includes the overlay id.
int category_of(int overlay, uint32_t func) {
    // The lookAt process does not call getMainPlayer itself, so the catalogue has no row for it.
    // Its callees still need the camera override while a per-player block is swapped in.
    if (func == 0x80236F54u) {
        return bm64::coop::kCatCamera;
    }
    struct Row { uint16_t overlay; uint32_t func; uint8_t category; };
    static const Row rows[] = {
#include "bm64_player_cats.inc"
    };
    uint16_t key_overlay = func >= 0x80200000u ? 0 : static_cast<uint16_t>(overlay);
    int lo = 0;
    int hi = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (rows[mid].overlay < key_overlay || (rows[mid].overlay == key_overlay && rows[mid].func < func)) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    if (lo < static_cast<int>(sizeof(rows) / sizeof(rows[0])) && rows[lo].overlay == key_overlay && rows[lo].func == func) {
        return rows[lo].category;
    }
    return 0;
}

// Category of one getMainPlayer return address or direct-read lw, or -1 when the site is not listed.
int category_of_site(int overlay, uint32_t site) {
    struct Row { uint16_t overlay; uint32_t site; uint8_t category; };
    static const Row rows[] = {
#include "bm64_player_sites.inc"
    };
    uint16_t key_overlay = site >= 0x80200000u ? 0 : static_cast<uint16_t>(overlay);
    const Row* end = rows + sizeof(rows) / sizeof(rows[0]);
    const Row* it = std::lower_bound(rows, end, Row{ key_overlay, site, 0 }, [](const Row& a, const Row& b) {
        return a.overlay < b.overlay || (a.overlay == b.overlay && a.site < b.site);
    });
    if (it != end && it->overlay == key_overlay && it->site == site) {
        return it->category;
    }
    return -1;
}

int nearest_living(uint8_t* rdram, float ox, float oy, float oz, bool have_origin) {
    int best = -1;
    float best_d = 0;
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        float x, y, z;
        if (!living(rdram, i) || !body_pos(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride, x, y, z)) {
            continue;
        }
        float d = 0;
        if (have_origin) {
            float dx = x - ox, dy = y - oy, dz = z - oz;
            d = dx * dx + dy * dy + dz * dz;
        }
        if (best < 0 || d < best_d || (d == best_d && i < best)) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

int sticky_target(uint8_t* rdram);

// `site` is the caller's return address for getMainPlayer, or the lw address for a direct read.
int resolved_slot(uint8_t* rdram, uint32_t site) {
    int slot = word(rdram, 0x802AC894u);
    if (!story(rdram)) {
        return slot;
    }
    int category = category_of_site(sess().overlay, site);
    if (category < 0) {
        uint32_t proc = static_cast<uint32_t>(word(rdram, 0x802AC344u));
        uint32_t func = (proc & 0xFF000000u) == 0x80000000u ? static_cast<uint32_t>(word(rdram, proc + 0x0C)) : 0;
        category = category_of(sess().overlay, func);
    }
    if (category < 0 || category >= bm64::coop::kCategories) {
        return slot;
    }
    int override_slot = static_cast<int8_t>(rdram[(bm64::coop::kOverrideBase + static_cast<uint32_t>(category)) - 0x80000000u]);
    if (override_slot >= 0) {
        return override_slot;
    }
    // Triggers and enemies both stick to the nearest living bomber. A trigger process whose
    // user_data is the goal object therefore follows whoever is standing there.
    if (category == bm64::coop::kCatEnemy || category == bm64::coop::kCatTrigger) {
        return sticky_target(rdram);
    }
    return slot;
}

int sticky_target(uint8_t* rdram) {
    constexpr uint32_t kPool = 0x80063370u;
    constexpr uint32_t kProcSize = 0x8A0u;
    uint32_t proc = static_cast<uint32_t>(word(rdram, 0x802AC344u));
    int index = -1;
    if (proc >= kPool && (proc - kPool) % kProcSize == 0) {
        index = static_cast<int>((proc - kPool) / kProcSize);
    }
    if (index < 0 || index >= static_cast<int>(bm64::coop::kProcessSlots)) {
        int leader = word(rdram, 0x802AC894u);
        return leader;
    }
    uint32_t rec = bm64::coop::kStickyBase + static_cast<uint32_t>(index) * 2;
    int stored = static_cast<int8_t>(rdram[rec - 0x80000000u]);
    uint8_t gen = rdram[rec - 0x80000000u + 1];
    uint8_t now = static_cast<uint8_t>(bm64::debug::tick() / 60);
    if (stored >= 0 && living(rdram, stored) && gen == now) {
        return stored;
    }
    float ox = 0, oy = 0, oz = 0;
    bool have = false;
    uint32_t user = static_cast<uint32_t>(word(rdram, proc + 0x10));
    have = obj_pos(rdram, user, ox, oy, oz);
    if (!have) {
        int leader = word(rdram, 0x802AC894u);
        if (leader >= 0 && leader < bm64::coop::kPlayerSlots) {
            have = body_pos(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(leader) * bm64::coop::kPlayerStride, ox, oy, oz);
        }
    }
    int pick = nearest_living(rdram, ox, oy, oz, have);
    if (pick < 0) {
        pick = word(rdram, 0x802AC894u);
    }
    rdram[rec - 0x80000000u] = static_cast<uint8_t>(pick);
    rdram[rec - 0x80000000u + 1] = now;
    return pick;
}

uint8_t* rule(uint8_t* rdram) {
    return rdram + (bm64::coop::kRuleBase - 0x80000000u);
}

int32_t* rule_s32(uint8_t* rdram, uint32_t offset) {
    return reinterpret_cast<int32_t*>(rule(rdram) + offset);
}

// Bytes 0x58-0x5F of the rule block: the controller port each player slot reads.
// 0 keeps the slot's own port, kPortSilent reads no input, anything else is port + 1.
constexpr uint32_t kPortMap = 0x58;
constexpr uint8_t kPortSilent = 0x80;

uint8_t* port_map(uint8_t* rdram) {
    return rule(rdram) + kPortMap;
}

// The session player whose pad drives this slot, or -1.
int player_of_slot(uint8_t* rdram, int slot) {
    uint8_t mapped = port_map(rdram)[slot];
    if (mapped == 0) {
        return slot;
    }
    if (mapped == kPortSilent) {
        return -1;
    }
    return mapped - 1;
}

// Distance between teammates placed in a row. Bombers 50 units apart or closer are stuck together.
constexpr float kPartySpacing = 60.0f;

// Position in the party, which spaces teammates kPartySpacing apart. Rival maps put teammates in
// slots 4..7, so the slot number itself is too far from the checkpoint.
int party_rank(uint8_t* rdram, int slot) {
    int player = player_of_slot(rdram, slot);
    return player >= 0 ? player : slot;
}

int slot_of_player(uint8_t* rdram, int player) {
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        int control = word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride + 0x24);
        if (control == 1 && player_of_slot(rdram, i) == player) {
            return i;
        }
    }
    return player;
}

void store_f32(uint8_t* rdram, uint32_t addr, float value) {
    set_word(rdram, addr, static_cast<int32_t>(std::bit_cast<uint32_t>(value)));
}

bool slot_body(uint8_t* rdram, int slot, uint32_t& body) {
    uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride;
    uint32_t obj = static_cast<uint32_t>(word(rdram, base + 0x28));
    if ((obj & 0xFF000000u) != 0x80000000u) {
        return false;
    }
    body = static_cast<uint32_t>(word(rdram, obj + 0x40));
    return (body & 0xFF000000u) == 0x80000000u;
}

void write_checkpoint(uint8_t* rdram, float x, float y, float z) {
    uint8_t* rec = rule(rdram);
    rec[0x28] = 1;
    store_f32(rdram, bm64::coop::kRuleBase + 0x2C, x);
    store_f32(rdram, bm64::coop::kRuleBase + 0x30, y);
    store_f32(rdram, bm64::coop::kRuleBase + 0x34, z);
    rec[0x29] = 0;
}

void place_human(uint8_t* rdram, int slot, float x, float y, float z, float angle) {
    uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride;
    store_f32(rdram, base + 0x00, x);
    store_f32(rdram, base + 0x04, y);
    store_f32(rdram, base + 0x08, z);
    store_f32(rdram, base + 0x0C, angle);
    uint32_t body = 0;
    if (!slot_body(rdram, slot, body)) {
        return;
    }
    store_f32(rdram, body + 0x10, x);
    store_f32(rdram, body + 0x14, y);
    store_f32(rdram, body + 0x18, z);
    store_f32(rdram, body + 0x20, angle);
    // The collision record's previous position. func_802909FC sweeps the body from there, so
    // after a fall the body went back below the kill plane and died again on every tick.
    uint32_t rec = static_cast<uint32_t>(word(rdram, base + 0x28));
    store_f32(rdram, rec + 0x58, x);
    store_f32(rdram, rec + 0x5C, y);
    store_f32(rdram, rec + 0x60, z);
}

// -10000 on every axis is where the game parks a bomber that is not on the map.
bool parked(float x) {
    return x <= -9000.0f;
}

// Some entries (a director warp into 5.1, for one) leave slot 0's spawn parked and place the
// leader's body from the stage entry instead. A checkpoint and teammates copied from the spawn
// would then sit in the void, so take both from the leader's body once it is on the map.
void settle_parked_checkpoint(uint8_t* rdram) {
    uint8_t* rec = rule(rdram);
    if (rec[0x28] == 0 || !parked(f32_at(rdram, bm64::coop::kRuleBase + 0x2C))) {
        return;
    }
    int leader = word(rdram, 0x802AC894u);
    if (leader < 0 || leader >= bm64::coop::kPlayerSlots) {
        leader = 0;
    }
    uint32_t leader_base = bm64::coop::kPlayerBase + static_cast<uint32_t>(leader) * bm64::coop::kPlayerStride;
    float x = 0, y = 0, z = 0;
    if (!obj_pos(rdram, static_cast<uint32_t>(word(rdram, leader_base + 0x28)), x, y, z) || parked(x)) {
        return;
    }
    write_checkpoint(rdram, x, y, z);
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        float bx = 0, by = 0, bz = 0;
        if (i == leader || word(rdram, base + 0x24) != 1 ||
            !obj_pos(rdram, static_cast<uint32_t>(word(rdram, base + 0x28)), bx, by, bz) || !parked(bx)) {
            continue;
        }
        place_human(rdram, i, x + kPartySpacing * static_cast<float>(party_rank(rdram, i)), y, z, f32_at(rdram, base + 0x0C));
    }
}

int leader_slot(uint8_t* rdram) {
    int leader = word(rdram, 0x802AC894u);
    return (leader < 0 || leader >= bm64::coop::kPlayerSlots) ? 0 : leader;
}

uint32_t slot_base(int slot) {
    return bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride;
}

// Moves a bomber's body without touching its spawn point (place_human also rewrites the spawn).
void move_body(uint8_t* rdram, int slot, float x, float y, float z) {
    uint32_t body = 0;
    if (!slot_body(rdram, slot, body)) {
        return;
    }
    store_f32(rdram, body + 0x10, x);
    store_f32(rdram, body + 0x14, y);
    store_f32(rdram, body + 0x18, z);
    uint32_t rec = static_cast<uint32_t>(word(rdram, slot_base(slot) + 0x28));
    store_f32(rdram, rec + 0x58, x);
    store_f32(rdram, rec + 0x5C, y);
    store_f32(rdram, rec + 0x60, z);
}

// Pause block (bm64::coop::kPauseBase). Only host code reads or writes these bytes.
uint8_t* pause_bytes(uint8_t* rdram) {
    return rdram + (bm64::coop::kPauseBase - 0x80000000u);
}
constexpr uint32_t kPauseFlags = bm64::coop::kPauseBase + 0x04;
constexpr uint32_t kPauseLast = bm64::coop::kPauseBase + 0x24;
constexpr uint32_t kPauseLastOvl = 0x84;
constexpr uint32_t kPauseHearts = 0x8C;

// Log only: hits absorbed while teammates waited with a parked leader.
int g_pause_hits = 0;

bool pause_active(uint8_t* rdram) {
    return pause_bytes(rdram)[0] != 0;
}

bool human_teammate(uint8_t* rdram, int slot, int leader) {
    uint32_t body = 0;
    return slot != leader && word(rdram, slot_base(slot) + 0x24) == 1 && slot_body(rdram, slot, body);
}

// A cutscene that shows another area (map 4.1's gate after the last orb, which loads 4.2 to show
// it) parks the leader's body at -10000 and gives the leader's slot flags 0x02010420, which keep
// it out of play there. A teammate parked beside it without them fell below the kill plane and
// died on every tick, spending the party's lives. While the leader is parked, every human teammate
// is parked next to it with the leader's flags and its own hearts. When the leader is back, each
// returns to where it last stood on that map, or next to the leader if the map changed.
void pause_with_parked_leader(uint8_t* rdram) {
    uint8_t* p = pause_bytes(rdram);
    // While a stage reloads (the way back from that cutscene reloads 4.1) there is no leader and
    // every body reads (0, 0, 0). The wait goes on until the reloaded stage has its leader again.
    if (word(rdram, 0x802AC894u) < 0) {
        return;
    }
    int leader = leader_slot(rdram);
    float lx = 0, ly = 0, lz = 0;
    bool have = body_pos(rdram, slot_base(leader), lx, ly, lz);
    bool leader_parked = have && parked(lx);
    uint8_t overlay = static_cast<uint8_t>(sess().overlay);
    int32_t leader_flags = word(rdram, slot_base(leader) + 0x10);
    if (leader_parked) {
        if (!p[0]) {
            p[0] = 1;
            p[1] = overlay;
            for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
                set_word(rdram, kPauseFlags + static_cast<uint32_t>(i) * 4, word(rdram, slot_base(i) + 0x10));
                p[kPauseHearts + i] = static_cast<uint8_t>(std::clamp(word(rdram, slot_base(i) + 0x14), 0, 255));
            }
            std::printf("[bm64 coop] Leader parked off the map (tick %u); teammates wait with it\n",
                static_cast<unsigned>(bm64::debug::tick()));
        }
        for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
            if (!human_teammate(rdram, i, leader)) {
                continue;
            }
            set_word(rdram, slot_base(i) + 0x10, leader_flags);
            if (p[kPauseHearts + i] > 0) {
                set_word(rdram, slot_base(i) + 0x14, p[kPauseHearts + i]);
            }
            // The parked point is below the stage's kill plane (D_802AFC5C, func_8026E8B8). The leader
            // is left alone there, but a teammate's body is flagged by func_80232FF8 and takes a
            // kind-3 hit (animation and sound) on every tick. Hold it above the plane, with no fall
            // speed and no latched pit flag.
            float kill = f32_at(rdram, 0x802AFC5Cu);
            float wy = std::max(ly, kill + 500.0f);
            move_body(rdram, i, lx + kPartySpacing * static_cast<float>(party_rank(rdram, i)), wy, lz);
            uint32_t obj = static_cast<uint32_t>(word(rdram, slot_base(i) + 0x28));
            store_f32(rdram, obj + 0x6C, 0.0f);
            for (uint32_t off : { 0x10u, 0x14u }) {
                set_word(rdram, obj + off, static_cast<int32_t>(static_cast<uint32_t>(word(rdram, obj + off)) & ~0x40000000u));
            }
        }
        return;
    }
    if (p[0]) {
        // The overlay id can change while the leader is parked (the cutscene loads 4.2, the way
        // back reloads 4.1), so only the map of each teammate's last spot is compared.
        bool same_map = false;
        for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
            if (!human_teammate(rdram, i, leader)) {
                continue;
            }
            set_word(rdram, slot_base(i) + 0x10, word(rdram, kPauseFlags + static_cast<uint32_t>(i) * 4));
            uint32_t last = kPauseLast + static_cast<uint32_t>(i) * 12;
            float tx = lx + kPartySpacing * static_cast<float>(party_rank(rdram, i)), ty = ly, tz = lz;
            if (p[kPauseLastOvl + i] == overlay && overlay != 0) {
                same_map = true;
                tx = f32_at(rdram, last);
                ty = f32_at(rdram, last + 4);
                tz = f32_at(rdram, last + 8);
            }
            if (have) {
                move_body(rdram, i, tx, ty, tz);
                // The game puts every bomber back on its slot's spawn point over the next ticks
                // (map 4.1 after the gate cutscene), which for a teammate was the stage start.
                store_f32(rdram, slot_base(i) + 0x00, tx);
                store_f32(rdram, slot_base(i) + 0x04, ty);
                store_f32(rdram, slot_base(i) + 0x08, tz);
            }
        }
        // The leader too: after a cutscene on the same map (map 4.1's orb lights) the game puts the
        // leader where it expects the main player to have stood, the light a teammate may have
        // triggered. Back to its own spot; when the leader triggered it, that is the same place.
        if (have && p[kPauseLastOvl + leader] == overlay && overlay != 0) {
            uint32_t last = kPauseLast + static_cast<uint32_t>(leader) * 12;
            float tx = f32_at(rdram, last), ty = f32_at(rdram, last + 4), tz = f32_at(rdram, last + 8);
            move_body(rdram, leader, tx, ty, tz);
            store_f32(rdram, slot_base(leader) + 0x00, tx);
            store_f32(rdram, slot_base(leader) + 0x04, ty);
            store_f32(rdram, slot_base(leader) + 0x08, tz);
        }
        p[0] = 0;
        std::printf("[bm64 coop] Leader back on the map (tick %u); teammates %s; %d hits on waiting teammates ignored\n",
            static_cast<unsigned>(bm64::debug::tick()), same_map ? "return to their spots" : "join the leader", g_pause_hits);
        g_pause_hits = 0;
    }
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        float x = 0, y = 0, z = 0;
        // Every human, the leader included.
        if (!human_teammate(rdram, i, -1) || !body_pos(rdram, slot_base(i), x, y, z) || parked(x)) {
            continue;
        }
        uint32_t last = kPauseLast + static_cast<uint32_t>(i) * 12;
        store_f32(rdram, last, x);
        store_f32(rdram, last + 4, y);
        store_f32(rdram, last + 8, z);
        p[kPauseLastOvl + i] = overlay;
    }
}

uint32_t g_cutscene_func = 0;

bool cutscene_running(uint8_t* rdram) {
    uint32_t head = 0x802AB200u;
    uint32_t proc = static_cast<uint32_t>(word(rdram, head + 0x89C));
    for (int n = 0; n < 80 && proc != 0 && proc != head; n++) {
        int mode = word(rdram, proc + 0x04);
        uint32_t func = static_cast<uint32_t>(word(rdram, proc + 0x0C));
        // func_802553D4 is the scripted-bomber update. It drives a rival for its whole fight, so
        // counting it hid (no collision) every teammate for the fight on rival maps.
        if (mode != 0 && func != 0x802553D4u && category_of(sess().overlay, func) == bm64::coop::kCatCutscene) {
            g_cutscene_func = func;
            return true;
        }
        proc = static_cast<uint32_t>(word(rdram, proc + 0x89C));
    }
    return false;
}

void apply_cutscene_hide(uint8_t* rdram) {
    bool hide = cutscene_running(rdram);
    int leader = word(rdram, 0x802AC894u);
    if (leader < 0) {
        hide = false;
    }
    uint8_t* hidden = rule(rdram);
    int32_t* saved = rule_s32(rdram, 0x08);
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        int control = word(rdram, base + 0x24);
        bool teammate = control == 1 && i != leader;
        if (hide && teammate) {
            if (hidden[i] == 0) {
                saved[i] = word(rdram, base + 0x48);
                hidden[i] = 1;
            }
            // Bit 30 of the bomber object's flag word is not a hide flag: func_8024E088 turns it
            // into a kind-3 hit (0x8024E300) every tick, so only collision is switched off.
            set_word(rdram, base + 0x48, 0);
        }
        else if (hidden[i] != 0) {
            set_word(rdram, base + 0x48, saved[i]);
            hidden[i] = 0;
        }
    }
}

// Two bombers whose bodies are 50 units apart or closer can neither of them move (52 is free).
// Map 4.10 moves the leader's body 50 units from its spawn floats, onto a spaced teammate.
// Push a teammate out to kPartySpacing from any human it overlaps.
void separate_humans(uint8_t* rdram) {
    constexpr float kOverlap = 50.25f;
    constexpr float kGap = kPartySpacing;
    int leader = word(rdram, 0x802AC894u);
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        uint32_t ibase = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        uint32_t ibody = 0;
        if (i == leader || word(rdram, ibase + 0x24) != 1 || !slot_body(rdram, i, ibody)) {
            continue;
        }
        for (int j = 0; j < bm64::coop::kPlayerSlots; j++) {
            uint32_t jbase = bm64::coop::kPlayerBase + static_cast<uint32_t>(j) * bm64::coop::kPlayerStride;
            uint32_t jbody = 0;
            if (j == i || word(rdram, jbase + 0x24) != 1 || !slot_body(rdram, j, jbody)) {
                continue;
            }
            float dx = f32_at(rdram, ibody + 0x10) - f32_at(rdram, jbody + 0x10);
            float dz = f32_at(rdram, ibody + 0x18) - f32_at(rdram, jbody + 0x18);
            float d2 = dx * dx + dz * dz;
            if (d2 >= kOverlap * kOverlap) {
                continue;
            }
            float d = std::sqrt(d2);
            float ux = d > 0.001f ? dx / d : 1.0f;
            float uz = d > 0.001f ? dz / d : 0.0f;
            store_f32(rdram, ibody + 0x10, f32_at(rdram, jbody + 0x10) + ux * kGap);
            store_f32(rdram, ibody + 0x18, f32_at(rdram, jbody + 0x18) + uz * kGap);
            static int logged = 0;
            if (logged < 8) {
                logged++;
                std::printf("[bm64 coop] Moved player slot %d out of slot %d (%.0f apart)\n", i, j, d);
            }
        }
    }
}

// A rival's AI record (func_80251804's layout at D_800BCFE8, stride 0x128) holds its target slot at
// +0x10 and that slot's object at +0x1C. func_8025DC60 spawns rivals aimed at slot 0, and in story
// the AI never picks again, so a teammate was never chased. Every two seconds each rival turns to
// the nearest living human, if that one is clearly closer than its current target.
void retarget_rivals(uint8_t* rdram) {
    constexpr uint32_t kAiRecords = 0x800BCFE8u;
    constexpr uint32_t kAiStride = 0x128u;
    constexpr uint32_t kEvery = 120;
    if (bm64::debug::tick() % kEvery != 0 || cutscene_running(rdram)) {
        return;
    }
    for (int r = 0; r < bm64::coop::kPlayerSlots; r++) {
        uint32_t rbase = bm64::coop::kPlayerBase + static_cast<uint32_t>(r) * bm64::coop::kPlayerStride;
        uint32_t rec = kAiRecords + static_cast<uint32_t>(r) * kAiStride;
        uint32_t rbody = 0;
        if (word(rdram, rbase + 0x24) != 2 || word(rdram, rec + 0x0C) != r || !slot_body(rdram, r, rbody)) {
            continue;
        }
        int current = word(rdram, rec + 0x10);
        if (current < 0 || current >= bm64::coop::kPlayerSlots ||
            word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(current) * bm64::coop::kPlayerStride + 0x24) != 1) {
            continue;
        }
        float rx = f32_at(rdram, rbody + 0x10);
        float rz = f32_at(rdram, rbody + 0x18);
        int best = -1;
        float best_d2 = 0;
        float current_d2 = -1;
        for (int h = 0; h < bm64::coop::kPlayerSlots; h++) {
            uint32_t hbase = bm64::coop::kPlayerBase + static_cast<uint32_t>(h) * bm64::coop::kPlayerStride;
            uint32_t hbody = 0;
            if (word(rdram, hbase + 0x24) != 1 || word(rdram, hbase + 0x14) <= 0 || !slot_body(rdram, h, hbody)) {
                continue;
            }
            float dx = f32_at(rdram, hbody + 0x10) - rx;
            float dz = f32_at(rdram, hbody + 0x18) - rz;
            float d2 = dx * dx + dz * dz;
            if (h == current) {
                current_d2 = d2;
            }
            if (best < 0 || d2 < best_d2) {
                best = h;
                best_d2 = d2;
            }
        }
        // 0.5625 is 0.75 squared: the new target must be at least a quarter nearer.
        if (best < 0 || best == current || (current_d2 >= 0 && best_d2 >= 0.5625f * current_d2)) {
            continue;
        }
        set_word(rdram, rec + 0x10, best);
        set_word(rdram, rec + 0x1C,
            word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(best) * bm64::coop::kPlayerStride + 0x28));
        static int logged = 0;
        if (logged < 8) {
            logged++;
            std::printf("[bm64 coop] Rival slot %d now after slot %d\n", r, best);
        }
    }
}

void note_checkpoint_hearts(uint8_t* rdram) {
    uint8_t* rec = rule(rdram);
    if (rec[0x28] == 0 || rec[0x29] != 0) {
        return;
    }
    bool any = false;
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        int hearts = word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride + 0x14);
        if (hearts > 0) {
            any = true;
        }
        rule_s32(rdram, 0x38)[i] = hearts;
    }
    if (any) {
        rec[0x29] = 1;
    }
}

} // namespace

namespace bm64::coop {

void set_session(int players, int local_player, bool unlimited_lives) {
    Session& s = sess();
    s.on = players > 1;
    s.players = players;
    s.local_player = local_player;
    s.unlimited = unlimited_lives;
}

void note_overlay(int overlay_id) {
    sess().overlay = overlay_id;
}

bool active() {
    return sess().on;
}

int local_player() {
    return sess().local_player;
}

int view_player() {
    static const bool seeded = [] {
        if (const char* v = std::getenv("BM64_VIEW_PLAYER")) {
            g_view_player.store(std::atoi(v) - 1);
        }
        return true;
    }();
    (void)seeded;
    int v = g_view_player.load();
    return v >= 0 && v < sess().players ? v : sess().local_player;
}

void cycle_view() {
    if (!sess().on) {
        return;
    }
    int next = (view_player() + 1) % sess().players;
    g_view_player.store(next);
    std::printf("[bm64 coop] Viewing player %d\n", next + 1);
}

int session_players() {
    return sess().on ? sess().players : 0;
}

bool unlimited_lives() {
    return sess().unlimited;
}

bool omit_render_camera() {
    return sess().on;
}

void hand_to_ai(uint8_t* rdram, int player) {
    int slot = story(rdram) ? slot_of_player(rdram, player) : player;
    if (slot < 0 || slot >= bm64::coop::kPlayerSlots) {
        return;
    }
    uint32_t control = bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride + 0x24;
    if (word(rdram, control) == 1) {
        set_word(rdram, control, 2);
    }
}

void apply_rules(uint8_t* rdram) {
    Session& s = sess();
    if (story(rdram)) {
        if (s.unlimited) {
            set_word(rdram, 0x802AC624, 99);
        }
        settle_parked_checkpoint(rdram);
        note_checkpoint_hearts(rdram);
        pause_with_parked_leader(rdram);
        apply_cutscene_hide(rdram);
        separate_humans(rdram);
        retarget_rivals(rdram);
    }
    else if (s.on) {
        // Off the story maps (world map, menus) the remembered spots belong to a visit that is
        // over; entering the same map again must not send teammates back to them.
        uint8_t* p = pause_bytes(rdram);
        p[0] = 0;
        std::memset(p + kPauseLastOvl, 0, bm64::coop::kPlayerSlots);
    }
    static const int trace_every = [] {
        const char* v = std::getenv("BM64_COOP_TRACE");
        return v ? std::atoi(v) : 0;
    }();
    if (trace_every > 0 && story_map(sess().overlay) && bm64::debug::tick() % static_cast<uint32_t>(trace_every) == 0) {
        bool cut = cutscene_running(rdram);
        std::printf("[bm64 trace] tick %u leader %d cut %08X", bm64::debug::tick(), word(rdram, 0x802AC894u),
            cut ? g_cutscene_func : 0u);
        for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
            uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
            int control = word(rdram, base + 0x24);
            if (control == 0) {
                continue;
            }
            float x = 0, y = 0, z = 0;
            obj_pos(rdram, static_cast<uint32_t>(word(rdram, base + 0x28)), x, y, z);
            std::printf(" | s%d c%d h%d f%08X a%X col%X 68:%08X 2C:%08X 30:%08X (%.0f,%.0f,%.0f)", i, control,
                word(rdram, base + 0x14), word(rdram, base + 0x10), word(rdram, base + 0x44),
                word(rdram, base + 0x48), word(rdram, base + 0x68), word(rdram, base + 0x2C),
                word(rdram, base + 0x30), x, y, z);
            if (control == 2) {
                // func_80251804's AI record for the slot; +0x10 is the slot it is after.
                std::printf(" t%d", word(rdram, 0x800BCFE8u + static_cast<uint32_t>(i) * 0x128u + 0x10u));
            }
        }
        std::printf("\n");
    }
    if (const char* probe = std::getenv("BM64_SLOT_PROBE")) {
        if (probe[0] != '\0' && probe[0] != '0' && !s.probed && bm64::debug::tick() == 200) {
            s.probed = true;
            std::printf("[bm64 coop] slot0 control %d hearts %d at %08X\n",
                word(rdram, bm64::coop::kPlayerBase + 0x24), word(rdram, bm64::coop::kPlayerBase + 0x14),
                bm64::coop::kPlayerBase);
        }
    }
}

} // namespace bm64::coop

// Set by the "main-player call sites" hooks in bm64.us.toml right before a listed getMainPlayer
// call, and taken by that call. Game thread only; never outlives the call.
static uint32_t g_main_site = 0;

extern "C" void bm64_main_site(uint32_t site) {
    g_main_site = site;
}

extern "C" void bm64_main_player(uint8_t* rdram, recomp_context* ctx) {
    uint32_t caller = g_main_site;
    g_main_site = 0;
    static const bool log_callers = std::getenv("BM64_LOG_MAIN_CALLERS") != nullptr;
    if (log_callers) {
        static int logged = 0;
        if (logged < 10) {
            logged++;
            std::printf("[bm64 coop] getMainPlayer caller %08X\n", caller);
        }
    }
    _return(ctx, resolved_slot(rdram, caller));
}

extern "C" int bm64_resolved_slot_at(uint8_t* rdram, uint32_t site) {
    return resolved_slot(rdram, site);
}

// Whether the goal and switch box tests (func_802457D8, func_802456FC) also check this slot:
// a human bomber with a body, in a story co-op session.
extern "C" void bm64_coop_trigger_slot(uint8_t* rdram, recomp_context* ctx) {
    int slot = _arg<0, int32_t>(rdram, ctx);
    uint32_t body = 0;
    bool ok = story(rdram) && slot >= 0 && slot < bm64::coop::kPlayerSlots &&
              word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride + 0x24) == 1 &&
              slot_body(rdram, slot, body);
    _return(ctx, ok ? 1 : 0);
}

// func_80248148 is about to take the hearts==0 path. t0 holds the heart count.
// A teammate still standing, or a shared life left, puts this bomber back at the checkpoint
// and makes that branch look like the hit was survived. A wipe with no lives falls through.
extern "C" void bm64_coop_downed(uint8_t* rdram, recomp_context* ctx) {
    if (!story(rdram)) {
        return;
    }
    int player = static_cast<int>(MEM_W(0x70, ctx->r29));
    if (player < 0 || player >= bm64::coop::kPlayerSlots) {
        return;
    }
    uint32_t self = bm64::coop::kPlayerBase + static_cast<uint32_t>(player) * bm64::coop::kPlayerStride;
    if (word(rdram, self + 0x24) != 1) {
        return;
    }
    // A teammate waiting with a parked leader is out of play. A hit there is not a death and
    // costs no life; pause_with_parked_leader puts it back beside the leader on the next tick.
    if (pause_active(rdram) && player != leader_slot(rdram)) {
        int kept = pause_bytes(rdram)[kPauseHearts + player];
        int hearts = kept > 0 ? kept : 1;
        set_word(rdram, self + 0x14, hearts);
        ctx->r8 = hearts;
        uint32_t obj = static_cast<uint32_t>(word(rdram, self + 0x28));
        if ((obj & 0xFF000000u) == 0x80000000u) {
            set_word(rdram, obj + 0x10, static_cast<int32_t>(static_cast<uint32_t>(word(rdram, obj + 0x10)) & ~0x40000000u));
        }
        if (g_pause_hits++ < 4) {
            std::printf("[bm64 coop] Player %d was hit while waiting with the parked leader (tick %u); no life lost\n",
                player, static_cast<unsigned>(bm64::debug::tick()));
        }
        return;
    }
    bool rival = rival_map(sess().overlay);
    bool other = false;
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        if (i == player || (rival && i >= 1 && i <= 3)) {
            continue;
        }
        uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        int control = word(rdram, base + 0x24);
        int hearts = word(rdram, base + 0x14);
        if ((control == 1 || control == 2) && hearts > 0) {
            other = true;
            break;
        }
    }
    int lives = word(rdram, 0x802AC624);
    bool unlimited = sess().unlimited;
    bool respawn = unlimited || other || lives > 1;
    if (!respawn) {
        return;
    }
    if (!unlimited && lives > 0) {
        set_word(rdram, 0x802AC624, lives - 1);
    }
    int hearts = 3;
    if (rule(rdram)[0x29] != 0) {
        int saved = rule_s32(rdram, 0x38)[player];
        if (saved > 0) {
            hearts = saved;
        }
    }
    uint32_t base = self;
    set_word(rdram, base + 0x14, hearts);
    set_word(rdram, base + 0x24, 1);
    ctx->r8 = hearts;
    if (rule(rdram)[0x28] != 0) {
        float x = f32_at(rdram, bm64::coop::kRuleBase + 0x2C);
        float y = f32_at(rdram, bm64::coop::kRuleBase + 0x30);
        float z = f32_at(rdram, bm64::coop::kRuleBase + 0x34);
        float angle = f32_at(rdram, base + 0x0C);
        place_human(rdram, player, x + kPartySpacing * static_cast<float>(party_rank(rdram, player)), y, z, angle);
    }
    // Bit 30 of the object's flags makes func_8024E088 call func_80248148 with kind 3 (an
    // instant kill) on every tick until it clears. The game's own death resets the object; a
    // respawn here keeps it, so without this the bomber dies and bursts hearts each tick.
    uint32_t obj = static_cast<uint32_t>(word(rdram, base + 0x28));
    if ((obj & 0xFF000000u) == 0x80000000u) {
        set_word(rdram, obj + 0x10, static_cast<int32_t>(static_cast<uint32_t>(word(rdram, obj + 0x10)) & ~0x40000000u));
    }
    std::printf("[bm64 coop] Player %d respawned, shared lives %d\n", player, word(rdram, 0x802AC624));
}

// func_802512F8 just moved one bomber onto that slot's spawn. The same point is the
// party's checkpoint, and every human is moved there.
extern "C" void bm64_coop_snap_party(uint8_t* rdram, recomp_context* ctx) {
    if (!story(rdram)) {
        return;
    }
    uint32_t snapped = static_cast<uint32_t>(ctx->r2);
    int slot = -1;
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        if (snapped == base) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = word(rdram, 0x802AC894u);
    }
    if (slot < 0 || slot >= bm64::coop::kPlayerSlots) {
        return;
    }
    uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride;
    float x = f32_at(rdram, base + 0x00);
    float y = f32_at(rdram, base + 0x04);
    float z = f32_at(rdram, base + 0x08);
    float angle = f32_at(rdram, base + 0x0C);
    write_checkpoint(rdram, x, y, z);
    // Coming back from a cutscene in another area, the game snaps the leader to its spawn (the
    // stage start on map 4.1). Teammates waiting with the parked leader are put back where they
    // stood by pause_with_parked_leader instead of being pulled to that spawn.
    if (pause_active(rdram)) {
        return;
    }
    int snapped_rank = party_rank(rdram, slot);
    for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
        if (i == slot) {
            continue;
        }
        uint32_t other = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        if (word(rdram, other + 0x24) != 1) {
            continue;
        }
        int rank = party_rank(rdram, i);
        place_human(rdram, i, x + kPartySpacing * static_cast<float>(rank == 0 ? (snapped_rank == 0 ? 1 : snapped_rank) : rank), y, z, angle);
    }
}

// g_SpawnPlayer loads the bomber's 12 parts from file 0x4A + slot (white, black, red, blue) for
// slots 0-3; slots 4-7 keep the file of the previous spawn, so rival-map teammates came out white.
// a1 holds the parts file for g_AssignModelParts. Co-op uses the Battle colors by party rank on
// every map, rival maps included: white, black, red, blue.
extern "C" void bm64_coop_spawn_color(uint8_t* rdram, recomp_context* ctx) {
    if (!story(rdram)) {
        return;
    }
    int slot = static_cast<int>(ctx->r19);
    if (slot < 0 || slot >= bm64::coop::kPlayerSlots) {
        return;
    }
    uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride;
    if (word(rdram, base + 0x24) != 1) {
        return;
    }
    int rank = party_rank(rdram, slot);
    if (rank < 0) {
        return;
    }
    static constexpr int32_t colors[4] = { 0x4A, 0x4B, 0x4C, 0x4D };
    ctx->r5 = colors[rank % 4];
}
extern "C" void bm64_coop_before_stage(uint8_t* rdram) {
    Session& s = sess();
    if (!story(rdram)) {
        return;
    }
    // func_80245CDC visits every reserved slot. Rival maps drive 1..3 themselves, so co-op
    // players go in 0 and 4..7 there, and slots 1..3 must not read a teammate's pad.
    bool rival = rival_map(s.overlay);
    uint8_t* ports = port_map(rdram);
    std::memset(ports, 0, bm64::coop::kPlayerSlots);
    if (rival) {
        ports[1] = ports[2] = ports[3] = kPortSilent;
    }
    int placed = 0;
    for (int i = 0; i < bm64::coop::kPlayerSlots && placed < s.players; i++) {
        if (rival && (i == 1 || i == 2 || i == 3)) {
            continue;
        }
        uint32_t slot = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        int control = word(rdram, slot + 0x24);
        if (control != 0 && control != 1) {
            continue;
        }
        if (control == 0) {
            set_word(rdram, slot + 0x24, 1);
            // g_SpawnPlayer copies these floats onto the bomber. Offset teammates along X so they are not inside the leader.
            if (i != 0) {
                for (int axis = 0; axis < 4; axis++) {
                    set_word(rdram, slot + static_cast<uint32_t>(axis) * 4,
                        word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(axis) * 4));
                }
                float x = std::bit_cast<float>(static_cast<uint32_t>(word(rdram, slot)));
                x += kPartySpacing * static_cast<float>(placed);
                set_word(rdram, slot, static_cast<int32_t>(std::bit_cast<uint32_t>(x)));
            }
        }
        // Only slot 0's hearts come from the save. A teammate left at 0 goes down on its first hit.
        int leader_hearts = word(rdram, bm64::coop::kPlayerBase + 0x14);
        if (i != 0 && word(rdram, slot + 0x14) <= 0 && leader_hearts > 0) {
            set_word(rdram, slot + 0x14, leader_hearts);
        }
        // Session player n's pad is on port n. A bomber reads the port equal to its slot.
        ports[i] = static_cast<uint8_t>(placed + 1);
        placed++;
    }
    if (placed < s.players) {
        std::printf("[bm64 coop] Overlay %d fit %d of %d players into the spawn loop\n",
            s.overlay, placed, s.players);
    }
    float x = f32_at(rdram, bm64::coop::kPlayerBase + 0x00);
    float y = f32_at(rdram, bm64::coop::kPlayerBase + 0x04);
    float z = f32_at(rdram, bm64::coop::kPlayerBase + 0x08);
    write_checkpoint(rdram, x, y, z);
}

// Pieces of the camera the controller and the lookAt process write. Copied as raw RDRAM
// bytes so a round trip through a block is bit-identical. The 48 x 0x48 table that follows
// the eye at 0x8008E658 is shared world state (the bomb pool), so it stays out of the blocks.
struct CamPiece {
    uint32_t live;
    uint32_t size;
    uint32_t offset;
};

constexpr CamPiece kCamPieces[] = {
    { 0x8008E648u, 0x10, 0x000 }, // eye
    { 0x8008F3D8u, 0x60, 0x010 }, // look-at, rotation, mode
    { 0x8008FFE8u, 0x98, 0x070 }, // controller
    { 0x8029FEA0u, 0x1B0, 0x110 }, // camera tables
    { 0x802A5A50u, 0x10, 0x2C0 },
};
static_assert(0x2C0 + 0x10 <= bm64::coop::kCamStride);

// Camera rotation (pitch, yaw, roll in degrees) at 0x8008F3F8, inside the core piece.
constexpr uint32_t kCamRotation = 0x010 + 0x20;

uint8_t* ram(uint8_t* rdram, uint32_t addr) {
    return rdram + (addr - 0x80000000u);
}

uint32_t block_addr(int slot) {
    return bm64::coop::kCamBlocks + static_cast<uint32_t>(slot) * bm64::coop::kCamStride;
}

void copy_block(uint8_t* rdram, int slot, bool to_live) {
    uint32_t block = block_addr(slot);
    for (const CamPiece& piece : kCamPieces) {
        uint32_t src = to_live ? block + piece.offset : piece.live;
        uint32_t dst = to_live ? piece.live : block + piece.offset;
        std::memcpy(ram(rdram, dst), ram(rdram, src), piece.size);
    }
}

uint8_t meta(uint8_t* rdram, uint32_t offset) {
    return ram(rdram, bm64::coop::kCamMeta)[offset];
}

void set_meta(uint8_t* rdram, uint32_t offset, uint8_t value) {
    ram(rdram, bm64::coop::kCamMeta)[offset] = value;
}

void bind_camera_player(uint8_t* rdram, int slot) {
    ram(rdram, bm64::coop::kOverrideBase)[bm64::coop::kCatCamera] = static_cast<uint8_t>(slot);
    set_meta(rdram, 3, static_cast<uint8_t>(slot));
}

void unbind_camera_player(uint8_t* rdram) {
    ram(rdram, bm64::coop::kOverrideBase)[bm64::coop::kCatCamera] = 0xFF;
    set_meta(rdram, 3, 0);
}

// Human bombers only. AI keeps the shared camera, which is the block left live between updates.
// The controller dereferences the bomber's body, which stage teardown (such as leaving the
// attract demo) clears before the camera process stops.
int rebuild_subjects(uint8_t* rdram) {
    int count = 0;
    uint8_t* subjects = ram(rdram, bm64::coop::kCamMeta) + 8;
    for (int i = 0; i < bm64::coop::kPlayerSlots && count < 8; i++) {
        int control = word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride + 0x24);
        uint32_t body;
        if (control == 1 && slot_body(rdram, i, body)) {
            subjects[count++] = static_cast<uint8_t>(i);
        }
    }
    set_meta(rdram, 0, static_cast<uint8_t>(count));
    return count;
}

void ensure_blocks(uint8_t* rdram) {
    if (meta(rdram, 4) != 0) {
        return;
    }
    for (int i = 0; i < bm64::coop::kCamCount; i++) {
        copy_block(rdram, i, false);
    }
    set_meta(rdram, 4, 1);
}

bool camera_multi(uint8_t* rdram) {
    return story(rdram) && meta(rdram, 2) != 0;
}

// True when `slot` is a human with its own camera block this tick.
bool has_camera_block(uint8_t* rdram, int slot) {
    if (!story(rdram) || meta(rdram, 4) == 0 || slot < 0 || slot >= bm64::coop::kCamCount) {
        return false;
    }
    int count = meta(rdram, 0);
    if (count < 2) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (ram(rdram, bm64::coop::kCamMeta)[8 + i] == static_cast<uint8_t>(slot)) {
            return true;
        }
    }
    return false;
}

int g_broadcasts = 0;
// BM64_CAM_TRACE: which process entries broadcast camera commands since the last trace line.
struct BroadcastSource {
    uint32_t func;
    int count;
};
BroadcastSource g_broadcast_sources[8]{};

constexpr uint32_t kCurrentProcess = 0x802AC344u;

uint32_t process_func(uint8_t* rdram, uint32_t proc);

// Host log only. `partial` is 1 for a command copied component by component (see bm64_camera_command).
void note_broadcast_source(uint8_t* rdram, int partial) {
    uint32_t func = process_func(rdram, static_cast<uint32_t>(word(rdram, kCurrentProcess))) | (partial ? 1u : 0u);
    for (BroadcastSource& src : g_broadcast_sources) {
        if (src.count == 0 || src.func == func) {
            src.func = func;
            src.count++;
            break;
        }
    }
}

constexpr uint32_t kProcessPool = 0x80063370u;
constexpr uint32_t kProcessSize = 0x8A0u;
constexpr uint32_t kCameraController = 0x80235D7Cu;
constexpr uint32_t kCameraAnimation = 0x80236658u;

uint32_t process_func(uint8_t* rdram, uint32_t proc) {
    return proc != 0 ? static_cast<uint32_t>(word(rdram, proc + 0x0C)) : 0u;
}

int process_index(uint32_t proc) {
    if (proc < kProcessPool || (proc - kProcessPool) % kProcessSize != 0) {
        return -1;
    }
    uint32_t index = (proc - kProcessPool) / kProcessSize;
    return index < bm64::coop::kProcessSlots ? static_cast<int>(index) : -1;
}

uint8_t* process_owner(uint8_t* rdram, int index) {
    return ram(rdram, bm64::coop::kCamProcOwner + static_cast<uint32_t>(index));
}

// func_80235D7C is the camera controller process's entry, so its frame (0x90 bytes) sits at the top
// of the process stack. func_80235934 keeps the rotation copy ($s7, sp + 0x7C) and the C-button
// halfword ($fp, sp + 0x8A) there between calls; one copy would serve every human's pass, so the
// 16 bytes are kept per camera slot like the blocks.
uint32_t controller_locals(uint8_t* rdram) {
    uint32_t proc = static_cast<uint32_t>(word(rdram, kCurrentProcess));
    if (process_func(rdram, proc) != kCameraController) {
        return 0;
    }
    uint32_t top = static_cast<uint32_t>(word(rdram, proc + 0x24)) + static_cast<uint32_t>(word(rdram, proc + 0x20));
    return top - 0x90 + 0x7C;
}

uint8_t* saved_locals(uint8_t* rdram, int slot) {
    return ram(rdram, bm64::coop::kCamLocals + static_cast<uint32_t>(slot) * bm64::coop::kCamLocalsSize);
}

void load_pass(uint8_t* rdram, int slot) {
    copy_block(rdram, slot, true);
    if (uint32_t addr = controller_locals(rdram)) {
        std::memcpy(ram(rdram, addr), saved_locals(rdram, slot), bm64::coop::kCamLocalsSize);
    }
}

void save_pass(uint8_t* rdram, int slot) {
    copy_block(rdram, slot, false);
    if (uint32_t addr = controller_locals(rdram)) {
        std::memcpy(saved_locals(rdram, slot), ram(rdram, addr), bm64::coop::kCamLocalsSize);
    }
}

void trace_camera(uint8_t* rdram) {
    static const int every = [] {
        const char* v = std::getenv("BM64_CAM_TRACE");
        return v ? std::atoi(v) : 0;
    }();
    if (every <= 0 || bm64::debug::tick() % static_cast<uint32_t>(every) != 0) {
        return;
    }
    auto f = [&](uint32_t addr) { return std::bit_cast<float>(static_cast<uint32_t>(word(rdram, addr))); };
    auto h = [&](uint32_t addr) { return static_cast<unsigned>(MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(addr)))); };
    std::printf("[bm64 cam] tick %u meta %d %d %d %d %d subj %d %d view %d bc %d live eye (%.0f,%.0f,%.0f) rot (%.1f,%.1f,%.1f) st %X/%X idx %d",
        bm64::debug::tick(), meta(rdram, 0), meta(rdram, 1), meta(rdram, 2), meta(rdram, 3), meta(rdram, 4),
        meta(rdram, 8), meta(rdram, 9), bm64::coop::view_player(), g_broadcasts,
        f(0x8008E648u), f(0x8008E64Cu), f(0x8008E650u), f(0x8008F3F8u), f(0x8008F3FCu), f(0x8008F400u),
        h(0x8008F426u), h(0x8008F424u), word(rdram, 0x8008F42Cu));
    g_broadcasts = 0;
    if (uint32_t addr = controller_locals(rdram)) {
        std::printf(" btn %X rotcopy %.1f", h(addr + 0x0E), f(addr));
    }
    for (int i = 0; i < meta(rdram, 0) && i < 4; i++) {
        uint32_t b = block_addr(meta(rdram, 8 + i));
        std::printf(" | b%d eye (%.0f,%.0f,%.0f) at (%.0f,%.0f,%.0f) rot (%.1f,%.1f,%.1f) st %X/%X", meta(rdram, 8 + i),
            f(b), f(b + 4), f(b + 8), f(b + 0x10), f(b + 0x14), f(b + 0x18),
            f(b + kCamRotation), f(b + kCamRotation + 4), f(b + kCamRotation + 8),
            h(b + 0x10 + 0x4E), h(b + 0x10 + 0x4C));
    }
    for (BroadcastSource& src : g_broadcast_sources) {
        if (src.count > 0) {
            std::printf(" | bc %08X x%d", src.func, src.count);
        }
        src = {};
    }
    std::printf("\n");
}

extern "C" void bm64_camera_enter(uint8_t* rdram) {
    if (!story(rdram)) {
        return;
    }
    if (meta(rdram, 1) == 0) {
        trace_camera(rdram);
    }
    int index = meta(rdram, 1);
    if (index == 0) {
        ensure_blocks(rdram);
        int count = rebuild_subjects(rdram);
        set_meta(rdram, 2, count >= 2 ? 1 : 0);
        if (count < 2) {
            if (count == 1) {
                copy_block(rdram, ram(rdram, bm64::coop::kCamMeta)[8], true);
            }
            unbind_camera_player(rdram);
            return;
        }
        save_pass(rdram, ram(rdram, bm64::coop::kCamMeta)[8]);
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::printf("[bm64 coop] Per-player cameras, %d bombers, local P%d\n", count, sess().local_player);
        }
    }
    if (!camera_multi(rdram)) {
        return;
    }
    int slot = ram(rdram, bm64::coop::kCamMeta)[8 + index];
    load_pass(rdram, slot);
    bind_camera_player(rdram, slot);
}

extern "C" int bm64_camera_next(uint8_t* rdram) {
    if (!camera_multi(rdram)) {
        return 0;
    }
    int index = meta(rdram, 1);
    int count = meta(rdram, 0);
    int slot = ram(rdram, bm64::coop::kCamMeta)[8 + index];
    save_pass(rdram, slot);
    index++;
    uint32_t body;
    while (index < count && !slot_body(rdram, ram(rdram, bm64::coop::kCamMeta)[8 + index], body)) {
        index++;
    }
    if (index < count) {
        set_meta(rdram, 1, static_cast<uint8_t>(index));
        int next = ram(rdram, bm64::coop::kCamMeta)[8 + index];
        load_pass(rdram, next);
        bind_camera_player(rdram, next);
        return 1;
    }
    set_meta(rdram, 1, 0);
    set_meta(rdram, 2, 0);
    load_pass(rdram, ram(rdram, bm64::coop::kCamMeta)[8]);
    unbind_camera_player(rdram);
    return 0;
}

// HuPrcCreate. A camera animation (func_80236658, started by the C-buttons in func_80235934)
// created during a human's controller pass belongs to that human's camera block.
extern "C" void bm64_coop_process_created(uint8_t* rdram, recomp_context* ctx) {
    uint32_t proc = _arg<0, uint32_t>(rdram, ctx);
    int index = process_index(proc);
    if (index < 0) {
        return;
    }
    uint8_t owner = 0;
    uint32_t current = static_cast<uint32_t>(word(rdram, kCurrentProcess));
    if (story(rdram) && camera_multi(rdram) && process_func(rdram, proc) == kCameraAnimation &&
        process_func(rdram, current) == kCameraController) {
        owner = static_cast<uint8_t>(meta(rdram, 3) + 1);
    }
    *process_owner(rdram, index) = owner;
}

// The scheduler around each resume of a process. An owned camera animation runs with its
// human's block live; between processes live memory stays on the first human, as the hash expects.
extern "C" void bm64_coop_process_resume(uint8_t* rdram, recomp_context* ctx) {
    uint32_t proc = _arg<0, uint32_t>(rdram, ctx);
    bool entering = _arg<1, int32_t>(rdram, ctx) != 0;
    int index = process_index(proc);
    if (index < 0 || !story(rdram) || meta(rdram, 4) == 0) {
        return;
    }
    int owner = *process_owner(rdram, index);
    if (owner == 0) {
        return;
    }
    int slot = owner - 1;
    int primary = ram(rdram, bm64::coop::kCamMeta)[8];
    if (slot == primary || slot >= bm64::coop::kCamCount) {
        return;
    }
    if (entering) {
        copy_block(rdram, primary, false);
        copy_block(rdram, slot, true);
    } else {
        copy_block(rdram, slot, false);
        copy_block(rdram, primary, true);
    }
}

extern "C" int bm64_camera_mode_word(uint8_t* rdram) {
    return word(rdram, 0x8008F428u);
}

// The C-button read in func_80235934 passes this slot to func_802399AC, whose port hook
// turns it into the pad that drives the slot.
extern "C" int bm64_camera_port(uint8_t* rdram) {
    if (!story(rdram)) {
        return 0;
    }
    return meta(rdram, 3);
}

// Camera commands from cutscenes and overlays apply to every human. The controller's own calls
// (a pass is in progress) and a human's C-button animation only move that human's block.
extern "C" void bm64_camera_broadcast(uint8_t* rdram) {
    if (!story(rdram)) {
        return;
    }
    if (camera_multi(rdram)) {
        return;
    }
    int index = process_index(static_cast<uint32_t>(word(rdram, kCurrentProcess)));
    if (index >= 0 && *process_owner(rdram, index) != 0) {
        return;
    }
    ensure_blocks(rdram);
    g_broadcasts++;
    note_broadcast_source(rdram, 0);
    for (int i = 0; i < bm64::coop::kCamCount; i++) {
        copy_block(rdram, i, false);
    }
}

// Processes whose camera commands run during normal play every tick and must not overwrite
// the other humans' cameras. func_8025C840 is the rival-fight camera (map 4.10, 2026-11-25
// trace: 119 broadcasts every 120 ticks): each tick it takes the largest distance between a
// computer bomber (AI records at 0x800BCFE8, stride 0x128) and its target and sets the rotation
// to (0, 45, zoom) to fit it. Its look-at comes from the camera controller, which follows each
// human in their own pass. The scripted-bomber update func_802553D4 also sets the camera, but
// only for one-off staging (the rival's entrance and defeat) that every human should see.
bool partial_camera_source(uint32_t func) {
    return func == 0x8025C840u;
}

// End of func_80234778 (kind 0: set the rotation at 0x8008F3F8) or func_802346C8 (kind 1: set the
// eye at 0x8008E648). Each argument (f12, f14, f16) equal to 32768.0 leaves that component alone.
// During a cutscene, or while the leader is parked, every human sees the scripted camera, so the
// whole live camera is copied to every block as before. Otherwise (map 4.10's fight script, which
// calls both every tick) only the components the command wrote are copied, so each human's camera
// keeps following its own bomber.
extern "C" void bm64_camera_command(uint8_t* rdram, recomp_context* ctx, int kind) {
    if (!story(rdram) || camera_multi(rdram)) {
        return;
    }
    int index = process_index(static_cast<uint32_t>(word(rdram, kCurrentProcess)));
    if (index >= 0 && *process_owner(rdram, index) != 0) {
        return;
    }
    uint32_t source = process_func(rdram, static_cast<uint32_t>(word(rdram, kCurrentProcess)));
    if (!partial_camera_source(source) || cutscene_running(rdram) || pause_active(rdram)) {
        bm64_camera_broadcast(rdram);
        return;
    }
    ensure_blocks(rdram);
    g_broadcasts++;
    note_broadcast_source(rdram, 1);
    const float args[3] = { ctx->f12.fl, ctx->f14.fl, ctx->f16.fl };
    uint32_t live = kind == 0 ? 0x8008F3F8u : 0x8008E648u;
    uint32_t offset = kind == 0 ? kCamRotation : 0x000u;
    for (int c = 0; c < 3; c++) {
        if (args[c] == 32768.0f) {
            continue;
        }
        int32_t value = word(rdram, live + static_cast<uint32_t>(c) * 4);
        for (int i = 0; i < bm64::coop::kCamCount; i++) {
            set_word(rdram, block_addr(i) + offset + static_cast<uint32_t>(c) * 4, value);
        }
    }
}

extern "C" void bm64_camera_frame(uint8_t* rdram, recomp_context* ctx) {
    int local = slot_of_player(rdram, bm64::coop::view_player());
    int primary = ram(rdram, bm64::coop::kCamMeta)[8];
    if (local == primary || !has_camera_block(rdram, local)) {
        return;
    }
    uint32_t block = block_addr(local);
    auto ram_f = [&](uint32_t addr) {
        return std::bit_cast<float>(static_cast<uint32_t>(word(rdram, addr)));
    };
    float eye_x = ram_f(block + 0x00);
    float eye_y = ram_f(block + 0x04);
    float eye_z = ram_f(block + 0x08);
    float at_x = ram_f(block + 0x10);
    float at_y = ram_f(block + 0x14);
    float at_z = ram_f(block + 0x18);
    // func_80236F54 swaps eye and look-at when D_8008F404 is 1. Core starts at block+0x10.
    if (word(rdram, block + 0x10 + 0x2C) == 1) {
        std::swap(eye_x, at_x);
        std::swap(eye_y, at_y);
        std::swap(eye_z, at_z);
    }
    bm64::session::apply_camera_bias(eye_x, eye_y, eye_z);
    auto store = [&](gpr base, int offset, float value) {
        MEM_W(offset, base) = static_cast<int32_t>(std::bit_cast<uint32_t>(value));
    };
    store(ctx->r4, 0, eye_x);
    store(ctx->r4, 4, eye_y);
    store(ctx->r4, 8, eye_z);
    store(ctx->r5, 0, at_x);
    store(ctx->r5, 4, at_y);
    store(ctx->r5, 8, at_z);
}

// Entry of every input accessor that takes a port in a0. Bombers pass their slot, and only
// four ports exist, so a slot is mapped to the pad of the session player who owns it.
// Returns -1 when the slot must read no input.
extern "C" int bm64_coop_port(uint8_t* rdram, int port) {
    if (!story(rdram) || port < 0 || port >= bm64::coop::kPlayerSlots) {
        return port;
    }
    return player_of_slot(rdram, port);
}

namespace {
// Set just before func_8024E088 asks func_80234858 for the camera rotation, consumed by that call.
int g_rotation_slot = -1;
}

extern "C" void bm64_camera_for_bomber(int slot) {
    g_rotation_slot = slot;
}

// Movement turns the stick by the camera yaw. A teammate steers by their own camera, which is
// in their block; live memory holds the first human's camera between updates.
extern "C" int bm64_camera_rotation(uint8_t* rdram, recomp_context* ctx) {
    int slot = g_rotation_slot;
    g_rotation_slot = -1;
    int primary = ram(rdram, bm64::coop::kCamMeta)[8];
    if (slot < 0 || slot == primary || !has_camera_block(rdram, slot)) {
        return 0;
    }
    uint32_t rotation = block_addr(slot) + kCamRotation;
    gpr outs[3] = { ctx->r4, ctx->r5, ctx->r6 };
    for (int i = 0; i < 3; i++) {
        if (outs[i] != 0) {
            MEM_W(0, outs[i]) = word(rdram, rotation + static_cast<uint32_t>(i) * 4);
        }
    }
    return 1;
}

// func_80232FF8 (objects, mode 1) and func_8027CE10 (items) hide anything farther from the
// camera's pivot than its arm (scaled, for objects): the pivot is D_8008F3E8 and the arm runs to
// the eye, swapped when D_8008F404 is 1. Live memory holds the first human's camera, so only that
// human's surroundings stayed visible and in play; around a teammate, objects had no model and
// enemies did not start. Just before each function compares, this hands it whichever human's
// distance and arm put the thing nearest, relative to the arm, so it stays shown for anyone close.
// `arm`, `dist` and `pos` are stack offsets of the arm length, the distance and the object
// position the function computed from the live camera.
extern "C" void bm64_coop_range_test(uint8_t* rdram, recomp_context* ctx, int arm_off, int dist_off, int pos_off) {
    if (!story(rdram) || meta(rdram, 4) == 0 || meta(rdram, 2) != 0) {
        return;
    }
    int count = meta(rdram, 0);
    if (count < 2) {
        return;
    }
    auto stack_f = [&](int off) {
        return std::bit_cast<float>(static_cast<uint32_t>(MEM_W(off, ctx->r29)));
    };
    float arm = stack_f(arm_off);
    float dist = stack_f(dist_off);
    float px = stack_f(pos_off);
    float py = stack_f(pos_off + 4);
    float pz = stack_f(pos_off + 8);
    if (!(arm > 0.0f)) {
        return;
    }
    float best_ratio = dist / arm;
    float best_arm = arm;
    float best_dist = dist;
    int primary = ram(rdram, bm64::coop::kCamMeta)[8];
    for (int i = 0; i < count && i < 8; i++) {
        int slot = ram(rdram, bm64::coop::kCamMeta)[8 + i];
        if (slot == primary || slot >= bm64::coop::kCamCount) {
            continue;
        }
        uint32_t b = block_addr(slot);
        float ex = f32_at(rdram, b + 0x00), ey = f32_at(rdram, b + 0x04), ez = f32_at(rdram, b + 0x08);
        // D_8008F3E8 is the core piece (block + 0x10) + 0x10; D_8008F404 is core + 0x2C.
        float cx = f32_at(rdram, b + 0x20), cy = f32_at(rdram, b + 0x24), cz = f32_at(rdram, b + 0x28);
        if (word(rdram, b + 0x10 + 0x2C) == 1) {
            std::swap(ex, cx);
            std::swap(ey, cy);
            std::swap(ez, cz);
        }
        float ax = cx - ex, ay = cy - ey, az = cz - ez;
        float h_arm = std::sqrt(ax * ax + ay * ay + az * az);
        if (!(h_arm > 0.0f)) {
            continue;
        }
        float dx = px - cx, dy = py - cy, dz = pz - cz;
        float h_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float ratio = h_dist / h_arm;
        if (ratio < best_ratio) {
            best_ratio = ratio;
            best_arm = h_arm;
            best_dist = h_dist;
        }
    }
    if (best_arm != arm || best_dist != dist) {
        MEM_W(arm_off, ctx->r29) = static_cast<int32_t>(std::bit_cast<uint32_t>(best_arm));
        MEM_W(dist_off, ctx->r29) = static_cast<int32_t>(std::bit_cast<uint32_t>(best_dist));
    }
}

// The draw pass (func_802290CC) asks func_80237478 for the camera yaw into D_802A5468/D_802A546C,
// and func_802289C0 turns every model node with flag 0x2000 to it: flat objects such as map 4.1's
// pots. Live memory holds the first human's camera, so they faced that human's camera whichever
// camera the window showed. bm64_billboard_view(rdram, 1) swaps the viewed player's eye and core
// pieces in so the hook can ask again, and (rdram, 0) puts the first human's back. Both words are
// render output: a session leaves them out of the state hash.
namespace {
uint8_t g_billboard_saved[0x10 + 0x60];
bool g_billboard_swapped = false;
}

extern "C" int bm64_billboard_view(uint8_t* rdram, int enter) {
    if (!enter) {
        if (g_billboard_swapped) {
            std::memcpy(ram(rdram, kCamPieces[0].live), g_billboard_saved, kCamPieces[0].size);
            std::memcpy(ram(rdram, kCamPieces[1].live), g_billboard_saved + kCamPieces[0].size, kCamPieces[1].size);
            g_billboard_swapped = false;
        }
        return 0;
    }
    int local = slot_of_player(rdram, bm64::coop::view_player());
    int primary = ram(rdram, bm64::coop::kCamMeta)[8];
    if (local == primary || !has_camera_block(rdram, local)) {
        return 0;
    }
    uint32_t block = block_addr(local);
    std::memcpy(g_billboard_saved, ram(rdram, kCamPieces[0].live), kCamPieces[0].size);
    std::memcpy(g_billboard_saved + kCamPieces[0].size, ram(rdram, kCamPieces[1].live), kCamPieces[1].size);
    std::memcpy(ram(rdram, kCamPieces[0].live), ram(rdram, block + kCamPieces[0].offset), kCamPieces[0].size);
    std::memcpy(ram(rdram, kCamPieces[1].live), ram(rdram, block + kCamPieces[1].offset), kCamPieces[1].size);
    g_billboard_swapped = true;
    return 1;
}

// Camera mode commands from scripts: func_802362B8(mask) clears flag bits of D_8008F428,
// func_802362D4(mask) sets them, func_802350F8(m) sets D_8008F404 to 0 or 1. They changed only the
// live camera, so another human's block kept old modes. Map 4.10's rival entrance clears the follow
// bit 0x10 and sets it again later; the second human's block kept it clear and their camera stopped
// following them for the whole fight. Called at each command's entry with its argument.
extern "C" void bm64_camera_mode_command(uint8_t* rdram, recomp_context* ctx, int kind) {
    if (!story(rdram) || camera_multi(rdram) || meta(rdram, 4) == 0) {
        return;
    }
    int index = process_index(static_cast<uint32_t>(word(rdram, kCurrentProcess)));
    if (index >= 0 && *process_owner(rdram, index) != 0) {
        return;
    }
    uint32_t arg = static_cast<uint32_t>(ctx->r4);
    for (int i = 0; i < bm64::coop::kCamCount; i++) {
        uint32_t b = block_addr(i);
        if (kind == 0) {
            set_word(rdram, b + 0x60, static_cast<int32_t>(static_cast<uint32_t>(word(rdram, b + 0x60)) & ~arg));
        }
        else if (kind == 1) {
            set_word(rdram, b + 0x60, static_cast<int32_t>(static_cast<uint32_t>(word(rdram, b + 0x60)) | arg));
        }
        else if (kind == 2 && arg <= 1) {
            set_word(rdram, b + 0x3C, static_cast<int32_t>(arg));
        }
    }
}

// BM64_LOG_SPAWNS: log each g_spawnEnemyFromAllocation call (tick, running process, arguments, the
// two first humans' positions). Log only; used to find which spawn checks only look at the leader.
extern "C" void bm64_log_spawn(uint8_t* rdram, recomp_context* ctx) {
    static const bool on = std::getenv("BM64_LOG_SPAWNS") != nullptr;
    if (!on) {
        return;
    }
    uint32_t func = process_func(rdram, static_cast<uint32_t>(word(rdram, kCurrentProcess)));
    float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    body_pos(rdram, slot_base(0), x0, y0, z0);
    body_pos(rdram, slot_base(1), x1, y1, z1);
    std::printf("[bm64 spawn] tick %u process %08X a0 %08X a1 %.0f a2 %.0f a3 %.0f | p0 (%.0f,%.0f,%.0f) p1 (%.0f,%.0f,%.0f)\n",
        static_cast<unsigned>(bm64::debug::tick()), func, static_cast<uint32_t>(ctx->r4), ctx->f12.fl, ctx->f14.fl,
        std::bit_cast<float>(static_cast<uint32_t>(ctx->r6)), x0, y0, z0, x1, y1, z1);
}
