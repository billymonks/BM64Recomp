#ifndef __BM64_DEBUG_H__
#define __BM64_DEBUG_H__

#include <cstdint>
#include <filesystem>
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
//   BM64_SANDBOX=1            keep EEPROM in memory; the user's save file is not written
//   BM64_SANDBOX_IMAGE=<path> sandbox, starting from this image instead of the loaded save
//   BM64_SANDBOX_EXPORT=<path> write the in-memory save image here on exit (replay does this too)
//   BM64_CAMERA_BIAS=x,y,z    add this to the eye of the rendered lookAt only
//   BM64_AUTOSTART=1          skip the launcher and start the game immediately
//   BM64_DEBUG_OVERLAY=1      show the debug overlay (normally toggled with F6) from the moment the game starts
//   BM64_EXIT_ON_REPLAY_END=1 quit when the replay runs out of input
//   BM64_EXIT_TICK=<n>        quit at tick n
//   BM64_DISABLE_CHEATS=1     turn cheats off for this run (same switch netplay uses)
//   BM64_DIRECTOR=<hex list>  with BM64_RECORD: seeded random input on port 0 and a timed request for each
//                             scene (arenas through Battle setup). BM64_DIRECTOR_SECS per scene (60),
//                             BM64_DIRECTOR_BOOT ticks before the first (600), BM64_DIRECTOR_SEED (hex)
//   BM64_DIRECTOR_MENU=<list> scripted port 0 presses "tick:hexbuttons[:x:y]" before the random input; arenas
//                             then wait for Battle setup instead of warping (game-internals section 13)
//   BM64_SYNCTEST_DUMP=<dir>  on a SyncTest mismatch, write the window's forward images and the rerun image
//   BM64_SYNCTEST=<n>         savestate SyncTest, read in src/game/bm64_state.cpp. Unset or 0 is off.
//                             1 (or any other non-numeric text) rolls back one tick. 2..12 roll back
//                             that many ticks, then rerun them and compare. Above 12 is clamped to 12.
namespace bm64::debug {
    // Shows or hides the debug overlay (F6). Safe to call from any thread once the game has started.
    void toggle_overlay();
    bool autostart_requested();
    // Non-empty if the debug environment variables conflict and the game shouldn't start.
    std::string environment_error();
    // Netplay turns cheats off for the session. Offline play leaves this false.
    // Recording and replay suppress cheats on their own. BM64_DISABLE_CHEATS=1 sets this at boot.
    void set_cheats_disabled(bool disabled);

    // A process coroutine that is suspended (or created but not started): its MIPS stack top and saved sp (0 if never run).
    struct ProcessStack {
        uint32_t process_id;
        uint32_t top;
        uint32_t sp;
    };
    void get_process_stacks(std::vector<ProcessStack>& out);

    struct InputCursor {
        uint64_t input_frame = 0;
        uint64_t replay_cursor = 0;
        uint32_t mode = 0;
    };
    InputCursor input_cursor();
    void set_input_cursor(const InputCursor& cursor);
    // One-shot: the next controller sample is replaced with these 24 bytes.
    void set_pad_reuse(const uint8_t pads[24]);
    void copy_pads(uint8_t out[24]);
    // Replay is feeding frames from the file. False once the file has run out, and false while recording.
    bool input_replaying();
    bool input_replay_ended();
    uint64_t tick();
    void set_tick(uint64_t tick);
    uint64_t hash_state(uint8_t* rdram);
    void range_hashes(uint8_t* rdram, std::vector<std::string>& names, std::vector<uint64_t>& hashes);
    void set_quiet_log(bool quiet);
    bool booted();
    void dump_to(uint8_t* rdram, const std::filesystem::path& path);
    // The player array and co-op state, 0x80F00000 up to bm64::coop::kStateEnd, which dump_to does
    // not reach. `slots` points at the host copy of 0x80F00000. Big-endian, like dump_to.
    constexpr uint32_t kSlotDumpBase = 0x80F00000;
    constexpr uint32_t kSlotDumpBytes = 0x2000;
    void dump_slots_to(const uint8_t* slots, const std::filesystem::path& path);
    // End of the malloc heap. The sbrk patch in patches/required_patches.c (BM64_HEAP_END) lets
    // the break grow this far instead of the ROM's 0x80400000.
    constexpr uint32_t kHeapEnd = 0x80500000;
    constexpr uint32_t kHeapRomEnd = 0x80400000;
    // A segment ending at kHeapEnd is hashed and snapshotted only up to this address: the highest
    // break ever granted, rounded up to a page, never below kHeapRomEnd. Memory above it has never
    // been handed out, so it is still zero.
    uint32_t heap_top(const uint8_t* rdram);
}

#endif
