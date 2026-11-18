#ifndef __BM64_DEBUG_H__
#define __BM64_DEBUG_H__

#include <cstdint>

// Debug tooling: per-tick state hashing, the F3 debug overlay, and input record/replay.
// Configured through environment variables, read once when the game boots:
//   BM64_HASH_LOG=<path>      write "tick,input_frame,overlay,total,<range hashes...>" every tick
//   BM64_HASH_RANGES=<list>   override hashed RDRAM ranges, e.g. "80063000-800BEA60,802B36D0-80400000"
//   BM64_RECORD=<path>        record the seed, the save file and every controller read to <path>
//   BM64_REPLAY=<path>        replay a recording (uses a copy of the recorded save; your save is untouched)
//   BM64_SEED=<hex>           force the RNG seed (ignored while replaying)
namespace bm64::debug {
    void request_overlay_toggle();
}

#endif
