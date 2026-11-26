#include "bm64_rollback.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace {

struct World {
    int time = 0;
    int loss_percent = 0;
    uint32_t rng = 1;
    // Packets from hold_sender are dropped while time is in [hold_from, hold_until).
    // hold_tick >= 0 limits that to the input packet for one tick.
    int hold_sender = -1;
    int hold_tick = -1;
    int hold_from = 0;
    int hold_until = 0;
    struct Msg {
        int due;
        int dest;
        std::vector<uint8_t> bytes;
    };
    std::vector<Msg> msgs;

    bool drop() {
        rng = rng * 1664525u + 1013904223u;
        return loss_percent > 0 && static_cast<int>(rng % 100) < loss_percent;
    }

    bool held(int sender, const uint8_t* data) const {
        if (sender != hold_sender || time < hold_from || time >= hold_until) {
            return false;
        }
        if (hold_tick < 0) {
            return true;
        }
        uint64_t tick = 0;
        std::memcpy(&tick, data + 4, sizeof(tick));
        return data[0] == 1 && tick == static_cast<uint64_t>(hold_tick);
    }
};

struct Pipe : bm64::rollback::Transport {
    World* world = nullptr;
    int me = 0;
    int delay = 1;

    void send(const uint8_t* data, size_t size) override {
        if (world->drop() || world->held(me, data)) {
            return;
        }
        World::Msg msg;
        msg.due = world->time + delay;
        msg.dest = 1 - me;
        msg.bytes.assign(data, data + size);
        world->msgs.push_back(std::move(msg));
    }

    int recv(uint8_t* data, size_t cap) override {
        for (auto it = world->msgs.begin(); it != world->msgs.end(); ++it) {
            if (it->dest == me && it->due <= world->time) {
                size_t n = it->bytes.size();
                if (n > cap) {
                    n = cap;
                }
                std::memcpy(data, it->bytes.data(), n);
                world->msgs.erase(it);
                return static_cast<int>(n);
            }
        }
        return 0;
    }
};

struct Fake {
    uint64_t state = 0x12345678abcdefull;
    std::vector<uint64_t> history;

    void save(uint64_t tick) {
        if (history.size() <= tick) {
            history.resize(static_cast<size_t>(tick) + 1);
        }
        history[static_cast<size_t>(tick)] = state;
    }

    void load(uint64_t tick) {
        if (tick >= history.size()) {
            std::fprintf(stderr, "fake load missing tick %llu\n", static_cast<unsigned long long>(tick));
            std::exit(1);
        }
        state = history[static_cast<size_t>(tick)];
    }

    uint64_t at(uint64_t tick) const {
        return tick < history.size() ? history[static_cast<size_t>(tick)] : 0;
    }

    void step(const bm64::rollback::FrameInput& input) {
        uint64_t h = state;
        for (int i = 0; i < bm64::rollback::kMaxPlayers; i++) {
            h ^= static_cast<uint64_t>(input.player[i].button) << ((i * 8) & 63);
            h = h * 0x9E3779B97F4A7C15ull + static_cast<uint8_t>(input.player[i].stick_x);
            h ^= static_cast<uint64_t>(static_cast<uint8_t>(input.player[i].stick_y)) << 17;
            h += input.player[i].err;
        }
        state = h;
    }
};

bm64::rollback::Pad sample(int player, uint64_t n) {
    bm64::rollback::Pad pad;
    pad.button = static_cast<uint16_t>((n * 17 + player * 3) & 0xFFFF);
    pad.stick_x = static_cast<int8_t>((n + player) & 0x7F);
    pad.stick_y = static_cast<int8_t>(-(static_cast<int>(n) & 0x3F));
    pad.err = 0;
    return pad;
}

struct Peer {
    Fake fake;
    bm64::rollback::Session* session = nullptr;
    uint64_t samples = 0;
    uint64_t steps = 0;
    int idle = 0;
};

struct PairOptions {
    int frames = 120;
    int delay_packets = 1;
    int loss = 0;
    int input_delay = 1;
    int max_frame_advantage = 40;
    bool expect_rollback = false;
    int hold_sender = -1;
    int hold_tick = -1;
    int hold_from = 0;
    int hold_until = 0;
    uint64_t min_confirmed = 8;
    // This peer doesn't poll before late_frame, as if its game were still booting after the
    // handshake. -1 starts both at once.
    int late_peer = -1;
    int late_frame = 0;
};

bool run_pair(const PairOptions& opt, const char* name) {
    World world;
    world.loss_percent = opt.loss;
    world.hold_sender = opt.hold_sender;
    world.hold_tick = opt.hold_tick;
    world.hold_from = opt.hold_from;
    world.hold_until = opt.hold_until;
    int delay_packets = opt.delay_packets;
    Pipe pipes[2];
    pipes[0].world = &world;
    pipes[1].world = &world;
    pipes[0].me = 0;
    pipes[1].me = 1;
    pipes[0].delay = delay_packets;
    pipes[1].delay = delay_packets;

    bm64::rollback::Config cfg;
    cfg.player_count = 2;
    cfg.input_delay = opt.input_delay;
    cfg.max_rollback = 8;
    cfg.max_frame_advantage = opt.max_frame_advantage;
    cfg.clock_ms = [&]() { return static_cast<uint64_t>(world.time) * 16; };
    cfg.local_player = 0;
    bm64::rollback::Session a(cfg, pipes[0]);
    cfg.local_player = 1;
    bm64::rollback::Session b(cfg, pipes[1]);
    Peer peers[2];
    peers[0].session = &a;
    peers[1].session = &b;
    peers[0].fake.save(0);
    peers[1].fake.save(0);

    for (int frame = 0; frame < opt.frames; frame++) {
        world.time++;
        bool booting = frame < opt.late_frame;
        for (int p = 0; p < 2; p++) {
            if (booting && p == opt.late_peer) {
                continue;
            }
            bool sampled = false;
            bool stepped = false;
            for (int spins = 0; spins < 64 && !sampled; spins++) {
                bm64::rollback::Step step;
                bool stall = false;
                bool ok = peers[p].session->poll(step, stall, [&](uint64_t tick) { peers[p].fake.load(tick); });
                if (!ok) {
                    break;
                }
                peers[p].fake.step(step.input);
                peers[p].fake.save(step.tick + 1);
                peers[p].steps++;
                stepped = true;
                // The same rule as bm64::online::end_frame.
                if (step.confirmed && !step.speculative) {
                    peers[p].session->notify_hash(step.tick, peers[p].fake.state);
                }
                uint64_t predicted = peers[p].session->predicted_tick();
                uint64_t confirmed = peers[p].session->confirmed_tick();
                uint64_t ahead = predicted > confirmed ? predicted - confirmed : 0;
                if (ahead > static_cast<uint64_t>(cfg.max_rollback)) {
                    std::fprintf(stderr, "%s: peer %d predicted %llu ticks past the last confirmed tick\n",
                        name, p, static_cast<unsigned long long>(ahead));
                    return false;
                }
                if (step.accept_sample) {
                    peers[p].session->push_local(sample(p, peers[p].samples));
                    peers[p].samples++;
                    sampled = true;
                }
            }
            // Time sync may pause a peer for a few frames. A held peer, and anyone waiting on it, may
            // pause for the whole hold.
            bool holding = (world.time >= world.hold_from && world.time < world.hold_until) || booting;
            peers[p].idle = (stepped || sampled || holding) ? 0 : peers[p].idle + 1;
            if (peers[p].idle > 16) {
                std::fprintf(stderr, "%s: peer %d stalled out at frame %d\n", name, p, frame);
                return false;
            }
        }
    }

    for (int p = 0; p < 2; p++) {
        if (peers[p].session->ended()) {
            std::fprintf(stderr, "%s: peer %d ended: %s\n", name, p, peers[p].session->end_reason());
            return false;
        }
        if (peers[p].session->left(1 - p)) {
            std::fprintf(stderr, "%s: peer %d handed player %d to the AI\n", name, p, 1 - p);
            return false;
        }
    }
    uint64_t confirmed = a.confirmed_tick() < b.confirmed_tick() ? a.confirmed_tick() : b.confirmed_tick();
    if (confirmed < opt.min_confirmed || peers[0].fake.at(confirmed) != peers[1].fake.at(confirmed)) {
        std::fprintf(stderr, "%s: confirmed state differs or stopped short at %llu (%016llx %016llx) steps %llu/%llu rollbacks %d/%d\n",
            name, static_cast<unsigned long long>(confirmed),
            static_cast<unsigned long long>(peers[0].fake.at(confirmed)),
            static_cast<unsigned long long>(peers[1].fake.at(confirmed)),
            static_cast<unsigned long long>(peers[0].steps),
            static_cast<unsigned long long>(peers[1].steps),
            a.rollbacks(), b.rollbacks());
        return false;
    }
    if (a.desynced() || b.desynced()) {
        std::fprintf(stderr, "%s: false desync at %llu\n", name, static_cast<unsigned long long>(a.desync_tick()));
        return false;
    }
    if (opt.expect_rollback && a.rollbacks() + b.rollbacks() == 0) {
        std::fprintf(stderr, "%s: expected a rollback and none happened\n", name);
        return false;
    }
    std::printf("%s: ok  confirmed %llu state %016llx  rollbacks %d/%d  depth %d\n",
        name, static_cast<unsigned long long>(confirmed),
        static_cast<unsigned long long>(peers[0].fake.at(confirmed)),
        a.rollbacks(), b.rollbacks(),
        a.max_depth() > b.max_depth() ? a.max_depth() : b.max_depth());
    return true;
}

bool run_barrier() {
    World world;
    Pipe pipes[2];
    pipes[0].world = &world;
    pipes[0].me = 0;
    pipes[0].delay = 1;
    pipes[1].world = &world;
    pipes[1].me = 1;
    pipes[1].delay = 1;
    Fake fakes[2];
    fakes[0].save(0);
    fakes[1].save(0);
    bool sent[2] = { false, false };
    bool busy[2] = { false, false };
    bool real[2] = { false, false };
    uint64_t samples[2] = { 0, 0 };
    std::unique_ptr<bm64::rollback::Session> sessions[2];
    std::function<bool(int)> run_frame;
    for (int p = 0; p < 2; p++) {
        bm64::rollback::Config cfg;
        cfg.player_count = 2;
        cfg.input_delay = 1;
        cfg.max_rollback = 8;
        cfg.max_frame_advantage = 40;
        cfg.local_player = p;
        cfg.wait_idle = [&, p]() {
            world.time++;
            if (!busy[1 - p]) {
                run_frame(1 - p);
            }
        };
        sessions[p] = std::make_unique<bm64::rollback::Session>(cfg, pipes[p]);
    }
    run_frame = [&](int p) {
        busy[p] = true;
        bool sampled = false;
        for (int spins = 0; spins < 64 && !sampled; spins++) {
            bm64::rollback::Step step;
            bool stall = false;
            if (!sessions[p]->poll(step, stall, [&](uint64_t tick) { fakes[p].load(tick); })) {
                break;
            }
            fakes[p].step(step.input);
            fakes[p].save(step.tick + 1);
            if (!sent[p] && samples[p] == 10 && step.accept_sample) {
                real[p] = sessions[p]->notify_barrier(step.tick, 42);
                sent[p] = true;
            }
            if (step.accept_sample) {
                sessions[p]->push_local(sample(p, samples[p]));
                samples[p]++;
                sampled = true;
            }
        }
        busy[p] = false;
        return sampled;
    };
    for (int frame = 0; frame < 40; frame++) {
        world.time++;
        for (int p = 0; p < 2; p++) {
            if (!run_frame(p) && frame > 5) {
                std::fprintf(stderr, "barrier: peer %d did not sample at frame %d\n", p, frame);
                return false;
            }
        }
    }
    uint64_t confirmed = std::min(sessions[0]->confirmed_tick(), sessions[1]->confirmed_tick());
    if (confirmed < 8 || fakes[0].at(confirmed) != fakes[1].at(confirmed)) {
        std::fprintf(stderr, "barrier: states differ at %llu\n", static_cast<unsigned long long>(confirmed));
        return false;
    }
    // The barrier here does not follow from the input, so a peer that guessed tick 10 refuses it.
    std::printf("barrier: ok  waits %d/%d  accepted %d/%d\n", sessions[0]->barrier_waits(), sessions[1]->barrier_waits(), real[0], real[1]);
    return true;
}

// A game with a load: the first tick of a two-tick hold of 0x8000 by player 1 maps new code.
// `mapped` stands in for librecomp's func_map: host memory that a snapshot does not restore,
// so a load re-applies the log the way bm64::state::restore remaps the overlay slot.
struct LoadGame {
    struct State {
        uint64_t hash = 0x2545F4914F6CDD1Dull;
        bool held = false;
        int loads = 0;
        bool valid = true;
    };
    State state;
    std::vector<State> history;
    std::vector<uint64_t> log;
    std::vector<size_t> log_size;
    int mapped = 0;
    bool bad_map = false;

    void save(uint64_t tick) {
        if (history.size() <= tick) {
            history.resize(static_cast<size_t>(tick) + 1);
            log_size.resize(static_cast<size_t>(tick) + 1);
        }
        history[static_cast<size_t>(tick)] = state;
        log_size[static_cast<size_t>(tick)] = log.size();
    }

    void drop_before(uint64_t tick) {
        for (uint64_t t = 0; t < tick && t < history.size(); t++) {
            history[static_cast<size_t>(t)].valid = false;
        }
    }

    bool load(uint64_t tick) {
        if (tick >= history.size() || !history[static_cast<size_t>(tick)].valid) {
            return false;
        }
        state = history[static_cast<size_t>(tick)];
        log.resize(log_size[static_cast<size_t>(tick)]);
        mapped = static_cast<int>(log.size());
        return true;
    }

    // Returns true when this tick loads.
    bool step(uint64_t tick, const bm64::rollback::FrameInput& input) {
        bool pressed = (input.player[1].button & 0x8000) != 0;
        bool loads = pressed && state.held;
        state.held = pressed && !loads;
        if (mapped != state.loads) {
            bad_map = true;
        }
        for (int i = 0; i < bm64::rollback::kMaxPlayers; i++) {
            state.hash ^= static_cast<uint64_t>(input.player[i].button) << ((i * 8) & 63);
            state.hash = state.hash * 0x9E3779B97F4A7C15ull + static_cast<uint8_t>(input.player[i].stick_x);
            state.hash += input.player[i].err;
        }
        if (loads) {
            state.loads++;
            log.push_back(tick);
            mapped = static_cast<int>(log.size());
        }
        return loads;
    }
};

struct LoadOptions {
    int frames = 90;
    int input_delay = 1;
    // Ticks on which player 1 holds 0x8000.
    std::vector<uint64_t> press;
    int hold_tick = -1;
    int hold_frames = 0;
    int expect_loads = 1;
};

bool run_load(const LoadOptions& opt, const char* name) {
    World world;
    world.hold_sender = opt.hold_tick >= 0 ? 1 : -1;
    world.hold_tick = opt.hold_tick;
    world.hold_from = 0;
    world.hold_until = opt.hold_tick >= 0 ? opt.hold_tick + opt.hold_frames : 0;
    Pipe pipes[2];
    for (int p = 0; p < 2; p++) {
        pipes[p].world = &world;
        pipes[p].me = p;
        pipes[p].delay = 1;
    }

    struct LoadPeer {
        LoadGame game;
        std::unique_ptr<bm64::rollback::Session> session;
        uint64_t samples = 0;
        bool busy = false;
        bool failed = false;
        std::vector<uint64_t> barriers;
    };
    LoadPeer peers[2];
    std::function<void(int)> run_frame;

    for (int p = 0; p < 2; p++) {
        bm64::rollback::Config cfg;
        cfg.player_count = 2;
        cfg.input_delay = opt.input_delay;
        cfg.max_rollback = 8;
        cfg.max_frame_advantage = 40;
        cfg.local_player = p;
        // Waiting for a final tick runs the other peer, as a second process would.
        cfg.wait_idle = [&, p]() {
            world.time++;
            if (!peers[1 - p].busy) {
                run_frame(1 - p);
            }
        };
        peers[p].session = std::make_unique<bm64::rollback::Session>(cfg, pipes[p]);
        peers[p].game.save(0);
    }

    auto pad_at = [&](int p, uint64_t tick) {
        bm64::rollback::Pad pad;
        pad.err = 0;
        pad.stick_x = static_cast<int8_t>((tick * 3 + p) & 0x3F);
        if (p == 1) {
            for (uint64_t t : opt.press) {
                if (t == tick) {
                    pad.button = 0x8000;
                }
            }
        }
        return pad;
    };

    run_frame = [&](int p) {
        LoadPeer& peer = peers[p];
        peer.busy = true;
        for (int spins = 0; spins < 64; spins++) {
            bm64::rollback::Step step;
            bool stall = false;
            bool ok = peer.session->poll(step, stall, [&](uint64_t tick) {
                if (!peer.game.load(tick)) {
                    std::fprintf(stderr, "%s: peer %d rolled back to tick %llu, before a load barrier\n",
                        name, p, static_cast<unsigned long long>(tick));
                    peer.failed = true;
                }
            });
            if (!ok) {
                break;
            }
            if (peer.game.step(step.tick, step.input)) {
                if (peer.session->notify_barrier(step.tick, 7)) {
                    peer.game.drop_before(step.tick);
                    peer.barriers.push_back(step.tick);
                }
            }
            peer.game.save(step.tick + 1);
            if (step.confirmed && !step.speculative) {
                peer.session->notify_hash(step.tick, peer.game.state.hash);
            }
            if (step.accept_sample) {
                peer.session->push_local(pad_at(p, peer.samples + static_cast<uint64_t>(opt.input_delay)));
                peer.samples++;
                break;
            }
        }
        peer.busy = false;
    };

    for (int frame = 0; frame < opt.frames; frame++) {
        world.time++;
        run_frame(0);
        run_frame(1);
    }

    uint64_t confirmed = std::min(peers[0].session->confirmed_tick(), peers[1].session->confirmed_tick());
    bool ok = true;
    for (int p = 0; p < 2; p++) {
        if (peers[p].failed) {
            ok = false;
        }
        if (peers[p].game.bad_map) {
            std::fprintf(stderr, "%s: peer %d ran a tick with the wrong code mapped\n", name, p);
            ok = false;
        }
        if (peers[p].session->desynced()) {
            std::fprintf(stderr, "%s: peer %d reported a desync\n", name, p);
            ok = false;
        }
    }
    if (confirmed < 60) {
        std::fprintf(stderr, "%s: stopped short at confirmed tick %llu\n", name, static_cast<unsigned long long>(confirmed));
        return false;
    }
    const LoadGame::State& sa = peers[0].game.history[static_cast<size_t>(confirmed)];
    const LoadGame::State& sb = peers[1].game.history[static_cast<size_t>(confirmed)];
    if (sa.hash != sb.hash || sa.loads != sb.loads) {
        std::fprintf(stderr, "%s: states differ at %llu (loads %d/%d)\n", name, static_cast<unsigned long long>(confirmed), sa.loads, sb.loads);
        ok = false;
    }
    if (sa.loads != opt.expect_loads || sb.loads != opt.expect_loads) {
        std::fprintf(stderr, "%s: expected %d loads, got %d/%d\n", name, opt.expect_loads, sa.loads, sb.loads);
        ok = false;
    }
    if (peers[0].barriers != peers[1].barriers || static_cast<int>(peers[0].barriers.size()) != opt.expect_loads) {
        std::fprintf(stderr, "%s: barriers differ (%zu/%zu)\n", name, peers[0].barriers.size(), peers[1].barriers.size());
        ok = false;
    }
    if (!ok) {
        return false;
    }
    std::printf("%s: ok  confirmed %llu  loads %d  barrier tick %llu  rollbacks %d/%d\n",
        name, static_cast<unsigned long long>(confirmed), sa.loads,
        static_cast<unsigned long long>(peers[0].barriers.empty() ? 0 : peers[0].barriers[0]),
        peers[0].session->rollbacks(), peers[1].session->rollbacks());
    return true;
}

// Three or more peers, every packet sent to every other peer. `cut(sender, dest, time)` drops a
// packet on that link.
struct Mesh {
    int time = 0;
    int peers = 3;
    std::function<bool(int, int, int)> cut;
    struct Msg {
        int due;
        int dest;
        std::vector<uint8_t> bytes;
    };
    std::vector<Msg> msgs;
};

struct MeshPipe : bm64::rollback::Transport {
    Mesh* mesh = nullptr;
    int me = 0;

    void send(const uint8_t* data, size_t size) override {
        for (int dest = 0; dest < mesh->peers; dest++) {
            if (dest == me || (mesh->cut && mesh->cut(me, dest, mesh->time))) {
                continue;
            }
            Mesh::Msg msg;
            msg.due = mesh->time + 1;
            msg.dest = dest;
            msg.bytes.assign(data, data + size);
            mesh->msgs.push_back(std::move(msg));
        }
    }

    int recv(uint8_t* data, size_t cap) override {
        for (auto it = mesh->msgs.begin(); it != mesh->msgs.end(); ++it) {
            if (it->dest == me && it->due <= mesh->time) {
                size_t n = std::min(it->bytes.size(), cap);
                std::memcpy(data, it->bytes.data(), n);
                mesh->msgs.erase(it);
                return static_cast<int>(n);
            }
        }
        return 0;
    }
};

struct LeaveOptions {
    int frames = 700;
    // Player 2 stops running after this frame, as if its game closed.
    int quit_frame = 50;
    // Player 2's packets to player 0 stop at this time. -1 keeps them until the quit.
    int cut_to_host = -1;
};

bool run_leave(const LeaveOptions& opt, const char* name) {
    constexpr int kPeers = 3;
    Mesh mesh;
    mesh.peers = kPeers;
    mesh.cut = [&](int sender, int dest, int time) {
        return sender == 2 && dest == 0 && opt.cut_to_host >= 0 && time >= opt.cut_to_host;
    };
    MeshPipe pipes[kPeers];
    std::unique_ptr<bm64::rollback::Session> sessions[kPeers];
    Fake fakes[kPeers];
    // The final pad each tick ran with for player 2, per peer.
    std::vector<uint8_t> err2[kPeers];
    uint64_t samples[kPeers] = {};
    for (int p = 0; p < kPeers; p++) {
        pipes[p].mesh = &mesh;
        pipes[p].me = p;
        bm64::rollback::Config cfg;
        cfg.player_count = kPeers;
        cfg.local_player = p;
        cfg.input_delay = 2;
        cfg.max_rollback = 8;
        cfg.max_frame_advantage = 3;
        cfg.clock_ms = [&]() { return static_cast<uint64_t>(mesh.time) * 16; };
        sessions[p] = std::make_unique<bm64::rollback::Session>(cfg, pipes[p]);
        fakes[p].save(0);
    }
    uint64_t host_count_early = 0;
    for (int frame = 0; frame < opt.frames; frame++) {
        mesh.time++;
        for (int p = 0; p < kPeers; p++) {
            if (p == 2 && frame > opt.quit_frame) {
                continue;
            }
            for (int spins = 0; spins < 64; spins++) {
                bm64::rollback::Step step;
                bool stall = false;
                if (!sessions[p]->poll(step, stall, [&](uint64_t tick) { fakes[p].load(tick); })) {
                    break;
                }
                fakes[p].step(step.input);
                fakes[p].save(step.tick + 1);
                if (err2[p].size() <= step.tick) {
                    err2[p].resize(static_cast<size_t>(step.tick) + 1);
                }
                err2[p][static_cast<size_t>(step.tick)] = step.input.player[2].err;
                if (step.confirmed && !step.speculative) {
                    sessions[p]->notify_hash(step.tick, fakes[p].state);
                }
                if (step.accept_sample) {
                    sessions[p]->push_local(sample(p, samples[p]));
                    samples[p]++;
                    break;
                }
            }
        }
        if (frame == 100) {
            host_count_early = sessions[0]->received_count(2);
        }
    }

    bool ok = true;
    for (int p = 0; p < 2; p++) {
        if (sessions[p]->ended()) {
            std::fprintf(stderr, "%s: peer %d ended: %s\n", name, p, sessions[p]->end_reason());
            ok = false;
        }
        if (!sessions[p]->left(2)) {
            std::fprintf(stderr, "%s: peer %d never applied the leave\n", name, p);
            ok = false;
        }
        if (sessions[p]->desynced()) {
            std::fprintf(stderr, "%s: peer %d reported a desync\n", name, p);
            ok = false;
        }
    }
    if (!ok) {
        return false;
    }
    uint64_t leave = sessions[0]->leave_tick(2);
    if (sessions[1]->leave_tick(2) != leave) {
        std::fprintf(stderr, "%s: leave ticks differ (%llu/%llu)\n", name,
            static_cast<unsigned long long>(leave), static_cast<unsigned long long>(sessions[1]->leave_tick(2)));
        return false;
    }
    // Player 1 heard player 2 to the end. Its last sample was taken after its last send, so it
    // never left that machine.
    uint64_t heard = sessions[1]->received_count(2);
    uint64_t sampled = sessions[2]->received_count(2);
    if (leave != heard || heard + 1 != sampled) {
        std::fprintf(stderr, "%s: leave tick %llu, player 1 heard %llu inputs, player 2 sampled %llu\n", name,
            static_cast<unsigned long long>(leave), static_cast<unsigned long long>(heard),
            static_cast<unsigned long long>(sampled));
        return false;
    }
    if (opt.cut_to_host >= 0 && host_count_early >= leave) {
        std::fprintf(stderr, "%s: the host already had every input (%llu), so the cut tested nothing\n", name,
            static_cast<unsigned long long>(host_count_early));
        return false;
    }
    uint64_t confirmed = std::min(sessions[0]->confirmed_tick(), sessions[1]->confirmed_tick());
    if (confirmed < leave + 200) {
        std::fprintf(stderr, "%s: stopped at confirmed tick %llu, leave %llu\n", name,
            static_cast<unsigned long long>(confirmed), static_cast<unsigned long long>(leave));
        return false;
    }
    if (fakes[0].at(confirmed) != fakes[1].at(confirmed)) {
        std::fprintf(stderr, "%s: states differ at %llu\n", name, static_cast<unsigned long long>(confirmed));
        return false;
    }
    for (int p = 0; p < 2; p++) {
        if (err2[p][static_cast<size_t>(leave - 1)] == bm64::rollback::kLeftErr || err2[p][static_cast<size_t>(leave)] != bm64::rollback::kLeftErr) {
            std::fprintf(stderr, "%s: peer %d does not switch player 2 to the left pad at tick %llu\n", name, p,
                static_cast<unsigned long long>(leave));
            return false;
        }
    }
    std::printf("%s: ok  leave tick %llu  confirmed %llu  host had %llu at frame 100  rollbacks %d/%d\n",
        name, static_cast<unsigned long long>(leave), static_cast<unsigned long long>(confirmed),
        static_cast<unsigned long long>(host_count_early),
        sessions[0]->rollbacks(), sessions[1]->rollbacks());
    return true;
}

} // namespace

int main() {
    int failed = 0;
    PairOptions one_late;
    one_late.frames = 120;
    if (!run_pair(one_late, "one tick late")) {
        failed++;
    }
    PairOptions latency;
    latency.frames = 180;
    latency.delay_packets = 4;
    latency.expect_rollback = true;
    if (!run_pair(latency, "latency")) {
        failed++;
    }
    PairOptions loss;
    loss.frames = 200;
    loss.delay_packets = 2;
    loss.loss = 25;
    loss.input_delay = 2;
    if (!run_pair(loss, "loss")) {
        failed++;
    }
    // Input arrives in time, except tick 20, which is held back for six frames. The ticks after
    // it have their own inputs but were simulated on a guess for tick 20, so they must not be hashed.
    PairOptions late_packet;
    late_packet.frames = 80;
    late_packet.input_delay = 3;
    late_packet.hold_sender = 1;
    late_packet.hold_tick = 20;
    late_packet.hold_from = 0;
    late_packet.hold_until = 26;
    late_packet.expect_rollback = true;
    if (!run_pair(late_packet, "late packet")) {
        failed++;
    }
    // One peer goes silent for two seconds with the game's time-sync setting. The other must stop
    // inside the rollback window instead of guessing ticks it can no longer correct.
    PairOptions silence;
    silence.frames = 260;
    silence.input_delay = 3;
    silence.max_frame_advantage = 3;
    silence.hold_sender = 1;
    silence.hold_from = 20;
    silence.hold_until = 140;
    silence.min_confirmed = 120;
    if (!run_pair(silence, "silence")) {
        failed++;
    }
    // The host's game takes eight seconds (500 frames of 16 ms) to reach its first tick after the
    // handshake, longer than silence_ms + leave_decide_ms. The guest must wait for it, not end.
    PairOptions late_host;
    late_host.frames = 700;
    late_host.input_delay = 2;
    late_host.max_frame_advantage = 3;
    late_host.late_peer = 0;
    late_host.late_frame = 500;
    late_host.min_confirmed = 150;
    if (!run_pair(late_host, "late host start")) {
        failed++;
    }
    // The same with the guest booting late: the host must not hand the guest to the AI.
    PairOptions late_guest = late_host;
    late_guest.late_peer = 1;
    if (!run_pair(late_guest, "late guest start")) {
        failed++;
    }
    if (!run_barrier()) {
        failed++;
    }
    // Player 1 taps for one tick and its next input is late, so player 0 guesses the hold that
    // loads. That load must be rolled back, and the real hold later loads once on both peers.
    LoadOptions guessed;
    guessed.press = { 20, 40, 41 };
    guessed.hold_tick = 21;
    guessed.hold_frames = 6;
    if (!run_load(guessed, "guessed load")) {
        failed++;
    }
    LoadOptions real_load;
    real_load.press = { 30, 31 };
    if (!run_load(real_load, "real load")) {
        failed++;
    }
    // Player 2 of three closes the game. Everyone keeps going with player 2 left from one tick.
    LeaveOptions leave;
    if (!run_leave(leave, "leave")) {
        failed++;
    }
    // Player 2's last packets reach player 1 but not the host, so the leave tick comes from player 1.
    LeaveOptions partial;
    partial.cut_to_host = 45;
    if (!run_leave(partial, "leave, host missed the end")) {
        failed++;
    }
    if (failed) {
        std::fprintf(stderr, "bm64_net_tests: %d failed\n", failed);
        return 1;
    }
    std::printf("bm64_net_tests: all passed\n");
    return 0;
}
