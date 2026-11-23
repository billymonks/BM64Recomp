#ifndef __BM64_DEBUG_H__
#define __BM64_DEBUG_H__

#include "patch_helpers.h"

// Host hooks implemented in src/game/bm64_debug.cpp.
DECLARE_FUNC(void, bm64_debug_boot);
DECLARE_FUNC(u32, bm64_session_seed);
DECLARE_FUNC(void, bm64_camera_bias, float* eye);
DECLARE_FUNC(void, bm64_camera_frame, float* eye, float* at);
DECLARE_FUNC(s32, bm64_main_player);
DECLARE_FUNC(s32, bm64_coop_trigger_slot, s32 slot);
DECLARE_FUNC(void, bm64_coop_process_created, void* process);
DECLARE_FUNC(void, bm64_coop_process_resume, void* process, s32 entering);
DECLARE_FUNC(s32, bm64_cheats_allowed);
DECLARE_FUNC(void, bm64_debug_tick, s32 overlay_id);
DECLARE_FUNC(void, bm64_input_filter, OSContPad* pads);

#endif
