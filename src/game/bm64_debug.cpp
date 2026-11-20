#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <mutex>
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
#include "bm64_session.h"
#include "bm64_state.h"
#include "bm64_online.h"
#include "bm64_coop.h"

namespace {
    constexpr uint32_t num_ports = 4;
    constexpr uint32_t pad_size = 6; // OSContPad: u16 button, s8 stick_x, s8 stick_y, u8 errno (+ pad)
    constexpr char recording_magic[8] = { 'B', 'M', '6', '4', 'I', 'N', 'P', '\0' };
    constexpr uint32_t recording_version = 1;

    // Recording layout (little-endian):
    //   char magic[8]; u32 version; u32 seed; u32 save_size; u8 save[save_size];
    //   then one 24-byte frame per controller read: 4 x { u16 button; s8 stick_x; s8 stick_y; u8 errno; u8 reserved; }
    constexpr std::streamoff seed_offset = 12;

    struct Segment {
        uint32_t start;
        uint32_t end;
    };

    struct HashRange {
        std::string name;
        std::vector<Segment> segments;
    };

    // Ranges that hold game state, minus OS, scheduler, render-thread and audio objects that other threads write.
    // See "State hash ranges" in docs/game-internals.md for what each gap is.
    std::vector<HashRange> default_ranges() {
        return {
            { "ovl",  { { 0x80043000, 0x80063000 } } },
            { "bss",  { { 0x80063000, 0x800BD770 }, { 0x800BD818, 0x800BE848 } } },
            { "files", { { 0x8014B000, 0x8014C8D0 }, { 0x8019B0D0, 0x8019C2E0 } } },
            { "data", { { 0x8029F5E4, 0x8029F608 }, { 0x8029F6D8, 0x802A5300 } } },
            { "gfx",  { { 0x802A5300, 0x802A5330 }, { 0x802A5368, 0x802A5380 }, { 0x802A53BC, 0x802A5550 },
                        { 0x802A5554, 0x802A5A60 } } },
            { "prc",  { { 0x802AB200, 0x802AC3E0 } } },
            { "game", { { 0x802AC400, 0x802AD120 }, { 0x802AFC20, 0x802B36D0 } } },
            { "heap", { { 0x802B36D0, 0x80400000 } } },
            { "slots", { { 0x80F00000, bm64::coop::kStateEnd } } },
        };
    }

    constexpr uint32_t dump_start = 0x80000000;
    constexpr uint32_t dump_end = 0x80800000;

    constexpr uint32_t process_pool = 0x80063370;
    constexpr uint32_t process_size = 0x8A0;
    constexpr uint32_t process_count = 0x50;
    constexpr uint32_t process_stack = 0x28;
    constexpr uint32_t process_stack_size = 0x800;

    enum class InputMode { Live, Record, Replay, ReplayEnded };

    struct DebugState {
        bool booted = false;
        InputMode input_mode = InputMode::Live;
        std::fstream record_file;
        std::vector<uint8_t> replay_frames;
        size_t replay_cursor = 0;
        // Bit per port. Replay forces the status words at 0x802AC400 from this, because those
        // words otherwise follow whichever SDL devices this process opened.
        uint8_t replay_connected = 0x1;
        uint32_t replay_seed = 0;
        uint32_t seed = 0;

        std::vector<HashRange> ranges = default_ranges();
        bool hash_dead_stacks = false;
        FILE* hash_log = nullptr;

        std::vector<uint64_t> dump_ticks;
        std::filesystem::path dump_dir = "dumps";

        bool exit_on_replay_end = false;
        int64_t exit_tick = -1;
        bool exiting = false;

        uint64_t tick = 0;
        uint64_t input_frame = 0;
        bool quiet_log = false;
        bool pad_reuse = false;
        uint8_t reuse_pads[24]{};
        uint8_t last_pads[24]{};
    } state;

    // BM64_DIRECTOR: unattended coverage recordings. Port 0 gets seeded random input, and on a
    // timer a scene request goes into port 0's reserved byte, which replays apply the same way.
    struct Director {
        bool on = false;
        std::vector<uint8_t> scenes;
        uint64_t boot_ticks = 600;
        uint64_t scene_ticks = 3600;
        size_t index = 0;
        uint32_t rng = 0x2545F491u;
        uint32_t hold = 0;
        uint16_t button = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
        // BM64_DIRECTOR_MENU: presses that replace the menu A pulses until the last one.
        struct MenuPress {
            uint64_t tick;
            uint16_t buttons;
            int8_t stick_x;
            int8_t stick_y;
        };
        std::vector<MenuPress> menu;
    } director;
    constexpr uint64_t menu_press_ticks = 3;
    constexpr int32_t battle_setup_overlay = 13;

    constexpr uint32_t level_loader = 0x802AC5D0;
    constexpr uint32_t next_scene = 0x802AC5D4;
    constexpr uint32_t scene_lock = 0x802A01B4;
    constexpr uint32_t battle_settings = 0x802AC660;
    constexpr uint8_t first_arena_scene = 0x90;
    constexpr uint8_t last_arena_scene = 0x99;
    constexpr uint8_t battle_setup_scene = 0x23;
    // Command bytes 0xC0-0xC9: while a Battle arena request is pending, change it to arena 0-9.
    constexpr uint8_t arena_override = 0xC0;
    int32_t last_overlay = 0;

    // Toggled from the input thread, read by the game thread to decide whether to refresh the overlay text.
    std::atomic<bool> overlay_shown = false;
    std::mutex overlay_toggle_mutex;

    void set_overlay_shown(bool shown) {
        std::lock_guard lock{ overlay_toggle_mutex };
        overlay_shown = shown;
        recompui::set_debug_overlay_shown(shown);
    }

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

    void write_u32(uint8_t* rdram, uint32_t addr, uint32_t value) {
        MEM_W(0, addr_reg(addr)) = static_cast<int32_t>(value);
    }

    // The same writes as func_8023AE6C(scene, 0x10, 0), including its refusal while a load is
    // pending or D_802A01B4 is set. A Battle scene also gets its arena index in the settings block,
    // which is where the stage select menu leaves it.
    bool request_scene(uint8_t* rdram, uint8_t scene) {
        uint32_t loader = read_u32(rdram, level_loader);
        if (scene >= arena_override && scene <= arena_override + (last_arena_scene - first_arena_scene)) {
            uint32_t pending = read_u32(rdram, next_scene);
            if ((loader & 1) == 0 || pending < first_arena_scene || pending > last_arena_scene) {
                return false;
            }
            uint32_t arena = first_arena_scene + (scene - arena_override);
            write_u32(rdram, battle_settings + 0xA0, arena - first_arena_scene);
            write_u32(rdram, next_scene, arena);
            write_u32(rdram, 0x802AC5DC, arena);
            return true;
        }
        if ((loader & 1) != 0 || read_u32(rdram, scene_lock) != 0) {
            return false;
        }
        if (scene >= first_arena_scene && scene <= last_arena_scene) {
            write_u32(rdram, battle_settings + 0xA0, static_cast<uint32_t>(scene - first_arena_scene));
        }
        write_u32(rdram, next_scene, scene);
        write_u32(rdram, 0x802AC5DC, scene);
        // The game enters 2E only from 2D, with entry 0x12. Entry 0x10 there reads a player object
        // that does not exist yet (func_80250338).
        const uint32_t entry = scene == 0x2E ? 0x12 : 0x10;
        write_u32(rdram, 0x802AC5D8, entry);
        write_u32(rdram, 0x802AC5E0, entry);
        write_u32(rdram, level_loader, loader | 1);
        write_u32(rdram, 0x802AC5E4, 0);
        return true;
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

    std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t next = s.find(sep, pos);
            if (next == std::string::npos) {
                next = s.size();
            }
            if (next > pos) {
                out.push_back(s.substr(pos, next - pos));
            }
            pos = next + 1;
        }
        return out;
    }

    uint32_t director_next() {
        uint32_t x = director.rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        director.rng = x;
        return x;
    }

    void setup_director(const char* spec) {
        for (const std::string& item : split(spec, ',')) {
            uint32_t scene = 0;
            if (parse_hex(item, scene) && scene > 0 && scene <= 0xFF) {
                director.scenes.push_back(static_cast<uint8_t>(scene));
            }
            else {
                fprintf(stderr, "[bm64 director] Ignoring scene %s (expected hex 01-FF)\n", item.c_str());
            }
        }
        if (const char* secs = env("BM64_DIRECTOR_SECS")) {
            director.scene_ticks = std::max<uint64_t>(60, std::strtoull(secs, nullptr, 10) * 60);
        }
        if (const char* boot = env("BM64_DIRECTOR_BOOT")) {
            director.boot_ticks = std::strtoull(boot, nullptr, 10);
        }
        if (const char* seed = env("BM64_DIRECTOR_SEED")) {
            uint32_t parsed = 0;
            if (parse_hex(seed, parsed) && parsed != 0) {
                director.rng = parsed;
            }
        }
        if (const char* menu = env("BM64_DIRECTOR_MENU")) {
            for (const std::string& item : split(menu, ',')) {
                std::vector<std::string> parts = split(item, ':');
                uint32_t buttons = 0;
                if ((parts.size() == 2 || parts.size() == 4) && parse_hex(parts[1], buttons) && buttons <= 0xFFFF) {
                    Director::MenuPress press{ std::strtoull(parts[0].c_str(), nullptr, 10), static_cast<uint16_t>(buttons), 0, 0 };
                    if (parts.size() == 4) {
                        press.stick_x = static_cast<int8_t>(std::clamp(std::atoi(parts[2].c_str()), -80, 80));
                        press.stick_y = static_cast<int8_t>(std::clamp(std::atoi(parts[3].c_str()), -80, 80));
                    }
                    director.menu.push_back(press);
                }
                else {
                    fprintf(stderr, "[bm64 director] Ignoring menu press %s (expected tick:hexbuttons[:x:y])\n", item.c_str());
                }
            }
            std::sort(director.menu.begin(), director.menu.end(),
                [](const Director::MenuPress& a, const Director::MenuPress& b) { return a.tick < b.tick; });
            printf("[bm64 director] %zu scripted menu presses; arenas wait for Battle setup instead of jumping\n", director.menu.size());
        }
        director.on = !director.scenes.empty();
        if (director.on) {
            printf("[bm64 director] %zu scenes, %llu ticks each after %llu boot ticks, seed %08X\n",
                director.scenes.size(), (unsigned long long)director.scene_ticks,
                (unsigned long long)director.boot_ticks, director.rng);
        }
    }

    // Fills port 0 and returns the scene command for this frame (0 for none).
    uint8_t director_frame(uint8_t* rdram, gpr pads) {
        if (director.hold == 0) {
            director.hold = 4 + director_next() % 17;
            director.stick_x = static_cast<int8_t>(static_cast<int>(director_next() % 161) - 80);
            director.stick_y = static_cast<int8_t>(static_cast<int>(director_next() % 161) - 80);
            director.button = 0;
            if (director_next() % 4 == 0) {
                director.button |= 0x8000;
            }
            if (director_next() % 8 == 0) {
                director.button |= 0x4000;
            }
        }
        director.hold--;
        // Banks 1-3 are boot and menus: pulse A with the stick centred, so B never backs out.
        bool menu = last_overlay >= 1 && last_overlay <= 17;
        uint16_t button = menu ? ((state.tick % 30) < 2 ? 0x8000 : 0) : director.button;
        int8_t stick_x = menu ? 0 : director.stick_x;
        int8_t stick_y = menu ? 0 : director.stick_y;
        bool scripted = !director.menu.empty() && state.tick <= director.menu.back().tick + menu_press_ticks;
        if (scripted) {
            button = 0;
            stick_x = 0;
            stick_y = 0;
            for (const Director::MenuPress& press : director.menu) {
                if (state.tick >= press.tick && state.tick < press.tick + menu_press_ticks) {
                    button = press.buttons;
                    stick_x = press.stick_x;
                    stick_y = press.stick_y;
                }
            }
        }
        MEM_H(0, pads) = static_cast<int16_t>(button);
        MEM_B(2, pads) = stick_x;
        MEM_B(3, pads) = stick_y;
        MEM_B(4, pads) = 0;
        for (uint32_t port = 1; port < num_ports; port++) {
            MEM_H(port * pad_size + 0, pads) = 0;
            MEM_B(port * pad_size + 2, pads) = 0;
            MEM_B(port * pad_size + 3, pads) = 0;
        }

        if (state.tick < director.boot_ticks) {
            return 0;
        }
        uint64_t due = director.boot_ticks + director.index * director.scene_ticks;
        if (director.index < director.scenes.size() && state.tick >= due) {
            uint8_t scene = director.scenes[director.index];
            bool arena = scene >= first_arena_scene && scene <= last_arena_scene;
            uint32_t loader = read_u32(rdram, level_loader);
            if (arena && !director.menu.empty()) {
                if (!scripted && last_overlay == battle_setup_overlay) {
                    printf("[bm64 director] Tick %llu: scene %02X (reached Battle setup through the menus)\n",
                        (unsigned long long)state.tick, scene);
                    director.index++;
                }
            }
            else if ((loader & 1) == 0 && read_u32(rdram, scene_lock) == 0) {
                printf("[bm64 director] Tick %llu: scene %02X%s\n", (unsigned long long)state.tick, scene,
                    arena ? " (through battle setup)" : "");
                director.index++;
                return arena ? battle_setup_scene : scene;
            }
        }
        if (director.index > 0) {
            uint8_t current = director.scenes[director.index - 1];
            if (current >= first_arena_scene && current <= last_arena_scene) {
                uint32_t pending = read_u32(rdram, next_scene);
                if ((read_u32(rdram, level_loader) & 1) != 0 && pending >= first_arena_scene &&
                    pending <= last_arena_scene && pending != current) {
                    printf("[bm64 director] Tick %llu: battle setup chose %02X, overriding with %02X\n",
                        (unsigned long long)state.tick, pending, current);
                    return static_cast<uint8_t>(arena_override + (current - first_arena_scene));
                }
            }
        }
        return 0;
    }

    // "[name:]start-end[+start-end...]", comma-separated.
    void parse_hash_ranges(const char* spec) {
        std::vector<HashRange> ranges;
        for (const std::string& item : split(spec, ',')) {
            size_t colon = item.find(':');
            HashRange range{ colon == std::string::npos ? item : item.substr(0, colon), {} };
            bool ok = true;
            for (const std::string& seg : split(colon == std::string::npos ? item : item.substr(colon + 1), '+')) {
                size_t dash = seg.find('-');
                uint32_t start = 0, end = 0;
                ok = ok && dash != std::string::npos && parse_hex(seg.substr(0, dash), start) &&
                    parse_hex(seg.substr(dash + 1), end) && start >= 0x80000000 && end > start && end <= 0x80800000;
                range.segments.push_back({ start, end });
            }
            if (ok && !range.segments.empty()) {
                ranges.push_back(std::move(range));
            }
            else {
                fprintf(stderr, "[bm64 debug] Ignoring hash range %s\n", item.c_str());
            }
        }
        if (!ranges.empty()) {
            state.ranges = std::move(ranges);
        }
        else {
            fprintf(stderr, "[bm64 debug] BM64_HASH_RANGES had no valid ranges, using defaults\n");
        }
    }

    void parse_dump_ticks(const char* spec) {
        for (const std::string& item : split(spec, ',')) {
            char* end = nullptr;
            unsigned long long tick = std::strtoull(item.c_str(), &end, 10);
            if (end != nullptr && *end == '\0') {
                state.dump_ticks.push_back(tick);
            }
            else {
                fprintf(stderr, "[bm64 debug] Ignoring dump tick %s\n", item.c_str());
            }
        }
    }

    // Process stack memory below a suspended process's sp holds stale values (sometimes audio-heap pointers left by
    // sound calls) that never affect the game. The 16 bytes at sp are the o32 argument home: a callee may spill
    // a0-a3 there, and a yield inside that call leaves the same kind of scratch (new.bm64rec tick 8448, process
    // func_8023F26C, word at sp was 0 on one pass and 0xB on the other). Unused inline stacks (free pool entries,
    // or processes that were given their own stack) are dead entirely. Returns sorted, non-overlapping ranges.
    std::vector<Segment> dead_stacks(uint8_t* rdram) {
        std::vector<bm64::debug::ProcessStack> live;
        bm64::debug::get_process_stacks(live);
        std::vector<Segment> dead;
        for (uint32_t i = 0; i < process_count; i++) {
            uint32_t p = process_pool + i * process_size;
            uint32_t inline_lo = p + process_stack;
            uint32_t inline_hi = inline_lo + process_stack_size;
            uint32_t id = static_cast<uint16_t>(MEM_HU(0, addr_reg(p)));
            uint32_t size = read_u32(rdram, p + 0x20);
            uint32_t sptop = read_u32(rdram, p + 0x24);
            const bm64::debug::ProcessStack* s = nullptr;
            for (const auto& l : live) {
                if (l.process_id == id && l.top == sptop + size) {
                    s = &l;
                }
            }
            // Dead up through the 16-byte o32 argument home. Offsets at and above sp+0x10 are the frame proper.
            auto live_sp = [&](uint32_t lo, uint32_t hi) {
                if (s->sp > lo && s->sp <= hi) {
                    uint32_t above = s->sp + 0x10;
                    return above < hi ? above : hi;
                }
                return hi;
            };
            if (s != nullptr && sptop == inline_lo) {
                dead.push_back({ inline_lo, live_sp(inline_lo, inline_hi) });
                continue;
            }
            dead.push_back({ inline_lo, inline_hi });
            if (s != nullptr && is_rdram_ptr(sptop)) {
                dead.push_back({ sptop, live_sp(sptop, s->top) });
            }
        }
        std::sort(dead.begin(), dead.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
        std::vector<Segment> merged;
        for (const Segment& d : dead) {
            if (d.end <= d.start) {
                continue;
            }
            if (!merged.empty() && d.start <= merged.back().end) {
                merged.back().end = std::max(merged.back().end, d.end);
            }
            else {
                merged.push_back(d);
            }
        }
        return merged;
    }

    // Feeds [seg.start, seg.end) minus the masked ranges to both hash states.
    void hash_segment(uint8_t* rdram, const Segment& seg, const std::vector<Segment>& mask,
                      XXH3_state_t* range, XXH3_state_t* total) {
        std::vector<Segment> skips = mask;
        if (bm64::coop::omit_render_camera()) {
            // Per-peer lookAt output. The camera block itself stays shared.
            skips.push_back({ 0x802A5418, 0x802A5418 + 64 });
            skips.push_back({ 0x800BD5C8, 0x800BD5C8 + 0x40 });
            skips.push_back({ 0x800BD638, 0x800BD638 + 0x40 });
            std::sort(skips.begin(), skips.end(), [](const Segment& a, const Segment& b) {
                return a.start < b.start;
            });
        }
        const std::vector<Segment>& used = skips;
        uint32_t pos = seg.start;
        auto feed = [&](uint32_t from, uint32_t to) {
            if (to > from) {
                XXH3_64bits_update(range, rdram + (from - 0x80000000u), to - from);
                XXH3_64bits_update(total, rdram + (from - 0x80000000u), to - from);
            }
        };
        for (const Segment& m : used) {
            if (m.end <= pos || m.start >= seg.end) {
                continue;
            }
            feed(pos, std::min(m.start, seg.end));
            pos = std::max(pos, m.end);
        }
        feed(pos, seg.end);
    }

    // Writes RDRAM in N64 (big-endian) byte order; rdram holds each 32-bit word in host order.
    // The dead-stack ranges go to tick_<n>.mask for tools/re/dumpdiff.py.
    void dump_rdram(uint8_t* rdram, const std::vector<Segment>& mask) {
        std::filesystem::create_directories(state.dump_dir);
        std::filesystem::path path = state.dump_dir / ("tick_" + std::to_string(state.tick) + ".bin");
        std::vector<uint32_t> words((dump_end - dump_start) / 4);
        std::memcpy(words.data(), rdram + (dump_start - 0x80000000u), words.size() * 4);
        for (uint32_t& w : words) {
            w = (w >> 24) | ((w >> 8) & 0xFF00) | ((w << 8) & 0xFF0000) | (w << 24);
        }
        std::ofstream out{ path, std::ios::binary };
        out.write(reinterpret_cast<const char*>(words.data()), words.size() * 4);
        std::ofstream mask_out{ state.dump_dir / ("tick_" + std::to_string(state.tick) + ".mask") };
        for (const Segment& m : mask) {
            char line[32];
            snprintf(line, sizeof(line), "%08X %08X\n", m.start, m.end);
            mask_out << line;
        }
        printf("[bm64 debug] Dumped RDRAM at tick %llu to %s\n", (unsigned long long)state.tick, path.string().c_str());
    }

    void exit_game(const char* reason) {
        bm64::session::export_if_requested();
        state.exiting = true;
        if (state.hash_log) {
            std::fclose(state.hash_log);
            state.hash_log = nullptr;
        }
        if (state.record_file.is_open()) {
            state.record_file.close();
        }
        bm64::state::report_sound_counters();
        bm64::state::report_synctest();
        printf("[bm64 debug] Exiting at tick %llu: %s\n", (unsigned long long)state.tick, reason);
        fflush(stdout);
        ultramodern::quit();
    }

    void setup_recording(const char* path) {
        std::filesystem::path save_path = ultramodern::get_save_file_path();
        // The image the game is about to read, which is the sandbox image when one is installed.
        std::vector<uint8_t> save(ultramodern::save_image_size());
        if (!save.empty()) {
            ultramodern::export_save_image(save.data(), save.size());
        }
        std::filesystem::path record_path(path);
        std::error_code ec;
        if (record_path.has_parent_path()) {
            std::filesystem::create_directories(record_path.parent_path(), ec);
        }
        state.record_file.open(record_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!state.record_file.good()) {
            fprintf(stderr, "[bm64 debug] Could not open recording %s\n", record_path.string().c_str());
            ultramodern::error_handling::message_box(
                ("Could not open the recording file:\n" + record_path.string() + "\n\nThe game is running, but this session is not being recorded.").c_str());
            return;
        }
        state.record_file.write(recording_magic, sizeof(recording_magic));
        write_pod(state.record_file, recording_version);
        write_pod(state.record_file, uint32_t{ 0 }); // seed, filled in by bm64_session_seed
        write_pod(state.record_file, static_cast<uint32_t>(save.size()));
        state.record_file.write(reinterpret_cast<const char*>(save.data()), save.size());
        state.record_file.flush();
        state.input_mode = InputMode::Record;
        printf("[bm64 debug] Recording input to %s (save image %zu bytes, file %s)\n",
            path, save.size(), save_path.string().c_str());
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

        if (!bm64::session::begin_sandbox(data.data() + cursor, save_size)) {
            fprintf(stderr, "[bm64 debug] %s save image was not installed; playing live\n", path);
            return;
        }
        cursor += save_size;

        state.replay_seed = seed;
        state.replay_frames.assign(data.begin() + cursor, data.end());
        state.replay_cursor = 0;
        state.replay_connected = 0x1;
        size_t frames = state.replay_frames.size() / (num_ports * pad_size);
        for (size_t frame = 0; frame < frames; frame++) {
            for (uint32_t port = 1; port < num_ports; port++) {
                const uint8_t* sample = state.replay_frames.data() + (frame * num_ports + port) * pad_size;
                uint16_t button = 0;
                std::memcpy(&button, sample, sizeof(button));
                if (button != 0 || sample[2] != 0 || sample[3] != 0) {
                    state.replay_connected = static_cast<uint8_t>(state.replay_connected | (1u << port));
                }
            }
        }
        state.input_mode = InputMode::Replay;
        printf("[bm64 debug] Replaying %s: seed %08X, %zu input frames, save sandboxed\n",
            path, seed, state.replay_frames.size() / (num_ports * pad_size));
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
        constexpr uint32_t g_players = bm64::coop::kPlayerBase;
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
            if (i == 4) {
                lines.push_back(std::move(hashes));
                hashes = "    ";
            }
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

        bm64::state::sound_counter_line(buf, sizeof(buf));
        lines.emplace_back(buf);

        char net[160];
        bm64::online::status_line(net, sizeof(net));
        if (net[0] != '\0') {
            lines.emplace_back(net);
        }

        if (bm64::coop::active()) {
            int me = bm64::coop::local_player();
            if (me < 0 || me >= bm64::coop::kPlayerSlots) {
                me = 0;
            }
            uint32_t mine = g_players + static_cast<uint32_t>(me) * player_stride;
            snprintf(buf, sizeof(buf), "you P%d   hearts %d   bombs %d   fire %d   lives %d   gems %d",
                me, (int)read_u32(rdram, mine + 0x14), (int)read_u32(rdram, mine + 0x1C),
                (int)read_u32(rdram, mine + 0x20), (int)read_u32(rdram, 0x802AC624),
                (int)read_u32(rdram, 0x802AC610 + 0x1C));
            lines.emplace_back(buf);
        }

        uint32_t shown = bm64::coop::active() ? static_cast<uint32_t>(bm64::coop::kPlayerSlots) : 5;
        for (uint32_t i = 0; i < shown; i++) {
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

void bm64::debug::toggle_overlay() {
    set_overlay_shown(!overlay_shown);
}

bool bm64::debug::autostart_requested() {
    return env("BM64_AUTOSTART") != nullptr;
}

std::string bm64::debug::environment_error() {
    if (env("BM64_RECORD") && env("BM64_REPLAY")) {
        return std::string{ "Both BM64_RECORD and BM64_REPLAY are set, so it's unclear whether to record or replay.\n\n" }
            + "BM64_RECORD=" + env("BM64_RECORD") + "\nBM64_REPLAY=" + env("BM64_REPLAY") + "\n\n"
            + "Clear the one you don't want and start the game again, for example in PowerShell:\n"
            + "    Remove-Item Env:BM64_REPLAY\n"
            + "or in cmd:\n"
            + "    set BM64_REPLAY=";
    }
    return {};
}

extern "C" void bm64_debug_boot(uint8_t* rdram, recomp_context* ctx) {
    if (state.booted) {
        return;
    }
    state.booted = true;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (bm64::coop::kPlayerBase >= 0x80800000u) {
        uint8_t* slots = rdram + (bm64::coop::kPlayerBase - 0x80000000u);
        std::memset(slots, 0, bm64::coop::kStateEnd - bm64::coop::kPlayerBase);
        // func_80243BF0 sets field 0x18 to 0x200 on the original slots. The extra slots get the same value.
        for (int i = 0; i < bm64::coop::kPlayerSlots; i++) {
            uint32_t addr = bm64::coop::kPlayerBase + static_cast<uint32_t>(i) * bm64::coop::kPlayerStride + 0x18;
            *reinterpret_cast<uint32_t*>(rdram + (addr - 0x80000000u)) = 0x200;
        }
        std::memcpy(rdram + (bm64::coop::kHudBase - 0x80000000u),
            rdram + (bm64::coop::kHudOld - 0x80000000u), bm64::coop::kHudCopy);
        for (uint32_t i = 0; i < bm64::coop::kProcessSlots; i++) {
            rdram[(bm64::coop::kStickyBase - 0x80000000u) + i * 2] = 0xFF;
        }
        for (int c = 0; c < bm64::coop::kCategories; c++) {
            rdram[(bm64::coop::kOverrideBase - 0x80000000u) + static_cast<uint32_t>(c)] = 0xFF;
        }
        std::printf("[bm64 coop] Player array at %08X (%d slots, movement at %08X)\n",
            bm64::coop::kPlayerBase, bm64::coop::kPlayerSlots, bm64::coop::kMoveBase);
    }

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
    if (env("BM64_DEBUG_OVERLAY")) {
        set_overlay_shown(true);
    }
    state.hash_dead_stacks = env("BM64_HASH_DEAD_STACKS") != nullptr;
    if (const char* ticks = env("BM64_DUMP_TICKS")) {
        parse_dump_ticks(ticks);
    }
    if (const char* dir = env("BM64_DUMP_DIR")) {
        state.dump_dir = dir;
    }
    state.exit_on_replay_end = env("BM64_EXIT_ON_REPLAY_END") != nullptr;
    if (const char* tick = env("BM64_EXIT_TICK")) {
        state.exit_tick = std::strtoll(tick, nullptr, 10);
    }
    if (env("BM64_DISABLE_CHEATS")) {
        bm64::debug::set_cheats_disabled(true);
        printf("[bm64 debug] Cheats disabled (BM64_DISABLE_CHEATS)\n");
    }
    if (const char* seed = env("BM64_SEED")) {
        uint32_t parsed = 0;
        if (parse_hex(seed, parsed)) {
            bm64::session::set_seed(parsed);
        }
        else {
            fprintf(stderr, "[bm64 debug] Ignoring BM64_SEED=%s (expected hex)\n", seed);
        }
    }
    if (const char* bias = env("BM64_CAMERA_BIAS")) {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (std::sscanf(bias, "%f,%f,%f", &x, &y, &z) == 3) {
            bm64::session::set_camera_bias(x, y, z);
            printf("[bm64 session] Rendered camera eye bias %g, %g, %g\n", x, y, z);
        }
        else {
            fprintf(stderr, "[bm64 debug] Ignoring BM64_CAMERA_BIAS=%s (expected x,y,z)\n", bias);
        }
    }
    if (const char* exp = env("BM64_SANDBOX_EXPORT")) {
        bm64::session::set_export_path(exp);
    }

    const char* replay = env("BM64_REPLAY");
    const char* record = env("BM64_RECORD");
    if (replay) {
        setup_replay(replay);
    }
    else {
        if (const char* image = env("BM64_SANDBOX_IMAGE")) {
            std::vector<uint8_t> bytes = read_file(image);
            if (bytes.empty()) {
                fprintf(stderr, "[bm64 session] Could not read sandbox image %s\n", image);
                bm64::session::begin_sandbox(nullptr, 0);
            }
            else {
                bm64::session::begin_sandbox(bytes.data(), bytes.size());
            }
        }
        else if (env("BM64_SANDBOX")) {
            bm64::session::begin_sandbox(nullptr, 0);
        }
        if (record) {
            setup_recording(record);
        }
        if (const char* scenes = env("BM64_DIRECTOR")) {
            if (state.input_mode == InputMode::Record) {
                setup_director(scenes);
            }
            else {
                fprintf(stderr, "[bm64 director] BM64_DIRECTOR needs BM64_RECORD; ignored\n");
            }
        }
    }
    bm64::online::handshake();
}

extern "C" void bm64_session_seed(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    uint32_t seed;
    if (state.input_mode == InputMode::Replay || state.input_mode == InputMode::ReplayEnded) {
        seed = state.replay_seed;
        printf("[bm64 session] RNG seed %08X (replay)\n", seed);
    }
    else {
        seed = bm64::session::resolve_seed();
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

namespace {
    std::atomic_bool cheats_disabled{ false };

    // Same layout as a recording frame: button in host order, then stick x, stick y, errno, reserved.
    void read_pad(uint8_t* rdram, gpr pads, uint32_t port, uint8_t out[6]) {
        uint16_t button = static_cast<uint16_t>(MEM_HU(port * pad_size + 0, pads));
        std::memcpy(out, &button, sizeof(button));
        out[2] = MEM_BU(port * pad_size + 2, pads);
        out[3] = MEM_BU(port * pad_size + 3, pads);
        out[4] = MEM_BU(port * pad_size + 4, pads);
        out[5] = 0;
    }

    enum class NetSample { Hardware, Zero, Script, ScriptButtons };

    NetSample net_sample_mode() {
        static int mode = -1;
        if (mode < 0) {
            const char* v = std::getenv("BM64_NET_SAMPLE");
            NetSample m = NetSample::Hardware;
            if (v != nullptr && std::strcmp(v, "zero") == 0) {
                m = NetSample::Zero;
            }
            else if (v != nullptr && std::strcmp(v, "script") == 0) {
                m = NetSample::Script;
            }
            else if (v != nullptr && std::strcmp(v, "script-buttons") == 0) {
                m = NetSample::ScriptButtons;
            }
            mode = static_cast<int>(m);
        }
        return static_cast<NetSample>(mode);
    }

    // Local input, not game state: it advances only when the session accepts a sample, so a
    // resimulation never consumes it.
    struct ScriptPad {
        bool seeded = false;
        uint32_t rng = 0;
        int hold = 0;
        uint16_t button = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
    } script_pad;

    uint32_t script_next() {
        uint32_t x = script_pad.rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        script_pad.rng = x;
        return x;
    }

    void script_sample(bool buttons, uint8_t out[6]) {
        if (!script_pad.seeded) {
            uint32_t player = static_cast<uint32_t>(bm64::online::local_player());
            script_pad.rng = 0x9E3779B9u ^ (0x85EBCA6Bu * (player + 1));
            script_pad.seeded = true;
        }
        if (script_pad.hold == 0) {
            script_pad.hold = 4 + static_cast<int>(script_next() % 17);
            script_pad.stick_x = static_cast<int8_t>(static_cast<int>(script_next() % 161) - 80);
            script_pad.stick_y = static_cast<int8_t>(static_cast<int>(script_next() % 161) - 80);
            script_pad.button = 0;
            if (buttons) {
                // Never Start (0x1000): it would pause.
                if (script_next() % 4 == 0) {
                    script_pad.button |= 0x8000;
                }
                if (script_next() % 8 == 0) {
                    script_pad.button |= 0x4000;
                }
            }
        }
        script_pad.hold--;
        std::memcpy(out, &script_pad.button, sizeof(script_pad.button));
        out[2] = static_cast<uint8_t>(script_pad.stick_x);
        out[3] = static_cast<uint8_t>(script_pad.stick_y);
        out[4] = 0;
        out[5] = 0;
    }
}

void bm64::debug::set_cheats_disabled(bool disabled) {
    cheats_disabled.store(disabled);
}

extern "C" void bm64_cheats_allowed(uint8_t* rdram, recomp_context* ctx) {
    // Cheats edit simulation state from player-1 input. Recordings, replays and netplay leave them off.
    bool allowed = state.input_mode == InputMode::Live && !cheats_disabled.load();
    _return(ctx, static_cast<int32_t>(allowed));
}

extern "C" void bm64_input_filter(uint8_t* rdram, recomp_context* ctx) {
    gpr pads = ctx->r4;
    state.input_frame++;

    // Controller status sits in the 16 bytes before the pad samples. Each process otherwise
    // reports whichever SDL devices it managed to open, so peers disagree as soon as a port differs.
    if (bm64::online::active()) {
        int players = bm64::online::player_count();
        for (int port = 0; port < 4; port++) {
            uint8_t* status = rdram + (0x802AC400u - 0x80000000u) + static_cast<uint32_t>(port) * 4u;
            if (port < players) {
                status[0] = 0x00;
                status[1] = 0x05;
                status[2] = 0x01;
                status[3] = 0x00;
            }
            else {
                status[0] = 0x00;
                status[1] = 0x00;
                status[2] = 0x00;
                status[3] = 0x08;
            }
        }
    }

    auto remember = [&]() {
        // osContGetQuery fills 0x802AC400 from the devices this process opened. A replay has
        // no record of that, so derive it from which ports the file actually uses.
        if (state.input_mode == InputMode::Replay || (state.input_mode == InputMode::Record && director.on)) {
            // A director recording uses port 0 only, which is what its replay derives.
            uint8_t connected = state.input_mode == InputMode::Replay ? state.replay_connected : uint8_t{ 0x1 };
            for (uint32_t port = 0; port < num_ports; port++) {
                uint8_t* status = rdram + (0x802AC400u - 0x80000000u) + port * 4u;
                bool present = (connected & (1u << port)) != 0;
                status[0] = 0x00;
                status[1] = present ? 0x05 : 0x00;
                status[2] = present ? 0x01 : 0x00;
                status[3] = present ? 0x00 : 0x08;
            }
        }
        for (uint32_t port = 0; port < num_ports; port++) {
            read_pad(rdram, pads, port, state.last_pads + port * pad_size);
        }
    };
    auto write_pads = [&](const uint8_t* bytes) {
        for (uint32_t i = 0; i < num_ports; i++) {
            const uint8_t* f = bytes + i * pad_size;
            uint16_t button;
            std::memcpy(&button, f, sizeof(button));
            MEM_H(i * pad_size + 0, pads) = static_cast<int16_t>(button);
            MEM_B(i * pad_size + 2, pads) = static_cast<int8_t>(f[2]);
            MEM_B(i * pad_size + 3, pads) = static_cast<int8_t>(f[3]);
            MEM_B(i * pad_size + 4, pads) = static_cast<int8_t>(f[4]);
        }
        remember();
    };

    // Byte 5 of port 0 is padding in the game's pad struct. The remembered copy keeps the director's
    // scene command there, so a SyncTest rerun applies it on the same step as the first pass.
    if (state.pad_reuse) {
        state.pad_reuse = false;
        const uint8_t command = state.reuse_pads[5];
        write_pads(state.reuse_pads);
        if (command != 0) {
            request_scene(rdram, command);
        }
        state.last_pads[5] = command;
        return;
    }

    uint8_t injected[24];
    if (bm64_state_consume_inject(injected) || (bm64::online::active() && bm64::online::pads_for_step(injected))) {
        if (bm64::online::active() && bm64::online::accepting_sample()) {
            uint8_t local[6];
            // Two local instances otherwise sample the same physical stick twice and disagree.
            // Scripted modes leave player 0 on the hardware and generate every other player's pad.
            NetSample mode = net_sample_mode();
            bool scripted = (mode == NetSample::Script || mode == NetSample::ScriptButtons) && bm64::online::local_player() != 0;
            if (mode == NetSample::Zero) {
                std::memset(local, 0, sizeof(local));
            }
            else if (scripted) {
                script_sample(mode == NetSample::ScriptButtons, local);
            }
            else {
                read_pad(rdram, pads, 0, local);
            }
            bm64::online::push_local_pad(local);
        }
        write_pads(injected);
        return;
    }

    uint8_t command = 0;
    if (state.input_mode == InputMode::Record) {
        if (director.on) {
            command = director_frame(rdram, pads);
            if (command != 0) {
                request_scene(rdram, command);
            }
        }
        for (uint32_t i = 0; i < num_ports; i++) {
            uint16_t button = static_cast<uint16_t>(MEM_HU(i * pad_size + 0, pads));
            int8_t stick_x = MEM_B(i * pad_size + 2, pads);
            int8_t stick_y = MEM_B(i * pad_size + 3, pads);
            uint8_t err = MEM_BU(i * pad_size + 4, pads);
            write_pod(state.record_file, button);
            write_pod(state.record_file, stick_x);
            write_pod(state.record_file, stick_y);
            write_pod(state.record_file, err);
            write_pod(state.record_file, i == 0 ? command : uint8_t{ 0 });
        }
        if ((state.input_frame % 60) == 0) {
            state.record_file.flush();
        }
    }
    else if (state.input_mode == InputMode::Replay) {
        if (state.replay_cursor + num_ports * pad_size > state.replay_frames.size()) {
            state.input_mode = InputMode::ReplayEnded;
            printf("[bm64 debug] Replay ended at input frame %llu; input is live again\n", (unsigned long long)state.input_frame);
            remember();
            return;
        }
        command = state.replay_frames[state.replay_cursor + 5];
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
        if (command != 0) {
            request_scene(rdram, command);
        }
    }
    remember();
    state.last_pads[5] = command;
}

extern "C" void bm64_debug_tick(uint8_t* rdram, recomp_context* ctx) {
    int32_t overlay_id = _arg<0, int32_t>(rdram, ctx);
    bm64::coop::note_overlay(overlay_id);
    last_overlay = overlay_id;

    if (state.exiting) {
        return;
    }
    if (state.exit_tick >= 0 && state.tick >= static_cast<uint64_t>(state.exit_tick)) {
        exit_game("BM64_EXIT_TICK reached");
        return;
    }
    if (director.on && director.index == director.scenes.size() &&
        state.tick >= director.boot_ticks + director.scenes.size() * director.scene_ticks) {
        exit_game("director finished");
        return;
    }
    // A SyncTest window that is still being rerun has to finish. The restored cursor puts the
    // mode back to Replay for those steps, but a window that ran past the file does not.
    if (state.exit_on_replay_end && state.input_mode == InputMode::ReplayEnded && !bm64::state::synctest_busy()) {
        exit_game("replay ended");
        return;
    }

    if (bm64::coop::active()) {
        set_overlay_shown(true);
    }
    bool dump = std::find(state.dump_ticks.begin(), state.dump_ticks.end(), state.tick) != state.dump_ticks.end();
    bool update_overlay = overlay_shown && (state.tick % 4) == 0;
    std::vector<Segment> mask;
    if ((state.hash_log || update_overlay || dump) && !state.hash_dead_stacks) {
        mask = dead_stacks(rdram);
    }
    if (dump) {
        dump_rdram(rdram, mask);
    }

    if (state.hash_log || update_overlay) {
        std::vector<uint64_t> range_hashes;
        range_hashes.reserve(state.ranges.size());
        XXH3_state_t* total = XXH3_createState();
        XXH3_state_t* range = XXH3_createState();
        XXH3_64bits_reset(total);
        for (const HashRange& r : state.ranges) {
            XXH3_64bits_reset(range);
            for (const Segment& s : r.segments) {
                hash_segment(rdram, s, mask, range, total);
            }
            range_hashes.push_back(XXH3_64bits_digest(range));
        }
        uint64_t total_hash = XXH3_64bits_digest(total);
        XXH3_freeState(range);
        XXH3_freeState(total);

        if (state.hash_log && !state.quiet_log) {
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

namespace {
DebugState& dbg() { return state; }
}

namespace bm64::debug {

InputCursor input_cursor() {
    InputCursor cursor;
    cursor.input_frame = dbg().input_frame;
    cursor.replay_cursor = dbg().replay_cursor;
    cursor.mode = static_cast<uint32_t>(dbg().input_mode);
    return cursor;
}

void set_input_cursor(const InputCursor& cursor) {
    dbg().input_frame = cursor.input_frame;
    dbg().replay_cursor = static_cast<size_t>(cursor.replay_cursor);
    if (cursor.mode <= static_cast<uint32_t>(InputMode::ReplayEnded)) {
        dbg().input_mode = static_cast<InputMode>(cursor.mode);
    }
}

void set_pad_reuse(const uint8_t pads[24]) {
    std::memcpy(dbg().reuse_pads, pads, sizeof(dbg().reuse_pads));
    dbg().pad_reuse = true;
}

void copy_pads(uint8_t out[24]) {
    std::memcpy(out, dbg().last_pads, 24);
}

bool input_replaying() {
    return dbg().input_mode == InputMode::Replay;
}

bool input_replay_ended() {
    return dbg().input_mode == InputMode::ReplayEnded;
}

uint64_t tick() {
    return dbg().tick;
}

void set_tick(uint64_t tick_value) {
    dbg().tick = tick_value;
}

void set_quiet_log(bool quiet) {
    dbg().quiet_log = quiet;
}

bool booted() {
    return dbg().booted;
}

void range_hashes(uint8_t* rdram, std::vector<std::string>& names, std::vector<uint64_t>& hashes) {
    names.clear();
    hashes.clear();
    std::vector<Segment> mask;
    if (!dbg().hash_dead_stacks) {
        mask = dead_stacks(rdram);
    }
    XXH3_state_t* range = XXH3_createState();
    for (const HashRange& r : dbg().ranges) {
        XXH3_64bits_reset(range);
        for (const Segment& s : r.segments) {
            XXH3_state_t* ignored = XXH3_createState();
            XXH3_64bits_reset(ignored);
            hash_segment(rdram, s, mask, range, ignored);
            XXH3_freeState(ignored);
        }
        names.push_back(r.name);
        hashes.push_back(XXH3_64bits_digest(range));
    }
    XXH3_freeState(range);
}

uint64_t hash_state(uint8_t* rdram) {
    std::vector<Segment> mask;
    if (!dbg().hash_dead_stacks) {
        mask = dead_stacks(rdram);
    }
    XXH3_state_t* total = XXH3_createState();
    XXH3_state_t* range = XXH3_createState();
    XXH3_64bits_reset(total);
    for (const HashRange& r : dbg().ranges) {
        XXH3_64bits_reset(range);
        for (const Segment& s : r.segments) {
            hash_segment(rdram, s, mask, range, total);
        }
    }
    XXH3_freeState(range);
    uint64_t total_hash = XXH3_64bits_digest(total);
    XXH3_freeState(total);
    return total_hash;
}

void dump_to(uint8_t* rdram, const std::filesystem::path& path) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::vector<uint32_t> words((dump_end - dump_start) / 4);
    std::memcpy(words.data(), rdram + (dump_start - 0x80000000u), words.size() * 4);
    for (uint32_t& w : words) {
        w = (w >> 24) | ((w >> 8) & 0xFF00) | ((w << 8) & 0xFF0000) | (w << 24);
    }
    std::ofstream out{ path, std::ios::binary };
    out.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    std::printf("[bm64 debug] Wrote %s\n", path.string().c_str());
}

} // namespace bm64::debug
