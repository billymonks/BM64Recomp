#include "bm64_rollback.h"

#include <cstring>
#include <algorithm>
#include <limits>

namespace bm64::rollback {
namespace {

constexpr uint8_t kInput = 1;
constexpr uint8_t kHash = 2;
constexpr uint8_t kBarrier = 3;
constexpr uint8_t kPing = 4;
constexpr size_t kPadBytes = 6;

void write_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void write_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void write_u64(uint8_t* p, uint64_t v) {
    write_u32(p, static_cast<uint32_t>(v));
    write_u32(p + 4, static_cast<uint32_t>(v >> 32));
}

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

uint64_t read_u64(const uint8_t* p) {
    return read_u32(p) | (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

void encode_pad(uint8_t* p, const Pad& pad) {
    write_u16(p, pad.button);
    p[2] = static_cast<uint8_t>(pad.stick_x);
    p[3] = static_cast<uint8_t>(pad.stick_y);
    p[4] = pad.err;
    p[5] = 0;
}

Pad decode_pad(const uint8_t* p) {
    Pad pad;
    pad.button = read_u16(p);
    pad.stick_x = static_cast<int8_t>(p[2]);
    pad.stick_y = static_cast<int8_t>(p[3]);
    pad.err = p[4];
    return pad;
}

} // namespace

Session::Session(Config config, Transport& transport)
    : config_(config), transport_(transport) {
    if (config_.player_count < 1) {
        config_.player_count = 1;
    }
    if (config_.player_count > kMaxPlayers) {
        config_.player_count = kMaxPlayers;
    }
    if (config_.local_player < 0 || config_.local_player >= config_.player_count) {
        config_.local_player = 0;
    }
    // The sample is taken after poll, so it can only affect a later tick.
    if (config_.input_delay < 1) {
        config_.input_delay = 1;
    }
    if (config_.max_rollback < 1) {
        config_.max_rollback = 1;
    }
}

void Session::send_raw(uint8_t type, uint64_t tick, const uint8_t* payload, size_t size) {
    uint8_t header[16];
    header[0] = type;
    header[1] = static_cast<uint8_t>(config_.local_player);
    write_u16(header + 2, static_cast<uint16_t>(load_seq_));
    write_u64(header + 4, tick);
    write_u32(header + 12, static_cast<uint32_t>(simulated_));
    std::vector<uint8_t> packet(16 + size);
    std::memcpy(packet.data(), header, 16);
    if (size > 0) {
        std::memcpy(packet.data() + 16, payload, size);
    }
    transport_.send(packet.data(), packet.size());
}

void Session::send_inputs() {
    // Retransmit the recent window so a dropped packet still arrives before the rollback limit.
    uint64_t begin = simulated_ > static_cast<uint64_t>(config_.max_rollback + config_.input_delay)
        ? simulated_ - static_cast<uint64_t>(config_.max_rollback + config_.input_delay)
        : 0;
    auto& mine = inputs_[config_.local_player];
    for (uint64_t tick = begin; tick < mine.size(); tick++) {
        if (!mine[static_cast<size_t>(tick)].known) {
            continue;
        }
        uint8_t body[kPadBytes];
        encode_pad(body, mine[static_cast<size_t>(tick)].pad);
        send_raw(kInput, tick, body, kPadBytes);
    }
    send_raw(kPing, simulated_, nullptr, 0);
}

void Session::note_remote(int player, uint64_t tick, const Pad& pad) {
    if (player < 0 || player >= config_.player_count || player == config_.local_player) {
        return;
    }
    auto& row = inputs_[player];
    if (row.size() <= tick) {
        row.resize(static_cast<size_t>(tick) + 1);
    }
    row[static_cast<size_t>(tick)].known = true;
    row[static_cast<size_t>(tick)].pad = pad;
}

void Session::recv_all() {
    uint8_t buf[64];
    for (;;) {
        int n = transport_.recv(buf, sizeof(buf));
        if (n < 16) {
            break;
        }
        uint8_t type = buf[0];
        int player = buf[1];
        uint32_t seq = read_u16(buf + 2);
        uint64_t tick = read_u64(buf + 4);
        uint64_t remote_sim = read_u32(buf + 12);
        note_peer(player, seq, remote_sim);
        if (type == kInput && n >= 16 + static_cast<int>(kPadBytes)) {
            note_remote(player, tick, decode_pad(buf + 16));
        }
        else if (type == kHash && n >= 24) {
            uint64_t remote_hash = read_u64(buf + 16);
            remember_remote_hash(tick, remote_hash);
        }
    }
}

void Session::note_peer(int player, uint32_t seq, uint64_t remote_sim) {
    if (player < 0 || player >= config_.player_count || player == config_.local_player) {
        return;
    }
    peer_seen_[player] = true;
    peer_heard_[player] = simulated_;
    if (remote_sim > peer_sim_[player]) {
        peer_sim_[player] = remote_sim;
    }
    if (seq > peer_load_seq_[player]) {
        peer_load_seq_[player] = seq;
    }
}

bool Session::peers_caught_up() const {
    for (int player = 0; player < config_.player_count; player++) {
        if (player == config_.local_player) {
            continue;
        }
        if (peer_load_seq_[player] < load_seq_) {
            return false;
        }
    }
    return true;
}

uint64_t Session::ticks_since_peer(int player) const {
    if (player < 0 || player >= config_.player_count || player == config_.local_player || !peer_seen_[player]) {
        return 0;
    }
    if (simulated_ < peer_heard_[player]) {
        return 0;
    }
    return simulated_ - peer_heard_[player];
}

uint64_t Session::slowest_peer() const {
    uint64_t slowest = std::numeric_limits<uint64_t>::max();
    for (int player = 0; player < config_.player_count; player++) {
        if (player == config_.local_player) {
            continue;
        }
        uint64_t sim = peer_seen_[player] ? peer_sim_[player] : 0;
        if (sim < slowest) {
            slowest = sim;
        }
    }
    if (slowest == std::numeric_limits<uint64_t>::max()) {
        slowest = 0;
    }
    return slowest;
}

void Session::push_local(const Pad& pad) {
    uint64_t target = now_ + static_cast<uint64_t>(config_.input_delay);
    auto& mine = inputs_[config_.local_player];
    if (mine.size() <= target) {
        mine.resize(static_cast<size_t>(target) + 1);
    }
    mine[static_cast<size_t>(target)].known = true;
    mine[static_cast<size_t>(target)].pad = pad;
    now_++;
}

Pad Session::pad_for(int player, uint64_t tick, bool& predicted) const {
    const auto& row = inputs_[player];
    if (tick < row.size() && row[static_cast<size_t>(tick)].known) {
        predicted = false;
        return row[static_cast<size_t>(tick)].pad;
    }
    predicted = true;
    if (!row.empty()) {
        for (size_t i = row.size(); i > 0; i--) {
            if (i - 1 <= tick && row[i - 1].known) {
                return row[i - 1].pad;
            }
        }
    }
    Pad empty;
    empty.err = 0xFF;
    return empty;
}

int Session::rollback_target() const {
    if (simulated_ == 0) {
        return -1;
    }
    uint64_t earliest = simulated_;
    bool found = false;
    uint64_t begin = simulated_ > static_cast<uint64_t>(config_.max_rollback)
        ? simulated_ - static_cast<uint64_t>(config_.max_rollback)
        : 0;
    if (begin < barrier_tick_) {
        begin = barrier_tick_;
    }
    for (int player = 0; player < config_.player_count; player++) {
        const auto& row = inputs_[player];
        const auto& used = used_[player];
        for (uint64_t tick = begin; tick < simulated_; tick++) {
            if (tick >= used.size() || !used[static_cast<size_t>(tick)].simulated) {
                continue;
            }
            if (tick >= row.size() || !row[static_cast<size_t>(tick)].known) {
                continue;
            }
            if (row[static_cast<size_t>(tick)].pad != used[static_cast<size_t>(tick)].pad) {
                if (tick < earliest) {
                    earliest = tick;
                    found = true;
                }
                break;
            }
        }
    }
    if (!found || earliest < barrier_tick_) {
        return -1;
    }
    return static_cast<int>(earliest);
}

bool Session::poll(Step& step, bool& stall, const std::function<void(uint64_t tick)>& load) {
    stall = false;
    recv_all();
    send_inputs();

    if (barrier_open_ && !peers_caught_up()) {
        barrier_waits_++;
        stall = true;
        return false;
    }
    barrier_open_ = false;

    if (!resim_ && slowest_peer() + static_cast<uint64_t>(config_.max_frame_advantage) < simulated_ && stalls_ < 8 && simulated_ > 0) {
        stalls_++;
        stall = true;
        return false;
    }
    stalls_ = 0;
    step.loaded = false;

    if (!resim_) {
        int target = rollback_target();
        if (target >= 0 && load) {
            uint64_t tick = static_cast<uint64_t>(target);
            int depth = static_cast<int>(simulated_ - tick);
            if (depth > max_depth_) {
                max_depth_ = depth;
            }
            rollbacks_++;
            resim_ = true;
            resim_until_ = simulated_;
            simulated_ = tick;
            load(tick);
            step.loaded = true;
        }
    }

    uint64_t tick = simulated_;
    bool any_predicted = false;
    FrameInput input;
    for (int player = 0; player < kMaxPlayers; player++) {
        bool predicted = false;
        if (player < config_.player_count) {
            input.player[player] = pad_for(player, tick, predicted);
            any_predicted = any_predicted || predicted;
        }
    }

    bool last_resim = resim_ && simulated_ + 1 >= resim_until_;
    step.tick = tick;
    step.input = input;
    step.render = !resim_ || last_resim;
    // Intermediate corrections already had their sounds stopped on load. Starting them again
    // for every tick in the window stacks the whole window into one displayed frame.
    step.speculative = resim_ && !last_resim;
    step.confirmed = !any_predicted;
    step.accept_sample = tick >= furthest_;
    for (int player = 0; player < config_.player_count; player++) {
        auto& used = used_[player];
        if (used.size() <= tick) {
            used.resize(static_cast<size_t>(tick) + 1);
        }
        used[static_cast<size_t>(tick)].simulated = true;
        used[static_cast<size_t>(tick)].pad = input.player[player];
    }
    if (!any_predicted && tick + 1 > confirmed_) {
        confirmed_ = tick + 1;
    }
    if (tick + 1 > furthest_) {
        furthest_ = tick + 1;
    }
    simulated_ = tick + 1;
    if (last_resim) {
        resim_ = false;
    }
    return true;
}

void Session::remember_remote_hash(uint64_t tick, uint64_t remote_hash) {
    for (const auto& entry : hashes_) {
        if (entry.first == tick && entry.second != remote_hash && !desynced_) {
            desynced_ = true;
            desync_tick_ = tick;
        }
    }
    remote_hashes_.push_back({ tick, remote_hash });
    if (remote_hashes_.size() > 128) {
        remote_hashes_.erase(remote_hashes_.begin(), remote_hashes_.begin() + static_cast<std::ptrdiff_t>(remote_hashes_.size() - 128));
    }
}

void Session::notify_hash(uint64_t tick, uint64_t hash) {
    hashes_.push_back({ tick, hash });
    if (hashes_.size() > 128) {
        hashes_.erase(hashes_.begin(), hashes_.begin() + static_cast<std::ptrdiff_t>(hashes_.size() - 128));
    }
    for (const auto& entry : remote_hashes_) {
        if (entry.first == tick && entry.second != hash && !desynced_) {
            desynced_ = true;
            desync_tick_ = tick;
        }
    }
    uint8_t body[8];
    write_u64(body, hash);
    send_raw(kHash, tick, body, 8);
    recv_all();
}

uint64_t Session::local_hash_at(uint64_t tick) const {
    for (auto it = hashes_.rbegin(); it != hashes_.rend(); ++it) {
        if (it->first == tick) {
            return it->second;
        }
    }
    return 0;
}

void Session::notify_barrier(uint64_t tick, int32_t overlay) {
    (void)overlay;
    // Resimulating the load tick notifies again. Only the first one waits for peers.
    if (barrier_notified_ == tick) {
        return;
    }
    barrier_notified_ = tick;
    barrier_tick_ = tick;
    load_seq_++;
    barrier_open_ = config_.player_count > 1;
    uint8_t body[4];
    write_u32(body, static_cast<uint32_t>(overlay));
    send_raw(kBarrier, tick, body, 4);
}

} // namespace bm64::rollback
