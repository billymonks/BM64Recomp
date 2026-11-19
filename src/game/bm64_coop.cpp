#include "bm64_coop.h"

#include "bm64_debug.h"
#include "librecomp/helpers.hpp"
#include "recomp.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

struct Session {
    bool on = false;
    bool unlimited = false;
    int players = 1;
    int local_player = 0;
    int overlay = -1;
    int override_slot = -1;
    bool probed = false;
    bool ai_handed[bm64::coop::kPlayerSlots]{};
};

Session& sess() {
    static Session s;
    return s;
}

int32_t word(uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<int32_t*>(rdram + (addr - 0x80000000u));
}

void set_word(uint8_t* rdram, uint32_t addr, int32_t value) {
    *reinterpret_cast<int32_t*>(rdram + (addr - 0x80000000u)) = value;
}

bool rival_map(int id) {
    static const int kBanks[] = { 4, 6, 7, 10, 7, 15, 10, 13, 6, 2, 2, 2, 1, 10 };
    int remaining = id - 1;
    for (int bank = 0; bank < 14 && remaining >= 0; bank++) {
        if (remaining < kBanks[bank]) {
            int b = bank + 1;
            int sub = remaining + 1;
            return (b == 4 && sub == 10) || (b == 5 && sub == 7) || (b == 6 && sub == 15) ||
                (b == 8 && sub == 13) || (b == 9 && sub == 5) || (b == 9 && sub == 6);
        }
        remaining -= kBanks[bank];
    }
    return false;
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

bool omit_render_camera() {
    return sess().on;
}

void apply_rules(uint8_t* rdram) {
    Session& s = sess();
    if (s.unlimited) {
        set_word(rdram, 0x802AC624, 99);
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

void hand_to_ai(uint8_t* rdram, int player) {
    Session& s = sess();
    if (player < 0 || player >= kPlayerSlots || s.ai_handed[player]) {
        return;
    }
    uint32_t addr = kPlayerBase + static_cast<uint32_t>(player) * kPlayerStride + 0x24;
    if (word(rdram, addr) != 1) {
        return;
    }
    set_word(rdram, addr, 2);
    s.ai_handed[player] = true;
    std::printf("[bm64 coop] Player %d handed to AI\n", player);
}

} // namespace bm64::coop

extern "C" void bm64_main_player(uint8_t* rdram, recomp_context* ctx) {
    int slot = word(rdram, 0x802AC894u);
    if (sess().on) {
        uint32_t proc = static_cast<uint32_t>(word(rdram, 0x802AC344u));
        uint32_t func = (proc & 0xFF000000u) == 0x80000000u ? static_cast<uint32_t>(word(rdram, proc + 0x0C)) : 0;
        int category = category_of(sess().overlay, func);
        int override_slot = static_cast<int8_t>(rdram[(bm64::coop::kOverrideBase + static_cast<uint32_t>(category)) - 0x80000000u]);
        if (override_slot >= 0) {
            slot = override_slot;
        }
        else if (category == bm64::coop::kCatEnemy) {
            slot = sticky_target(rdram);
        }
    }
    _return(ctx, slot);
}

extern "C" void bm64_coop_before_stage(uint8_t* rdram) {
    Session& s = sess();
    if (!s.on || s.players <= 1) {
        return;
    }
    // func_80245CDC visits every reserved slot. Rival maps already use 1..3.
    bool rival = rival_map(s.overlay);
    int placed = 0;
    for (int i = 0; i < bm64::coop::kPlayerSlots && placed < s.players; i++) {
        if (rival && (i == 1 || i == 2 || i == 3)) {
            continue;
        }
        uint32_t slot = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride;
        if (word(rdram, slot + 0x24) == 0) {
            set_word(rdram, slot + 0x24, 1);
            // g_SpawnPlayer copies these floats onto the bomber. Offset teammates along X so they are not inside the leader.
            if (i != 0) {
                for (int axis = 0; axis < 4; axis++) {
                    set_word(rdram, slot + static_cast<uint32_t>(axis) * 4,
                        word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(axis) * 4));
                }
                float x = std::bit_cast<float>(static_cast<uint32_t>(word(rdram, slot)));
                x += 40.0f * static_cast<float>(placed);
                set_word(rdram, slot, static_cast<int32_t>(std::bit_cast<uint32_t>(x)));
            }
        }
        placed++;
    }
    if (placed < s.players) {
        std::printf("[bm64 coop] Overlay %d fit %d of %d players into the spawn loop\n",
            s.overlay, placed, s.players);
    }
}

extern "C" void bm64_camera_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!bm64::coop::active() || sess().local_player <= 0) {
        return;
    }
    uint32_t slot = bm64::coop::kPlayerBase +
        static_cast<uint32_t>(sess().local_player) * bm64::coop::kPlayerStride;
    uint32_t obj = static_cast<uint32_t>(word(rdram, slot + 0x28));
    if ((obj & 0xFF000000u) != 0x80000000u) {
        return;
    }
    uint32_t body = static_cast<uint32_t>(word(rdram, obj + 0x40));
    if ((body & 0xFF000000u) != 0x80000000u) {
        return;
    }
    auto load = [&](gpr base, int offset) {
        return std::bit_cast<float>(static_cast<uint32_t>(MEM_W(offset, base)));
    };
    auto store = [&](gpr base, int offset, float value) {
        MEM_W(offset, base) = static_cast<int32_t>(std::bit_cast<uint32_t>(value));
    };
    auto ram_f = [&](uint32_t addr) {
        return std::bit_cast<float>(static_cast<uint32_t>(word(rdram, addr)));
    };
    gpr eye = ctx->r4;
    gpr at = ctx->r5;
    float px = ram_f(body + 0x10);
    float py = ram_f(body + 0x14);
    float pz = ram_f(body + 0x18);
    store(eye, 0, load(eye, 0) + (px - load(at, 0)));
    store(eye, 4, load(eye, 4) + (py - load(at, 4)));
    store(eye, 8, load(eye, 8) + (pz - load(at, 8)));
    store(at, 0, px);
    store(at, 4, py);
    store(at, 8, pz);
}
