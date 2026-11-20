#include "bm64_rollback.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace bm64::rollback {
namespace {

constexpr uint8_t kInput = 1;
constexpr uint8_t kHash = 2;
constexpr uint8_t kBarrier = 3;
constexpr uint8_t kPing = 4;
// u8 player, u64 received_count: this peer has heard nothing from that player for silence_ms.
constexpr uint8_t kSilent = 5;
// u8 player, u64 leave tick. Host only.
constexpr uint8_t kLeave = 6;
// u8 player, then that player's pad for the header tick, forwarded by another peer.
constexpr uint8_t kRelay = 7;
constexpr size_t kPadBytes = 6;
// type, player, load sequence, tick, simulated tick, confirmed tick.
constexpr size_t kHeaderBytes = 20;
// The gap between a peer's confirmed tick and our newest input is bounded by twice
// (max_rollback + input_delay). This only guards against a peer that never reports.
constexpr uint64_t kMaxResend = 64;

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
    // No machine samples a pad for the first input_delay ticks. Every peer plays them with the
    // empty pad, so they count as received and the confirmed prefix can start at tick 0.
    for (int player = 0; player < config_.player_count; player++) {
        inputs_[player].resize(static_cast<size_t>(config_.input_delay));
        for (Stored& stored : inputs_[player]) {
            stored.known = true;
        }
    }
    uint64_t start = now_ms();
    for (int player = 0; player < kMaxPlayers; player++) {
        heard_ms_[player] = start;
    }
}

uint64_t Session::now_ms() const {
    if (config_.clock_ms) {
        return config_.clock_ms();
    }
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Session::remaining(int player) const {
    return player >= 0 && player < config_.player_count && player != config_.local_player && !left_[player];
}

bool Session::input_at(int player, uint64_t tick, Pad& pad) const {
    if (left_[player] && tick >= leave_tick_[player]) {
        pad = Pad{};
        pad.err = kLeftErr;
        return true;
    }
    const auto& row = inputs_[player];
    if (tick >= row.size() || !row[static_cast<size_t>(tick)].known) {
        return false;
    }
    pad = row[static_cast<size_t>(tick)].pad;
    return true;
}

uint64_t Session::received_count(int player) const {
    if (player < 0 || player >= config_.player_count) {
        return 0;
    }
    // Every tick before confirmed_ is known for every player, unless it was filled by a leave.
    const auto& row = inputs_[player];
    uint64_t n = std::min<uint64_t>(confirmed_, left_[player] ? leave_tick_[player] : confirmed_);
    while (n < row.size() && row[static_cast<size_t>(n)].known) {
        n++;
    }
    return n;
}

bool Session::tick_final(uint64_t tick) const {
    for (int player = 0; player < config_.player_count; player++) {
        const auto& used = used_[player];
        Pad pad;
        if (!input_at(player, tick, pad)) {
            return false;
        }
        if (tick >= used.size() || !used[static_cast<size_t>(tick)].simulated) {
            return false;
        }
        if (used[static_cast<size_t>(tick)].pad != pad) {
            return false;
        }
    }
    return true;
}

void Session::advance_confirmed() {
    while (confirmed_ < simulated_ && tick_final(confirmed_)) {
        confirmed_++;
    }
}

void Session::send_raw(uint8_t type, uint64_t tick, const uint8_t* payload, size_t size) {
    uint8_t header[kHeaderBytes];
    header[0] = type;
    header[1] = static_cast<uint8_t>(config_.local_player);
    write_u16(header + 2, static_cast<uint16_t>(load_seq_));
    write_u64(header + 4, tick);
    write_u32(header + 12, static_cast<uint32_t>(simulated_));
    write_u32(header + 16, static_cast<uint32_t>(confirmed_));
    std::vector<uint8_t> packet(kHeaderBytes + size);
    std::memcpy(packet.data(), header, kHeaderBytes);
    if (size > 0) {
        std::memcpy(packet.data() + kHeaderBytes, payload, size);
    }
    transport_.send(packet.data(), packet.size());
}

void Session::send_inputs() {
    // Resend everything since the slowest peer's confirmed tick. That peer is still missing at least
    // one of those inputs, and a window that followed only our own clock could pass it by for good.
    auto& mine = inputs_[config_.local_player];
    uint64_t begin = mine.size();
    for (int player = 0; player < config_.player_count; player++) {
        if (remaining(player)) {
            begin = std::min(begin, peer_confirmed_[player]);
        }
    }
    if (mine.size() > kMaxResend) {
        begin = std::max(begin, static_cast<uint64_t>(mine.size() - kMaxResend));
    }
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
    if (left_[player] && tick >= leave_tick_[player]) {
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
        if (n < static_cast<int>(kHeaderBytes)) {
            break;
        }
        uint8_t type = buf[0];
        int player = buf[1];
        uint64_t tick = read_u64(buf + 4);
        uint64_t remote_sim = read_u32(buf + 12);
        uint64_t remote_confirmed = read_u32(buf + 16);
        note_peer(player, remote_sim, remote_confirmed);
        const uint8_t* body = buf + kHeaderBytes;
        int body_size = n - static_cast<int>(kHeaderBytes);
        if (type == kInput && body_size >= static_cast<int>(kPadBytes)) {
            note_remote(player, tick, decode_pad(body));
        }
        else if (type == kHash && body_size >= 8) {
            uint64_t remote_hash = read_u64(body);
            remember_remote_hash(tick, remote_hash);
        }
        else if (type == kRelay && body_size >= static_cast<int>(1 + kPadBytes)) {
            note_remote(body[0], tick, decode_pad(body + 1));
        }
        else if (type == kSilent && body_size >= 9) {
            int about = body[0];
            if (player >= 0 && player < config_.player_count && about >= 0 && about < config_.player_count) {
                reported_[player][about] = true;
                reported_count_[player][about] = std::max(reported_count_[player][about], read_u64(body + 1));
            }
        }
        else if (type == kLeave && body_size >= 9 && player == 0) {
            int about = body[0];
            if (about == config_.local_player) {
                ended_ = "The host removed this player from the session";
            }
            else if (about > 0 && about < config_.player_count && !left_[about]) {
                apply_leave(about, read_u64(body + 1));
            }
        }
    }
}

void Session::apply_leave(int player, uint64_t tick) {
    left_[player] = true;
    leave_tick_[player] = tick;
    silent_[player] = false;
}

void Session::relay(int player, uint64_t end) {
    uint64_t begin = end;
    for (int other = 0; other < config_.player_count; other++) {
        if (remaining(other) && other != player) {
            begin = std::min(begin, peer_confirmed_[other]);
        }
    }
    if (end > kMaxResend) {
        begin = std::max(begin, end - kMaxResend);
    }
    const auto& row = inputs_[player];
    for (uint64_t tick = begin; tick < end && tick < row.size(); tick++) {
        if (!row[static_cast<size_t>(tick)].known) {
            continue;
        }
        uint8_t body[1 + kPadBytes];
        body[0] = static_cast<uint8_t>(player);
        encode_pad(body + 1, row[static_cast<size_t>(tick)].pad);
        send_raw(kRelay, tick, body, sizeof(body));
    }
}

void Session::update_leaves() {
    if (ended_ != nullptr) {
        return;
    }
    uint64_t now = now_ms();
    for (int player = 0; player < config_.player_count; player++) {
        if (player == config_.local_player) {
            continue;
        }
        if (left_[player]) {
            // Keep forwarding what this peer holds of the leaver's last inputs until every remaining
            // peer has finalized the leave tick itself. Finalizing every tick before it does not
            // need the leave, so it doesn't show that the peer applied it.
            bool behind = false;
            for (int other = 0; other < config_.player_count; other++) {
                if (remaining(other) && peer_confirmed_[other] <= leave_tick_[player]) {
                    behind = true;
                }
            }
            if (behind) {
                relay(player, leave_tick_[player]);
                if (config_.local_player == 0) {
                    uint8_t body[9];
                    body[0] = static_cast<uint8_t>(player);
                    write_u64(body + 1, leave_tick_[player]);
                    send_raw(kLeave, simulated_, body, sizeof(body));
                }
            }
            continue;
        }
        bool quiet = now >= heard_ms_[player] + static_cast<uint64_t>(config_.silence_ms);
        if (quiet && !silent_[player]) {
            silent_[player] = true;
            silent_since_ms_[player] = now;
        }
        else if (!quiet) {
            silent_[player] = false;
        }
        if (!silent_[player]) {
            // Someone else lost this player. Share what arrived here, so the leave tick the host
            // picks can't strand anyone without an input before it.
            bool reported = false;
            for (int other = 0; other < config_.player_count; other++) {
                reported = reported || reported_[other][player];
            }
            if (reported && player != 0) {
                relay(player, received_count(player));
            }
            continue;
        }
        if (player == 0) {
            if (now >= silent_since_ms_[0] + static_cast<uint64_t>(config_.leave_decide_ms)) {
                ended_ = "The host stopped responding";
            }
            continue;
        }
        uint64_t count = received_count(player);
        uint8_t body[9];
        body[0] = static_cast<uint8_t>(player);
        write_u64(body + 1, count);
        send_raw(kSilent, simulated_, body, sizeof(body));
        relay(player, count);
        if (config_.local_player != 0) {
            continue;
        }
        bool everyone = true;
        uint64_t leave = count;
        for (int other = 1; other < config_.player_count; other++) {
            if (other == player || left_[other]) {
                continue;
            }
            if (!reported_[other][player]) {
                everyone = false;
            }
            // No peer can have finalized a tick it lacks the player's input for, so a leave
            // after every count and every confirmed tick changes nothing already final.
            leave = std::max(leave, reported_count_[other][player]);
            leave = std::max(leave, peer_confirmed_[other]);
        }
        if (everyone || now >= silent_since_ms_[player] + static_cast<uint64_t>(config_.leave_decide_ms)) {
            apply_leave(player, leave);
        }
    }
}

void Session::note_peer(int player, uint64_t remote_sim, uint64_t remote_confirmed) {
    if (player < 0 || player >= config_.player_count || player == config_.local_player) {
        return;
    }
    peer_seen_[player] = true;
    peer_heard_[player] = simulated_;
    heard_ms_[player] = now_ms();
    if (remote_sim > peer_sim_[player]) {
        peer_sim_[player] = remote_sim;
    }
    if (remote_confirmed > peer_confirmed_[player]) {
        peer_confirmed_[player] = remote_confirmed;
    }
}

uint64_t Session::ticks_since_peer(int player) const {
    if (!remaining(player) || !peer_seen_[player]) {
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
        if (!remaining(player)) {
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
    Pad known;
    if (input_at(player, tick, known)) {
        predicted = false;
        return known;
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
        const auto& used = used_[player];
        for (uint64_t tick = begin; tick < simulated_; tick++) {
            if (tick >= used.size() || !used[static_cast<size_t>(tick)].simulated) {
                continue;
            }
            Pad pad;
            if (!input_at(player, tick, pad)) {
                continue;
            }
            if (pad != used[static_cast<size_t>(tick)].pad) {
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
    update_leaves();
    send_inputs();
    advance_confirmed();
    if (ended_ != nullptr) {
        stall = true;
        return false;
    }

    if (!resim_ && slowest_peer() + static_cast<uint64_t>(config_.max_frame_advantage) < simulated_ && stalls_ < 8 && simulated_ > 0) {
        stalls_++;
        stall = true;
        return false;
    }
    stalls_ = 0;
    step.loaded = false;

    // A guess older than max_rollback can no longer be corrected, so stop predicting until
    // the oldest unconfirmed tick is inside the window again.
    if (!resim_ && rollback_target() < 0 && simulated_ >= confirmed_ + static_cast<uint64_t>(config_.max_rollback)) {
        window_stalls_++;
        stall = true;
        return false;
    }

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
    FrameInput input;
    for (int player = 0; player < config_.player_count; player++) {
        bool predicted = false;
        input.player[player] = pad_for(player, tick, predicted);
    }

    bool last_resim = resim_ && simulated_ + 1 >= resim_until_;
    step.tick = tick;
    step.input = input;
    step.render = !resim_ || last_resim;
    // Intermediate corrections already had their sounds stopped on load. Starting them again
    // for every tick in the window stacks the whole window into one displayed frame.
    step.speculative = resim_ && !last_resim;
    step.accept_sample = tick >= furthest_;
    for (int player = 0; player < config_.player_count; player++) {
        auto& used = used_[player];
        if (used.size() <= tick) {
            used.resize(static_cast<size_t>(tick) + 1);
        }
        used[static_cast<size_t>(tick)].simulated = true;
        used[static_cast<size_t>(tick)].pad = input.player[player];
    }
    if (tick + 1 > furthest_) {
        furthest_ = tick + 1;
    }
    simulated_ = tick + 1;
    advance_confirmed();
    // Final only when every earlier tick also ran on received input. A tick whose own inputs
    // arrived while an earlier one was still a guess will be resimulated and must not be hashed.
    step.confirmed = tick < confirmed_;
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

bool Session::inputs_known_through(uint64_t tick) const {
    for (int player = 0; player < config_.player_count; player++) {
        for (uint64_t t = confirmed_; t <= tick; t++) {
            Pad pad;
            if (!input_at(player, t, pad)) {
                return false;
            }
        }
    }
    return true;
}

bool Session::wait_final(uint64_t tick, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool waited = false;
    for (;;) {
        recv_all();
        update_leaves();
        send_inputs();
        advance_confirmed();
        if (ended_ != nullptr) {
            return false;
        }
        if (inputs_known_through(tick)) {
            if (waited) {
                barrier_waits_++;
            }
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        waited = true;
        if (config_.wait_idle) {
            config_.wait_idle();
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

bool Session::notify_barrier(uint64_t tick, int32_t overlay) {
    // Resimulating the load tick on final input notifies again; the barrier already stands.
    if (barrier_notified_ == tick) {
        return true;
    }
    if (config_.player_count > 1 && !wait_final(tick, config_.wait_timeout_ms)) {
        if (ended_ != nullptr) {
            return false;
        }
        std::fprintf(stderr, "[bm64 net] Timed out waiting for the inputs of load tick %llu\n",
            static_cast<unsigned long long>(tick));
        std::exit(1);
    }
    // The step for `tick` already marked its pads as used, so this tick is final exactly when every
    // pad up to it was the real one. Otherwise the load came from a guess, and the next poll rolls
    // back before it.
    advance_confirmed();
    if (confirmed_ <= tick) {
        return false;
    }
    barrier_notified_ = tick;
    barrier_tick_ = tick;
    load_seq_++;
    uint8_t body[4];
    write_u32(body, static_cast<uint32_t>(overlay));
    send_raw(kBarrier, tick, body, 4);
    return true;
}

} // namespace bm64::rollback
