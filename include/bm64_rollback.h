#ifndef __BM64_ROLLBACK_H__
#define __BM64_ROLLBACK_H__

#include <cstddef>
#include <cstdint>
#include <limits>
#include <functional>
#include <vector>

// Game-independent rollback session. The game (or a unit test) saves and loads by tick and
// steps one tick at a time. Prediction repeats the last input. Late inputs that disagree with
// the prediction roll the session back, up to max_rollback ticks, and never across a barrier.
namespace bm64::rollback {

static constexpr int kMaxPlayers = 4;

struct Pad {
    uint16_t button = 0;
    int8_t stick_x = 0;
    int8_t stick_y = 0;
    uint8_t err = 0xFF;
};

struct FrameInput {
    Pad player[kMaxPlayers]{};
};

inline bool operator==(const Pad& a, const Pad& b) {
    return a.button == b.button && a.stick_x == b.stick_x && a.stick_y == b.stick_y && a.err == b.err;
}

inline bool operator!=(const Pad& a, const Pad& b) {
    return !(a == b);
}

class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const uint8_t* data, size_t size) = 0;
    // Non-blocking. Returns the number of bytes written to `data`, or 0.
    virtual int recv(uint8_t* data, size_t cap) = 0;
};

struct Config {
    int local_player = 0;
    int player_count = 2;
    int input_delay = 2;
    int max_rollback = 8;
    int hash_interval = 15;
    // Stall when this many ticks ahead of the furthest remote report.
    int max_frame_advantage = 3;
};

struct Step {
    uint64_t tick = 0;
    FrameInput input{};
    bool render = true;
    // Throwaway step: the caller should not start real sounds.
    bool speculative = false;
    bool loaded = false;
    // Every player's input for this tick was received, not predicted.
    bool confirmed = false;
    // True the first time this tick is reached. Resims must not sample hardware again.
    bool accept_sample = false;
};

class Session {
public:
    Session(Config config, Transport& transport);

    // The pad sampled on this machine for its local player. It is stored input_delay ticks ahead.
    void push_local(const Pad& pad);

    // One call produces one tick, or a stall (time sync or a load barrier).
    // `load` is invoked before the returned step when a rollback is required.
    bool poll(Step& step, bool& stall, const std::function<void(uint64_t tick)>& load);

    void notify_hash(uint64_t tick, uint64_t hash);
    // Overlay or file load. Snapshots before the next tick are dropped, and poll stalls until
    // every peer has reached the same load sequence.
    void notify_barrier(uint64_t tick, int32_t overlay);

    int rollbacks() const { return rollbacks_; }
    int max_depth() const { return max_depth_; }
    int input_delay() const { return config_.input_delay; }
    uint64_t confirmed_tick() const { return confirmed_; }
    uint64_t predicted_tick() const { return simulated_; }
    bool desynced() const { return desynced_; }
    uint64_t desync_tick() const { return desync_tick_; }
    uint64_t local_hash_at(uint64_t tick) const;
    int barrier_waits() const { return barrier_waits_; }
    // Ticks since a packet from this remote player. 0 if they have not been heard yet.
    uint64_t ticks_since_peer(int player) const;

private:
    struct Stored {
        bool known = false;
        Pad pad{};
    };

    Config config_;
    Transport& transport_;
    uint64_t simulated_ = 0;
    uint64_t confirmed_ = 0;
    uint64_t now_ = 0;
    int rollbacks_ = 0;
    int max_depth_ = 0;
    int barrier_waits_ = 0;
    bool desynced_ = false;
    uint64_t desync_tick_ = 0;
    uint64_t barrier_tick_ = 0;
    uint64_t barrier_notified_ = UINT64_MAX;
    uint32_t load_seq_ = 0;
    uint32_t peer_load_seq_[kMaxPlayers]{};
    uint64_t peer_sim_[kMaxPlayers]{};
    bool peer_seen_[kMaxPlayers]{};
    uint64_t peer_heard_[kMaxPlayers]{};
    bool barrier_open_ = false;
    int stalls_ = 0;

    bool resim_ = false;
    uint64_t resim_until_ = 0;

    struct Used {
        bool simulated = false;
        Pad pad{};
    };

    std::vector<Stored> inputs_[kMaxPlayers];
    std::vector<Used> used_[kMaxPlayers];
    std::vector<std::pair<uint64_t, uint64_t>> hashes_;
    std::vector<std::pair<uint64_t, uint64_t>> remote_hashes_;
    uint64_t furthest_ = 0;

    void recv_all();
    void note_peer(int player, uint32_t seq, uint64_t remote_sim);
    bool peers_caught_up() const;
    uint64_t slowest_peer() const;
    void remember_remote_hash(uint64_t tick, uint64_t remote_hash);
    void send_inputs();
    void send_raw(uint8_t type, uint64_t tick, const uint8_t* payload, size_t size);
    Pad pad_for(int player, uint64_t tick, bool& predicted) const;
    int rollback_target() const;
    void note_remote(int player, uint64_t tick, const Pad& pad);
};

} // namespace bm64::rollback

#endif
