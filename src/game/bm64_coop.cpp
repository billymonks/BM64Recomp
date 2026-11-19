#include "bm64_coop.h"

#include "bm64_debug.h"
#include "librecomp/helpers.hpp"
#include "recomp.h"

#include <bit>
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
    int slot = sess().override_slot;
    if (slot < 0) {
        slot = word(rdram, 0x802AC894u);
    }
    _return(ctx, slot);
}

extern "C" void bm64_coop_before_stage(uint8_t* rdram) {
    Session& s = sess();
    if (!s.on || s.players <= 1) {
        return;
    }
    // The spawn loop in func_80245CDC visits slots 0..4. Rival maps already use 1..3.
    bool rival = rival_map(s.overlay);
    int placed = 0;
    for (int i = 0; i < 5 && placed < s.players; i++) {
        if (rival && (i == 1 || i == 2 || i == 3)) {
            continue;
        }
        uint32_t addr = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride + 0x24;
        if (word(rdram, addr) == 0) {
            set_word(rdram, addr, 1);
        }
        placed++;
    }
    if (placed < s.players) {
        std::printf("[bm64 coop] Overlay %d fit %d of %d players into the 5-slot spawn loop\n",
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
