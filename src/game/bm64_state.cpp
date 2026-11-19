#include "bm64_state.h"

#include "bm64_debug.h"
#include "bm64_online.h"
#include "bm64_coop.h"
#include "librecomp/helpers.hpp"
#include "recomp.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace bm64::state {

constexpr uint32_t kMagic = 0x4E534D42u; // 'BMSN' little-endian
constexpr uint32_t kVersion = 1;
constexpr int kRing = 16;
constexpr uint32_t kSoundSlots = 0x800BD770;
constexpr uint32_t kSoundSlotsEnd = 0x800BD818;

enum class Phase {
    Idle,
    Discarding,
    CompareReady,
    Comparing,
    Record,
    NeedLoad,
    Compare
};

struct Voice {
    int32_t id = 0;
    int32_t sound = 0;
    int32_t source = 0;
    int32_t real = -1;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint64_t tick = 0;
};

struct Sound {
    int32_t next_id = 1;
    std::vector<Voice> voices;
};

struct Snap {
    bool used = false;
    uint64_t tick = 0;
    uint64_t seq = 0;
    std::vector<uint8_t> blob;
};

std::vector<Segment> segments() {
    return {
        { 0x80043000, 0x80063000 },
        { 0x80063000, 0x800BD770 },
        { 0x800BD818, 0x800BE848 },
        { 0x8014B000, 0x8014C8D0 },
        { 0x8029F5E4, 0x8029F608 },
        { 0x8029F6D8, 0x802A5300 },
        { 0x802A5300, 0x802A5330 },
        { 0x802A5368, 0x802A5380 },
        { 0x802A53BC, 0x802A5550 },
        { 0x802A5554, 0x802A5A60 },
        { 0x802AB200, 0x802AC3E0 },
        { 0x802AC400, 0x802AD120 },
        { 0x802AFC20, 0x802B36D0 },
        { 0x802B36D0, 0x80400000 },
        // Relocated player array. Leaving it out of the snapshot makes rollback
        // keep a dangling object pointer and fault on the next resim.
        { 0x80F00000, 0x80F00640 },
    };
}

const std::vector<Segment>& hash_segments_storage() {
    static const std::vector<Segment> ranges = segments();
    return ranges;
}

struct Engine {
    bool configured = false;
    bool running = false;
    bool synctest = false;
    int savetest_len = 0;
    int savetest_start = 200;
    Phase phase = Phase::Idle;
    bool quiet = false;
    bool speculative = false;
    bool skip_lookat = false;
    int scroll = 0;
    int boot_timer = 8;
    int overlay = -1;
    bool have_overlay = false;
    uint64_t sim = 0;
    uint8_t* rdram = nullptr;

    Sound sound;
    int noted_sound = 0;
    int noted_source = 0;
    int noted_x = 0;
    int noted_y = 0;
    int noted_z = 0;

    Snap ring[kRing];
    int ring_next = 0;
    uint64_t ring_seq = 0;
    std::chrono::steady_clock::time_point compare_started{};
    std::vector<uint8_t> dedicated;
    std::vector<uint8_t> quick;
    bool have_quick = false;
    std::atomic<bool> want_save{ false };
    std::atomic<bool> want_load{ false };

    uint64_t save_us = 0;
    uint64_t load_us = 0;

    uint64_t discard_hash = 0;
    std::vector<std::string> discard_names;
    std::vector<uint64_t> discard_ranges;
    std::vector<uint64_t> record_hashes;
    size_t compare_index = 0;
    bool record_saved = false;
    int synctest_mismatches = 0;
    int synctest_ticks = 0;
    int inject_tick = -1;
    uint32_t inject_addr = 0;
    uint8_t inject_value = 0;
    bool inject_done = false;
};

Engine& eng() {
    static Engine e;
    return e;
}

uint64_t micros(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
}

void configure() {
    Engine& e = eng();
    if (e.configured) {
        return;
    }
    e.configured = true;
    if (const char* v = std::getenv("BM64_SYNCTEST")) {
        e.synctest = v[0] != '0';
    }
    if (const char* v = std::getenv("BM64_SAVETEST")) {
        e.savetest_len = std::atoi(v);
        if (e.savetest_len < 1) {
            e.savetest_len = 0;
        }
    }
    if (const char* v = std::getenv("BM64_SAVETEST_START")) {
        e.savetest_start = std::atoi(v);
    }
    if (e.savetest_len > 0 && e.synctest) {
        std::printf("[bm64 state] BM64_SAVETEST is set, so SyncTest stays off for this run\n");
        e.synctest = false;
    }
    if (const char* v = std::getenv("BM64_INJECT_DESYNC")) {
        unsigned tick = 0, addr = 0, value = 0;
        if (std::sscanf(v, "%u,0x%x,%u", &tick, &addr, &value) == 3 || std::sscanf(v, "%u,%x,%u", &tick, &addr, &value) == 3) {
            e.inject_tick = static_cast<int>(tick);
            e.inject_addr = addr;
            e.inject_value = static_cast<uint8_t>(value);
            std::printf("[bm64 state] Will xor RDRAM %08X with %u at tick %d\n", addr, value, e.inject_tick);
        }
    }
    if (e.synctest) {
        std::printf("[bm64 state] SyncTest on (save, step, load, step, compare)\n");
    }
    if (e.savetest_len > 0) {
        std::printf("[bm64 state] Savetest at tick %d for %d ticks\n", e.savetest_start, e.savetest_len);
    }
}

void write_u32(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t b[4] = { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24) };
    o.insert(o.end(), b, b + 4);
}

void write_u64(std::vector<uint8_t>& o, uint64_t v) {
    write_u32(o, static_cast<uint32_t>(v));
    write_u32(o, static_cast<uint32_t>(v >> 32));
}

void write_bytes(std::vector<uint8_t>& o, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    o.insert(o.end(), b, b + n);
}

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    uint32_t u32() {
        if (p + 4 > end) {
            ok = false;
            return 0;
        }
        uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        p += 4;
        return v;
    }
    uint64_t u64() {
        uint32_t lo = u32();
        uint32_t hi = u32();
        return lo | (static_cast<uint64_t>(hi) << 32);
    }
    void bytes(void* dst, size_t n) {
        if (p + n > end) {
            ok = false;
            return;
        }
        std::memcpy(dst, p, n);
        p += n;
    }
};

void stop_real(int real) {
    Engine& e = eng();
    if (real < 0 || e.rdram == nullptr) {
        return;
    }
    recomp_func_t* fn = get_function(0x80266930);
    if (fn == nullptr) {
        return;
    }
    recomp_context ctx{};
    ctx.r4 = static_cast<gpr>(real);
    ctx.f_odd = &ctx.f0.u32h;
    fn(e.rdram, &ctx);
}

void restore_sound(const Sound& saved) {
    Engine& e = eng();
    for (const Voice& cur : e.sound.voices) {
        if (cur.real < 0) {
            continue;
        }
        bool keep = false;
        for (const Voice& v : saved.voices) {
            if (v.id == cur.id && v.real == cur.real) {
                keep = true;
            }
        }
        if (!keep) {
            stop_real(cur.real);
        }
    }
    e.sound = saved;
}

std::vector<uint8_t> capture(uint8_t* rdram) {
    Engine& e = eng();
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> out;
    out.reserve(2 * 1024 * 1024);
    write_u32(out, kMagic);
    write_u32(out, kVersion);
    bm64::debug::InputCursor cursor = bm64::debug::input_cursor();
    write_u64(out, bm64::debug::tick());
    write_u64(out, cursor.input_frame);
    write_u64(out, cursor.replay_cursor);
    write_u32(out, cursor.mode);
    write_u32(out, static_cast<uint32_t>(e.overlay));
    write_u32(out, static_cast<uint32_t>(e.scroll));
    write_u32(out, static_cast<uint32_t>(e.boot_timer));
    write_u32(out, static_cast<uint32_t>(e.sound.next_id));
    write_u32(out, static_cast<uint32_t>(e.sound.voices.size()));
    for (const Voice& v : e.sound.voices) {
        write_bytes(out, &v, sizeof(v));
    }
    std::vector<uint8_t> coros;
    bm64::proc::save(coros);
    write_u32(out, static_cast<uint32_t>(coros.size()));
    write_bytes(out, coros.data(), coros.size());

    auto append_range = [&](uint32_t start, uint32_t end) {
        write_bytes(out, rdram + (start - 0x80000000u), end - start);
    };
    for (const Segment& s : hash_segments_storage()) {
        append_range(s.start, s.end);
    }
    // Not part of the sync hash. The file loader's lock sits just before gFrontBufferID. The first
    // pass leaves it set, and the reload then allocates from a different free-list bin.
    append_range(0x8029F580, 0x8029F5E4);
    append_range(kSoundSlots, kSoundSlotsEnd);
    e.save_us = micros(t0, std::chrono::steady_clock::now());
    return out;
}

bool restore(uint8_t* rdram, const uint8_t* data, size_t size) {
    Engine& e = eng();
    auto t0 = std::chrono::steady_clock::now();
    Reader r{ data, data + size, true };
    if (r.u32() != kMagic || r.u32() != kVersion) {
        std::fprintf(stderr, "[bm64 state] Snapshot header rejected\n");
        return false;
    }
    uint64_t tick = r.u64();
    bm64::debug::InputCursor cursor;
    cursor.input_frame = r.u64();
    cursor.replay_cursor = r.u64();
    cursor.mode = r.u32();
    int overlay = static_cast<int>(r.u32());
    int scroll = static_cast<int>(r.u32());
    int boot_timer = static_cast<int>(r.u32());
    Sound sound;
    sound.next_id = static_cast<int32_t>(r.u32());
    uint32_t voices = r.u32();
    sound.voices.resize(voices);
    for (uint32_t i = 0; i < voices; i++) {
        r.bytes(&sound.voices[i], sizeof(Voice));
    }
    uint32_t coro_size = r.u32();
    if (!r.ok || r.p + coro_size > r.end) {
        std::fprintf(stderr, "[bm64 state] Snapshot truncated\n");
        return false;
    }
    if (!bm64::proc::load(r.p, coro_size)) {
        std::fprintf(stderr, "[bm64 state] Coroutine restore failed\n");
        return false;
    }
    r.p += coro_size;
    auto read_range = [&](uint32_t start, uint32_t end) {
        size_t n = end - start;
        r.bytes(rdram + (start - 0x80000000u), n);
    };
    for (const Segment& s : hash_segments_storage()) {
        read_range(s.start, s.end);
    }
    read_range(0x8029F580, 0x8029F5E4);
    read_range(kSoundSlots, kSoundSlotsEnd);
    if (!r.ok) {
        std::fprintf(stderr, "[bm64 state] Snapshot RDRAM truncated\n");
        return false;
    }
    restore_sound(sound);
    bm64::debug::set_tick(tick);
    bm64::debug::set_input_cursor(cursor);
    e.overlay = overlay;
    e.have_overlay = true;
    e.scroll = scroll;
    e.boot_timer = boot_timer;
    e.load_us = micros(t0, std::chrono::steady_clock::now());
    return true;
}

void ring_save(uint8_t* rdram) {
    Engine& e = eng();
    Snap& slot = e.ring[e.ring_next];
    e.ring_next = (e.ring_next + 1) % kRing;
    slot.used = true;
    slot.tick = bm64::debug::tick();
    slot.seq = ++e.ring_seq;
    slot.blob = capture(rdram);
}

bool ring_load(uint64_t tick, uint8_t* rdram) {
    Engine& e = eng();
    const Snap* best = nullptr;
    for (const Snap& slot : e.ring) {
        if (slot.used && slot.tick == tick && (best == nullptr || slot.seq > best->seq)) {
            best = &slot;
        }
    }
    if (best == nullptr) {
        std::fprintf(stderr, "[bm64 state] No snapshot for tick %llu\n", static_cast<unsigned long long>(tick));
        return false;
    }
    return restore(rdram, best->blob.data(), best->blob.size());
}

void fail(const char* why) {
    std::printf("BM64 SAVETEST FAIL %s\n", why);
    std::fflush(stdout);
    std::exit(1);
}

void finish_savetest() {
    Engine& e = eng();
    uint64_t resim_us = micros(e.compare_started, std::chrono::steady_clock::now());
    uint64_t per = e.savetest_len > 0 ? resim_us / static_cast<uint64_t>(e.savetest_len) : 0;
    std::printf("BM64 SAVETEST PASS ticks %d save_us %llu load_us %llu resim_us %llu per_tick_us %llu stacks %s\n",
        e.savetest_len,
        static_cast<unsigned long long>(e.save_us),
        static_cast<unsigned long long>(e.load_us),
        static_cast<unsigned long long>(resim_us),
        static_cast<unsigned long long>(per),
        stacks_stable() ? "stable" : "MOVED");
    std::fflush(stdout);
    if (!stacks_stable()) {
        std::exit(1);
    }
    std::exit(0);
}

void step_end_tests(uint8_t* rdram) {
    Engine& e = eng();
    if (e.inject_tick >= 0 && !e.inject_done && bm64::debug::tick() == static_cast<uint64_t>(e.inject_tick) + 1) {
        uint32_t addr = e.inject_addr;
        if (addr >= 0x80000000u && addr < 0x80800000u) {
            rdram[addr - 0x80000000u] ^= e.inject_value;
            e.inject_done = true;
            std::printf("[bm64 state] Injected desync at %08X on tick %d\n", addr, e.inject_tick);
        }
    }
    uint64_t hash = bm64::debug::hash_state(rdram);
    if (e.phase == Phase::Discarding) {
        e.discard_hash = hash;
        bm64::debug::range_hashes(rdram, e.discard_names, e.discard_ranges);
        e.phase = Phase::CompareReady;
        return;
    }
    if (e.phase == Phase::Comparing) {
        e.synctest_ticks++;
        if (hash != e.discard_hash) {
            e.synctest_mismatches++;
            std::vector<std::string> names;
            std::vector<uint64_t> ranges;
            bm64::debug::range_hashes(rdram, names, ranges);
            std::printf("BM64 SYNCTEST FAIL tick %llu %016llx %016llx\n",
                static_cast<unsigned long long>(e.sim),
                static_cast<unsigned long long>(e.discard_hash),
                static_cast<unsigned long long>(hash));
            for (size_t i = 0; i < names.size() && i < e.discard_ranges.size(); i++) {
                if (names[i] != e.discard_names[i] || ranges[i] != e.discard_ranges[i]) {
                    std::printf("  %s %016llx %016llx\n", names[i].c_str(),
                        static_cast<unsigned long long>(e.discard_ranges[i]),
                        static_cast<unsigned long long>(ranges[i]));
                }
            }
            std::fflush(stdout);
            std::exit(1);
        }
        e.phase = Phase::Idle;
        e.speculative = false;
        return;
    }
    if (e.phase == Phase::Record) {
        e.record_hashes.push_back(hash);
        if (static_cast<int>(e.record_hashes.size()) >= e.savetest_len) {
            e.phase = Phase::NeedLoad;
        }
        return;
    }
    if (e.phase == Phase::Compare) {
        if (e.compare_index >= e.record_hashes.size() || hash != e.record_hashes[e.compare_index]) {
            fail("hash mismatch");
        }
        e.compare_index++;
        if (e.compare_index == e.record_hashes.size()) {
            finish_savetest();
        }
    }
}

int begin_step(uint8_t* rdram) {
    configure();
    Engine& e = eng();
    e.rdram = rdram;
    e.running = true;
    bm64::coop::apply_rules(rdram);

    if (e.want_load.exchange(false)) {
        if (e.have_quick) {
            restore(rdram, e.quick.data(), e.quick.size());
            std::printf("[bm64 state] Quickload at tick %llu\n", static_cast<unsigned long long>(bm64::debug::tick()));
        }
        else {
            std::printf("[bm64 state] Quickload ignored; nothing is saved this session\n");
        }
    }

    int render = 1;
    bool spec = false;
    bool quiet = false;

    if (e.phase == Phase::NeedLoad) {
        if (!restore(rdram, e.dedicated.data(), e.dedicated.size())) {
            fail("could not restore");
        }
        e.phase = Phase::Compare;
        e.compare_index = 0;
        e.compare_started = std::chrono::steady_clock::now();
        spec = true;
        render = 0;
        if (bm64::debug::input_cursor().mode == 0) {
            uint8_t pads[24];
            bm64::debug::copy_pads(pads);
            bm64::debug::set_pad_reuse(pads);
        }
    }
    else if (e.phase == Phase::CompareReady) {
        if (!restore(rdram, e.dedicated.data(), e.dedicated.size())) {
            std::printf("BM64 SYNCTEST FAIL restore\n");
            std::exit(1);
        }
        e.phase = Phase::Comparing;
        spec = true;
        render = 0;
        if (bm64::debug::input_cursor().mode == 0) {
            uint8_t pads[24];
            bm64::debug::copy_pads(pads);
            bm64::debug::set_pad_reuse(pads);
        }
    }
    else if (bm64::online::active()) {
        render = bm64::online::begin_frame(rdram);
        spec = bm64::online::speculative();
    }
    else if (e.savetest_len > 0 && e.phase == Phase::Idle && static_cast<int>(bm64::debug::tick()) >= e.savetest_start) {
        e.phase = Phase::Record;
        spec = true;
        render = 0;
    }
    else if (e.synctest && e.phase == Phase::Idle) {
        e.phase = Phase::Discarding;
        spec = true;
        render = 0;
        quiet = true;
    }
    else if (e.phase == Phase::Record || e.phase == Phase::Compare) {
        spec = true;
        render = 0;
    }

    if (e.phase == Phase::Discarding || (e.phase == Phase::Record && !e.record_saved)) {
        e.dedicated = capture(rdram);
        e.record_saved = e.phase == Phase::Record;
    }

    if (e.want_save.exchange(false)) {
        e.quick = capture(rdram);
        e.have_quick = true;
        std::printf("[bm64 state] Quicksave at tick %llu (%zu bytes, save %llu us)\n",
            static_cast<unsigned long long>(bm64::debug::tick()), e.quick.size(),
            static_cast<unsigned long long>(e.save_us));
    }

    ring_save(rdram);
    e.sim = bm64::debug::tick();
    e.speculative = spec;
    e.skip_lookat = render == 0;
    e.quiet = quiet;
    bm64::debug::set_quiet_log(quiet);
    return render;
}

const std::vector<Segment>& hash_segments() {
    return hash_segments_storage();
}

bool stacks_stable() {
    return bm64::proc::stacks_moved() == false;
}

void last_snapshot_cost(uint64_t& save_us, uint64_t& load_us) {
    save_us = eng().save_us;
    load_us = eng().load_us;
}

bool save_tick(uint64_t tick, uint8_t* rdram) {
    (void)tick;
    Engine& e = eng();
    e.rdram = rdram;
    Snap& slot = e.ring[e.ring_next];
    e.ring_next = (e.ring_next + 1) % kRing;
    slot.used = true;
    slot.tick = tick;
    slot.seq = ++e.ring_seq;
    slot.blob = capture(rdram);
    return true;
}

bool load_tick(uint64_t tick, uint8_t* rdram) {
    eng().rdram = rdram;
    return ring_load(tick, rdram);
}

void drop_snapshots_before(uint64_t tick) {
    for (Snap& slot : eng().ring) {
        if (slot.used && slot.tick < tick) {
            slot.used = false;
            slot.blob.clear();
        }
    }
}

uint64_t sim_tick() {
    return eng().sim;
}

void request_quicksave() {
    if (eng().running) {
        eng().want_save.store(true);
    }
}

void request_quickload() {
    if (eng().running) {
        eng().want_load.store(true);
    }
}

} // namespace bm64::state

namespace {

bool g_inject = false;
uint8_t g_inject_pads[24];

} // namespace

void bm64_state_set_pad_inject(const uint8_t* pads24) {
    if (pads24 == nullptr) {
        g_inject = false;
        return;
    }
    std::memcpy(g_inject_pads, pads24, 24);
    g_inject = true;
}

bool bm64_state_consume_inject(uint8_t out[24]) {
    if (!g_inject) {
        return false;
    }
    std::memcpy(out, g_inject_pads, 24);
    g_inject = false;
    return true;
}

// The process module reports whether a stack address changed. Defined with the coroutines.
extern "C" void bm64_sound_enter(int sound_id, int source, int xbits, int ybits, int zbits) {
    auto& e = bm64::state::eng();
    e.noted_sound = sound_id;
    e.noted_source = source;
    e.noted_x = xbits;
    e.noted_y = ybits;
    e.noted_z = zbits;
}

extern "C" int bm64_sound_speculative(void) {
    return bm64::state::eng().speculative ? 1 : 0;
}

extern "C" int bm64_sound_should_skip(void) {
    auto& e = bm64::state::eng();
    if (e.speculative) {
        return 1;
    }
    for (const auto& v : e.sound.voices) {
        if (v.tick == e.sim && v.sound == e.noted_sound && v.source == e.noted_source &&
            v.x == e.noted_x && v.y == e.noted_y && v.z == e.noted_z) {
            return 1;
        }
    }
    return 0;
}

extern "C" int bm64_sound_leave(int real_handle) {
    auto& e = bm64::state::eng();
    for (const auto& v : e.sound.voices) {
        if (v.tick == e.sim && v.sound == e.noted_sound && v.source == e.noted_source &&
            v.x == e.noted_x && v.y == e.noted_y && v.z == e.noted_z) {
            return v.id;
        }
    }
    bm64::state::Voice voice;
    voice.id = e.sound.next_id++;
    voice.sound = e.noted_sound;
    voice.source = e.noted_source;
    voice.x = e.noted_x;
    voice.y = e.noted_y;
    voice.z = e.noted_z;
    voice.tick = e.sim;
    voice.real = (!e.speculative && real_handle >= 0) ? real_handle : -1;
    e.sound.voices.push_back(voice);
    return voice.id;
}

extern "C" int bm64_sound_resolve(int virtual_id) {
    if (virtual_id < 0) {
        return -1;
    }
    for (const auto& v : bm64::state::eng().sound.voices) {
        if (v.id == virtual_id) {
            return v.real;
        }
    }
    return -1;
}

extern "C" void bm64_restore_pending(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    auto phase = bm64::state::eng().phase;
    int pending = (phase == bm64::state::Phase::CompareReady || phase == bm64::state::Phase::NeedLoad) ? 1 : 0;
    if (bm64::online::active()) {
        // The step may roll state back. Wait for the in-flight display list first. The wait itself
        // stays in the recompiled loop: bm64_wait_gfx is recompiled code and cannot be called from here.
        pending = 1;
    }
    _return(ctx, pending);
}

extern "C" void bm64_step_begin(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, bm64::state::begin_step(rdram));
}

extern "C" void bm64_boot_timer(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return(ctx, bm64::state::eng().boot_timer);
}

extern "C" void bm64_set_boot_timer(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    bm64::state::eng().boot_timer = static_cast<int>(_arg<0, int32_t>(rdram, ctx));
}

extern "C" void bm64_take_overlay(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    auto& e = bm64::state::eng();
    int value = -1;
    if (e.have_overlay) {
        value = e.overlay;
        e.have_overlay = false;
    }
    _return(ctx, value);
}

extern "C" void bm64_take_scroll_pending(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    auto& e = bm64::state::eng();
    int value = e.scroll;
    e.scroll = 0;
    _return(ctx, value);
}

extern "C" void bm64_set_scroll_pending(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    bm64::state::eng().scroll = _arg<0, int32_t>(rdram, ctx);
}

extern "C" void bm64_skip_lookat(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return(ctx, bm64::state::eng().skip_lookat ? 1 : 0);
}

extern "C" void bm64_step_end(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    bm64::state::step_end_tests(rdram);
    if (bm64::online::active()) {
        bm64::online::end_frame(rdram, bm64::debug::hash_state(rdram));
    }
}

extern "C" void bm64_load_barrier(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    int32_t overlay = _arg<0, int32_t>(rdram, ctx);
    bm64::state::eng().overlay = overlay;
    if (bm64::online::active()) {
        bm64::online::note_barrier(overlay);
    }
}

namespace zelda64 {
void quicksave_save() {
    bm64::state::request_quicksave();
}
void quicksave_load() {
    bm64::state::request_quickload();
}
}
