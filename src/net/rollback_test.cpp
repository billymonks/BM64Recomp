#include "bm64_rollback.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct World {
    int time = 0;
    int loss_percent = 0;
    uint32_t rng = 1;
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
};

struct Pipe : bm64::rollback::Transport {
    World* world = nullptr;
    int me = 0;
    int delay = 1;

    void send(const uint8_t* data, size_t size) override {
        if (world->drop()) {
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
};

bool run_pair(int frames, int delay_packets, int loss, int input_delay, bool expect_rollback, const char* name) {
    World world;
    world.loss_percent = loss;
    Pipe pipes[2];
    pipes[0].world = &world;
    pipes[1].world = &world;
    pipes[0].me = 0;
    pipes[1].me = 1;
    pipes[0].delay = delay_packets;
    pipes[1].delay = delay_packets;

    bm64::rollback::Config cfg;
    cfg.player_count = 2;
    cfg.input_delay = input_delay;
    cfg.max_rollback = 8;
    cfg.max_frame_advantage = 40;
    cfg.local_player = 0;
    bm64::rollback::Session a(cfg, pipes[0]);
    cfg.local_player = 1;
    bm64::rollback::Session b(cfg, pipes[1]);
    Peer peers[2];
    peers[0].session = &a;
    peers[1].session = &b;
    peers[0].fake.save(0);
    peers[1].fake.save(0);

    for (int frame = 0; frame < frames; frame++) {
        world.time++;
        for (int p = 0; p < 2; p++) {
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
                if (step.confirmed && (step.tick % 7) == 0) {
                    peers[p].session->notify_hash(step.tick, peers[p].fake.state);
                }
                if (step.accept_sample) {
                    peers[p].session->push_local(sample(p, peers[p].samples));
                    peers[p].samples++;
                    sampled = true;
                }
            }
            if (!stepped && !sampled) {
                std::fprintf(stderr, "%s: peer %d stalled out at frame %d\n", name, p, frame);
                return false;
            }
        }
    }

    uint64_t confirmed = a.confirmed_tick() < b.confirmed_tick() ? a.confirmed_tick() : b.confirmed_tick();
    if (confirmed < 8 || peers[0].fake.at(confirmed) != peers[1].fake.at(confirmed)) {
        std::fprintf(stderr, "%s: confirmed state differs at %llu (%016llx %016llx) steps %llu/%llu rollbacks %d/%d\n",
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
    if (expect_rollback && a.rollbacks() + b.rollbacks() == 0) {
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
    bm64::rollback::Config cfg;
    cfg.player_count = 2;
    cfg.input_delay = 1;
    cfg.max_rollback = 8;
    cfg.max_frame_advantage = 40;
    cfg.local_player = 0;
    bm64::rollback::Session a(cfg, pipes[0]);
    cfg.local_player = 1;
    bm64::rollback::Session b(cfg, pipes[1]);
    Fake fa, fb;
    fa.save(0);
    fb.save(0);
    bool sent[2] = { false, false };
    uint64_t samples[2] = { 0, 0 };
    for (int frame = 0; frame < 40; frame++) {
        world.time++;
        bm64::rollback::Session* sessions[2] = { &a, &b };
        Fake* fakes[2] = { &fa, &fb };
        for (int p = 0; p < 2; p++) {
            bool sampled = false;
            for (int spins = 0; spins < 64 && !sampled; spins++) {
                bm64::rollback::Step step;
                bool stall = false;
                if (!sessions[p]->poll(step, stall, [&](uint64_t tick) { fakes[p]->load(tick); })) {
                    break;
                }
                fakes[p]->step(step.input);
                fakes[p]->save(step.tick + 1);
                if (!sent[p] && samples[p] == 10 && step.accept_sample) {
                    sessions[p]->notify_barrier(step.tick, 42);
                    sent[p] = true;
                }
                if (step.accept_sample) {
                    sessions[p]->push_local(sample(p, samples[p]));
                    samples[p]++;
                    sampled = true;
                }
            }
            if (!sampled && frame > 5) {
                std::fprintf(stderr, "barrier: peer %d did not sample at frame %d\n", p, frame);
                return false;
            }
        }
    }
    uint64_t confirmed = a.confirmed_tick() < b.confirmed_tick() ? a.confirmed_tick() : b.confirmed_tick();
    if (confirmed < 8 || fa.at(confirmed) != fb.at(confirmed)) {
        std::fprintf(stderr, "barrier: states differ at %llu\n", static_cast<unsigned long long>(confirmed));
        return false;
    }
    std::printf("barrier: ok  waits %d/%d\n", a.barrier_waits(), b.barrier_waits());
    return true;
}

} // namespace

int main() {
    int failed = 0;
    if (!run_pair(120, 1, 0, 1, false, "lockstep")) {
        failed++;
    }
    if (!run_pair(180, 4, 0, 1, true, "latency")) {
        failed++;
    }
    if (!run_pair(200, 2, 25, 2, false, "loss")) {
        failed++;
    }
    if (!run_barrier()) {
        failed++;
    }
    if (failed) {
        std::fprintf(stderr, "bm64_net_tests: %d failed\n", failed);
        return 1;
    }
    std::printf("bm64_net_tests: all passed\n");
    return 0;
}
