#ifndef __BM64_STATE_H__
#define __BM64_STATE_H__

#include <cstddef>
#include <cstdint>
#include <vector>

// In-process savestates: RDRAM ranges from the state-hash list, the sound-slot table, patch
// globals, and the native coroutine stacks. Restore only happens at the top of the main loop.
namespace bm64::state {
    struct Segment {
        uint32_t start;
        uint32_t end;
    };

    // The ranges the state hash covers. The snapshot copies these, plus the sound-slot table.
    const std::vector<Segment>& hash_segments();

    bool stacks_stable();
    // Every recomp_load_overlays call. The snapshot keeps the last one and maps it again on restore.
    void note_overlay_load(uint32_t rom, int32_t ram, uint32_t size);
    // Microseconds for the most recent save and load. Zero until one has run.
    void last_snapshot_cost(uint64_t& save_us, uint64_t& load_us);

    bool save_tick(uint64_t tick, uint8_t* rdram);
    bool load_tick(uint64_t tick, uint8_t* rdram);
    void drop_snapshots_before(uint64_t tick);
    // The tick currently being simulated (the value at the top of the step, before it increments).
    uint64_t sim_tick();

    // F5 / F7. Applied on the next main-loop step. Ignored until the game loop is running.
    void request_quicksave();
    void request_quickload();

    // Phase 3.6. Printed once, from the normal quit path and from std::exit.
    // speculative starts must stay 0: a real voice returned while the step was speculative.
    // virtual ids reused: bm64_sound_enter saw a start that already had a virtual id.
    void report_sound_counters();
    void sound_counter_line(char* buf, size_t cap);

    // True between the load and the last compared step of a SyncTest window. Replay exit waits
    // so a recording that ends mid-window is still compared.
    bool synctest_busy();
    // Prints "BM64 SYNCTEST PASS" once, on a clean exit. A failure has already printed and exited.
    void report_synctest();
}

namespace bm64::proc {
    bool save(std::vector<uint8_t>& out);
    bool load(const uint8_t* data, size_t size);
    bool stacks_moved();
}

// Pad injection for netplay and the second pass of a save/load test. A null pointer clears it.
void bm64_state_set_pad_inject(const uint8_t* pads24);
// Copies the injected pads and clears the request. Returns false if nothing is pending.
bool bm64_state_consume_inject(uint8_t out[24]);

#endif
