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
    // Called on each pass while waiting for a tick to become final. Empty means sleep 1 ms.
    // A unit test runs the other peers here, because they share its thread.
    std::function<void()> wait_idle;
    // Longer than silence_ms + leave_decide_ms, so a player leaving during a load wait resolves.
    int wait_timeout_ms = 10000;
    // Milliseconds on a host clock. Empty means steady_clock. Used only to notice silence; what
    // happens to a silent player reaches the game through the input stream.
    std::function<uint64_t()> clock_ms;
    // A peer that hears nothing from a player for this long reports them silent.
    int silence_ms = 3000;
    // The host decides a leave once every remaining peer reported it, or after this long.
    int leave_decide_ms = 2000;
};

// The pad of a player who left, from their leave tick on.
constexpr uint8_t kLeftErr = 0xFE;

struct Step {
    uint64_t tick = 0;
    FrameInput input{};
    bool render = true;
    // Throwaway step: the caller should not start real sounds.
    bool speculative = false;
    bool loaded = false;
    // This tick and every tick before it ran on received input. Its state is final and will
    // not be resimulated, so it is the only kind of tick whose hash peers can compare.
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
    // Overlay or file load during the step for `tick`. Blocks until every input up to `tick` is
    // known. Returns true when they all match what was simulated: the load is real, and nothing
    // rolls back past it, so the caller may drop older snapshots. Returns false when the load came
    // from a guess; the next poll rolls back before it.
    bool notify_barrier(uint64_t tick, int32_t overlay);
    // Blocks until every player's input is known for every tick up to `tick`. False on timeout.
    bool wait_final(uint64_t tick, int timeout_ms);

    int rollbacks() const { return rollbacks_; }
    int max_depth() const { return max_depth_; }
    int input_delay() const { return config_.input_delay; }
    // Ticks before this one are final.
    uint64_t confirmed_tick() const { return confirmed_; }
    // Polls that waited because the prediction window was full.
    int window_stalls() const { return window_stalls_; }
    uint64_t predicted_tick() const { return simulated_; }
    bool desynced() const { return desynced_; }
    uint64_t desync_tick() const { return desync_tick_; }
    uint64_t local_hash_at(uint64_t tick) const;
    int barrier_waits() const { return barrier_waits_; }
    // Ticks since a packet from this remote player. 0 if they have not been heard yet.
    uint64_t ticks_since_peer(int player) const;
    // From leave_tick(player) on, that player's input is known and has err kLeftErr.
    bool left(int player) const { return player >= 0 && player < kMaxPlayers && left_[player]; }
    uint64_t leave_tick(int player) const { return left(player) ? leave_tick_[player] : 0; }
    // Ticks 0 .. n-1 of this player's input have arrived here, ignoring any leave.
    uint64_t received_count(int player) const;
    // The host went silent, or the host removed this player. The session cannot go on.
    bool ended() const { return ended_ != nullptr; }
    const char* end_reason() const { return ended_ != nullptr ? ended_ : ""; }

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
    // Real loads so far. Sent in every header for logs.
    uint32_t load_seq_ = 0;
    uint64_t peer_sim_[kMaxPlayers]{};
    uint64_t peer_confirmed_[kMaxPlayers]{};
    bool peer_seen_[kMaxPlayers]{};
    uint64_t peer_heard_[kMaxPlayers]{};
    uint64_t heard_ms_[kMaxPlayers]{};
    bool silent_[kMaxPlayers]{};
    uint64_t silent_since_ms_[kMaxPlayers]{};
    // Host only: reported_[reporter][player] and the received_count that reporter sent.
    bool reported_[kMaxPlayers][kMaxPlayers]{};
    uint64_t reported_count_[kMaxPlayers][kMaxPlayers]{};
    bool left_[kMaxPlayers]{};
    uint64_t leave_tick_[kMaxPlayers]{};
    const char* ended_ = nullptr;
    int stalls_ = 0;
    int window_stalls_ = 0;

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
    void note_peer(int player, uint64_t remote_sim, uint64_t remote_confirmed);
    bool inputs_known_through(uint64_t tick) const;
    bool input_at(int player, uint64_t tick, Pad& pad) const;
    bool remaining(int player) const;
    uint64_t now_ms() const;
    void update_leaves();
    void apply_leave(int player, uint64_t tick);
    void relay(int player, uint64_t end);
    uint64_t slowest_peer() const;
    void remember_remote_hash(uint64_t tick, uint64_t remote_hash);
    void send_inputs();
    void send_raw(uint8_t type, uint64_t tick, const uint8_t* payload, size_t size);
    Pad pad_for(int player, uint64_t tick, bool& predicted) const;
    int rollback_target() const;
    bool tick_final(uint64_t tick) const;
    void advance_confirmed();
    void note_remote(int player, uint64_t tick, const Pad& pad);
};

} // namespace bm64::rollback

#endif
