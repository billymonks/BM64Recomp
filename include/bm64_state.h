#ifndef __BM64_STATE_H__
#define __BM64_STATE_H__

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
