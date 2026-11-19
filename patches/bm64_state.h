#ifndef __BM64_STATE_PATCH_H__
#define __BM64_STATE_PATCH_H__

#include "patch_helpers.h"

// Host side of the main-loop step and the snapshot patch globals.
// bm64_step_begin may restore a snapshot. It returns 1 when this step should submit a frame.
DECLARE_FUNC(s32, bm64_restore_pending);
DECLARE_FUNC(s32, bm64_step_begin);
DECLARE_FUNC(s32, bm64_boot_timer);
DECLARE_FUNC(void, bm64_set_boot_timer, s32 timer);
DECLARE_FUNC(s32, bm64_take_overlay);
DECLARE_FUNC(s32, bm64_take_scroll_pending);
DECLARE_FUNC(void, bm64_set_scroll_pending, s32 pending);
DECLARE_FUNC(s32, bm64_skip_lookat);
DECLARE_FUNC(void, bm64_step_end);
DECLARE_FUNC(void, bm64_load_barrier, s32 overlay_id);

// Sound hooks in bm64.us.toml call these directly (host C, not the recomp calling convention).
// source: 0 global, 1 local. x/y/z are the raw argument bits (0 for a global sound).

#endif
