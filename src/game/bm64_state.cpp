#include "bm64_state.h"

#include "bm64_debug.h"
#include "bm64_online.h"
#include "bm64_coop.h"
#include "librecomp/helpers.hpp"
#include "librecomp/overlays.hpp"
#include "recomp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace bm64::state {

constexpr uint32_t kMagic = 0x4E534D42u; // 'BMSN' little-endian
constexpr uint32_t kVersion = 3;
constexpr int kRing = 16;
// Online rollback is clamped to 12 so it never rewinds into a snapshot the ring has overwritten.
// SyncTest refuses a deeper window than that, because a deeper one cannot happen in a session.
constexpr int kSyncDepthMax = 12;
constexpr uint32_t kSoundSlots = 0x800BD770;
constexpr uint32_t kSoundSlotsEnd = 0x800BD818;
constexpr uint32_t kOverlaySlot = 0x80043000;
// One forward step in a SyncTest window: the hash to match, the per-range hashes for the failure
// log, and the pad sample so a live or recording run can feed the same bytes back on the rerun.
struct SyncFrame {
    uint64_t hash = 0;
    std::vector<std::string> names;
    std::vector<uint64_t> ranges;
    uint8_t pads[24]{};
    // Only with BM64_SYNCTEST_DUMP: RDRAM 0x80000000-0x80800000 after the forward step, then
    // kSlotDumpBytes from kSlotDumpBase.
    std::vector<uint8_t> image;
    std::vector<bm64::debug::ProcessStack> stacks;
};

void write_stacks(const std::filesystem::path& path, const std::vector<bm64::debug::ProcessStack>& stacks) {
    if (FILE* f = std::fopen(path.string().c_str(), "w")) {
        for (const auto& s : stacks) {
            std::fprintf(f, "%04X %08X %08X\n", s.process_id, s.top, s.sp);
        }
        std::fclose(f);
    }
}

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
        { 0x8019B0D0, 0x8019C2E0 },
        { 0x8029F5E4, 0x8029F608 },
        { 0x8029F6D8, 0x802A5300 },
        { 0x802A5300, 0x802A5330 },
        { 0x802A5368, 0x802A5380 },
        { 0x802A53BC, 0x802A5550 },
        { 0x802A5554, 0x802A5A60 },
        { 0x802AB200, 0x802AC3E0 },
        { 0x802AC400, 0x802AD120 },
        { 0x802AFC20, 0x802B36D0 },
        { 0x802B36D0, bm64::debug::kHeapEnd },
        // Relocated player array. Leaving it out of the snapshot makes rollback
        // keep a dangling object pointer and fault on the next resim.
        { 0x80F00000, bm64::coop::kStateEnd },
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
    int synctest_depth = 1;
    std::string synctest_dump;
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
    // The ROM range last mapped at the overlay slot. RDRAM restore brings back the code bytes but
    // not librecomp's function map, so a restore across an overlay change has to map it again.
    uint32_t mapped_rom = 0;
    int32_t mapped_ram = 0;
    uint32_t mapped_size = 0;
    uint64_t sim = 0;
    uint8_t* rdram = nullptr;

    Sound sound;
    // Real voices returned by a play function while the step was speculative. Must stay 0.
    uint64_t sound_spec_starts = 0;
    // Starts whose (tick, sound, source, position) already had a virtual id.
    uint64_t sound_ids_reused = 0;
    std::atomic<bool> sound_reported{ false };
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

    std::vector<uint64_t> record_hashes;
    size_t compare_index = 0;
    bool record_saved = false;
    std::vector<SyncFrame> sync_window;
    size_t sync_index = 0;
    int synctest_mismatches = 0;
    int synctest_ticks = 0;
    bool synctest_failed = false;
    std::atomic<bool> synctest_reported{ false };
    int inject_tick = -1;
    uint32_t inject_addr = 0;
    uint8_t inject_value = 0;
    bool inject_done = false;
};

Engine& eng() {
    static Engine e;
    return e;
}

void report_sound_counters() {
    Engine& e = eng();
    if (e.sound_reported.exchange(true)) {
        return;
    }
    std::printf("[bm64 state] Sound counters: speculative starts %llu, virtual ids reused %llu\n",
        static_cast<unsigned long long>(e.sound_spec_starts),
        static_cast<unsigned long long>(e.sound_ids_reused));
    std::fflush(stdout);
}

void report_synctest() {
    Engine& e = eng();
    if (!e.synctest || e.synctest_reported.exchange(true)) {
        return;
    }
    if (e.synctest_failed || e.synctest_ticks <= 0) {
        return;
    }
    std::printf("BM64 SYNCTEST PASS ticks %d depth %d\n", e.synctest_ticks, e.synctest_depth);
    std::fflush(stdout);
}

bool synctest_busy() {
    const Phase phase = eng().phase;
    return phase == Phase::CompareReady || phase == Phase::Comparing;
}

void sound_counters_atexit() {
    report_sound_counters();
    report_synctest();
}

void sound_counter_line(char* buf, size_t cap) {
    if (buf == nullptr || cap == 0) {
        return;
    }
    const Engine& e = eng();
    std::snprintf(buf, cap, "sound speculative starts %llu   virtual ids reused %llu",
        static_cast<unsigned long long>(e.sound_spec_starts),
        static_cast<unsigned long long>(e.sound_ids_reused));
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
    // Registered after Engine is constructed, so this runs before that static is destroyed.
    std::atexit(sound_counters_atexit);
    if (const char* v = std::getenv("BM64_SYNCTEST")) {
        // A leading 0 stays off, including "0" and "08", matching the old on/off check.
        if (v[0] != '\0' && v[0] != '0') {
            e.synctest = true;
            e.synctest_depth = 1;
            bool digits = true;
            for (const char* p = v; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                int n = std::atoi(v);
                if (n > kSyncDepthMax) {
                    std::printf("[bm64 state] BM64_SYNCTEST depth %d clamped to %d\n", n, kSyncDepthMax);
                    n = kSyncDepthMax;
                }
                if (n >= 1) {
                    e.synctest_depth = n;
                }
            }
        }
    }
    if (const char* v = std::getenv("BM64_SYNCTEST_DUMP")) {
        if (e.synctest && v[0] != '\0') {
            e.synctest_dump = v;
            std::printf("[bm64 state] SyncTest failure dumps go to %s\n", v);
        }
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
        e.sync_window.reserve(static_cast<size_t>(e.synctest_depth));
        std::printf("[bm64 state] SyncTest on, rollback depth %d (save, step, load, step, compare)\n", e.synctest_depth);
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
    // func_80266930 opens a 0x40-byte frame and calls into the audio library, so it needs a stack.
    // With sp 0 its first store went outside RDRAM: a rollback that dropped a real sound (the
    // guest's random stick moving a menu cursor) crashed the game (loopback, 2026-11-26). The
    // scratch stack is above the co-op state, below the recomp heap at 0x81000000, and is neither
    // hashed nor snapshotted.
    constexpr uint32_t kHostCallStackTop = 0x80FF0000u;
    ctx.r29 = static_cast<gpr>(static_cast<int32_t>(kHostCallStackTop));
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
    write_u32(out, e.mapped_rom);
    write_u32(out, static_cast<uint32_t>(e.mapped_ram));
    write_u32(out, e.mapped_size);
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
        uint32_t end = s.end;
        if (end == bm64::debug::kHeapEnd) {
            end = bm64::debug::heap_top(rdram);
            write_u32(out, end);
        }
        append_range(s.start, end);
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
    uint32_t mapped_rom = r.u32();
    int32_t mapped_ram = static_cast<int32_t>(r.u32());
    uint32_t mapped_size = r.u32();
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
    // Memory above the snapshot's heap top had never been handed out at that tick, so it was zero.
    const uint32_t live_top = bm64::debug::heap_top(rdram);
    for (const Segment& s : hash_segments_storage()) {
        uint32_t end = s.end;
        if (end == bm64::debug::kHeapEnd) {
            end = r.u32();
            if (!r.ok || end < bm64::debug::kHeapRomEnd || end > bm64::debug::kHeapEnd) {
                std::fprintf(stderr, "[bm64 state] Snapshot heap top rejected\n");
                return false;
            }
            if (live_top > end) {
                std::memset(rdram + (end - 0x80000000u), 0, live_top - end);
            }
        }
        read_range(s.start, end);
    }
    read_range(0x8029F580, 0x8029F5E4);
    read_range(kSoundSlots, kSoundSlotsEnd);
    if (!r.ok) {
        std::fprintf(stderr, "[bm64 state] Snapshot RDRAM truncated\n");
        return false;
    }
    if (mapped_size != 0 && (mapped_rom != e.mapped_rom || mapped_ram != e.mapped_ram || mapped_size != e.mapped_size)) {
        load_overlays(mapped_rom, mapped_ram, mapped_size);
        e.mapped_rom = mapped_rom;
        e.mapped_ram = mapped_ram;
        e.mapped_size = mapped_size;
    }
    restore_sound(sound);
    bm64::debug::set_tick(tick);
    bm64::debug::set_input_cursor(cursor);
    e.overlay = overlay;
    e.have_overlay = true;
    bm64::coop::note_overlay(overlay);
    e.scroll = scroll;
    e.boot_timer = boot_timer;
    e.load_us = micros(t0, std::chrono::steady_clock::now());
    return true;
}

void note_overlay_load(uint32_t rom, int32_t ram, uint32_t size) {
    // Every ROM read comes through here. Only the sub-overlay slot holds recompiled code that changes.
    if (static_cast<uint32_t>(ram) != kOverlaySlot) {
        return;
    }
    Engine& e = eng();
    e.mapped_rom = rom;
    e.mapped_ram = ram;
    e.mapped_size = size;
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
        e.sync_window.emplace_back();
        SyncFrame& frame = e.sync_window.back();
        frame.hash = hash;
        bm64::debug::range_hashes(rdram, frame.names, frame.ranges);
        bm64::debug::copy_pads(frame.pads);
        if (!e.synctest_dump.empty()) {
            frame.image.assign(rdram, rdram + 0x800000);
            const uint8_t* slots = rdram + (bm64::debug::kSlotDumpBase - 0x80000000u);
            frame.image.insert(frame.image.end(), slots, slots + bm64::debug::kSlotDumpBytes);
            bm64::debug::get_process_stacks(frame.stacks);
        }
        // A replay that runs out closes the window early so the tail is compared before exit.
        if (e.sync_window.size() >= static_cast<size_t>(e.synctest_depth) || bm64::debug::input_replay_ended()) {
            e.phase = Phase::CompareReady;
        }
        return;
    }
    if (e.phase == Phase::Comparing) {
        if (e.sync_index >= e.sync_window.size()) {
            e.synctest_failed = true;
            std::printf("BM64 SYNCTEST FAIL window\n");
            std::fflush(stdout);
            std::exit(1);
        }
        e.synctest_ticks++;
        const SyncFrame& want = e.sync_window[e.sync_index];
        if (hash != want.hash) {
            e.synctest_failed = true;
            e.synctest_mismatches++;
            std::vector<std::string> names;
            std::vector<uint64_t> ranges;
            bm64::debug::range_hashes(rdram, names, ranges);
            std::printf("BM64 SYNCTEST FAIL tick %llu %016llx %016llx\n",
                static_cast<unsigned long long>(e.sim),
                static_cast<unsigned long long>(want.hash),
                static_cast<unsigned long long>(hash));
            for (size_t i = 0; i < names.size() && i < ranges.size() && i < want.ranges.size(); i++) {
                if (i >= want.names.size() || names[i] != want.names[i] || ranges[i] != want.ranges[i]) {
                    std::printf("  %s %016llx %016llx\n", names[i].c_str(),
                        static_cast<unsigned long long>(i < want.ranges.size() ? want.ranges[i] : 0),
                        static_cast<unsigned long long>(ranges[i]));
                }
            }
            std::printf("  window frame %zu of %zu, depth %d\n",
                e.sync_index + 1, e.sync_window.size(), e.synctest_depth);
            if (!e.synctest_dump.empty() && !want.image.empty()) {
                const std::filesystem::path dir{ e.synctest_dump };
                const uint64_t first = e.sim - e.sync_index;
                for (size_t i = 0; i <= e.sync_index; i++) {
                    const std::string tick = std::to_string(first + i);
                    bm64::debug::dump_to(e.sync_window[i].image.data(), dir / ("forward_" + tick + ".bin"));
                    bm64::debug::dump_slots_to(e.sync_window[i].image.data() + 0x800000, dir / ("forward_" + tick + ".slots.bin"));
                    write_stacks(dir / ("forward_" + tick + ".stacks"), e.sync_window[i].stacks);
                }
                std::vector<bm64::debug::ProcessStack> stacks;
                bm64::debug::get_process_stacks(stacks);
                bm64::debug::dump_to(rdram, dir / ("rerun_" + std::to_string(e.sim) + ".bin"));
                bm64::debug::dump_slots_to(rdram + (bm64::debug::kSlotDumpBase - 0x80000000u),
                    dir / ("rerun_" + std::to_string(e.sim) + ".slots.bin"));
                write_stacks(dir / ("rerun_" + std::to_string(e.sim) + ".stacks"), stacks);
            }
            std::fflush(stdout);
            std::exit(1);
        }
        e.sync_index++;
        if (e.sync_index >= e.sync_window.size()) {
            e.sync_window.clear();
            e.sync_index = 0;
            e.phase = Phase::Idle;
            e.speculative = false;
        }
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

// Replay reads the file again after the cursor restore. Live and record must not sample the device twice.
void reuse_recorded_pads() {
    Engine& e = eng();
    if (bm64::debug::input_replaying() || e.sync_index >= e.sync_window.size()) {
        return;
    }
    bm64::debug::set_pad_reuse(e.sync_window[e.sync_index].pads);
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
            e.synctest_failed = true;
            std::printf("BM64 SYNCTEST FAIL restore\n");
            std::fflush(stdout);
            std::exit(1);
        }
        e.phase = Phase::Comparing;
        e.sync_index = 0;
        spec = true;
        render = 0;
        reuse_recorded_pads();
    }
    else if (e.phase == Phase::Discarding) {
        // The rest of the window. Still unrendered: a render-thread yield between the two passes
        // can write game memory and fail the compare.
        spec = true;
        render = 0;
        quiet = true;
    }
    else if (e.phase == Phase::Comparing) {
        spec = true;
        render = 0;
        reuse_recorded_pads();
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
        if (bm64::debug::input_replay_ended()) {
            // The open window was already compared. Don't start another on the way out.
            spec = true;
            render = 0;
            quiet = true;
        }
        else {
            e.phase = Phase::Discarding;
            spec = true;
            render = 0;
            quiet = true;
        }
    }
    else if (e.phase == Phase::Record || e.phase == Phase::Compare) {
        spec = true;
        render = 0;
    }

    // BM64_TEST_NORENDER=N: every Nth step runs the way an intermediate online resim does
    // (no frame handed to the render thread, no lookAt write), with nothing restored. The game
    // state must come out the same as a normal replay; a difference is a render-dependent write
    // that would desync a peer that resimulated against one that did not.
    static const int norender_every = [] {
        const char* v = std::getenv("BM64_TEST_NORENDER");
        int n = v ? std::atoi(v) : 0;
        if (n > 0) {
            std::printf("[bm64 state] BM64_TEST_NORENDER: every %d steps unrendered\n", n);
        }
        return n;
    }();
    // BM64_TEST_SPECULATIVE also marks those steps speculative, as an intermediate resim is (no real
    // sound starts), so sound-dependent game state shows up as well.
    static const bool norender_spec = std::getenv("BM64_TEST_SPECULATIVE") != nullptr;
    if (norender_every > 0 && render != 0 && !bm64::online::active() && e.phase == Phase::Idle && !e.synctest &&
        bm64::debug::tick() % static_cast<uint64_t>(norender_every) == static_cast<uint64_t>(norender_every - 1)) {
        render = 0;
        if (norender_spec) {
            spec = true;
        }
    }

    // Only the first step of a window. Later forward steps must keep this snapshot.
    if ((e.phase == Phase::Discarding && e.sync_window.empty()) || (e.phase == Phase::Record && !e.record_saved)) {
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
    // After the snapshot: it depends on this tick's input, and a rollback to this tick restores
    // the state from before it and applies the corrected input again.
    if (bm64::online::active()) {
        for (int player = 0; player < bm64::online::player_count(); player++) {
            if (bm64::online::player_left_this_step(player)) {
                bm64::coop::hand_to_ai(rdram, player);
            }
        }
    }
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
    for (const auto& v : e.sound.voices) {
        if (v.tick == e.sim && v.sound == sound_id && v.source == source &&
            v.x == xbits && v.y == ybits && v.z == zbits) {
            e.sound_ids_reused++;
            break;
        }
    }
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
    if (e.speculative && real_handle >= 0) {
        e.sound_spec_starts++;
    }
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
    // Stage setup (func_80245CDC) runs during this overlay's first tick, before bm64_debug_tick
    // reports the new id, so co-op must learn it here or the first stage after a menu is solo.
    bm64::coop::note_overlay(overlay);
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
