#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "librecomp/helpers.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recomp_ui.h"
#include "bm64_debug.h"

namespace {
    constexpr uint32_t num_ports = 4;
    constexpr uint32_t pad_size = 6; // OSContPad: u16 button, s8 stick_x, s8 stick_y, u8 errno (+ pad)
    constexpr char recording_magic[8] = { 'B', 'M', '6', '4', 'I', 'N', 'P', '\0' };
    constexpr uint32_t recording_version = 1;

    // Recording layout (little-endian):
    //   char magic[8]; u32 version; u32 seed; u32 save_size; u8 save[save_size];
    //   then one 24-byte frame per controller read: 4 x { u16 button; s8 stick_x; s8 stick_y; u8 errno; u8 reserved; }
    constexpr std::streamoff seed_offset = 12;

    struct HashRange {
        std::string name;
        uint32_t start;
        uint32_t end;
    };

    // Ranges that hold game state; see the RDRAM map in docs/game-internals.md.
    std::vector<HashRange> default_ranges() {
        return {
            { "ovl",  0x80043000, 0x80063000 },
            { "bss",  0x80063000, 0x800BEA60 },
            { "main", 0x8029F5E0, 0x802B36D0 },
            { "heap", 0x802B36D0, 0x80400000 },
        };
    }

    enum class InputMode { Live, Record, Replay, ReplayEnded };

    struct DebugState {
        bool booted = false;
        InputMode input_mode = InputMode::Live;
        std::fstream record_file;
        std::vector<uint8_t> replay_frames;
        size_t replay_cursor = 0;
        uint32_t replay_seed = 0;
        bool seed_forced = false;
        uint32_t forced_seed = 0;
        uint32_t seed = 0;

        std::vector<HashRange> ranges = default_ranges();
        FILE* hash_log = nullptr;

        uint64_t tick = 0;
        uint64_t input_frame = 0;
        bool overlay_shown = false;
    } state;

    std::atomic<bool> overlay_toggle_requested = false;

    const char* env(const char* name) {
        const char* v = std::getenv(name);
        return (v && *v) ? v : nullptr;
    }

    gpr addr_reg(uint32_t addr) {
        return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(addr)));
    }

    uint32_t read_u32(uint8_t* rdram, uint32_t addr) {
        return static_cast<uint32_t>(MEM_W(0, addr_reg(addr)));
    }

    float read_f32(uint8_t* rdram, uint32_t addr) {
        return std::bit_cast<float>(read_u32(rdram, addr));
    }

    bool is_rdram_ptr(uint32_t addr) {
        return addr >= 0x80000400 && addr < 0x80800000 && (addr & 3) == 0;
    }

    std::vector<uint8_t> read_file(const std::filesystem::path& path) {
        std::ifstream f{ path, std::ios::binary };
        if (!f.good()) {
            return {};
        }
        return std::vector<uint8_t>{ std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
    }

    template <typename T>
    void write_pod(std::ostream& os, const T& value) {
        os.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    template <typename T>
    bool read_pod(const std::vector<uint8_t>& data, size_t& cursor, T& out) {
        if (cursor + sizeof(T) > data.size()) {
            return false;
        }
        std::memcpy(&out, data.data() + cursor, sizeof(T));
        cursor += sizeof(T);
        return true;
    }

    bool parse_hex(const std::string& text, uint32_t& out) {
        char* end = nullptr;
        unsigned long value = std::strtoul(text.c_str(), &end, 16);
        if (text.empty() || end == nullptr || *end != '\0') {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    }

    void parse_hash_ranges(const char* spec) {
        std::vector<HashRange> ranges;
        std::string s{ spec };
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t dash = item.find('-');
            uint32_t start = 0, end = 0;
            if (dash != std::string::npos && parse_hex(item.substr(0, dash), start) && parse_hex(item.substr(dash + 1), end) &&
                start >= 0x80000000 && end > start && end <= 0x80800000) {
                ranges.push_back({ item, start, end });
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
        if (!ranges.empty()) {
            state.ranges = std::move(ranges);
        }
        else {
            fprintf(stderr, "[bm64 debug] BM64_HASH_RANGES had no valid ranges, using defaults\n");
        }
    }

    void setup_recording(const char* path) {
        std::filesystem::path save_path = ultramodern::get_save_file_path();
        std::vector<uint8_t> save = read_file(save_path);
        state.record_file.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!state.record_file.good()) {
            fprintf(stderr, "[bm64 debug] Could not open recording %s\n", path);
            return;
        }
        state.record_file.write(recording_magic, sizeof(recording_magic));
        write_pod(state.record_file, recording_version);
        write_pod(state.record_file, uint32_t{ 0 }); // seed, filled in by bm64_session_seed
        write_pod(state.record_file, static_cast<uint32_t>(save.size()));
        state.record_file.write(reinterpret_cast<const char*>(save.data()), save.size());
        state.record_file.flush();
        state.input_mode = InputMode::Record;
        printf("[bm64 debug] Recording input to %s (save %s, %zu bytes)\n", path, save_path.string().c_str(), save.size());
    }

    void setup_replay(const char* path) {
        std::vector<uint8_t> data = read_file(path);
        size_t cursor = 0;
        char magic[8];
        uint32_t version = 0, seed = 0, save_size = 0;
        bool ok = data.size() >= sizeof(magic) && std::memcmp(data.data(), recording_magic, sizeof(magic)) == 0;
        cursor = sizeof(magic);
        ok = ok && read_pod(data, cursor, version) && version == recording_version;
        ok = ok && read_pod(data, cursor, seed) && read_pod(data, cursor, save_size);
        ok = ok && cursor + save_size <= data.size();
        if (!ok) {
            fprintf(stderr, "[bm64 debug] %s is not a valid BM64 recording; playing live\n", path);
            return;
        }

        // Replay against a copy of the recorded save so the user's real save is never read or written.
        std::filesystem::path live_save = ultramodern::get_save_file_path();
        std::filesystem::path replay_save = live_save.parent_path() / "replay" / live_save.filename();
        std::filesystem::create_directories(replay_save.parent_path());
        std::filesystem::remove(replay_save);
        if (save_size != 0) {
            std::ofstream out{ replay_save, std::ios::binary };
            out.write(reinterpret_cast<const char*>(data.data() + cursor), save_size);
        }
        ultramodern::change_save_file(u8"replay", recomp::current_game_id());
        cursor += save_size;

        state.replay_seed = seed;
        state.replay_frames.assign(data.begin() + cursor, data.end());
        state.replay_cursor = 0;
        state.input_mode = InputMode::Replay;
        printf("[bm64 debug] Replaying %s: seed %08X, %zu input frames, save copied to %s\n",
            path, seed, state.replay_frames.size() / (num_ports * pad_size), replay_save.string().c_str());
    }

    const char* overlay_name(int32_t id, char* buf, size_t len) {
        constexpr std::array<int, 14> bank_sizes = { 4, 6, 7, 10, 7, 15, 10, 13, 6, 2, 2, 2, 1, 10 };
        if (id == 96) {
            return "main";
        }
        int remaining = id - 1;
        for (size_t bank = 0; bank < bank_sizes.size() && remaining >= 0; bank++) {
            if (remaining < bank_sizes[bank]) {
                snprintf(buf, len, "%zu.%d", bank + 1, remaining + 1);
                return buf;
            }
            remaining -= bank_sizes[bank];
        }
        return "none";
    }

    const char* mode_name() {
        switch (state.input_mode) {
            case InputMode::Record: return "REC";
            case InputMode::Replay: return "REPLAY";
            case InputMode::ReplayEnded: return "REPLAY ENDED";
            default: return "live";
        }
    }

    std::vector<std::string> build_overlay_lines(uint8_t* rdram, int32_t overlay_id, uint64_t total,
                                                 const std::vector<uint64_t>& range_hashes) {
        constexpr uint32_t g_players = 0x800AEDF0;
        constexpr uint32_t player_stride = 0xC8;
        constexpr uint32_t main_player = 0x802AC894;
        constexpr uint32_t process_count = 0x802AC358;
        constexpr uint32_t random_index = 0x802ACFE0;
        constexpr uint32_t camera_eye = 0x8008E648;
        constexpr uint32_t camera_at = 0x8008F3D8;

        std::vector<std::string> lines;
        char buf[256];
        char ovl[16];

        snprintf(buf, sizeof(buf), "tick %llu   input %llu   overlay %s   input mode %s",
            (unsigned long long)state.tick, (unsigned long long)state.input_frame,
            overlay_name(overlay_id, ovl, sizeof(ovl)), mode_name());
        lines.emplace_back(buf);

        std::string hashes = "hash " + std::string(buf, snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)total));
        for (size_t i = 0; i < state.ranges.size() && i < range_hashes.size(); i++) {
            snprintf(buf, sizeof(buf), "   %s %08X", state.ranges[i].name.c_str(), (uint32_t)range_hashes[i]);
            hashes += buf;
        }
        lines.push_back(std::move(hashes));

        snprintf(buf, sizeof(buf), "main player %d   processes %d   rng index %d   seed %08X",
            (int32_t)read_u32(rdram, main_player), (int32_t)read_u32(rdram, process_count),
            (int32_t)read_u32(rdram, random_index), state.seed);
        lines.emplace_back(buf);

        snprintf(buf, sizeof(buf), "camera eye (%.1f, %.1f, %.1f)   at (%.1f, %.1f, %.1f)",
            read_f32(rdram, camera_eye), read_f32(rdram, camera_eye + 4), read_f32(rdram, camera_eye + 8),
            read_f32(rdram, camera_at), read_f32(rdram, camera_at + 4), read_f32(rdram, camera_at + 8));
        lines.emplace_back(buf);

        for (uint32_t i = 0; i < 5; i++) {
            uint32_t p = g_players + i * player_stride;
            uint32_t bomb_type = read_u32(rdram, p + 0x18) & 0xFFFF;
            std::string pos = "-";
            uint32_t obj = read_u32(rdram, p + 0x28);
            if (is_rdram_ptr(obj)) {
                uint32_t object = read_u32(rdram, obj + 0x40);
                if (is_rdram_ptr(object)) {
                    snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)",
                        read_f32(rdram, object + 0x10), read_f32(rdram, object + 0x14), read_f32(rdram, object + 0x18));
                    pos = buf;
                }
            }
            snprintf(buf, sizeof(buf), "P%u  control %d   hearts %d   bombs %d   fire %d   bomb type %X   pos %s",
                i, (int32_t)read_u32(rdram, p + 0x24), (int32_t)read_u32(rdram, p + 0x14),
                (int32_t)read_u32(rdram, p + 0x1C), (int32_t)read_u32(rdram, p + 0x20), bomb_type, pos.c_str());
            lines.emplace_back(buf);
        }
        return lines;
    }
}

void bm64::debug::request_overlay_toggle() {
    overlay_toggle_requested = true;
}

extern "C" void bm64_debug_boot(uint8_t* rdram, recomp_context* ctx) {
    if (state.booted) {
        return;
    }
    state.booted = true;

    if (const char* ranges = env("BM64_HASH_RANGES")) {
        parse_hash_ranges(ranges);
    }
    if (const char* path = env("BM64_HASH_LOG")) {
        state.hash_log = std::fopen(path, "w");
        if (state.hash_log) {
            std::fprintf(state.hash_log, "tick,input_frame,overlay,total");
            for (const HashRange& r : state.ranges) {
                std::fprintf(state.hash_log, ",%s", r.name.c_str());
            }
            std::fprintf(state.hash_log, "\n");
            printf("[bm64 debug] Logging state hashes to %s\n", path);
        }
        else {
            fprintf(stderr, "[bm64 debug] Could not open hash log %s\n", path);
        }
    }
    if (const char* seed = env("BM64_SEED")) {
        state.seed_forced = parse_hex(seed, state.forced_seed);
        if (!state.seed_forced) {
            fprintf(stderr, "[bm64 debug] Ignoring BM64_SEED=%s (expected hex)\n", seed);
        }
    }

    const char* replay = env("BM64_REPLAY");
    const char* record = env("BM64_RECORD");
    if (replay) {
        setup_replay(replay);
    }
    else if (record) {
        setup_recording(record);
    }
}

extern "C" void bm64_session_seed(uint8_t* rdram, recomp_context* ctx) {
    uint32_t seed = _arg<0, uint32_t>(rdram, ctx);
    if (state.input_mode == InputMode::Replay || state.input_mode == InputMode::ReplayEnded) {
        seed = state.replay_seed;
    }
    else if (state.seed_forced) {
        seed = state.forced_seed;
    }
    state.seed = seed;

    if (state.input_mode == InputMode::Record) {
        std::streampos end = state.record_file.tellp();
        state.record_file.seekp(seed_offset);
        write_pod(state.record_file, seed);
        state.record_file.seekp(end);
        state.record_file.flush();
    }
    _return(ctx, seed);
}

extern "C" void bm64_cheats_allowed(uint8_t* rdram, recomp_context* ctx) {
    // Cheats read live input and edit state, so they would break recordings.
    _return(ctx, static_cast<int32_t>(state.input_mode == InputMode::Live));
}

extern "C" void bm64_input_filter(uint8_t* rdram, recomp_context* ctx) {
    gpr pads = ctx->r4;
    state.input_frame++;

    if (state.input_mode == InputMode::Record) {
        for (uint32_t i = 0; i < num_ports; i++) {
            uint16_t button = static_cast<uint16_t>(MEM_HU(i * pad_size + 0, pads));
            int8_t stick_x = MEM_B(i * pad_size + 2, pads);
            int8_t stick_y = MEM_B(i * pad_size + 3, pads);
            uint8_t err = MEM_BU(i * pad_size + 4, pads);
            write_pod(state.record_file, button);
            write_pod(state.record_file, stick_x);
            write_pod(state.record_file, stick_y);
            write_pod(state.record_file, err);
            write_pod(state.record_file, uint8_t{ 0 });
        }
        if ((state.input_frame % 60) == 0) {
            state.record_file.flush();
        }
    }
    else if (state.input_mode == InputMode::Replay) {
        if (state.replay_cursor + num_ports * pad_size > state.replay_frames.size()) {
            state.input_mode = InputMode::ReplayEnded;
            printf("[bm64 debug] Replay ended at input frame %llu; input is live again\n", (unsigned long long)state.input_frame);
            return;
        }
        for (uint32_t i = 0; i < num_ports; i++) {
            const uint8_t* f = state.replay_frames.data() + state.replay_cursor;
            uint16_t button;
            std::memcpy(&button, f, sizeof(button));
            MEM_H(i * pad_size + 0, pads) = static_cast<int16_t>(button);
            MEM_B(i * pad_size + 2, pads) = static_cast<int8_t>(f[2]);
            MEM_B(i * pad_size + 3, pads) = static_cast<int8_t>(f[3]);
            MEM_B(i * pad_size + 4, pads) = static_cast<int8_t>(f[4]);
            state.replay_cursor += pad_size;
        }
    }
}

extern "C" void bm64_debug_tick(uint8_t* rdram, recomp_context* ctx) {
    int32_t overlay_id = _arg<0, int32_t>(rdram, ctx);

    if (overlay_toggle_requested.exchange(false)) {
        state.overlay_shown = !state.overlay_shown;
        recompui::set_debug_overlay_shown(state.overlay_shown);
    }

    bool update_overlay = state.overlay_shown && (state.tick % 4) == 0;
    if (state.hash_log || update_overlay) {
        std::vector<uint64_t> range_hashes;
        range_hashes.reserve(state.ranges.size());
        XXH3_state_t* total = XXH3_createState();
        XXH3_64bits_reset(total);
        for (const HashRange& r : state.ranges) {
            const uint8_t* base = rdram + (r.start - 0x80000000u);
            size_t len = r.end - r.start;
            range_hashes.push_back(XXH3_64bits(base, len));
            XXH3_64bits_update(total, base, len);
        }
        uint64_t total_hash = XXH3_64bits_digest(total);
        XXH3_freeState(total);

        if (state.hash_log) {
            std::fprintf(state.hash_log, "%llu,%llu,%d,%016llX", (unsigned long long)state.tick,
                (unsigned long long)state.input_frame, overlay_id, (unsigned long long)total_hash);
            for (uint64_t h : range_hashes) {
                std::fprintf(state.hash_log, ",%016llX", (unsigned long long)h);
            }
            std::fprintf(state.hash_log, "\n");
            if ((state.tick % 60) == 0) {
                std::fflush(state.hash_log);
            }
        }
        if (update_overlay) {
            recompui::set_debug_overlay_lines(build_overlay_lines(rdram, overlay_id, total_hash, range_hashes));
        }
    }

    state.tick++;
}
