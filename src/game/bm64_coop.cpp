#include "bm64_coop.h"

#include "bm64_debug.h"
#include "bm64_session.h"
#include "librecomp/helpers.hpp"
#include "recomp.h"

#include <bit>
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

int resolved_slot(uint8_t* rdram) {
    int slot = word(rdram, 0x802AC894u);
    if (!sess().on) {
        return slot;
    }
    uint32_t proc = static_cast<uint32_t>(word(rdram, 0x802AC344u));
    uint32_t func = (proc & 0xFF000000u) == 0x80000000u ? static_cast<uint32_t>(word(rdram, proc + 0x0C)) : 0;
    int category = category_of(sess().overlay, func);
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
    _return(ctx, resolved_slot(rdram));
}

extern "C" int bm64_resolved_slot(uint8_t* rdram) {
    return resolved_slot(rdram);
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

// Pieces of the camera the controller and the lookAt process write. Copied as raw RDRAM
// bytes so a round trip through a block is bit-identical. The 48-entry object table between
// the eye and the look-at is not part of this: func_80251C88 searches it for an object, and
// camera code does not own it.
struct CamPiece {
    uint32_t live;
    uint32_t size;
    uint32_t offset;
};

constexpr CamPiece kCamPieces[] = {
    { 0x8008E648u, 0x10, 0x000 }, // eye
    { 0x8008F3D8u, 0x60, 0x010 }, // look-at, mode, orbit
    { 0x8008FFE8u, 0x98, 0x070 }, // controller
    { 0x8029FEA0u, 0x1B0, 0x110 }, // camera tables
    { 0x802A5A50u, 0x10, 0x2C0 },
};

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
int rebuild_subjects(uint8_t* rdram) {
    int count = 0;
    uint8_t* subjects = ram(rdram, bm64::coop::kCamMeta) + 8;
    for (int i = 0; i < bm64::coop::kPlayerSlots && count < 8; i++) {
        int control = word(rdram, bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride + 0x24);
        if (control == 1) {
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
    return sess().on && meta(rdram, 2) != 0;
}

extern "C" void bm64_camera_enter(uint8_t* rdram) {
    if (!sess().on) {
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

extern "C" int bm64_camera_port(uint8_t* rdram) {
    if (!sess().on) {
        return 0;
    }
    return meta(rdram, 3);
}

extern "C" void bm64_camera_broadcast(uint8_t* rdram) {
    if (!sess().on) {
        return;
    }
    ensure_blocks(rdram);
    for (int i = 0; i < bm64::coop::kCamCount; i++) {
        copy_block(rdram, i, false);
    }
}

extern "C" void bm64_camera_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!bm64::coop::active() || meta(rdram, 4) == 0) {
        return;
    }
    int local = sess().local_player;
    int count = meta(rdram, 0);
    if (local <= 0 || count < 2) {
        return;
    }
    int primary = ram(rdram, bm64::coop::kCamMeta)[8];
    if (local == primary) {
        return;
    }
    bool listed = false;
    for (int i = 0; i < count; i++) {
        if (ram(rdram, bm64::coop::kCamMeta)[8 + i] == static_cast<uint8_t>(local)) {
            listed = true;
            break;
        }
    }
    if (!listed || local >= bm64::coop::kCamCount) {
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
