#ifndef __BM64_DEBUG_H__
#define __BM64_DEBUG_H__

#include "patch_helpers.h"

// Host hooks implemented in src/game/bm64_debug.cpp.
DECLARE_FUNC(void, bm64_debug_boot);
DECLARE_FUNC(u32, bm64_session_seed);
DECLARE_FUNC(void, bm64_camera_bias, float* eye);
DECLARE_FUNC(void, bm64_camera_frame, float* eye, float* at);
DECLARE_FUNC(s32, bm64_main_player, u32 caller);
DECLARE_FUNC(s32, bm64_cheats_allowed);
DECLARE_FUNC(void, bm64_debug_tick, s32 overlay_id);
DECLARE_FUNC(void, bm64_input_filter, OSContPad* pads);

#endif
