#ifndef __BM64_COOP_H__
#define __BM64_COOP_H__

#include <cstdint>

// Story co-op. The array the game uses is kPlayerBase (tools/re/relocate_players.py).
// Inactive unless a net session, BM64_COOP_LOCAL or a version 2 recording has more than one
// player, so older replays stay single-player.
// Spawn and update loops visit all kPlayerSlots. Empty slots stay controlType 0.
namespace bm64::coop {

constexpr uint32_t kPlayerBase = 0x80F00000;
constexpr uint32_t kPlayerStride = 0xC8;
constexpr int kPlayerSlots = 8;
// g_ComputerPlayerMovement, 8 x 0x10, immediately after the player array.
constexpr uint32_t kMoveBase = 0x80F00640;
constexpr uint32_t kMoveBytes = 0x80;
// func_80241BB0's side table. The first 0x64 bytes are the original initialized data.
constexpr uint32_t kHudBase = 0x80F006C0;
constexpr uint32_t kHudBytes = 0xC0;
constexpr uint32_t kHudCopy = 0x64;
constexpr uint32_t kHudOld = 0x802A07D4;
// One sticky target per process slot: int8 player, uint8 generation (tick / 60). Then one int8 override per category.
constexpr uint32_t kStickyBase = 0x80F00780;
constexpr uint32_t kProcessSlots = 0x50;
constexpr uint32_t kOverrideBase = kStickyBase + kProcessSlots * 2;
constexpr int kCategories = 8;
constexpr int kCatOther = 0;
constexpr int kCatCamera = 1;
constexpr int kCatEnemy = 2;
constexpr int kCatTrigger = 3;
constexpr int kCatCutscene = 4;
// Per-player camera pieces. Live memory stays on subjects[0] except while a camera
// process is updating another player, so peers hash the same block at tick boundaries.
constexpr uint32_t kCamMeta = kOverrideBase + kCategories;
constexpr uint32_t kCamBlocks = kCamMeta + 0x18;
constexpr int kCamCount = 8;
constexpr uint32_t kCamStride = 0x2D0;
constexpr uint32_t kCamStateEnd = kCamBlocks + kCamStride * kCamCount;
// Checkpoint, cutscene hide and port map. Snapped with the rest of the player state.
// 8 hidden flags, 8 saved collision words, one party checkpoint, 8 heart counts, 8 port bytes.
constexpr uint32_t kRuleBase = kCamStateEnd;
// Highest break the sbrk patch has granted (BM64_HEAP_MARK in patches/required_patches.c).
// It lives here so the snapshot and the hash carry it; see bm64::debug::heap_top.
constexpr uint32_t kHeapMark = kRuleBase + 0x60;
// One byte per process slot: 1 + the bomber slot whose camera block a camera-animation process
// (func_80236658) works on, or 0 for the shared live camera.
constexpr uint32_t kCamProcOwner = kHeapMark + 0x10;
// Per camera slot, the 16 bytes of func_80235D7C's frame that func_80235934 keeps between calls
// (the rotation copy at sp+0x7C and the C-button halfword at sp+0x8A) for its 0x8000 follow-up.
constexpr uint32_t kCamLocals = kCamProcOwner + kProcessSlots;
constexpr uint32_t kCamLocalsSize = 0x10;
constexpr uint32_t kStateEnd = kCamLocals + kCamCount * kCamLocalsSize;

int local_player();
// The session player whose camera is rendered. Host-only: it never touches game memory.
int view_player();
void cycle_view();

void set_session(int players, int local_player, bool unlimited_lives);
int session_players();
bool unlimited_lives();
void note_overlay(int overlay_id);
bool active();
bool omit_render_camera();
void apply_rules(uint8_t* rdram);
// A human whose session player left becomes an AI bomber (controlType 1 to 2).
void hand_to_ai(uint8_t* rdram, int player);

} // namespace bm64::coop

#endif
