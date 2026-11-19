#ifndef __BM64_ONLINE_H__
#define __BM64_ONLINE_H__

#include <cstdint>

// Session handshake and the rollback driver. Inactive unless BM64_NET is set.
namespace bm64::online {
    bool active();
    int player_count();
    // Launcher buttons. Ignored when BM64_NET is already set.
    void prepare_loopback(bool host_side);
    // Launcher match options. Environment variables win when they are set.
    void set_launch_options(int players, int delay, bool unlimited_lives);
    int launch_players();
    int launch_delay();
    bool launch_unlimited_lives();
    // Blocks until the host's seed and save image are installed. Called from boot, before EEPROM is read.
    void handshake();
    // Returns 1 when this step should submit a frame. May restore a snapshot first.
    int begin_frame(uint8_t* rdram);
    void end_frame(uint8_t* rdram, uint64_t hash);
    void note_barrier(int32_t overlay);
    bool speculative();
    bool accepting_sample();
    // Writes the 4 pads for the tick about to be simulated. Returns false if online is off.
    bool pads_for_step(uint8_t out[24]);
    void push_local_pad(const uint8_t pad[6]);
    // Empty when online is off. Delay, rollback count, and ticks since the quietest remote peer.
    void status_line(char* buf, size_t cap);
}

#endif
