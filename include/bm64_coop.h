#ifndef __BM64_COOP_H__
#define __BM64_COOP_H__

#include <cstdint>

// Story co-op. The array the game uses is kPlayerBase (tools/re/relocate_players.py).
// Inactive unless a net session has more than one player, so replays stay single-player.
// The spawn loop still visits five slots. Eight are reserved so a later widen has room.
namespace bm64::coop {

constexpr uint32_t kPlayerBase = 0x80F00000;
constexpr uint32_t kPlayerStride = 0xC8;
constexpr int kPlayerSlots = 8;

void set_session(int players, int local_player, bool unlimited_lives);
void note_overlay(int overlay_id);
bool active();
bool omit_render_camera();
void apply_rules(uint8_t* rdram);
void hand_to_ai(uint8_t* rdram, int player);

} // namespace bm64::coop

#endif
