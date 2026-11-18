#ifndef __BM64_DEBUG_H__
#define __BM64_DEBUG_H__

#include <cstdint>
#include <string>
#include <vector>

// Debug tooling: per-tick state hashing, the debug overlay (F6), and input record/replay.
// Configured through environment variables, read once when the game boots:
//   BM64_HASH_LOG=<path>      write "tick,input_frame,overlay,total,<range hashes...>" every tick
//   BM64_HASH_RANGES=<list>   override hashed RDRAM ranges: comma-separated "[name:]start-end[+start-end...]",
//                             e.g. "bss:80063000-800BE848,heap:802B36D0-80400000"
//   BM64_HASH_DEAD_STACKS=1   also hash process stack memory below each suspended process's sp (skipped by default)
//   BM64_DUMP_TICKS=<list>    write RDRAM 0x80000000-0x80800000 (N64 byte order) at these ticks, e.g. "0,1,1702"
//   BM64_DUMP_DIR=<dir>       where dumps go (default "dumps"): tick_<n>.bin plus tick_<n>.mask (dead stack ranges)
//   BM64_RECORD=<path>        record the seed, the save file and every controller read to <path>
//   BM64_REPLAY=<path>        replay a recording (uses a copy of the recorded save; your save is untouched).
//                             The game refuses to start if both BM64_RECORD and BM64_REPLAY are set.
//   BM64_SEED=<hex>           force the RNG seed (ignored while replaying)
//   BM64_AUTOSTART=1          skip the launcher and start the game immediately
//   BM64_DEBUG_OVERLAY=1      show the debug overlay (normally toggled with F6) from the moment the game starts
//   BM64_EXIT_ON_REPLAY_END=1 quit when the replay runs out of input
//   BM64_EXIT_TICK=<n>        quit at tick n
namespace bm64::debug {
    // Shows or hides the debug overlay (F6). Safe to call from any thread once the game has started.
    void toggle_overlay();
    bool autostart_requested();
    // Non-empty if the debug environment variables conflict and the game shouldn't start.
    std::string environment_error();

    // A process coroutine that is suspended (or created but not started): its MIPS stack top and saved sp (0 if never run).
    struct ProcessStack {
        uint32_t process_id;
        uint32_t top;
        uint32_t sp;
    };
    void get_process_stacks(std::vector<ProcessStack>& out);
}

#endif
