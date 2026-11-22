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

bool cutscene_running(uint8_t* rdram) {
    uint32_t head = 0x802AB200u;
    uint32_t proc = static_cast<uint32_t>(word(rdram, head + 0x89C));
    for (int n = 0; n < 80 && proc != 0 && proc != head; n++) {
        int mode = word(rdram, proc + 0x04);
        uint32_t func = static_cast<uint32_t>(word(rdram, proc + 0x0C));
        if (mode != 0 && category_of(sess().overlay, func) == bm64::coop::kCatCutscene) {
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
        apply_cutscene_hide(rdram);
        separate_humans(rdram);
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
// a1 holds the parts file for g_AssignModelParts. Every rival on all six rival maps comes from
// func_8025DC60, which always loads black (0x4B), so black goes last there.
extern "C" void bm64_coop_spawn_color(uint8_t* rdram, recomp_context* ctx) {
    if (!story(rdram)) {
        return;
    }
    int slot = static_cast<int>(ctx->r19);
    if (slot <= 0 || slot >= bm64::coop::kPlayerSlots) {
        return;
    }
    uint32_t base = bm64::coop::kPlayerBase + static_cast<uint32_t>(slot) * bm64::coop::kPlayerStride;
    if (word(rdram, base + 0x24) != 1) {
        return;
    }
    int rank = party_rank(rdram, slot);
    if (rank <= 0) {
        return;
    }
    static constexpr int32_t normal[3] = { 0x4B, 0x4C, 0x4D };
    static constexpr int32_t rival[3] = { 0x4C, 0x4D, 0x4B };
    const int32_t* colors = rival_map(sess().overlay) ? rival : normal;
    ctx->r5 = colors[(rank - 1) % 3];
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

extern "C" void bm64_camera_enter(uint8_t* rdram) {
    if (!story(rdram)) {
        return;
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
        copy_block(rdram, ram(rdram, bm64::coop::kCamMeta)[8], false);
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
    copy_block(rdram, slot, true);
    bind_camera_player(rdram, slot);
}

extern "C" int bm64_camera_next(uint8_t* rdram) {
    if (!camera_multi(rdram)) {
        return 0;
    }
    int index = meta(rdram, 1);
    int count = meta(rdram, 0);
    int slot = ram(rdram, bm64::coop::kCamMeta)[8 + index];
    copy_block(rdram, slot, false);
    index++;
    uint32_t body;
    while (index < count && !slot_body(rdram, ram(rdram, bm64::coop::kCamMeta)[8 + index], body)) {
        index++;
    }
    if (index < count) {
        set_meta(rdram, 1, static_cast<uint8_t>(index));
        int next = ram(rdram, bm64::coop::kCamMeta)[8 + index];
        copy_block(rdram, next, true);
        bind_camera_player(rdram, next);
        return 1;
    }
    set_meta(rdram, 1, 0);
    set_meta(rdram, 2, 0);
    copy_block(rdram, ram(rdram, bm64::coop::kCamMeta)[8], true);
    unbind_camera_player(rdram);
    return 0;
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

extern "C" void bm64_camera_broadcast(uint8_t* rdram) {
    if (!story(rdram)) {
        return;
    }
    ensure_blocks(rdram);
    for (int i = 0; i < bm64::coop::kCamCount; i++) {
        copy_block(rdram, i, false);
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
