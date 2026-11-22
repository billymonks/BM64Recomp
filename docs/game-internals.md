# Bomberman 64 (USA) internals

Reference for everything learned about the game and this recomp while planning online play. All addresses are N64 virtual addresses in the USA 1.0 ROM (`Bomberman 64 (USA).z64`, 8 MB). Names like `func_8024E088` come from `BM64Syms`; names in quotes (for example "getPlayerObj") are our own working names.

Confidence markers: **(verified)** means read directly from disassembly; **(likely)** means inferred from usage patterns and not yet confirmed at runtime. When you confirm or disprove something, update this file.

Companion document: [online-play-plan.md](online-play-plan.md).

---

## 1. Sources and tooling

- **ROM**: `C:\Users\willi\Downloads\Bomberman 64 (USA)\Bomberman 64 (USA).z64` (override with env `BM64_ROM`). The game stores its own copy at `%LOCALAPPDATA%\BM64Recompiled\bm64_us.z64`, and saves at `%LOCALAPPDATA%\BM64Recompiled\saves\bm64_us.bin`.
- **Symbols**: `BM64Syms/dump.toml` (functions) and `BM64Syms/data_dump.toml` (data), from the `BM64Syms` submodule.
- **Build**: `tools/build-windows.ps1` (see `BUILDING.md`). The toolchain (portable LLVM 19.1.3 for the MIPS patches, N64Recomp at commit `98bf104b`) lives in `..\toolchain` next to the repo.
  - MSVC 14.51's standard library rejects clang older than 20, so the game builds with VS 18's clang-cl 22.
  - Clang 20+ conflicts with SDL 2.26's `SDL_endian.h` `_m_prefetch` definition; `-D__PRFCHWINTRIN_H` skips it.
- **Launch** (verified 2026-11-17): `zelda64::get_program_path()` is the base for `assets/` and `recompcontrollerdb.txt`. It stays empty (paths relative to the working directory) when that directory contains `assets/recomp.rcss`. Otherwise it uses the executable's directory, then that directory's parent, so double-clicking `build\BM64Recompiled.exe` finds the repo's `assets`. Before that search existed, opening the exe (working directory = `build`, which has no `assets`) died in `recompui::init_styling`: a failed `ifstream` makes `tellg()` return -1, `std::string::resize` of that value throws `std::length_error` on the render thread, and the uncaught exception `abort()`s. Windows reports that as `0xC0000409` (`FAST_FAIL_FATAL_APP_EXIT`, subcode 7) in `ucrtbase.dll`, with no window left open. A missing stylesheet or UI document now shows a message box and exits with a failure status instead.
- **Recompiler config**: `bm64.us.toml` (22 code hooks plus 68,255 instruction patches, mostly screen-size and null-pointer fixes). `patches.toml` configures the MIPS patch build.
- **Analysis toolkit**: `tools/re/` (Python 3.10 with `rabbitizer`). Scripts find `BM64Syms` in this repo, its parent folder, or env `BM64_SYMS_ROOT`.

| Script | Purpose |
|---|---|
| `syms.py` | Loads sections, functions and data symbols; loads the ROM |
| `bmdis.py` | Disassembler with symbol annotation. `python bmdis.py func_8024E088`, `python bmdis.py func_ovl4_10_80043010` (overlay loads automatically; calls through stubs show `-> target`), or `python bmdis.py .ovl_4_subovl_10 0x80043010` |
| `find.py` | Name search or address lookup: `python find.py HuPrc`, `python find.py 0x802AC896`, `python find.py --func --ovl 4.10 Sleep` |
| `ovl.py` | Decompresses all 95 overlays from the retail ROM (see section 2) |
| `stubs.py` | Resolves overlay secure-call stubs to their main-code targets |
| `xref.py` | Finds references to an address range: `python xref.py 0x802AC894 0x802AC898`. `--all-ovl` also scans every overlay, counting stub calls as calls to the target; `.ovl_x_subovl_y` arguments scan only those |
| `make_decomp_rom.py` | Writes `bm64.decomp.us.z64` (the layout N64Recomp expects) from the retail ROM |
| `compare_hashes.py` | Compares two `BM64_HASH_LOG` files: per range, the first differing tick, how many ticks differ, and the last one |
| `dumpdiff.py` | Diffs two `BM64_DUMP_TICKS` RDRAM dumps into address ranges with region labels and symbols; flags gfx-buffer-parity pointers and host pointers; skips dead process stacks. `python dumpdiff.py a/dumps/tick_1702.bin b/dumps/tick_1702.bin [--ranges default\|legacy\|all\|<spec>]` |
| `osobjs.py` | Lists OS, scheduler and audio objects (threads, message queues, `OSSched`, audio heap) by the constant arguments passed to libultra calls, including calls through secure-call stubs |
| `argclass.py` | Classifies the `$a0` argument at every call site of given functions |
| `ovlsurvey.py`, `strs.py`, `slots.py`, `players.py`, `p0api.py`, `rammap.py`, `survey.py` | One-off surveys used to produce this document |

---

## 2. ROM layout and overlays

- **Boot code**: ROM `0x1000` loads to `0x80000400` (size `0x1BBC0`). libultra, the overlay loader and the "zerojump" secure-call system live here.
- **Main code**: ROM `0x40000` loads uncompressed to `0x80225800` (code and data size `0x7F800`; the loader asks for `0xE0000`). Almost all engine and gameplay systems live here.
- **Overlays**: 14 banks with 95 sub-overlays in total. Every sub-overlay loads at `0x80043000` and only one is resident at a time. In the retail ROM each bank starts at `0x100000 + bank * 0x20000` with a small header, followed by compressed sub-overlays. The retail addresses of each sub-overlay are listed in `patches/required_patches.c` (`load_from_rom_to_addr`). The `0x008xxxxx` addresses in that file and in the symbol sections refer to the decomp ROM layout and are only identifiers for `recomp_load_overlays`.
- **Compression (verified)**: Hudson LZSS, the same as Mario Party:
  - A big-endian u32 decompressed size comes first.
  - Then flag bytes, read LSB first, where 1 means a literal byte.
  - A back-reference is 2 bytes `b1 b2`: position = `b1 | ((b2 & 0xC0) << 2)`, length = `(b2 & 0x3F) + 3`.
  - The window is 1 KB, zero-initialised, with the write position starting at `0x3BE`.
  - All 95 overlays decompress to exactly their symbol-section sizes (`tools/re/ovl.py`).
- **Overlay calls into main code**: overlays almost never `jal` main code directly. Each overlay carries secure-call stubs named `<target>__0xNNNN__secure_call` (for example `func_80297498__0x0302__secure_call`); `stubs.py` maps them back. Overlays never reference `g_Players` directly; they only use main-code accessors.
- **Text encoding**: menu strings are byte-shifted. Adding 7 decodes most characters; capitals use a different offset. Example: `HG@K:MNE:MBHGL` decodes to "ONGRATULATIONS". See `strs.py`.
- **Demo/cutscene scripts**: plain-text bomber command strings such as `WM1 WAP WG1 WSH WDE WB5 ST1 M1 ST10` appear in stage overlays. They're likely walk, direction, bomb and sleep commands for scripted bombers.

### Overlay bank identification

| Bank | Sub-overlays | Contents |
|---|---|---|
| 1 | 4 | Debug maps (strings `debmap0 : Mapchange happened!`, `Playerout happened!`) (verified) |
| 2 | 6 | Sequences such as intro and ending (likely); 2.6 is Controller Pak save management (verified by strings) |
| 3 | 7 | Menus (verified by strings): 3.1 title and mode select ("Adventure", "Battle"); 3.3 and 3.7 battle setup and stage select; 3.4 Custom Bomberman parts; 3.5 options and sound; 3.6 adventure file select and difficulty; 3.2 unknown |
| 4–9 | 10, 7, 15, 10, 13, 6 | Story-mode maps. Likely one bank per world in order (Green Garden, Blue Resort, Red Mountain, White Glacier, Black Fortress, Rainbow Palace). Bank 8 contains "trap task" code (likely Black Fortress) |
| 10–12 | 2 each | Large maps, likely bosses or large stages |
| 13 | 1 | Map object test ("MapObjEntry", "request resource") (likely debug) |
| 14 | 10 | Likely battle arenas |

Every map overlay calls stage setup (`func_80245CDC`) once, with a map ID constant. For example bank 4 uses IDs 588–597, bank 5 uses 320–325, and bank 6 uses 400–408 and 421–425; `slots.py` lists them all.

### Scenes

- **Scene request** (verified): `func_8023AE6C(scene, entry, transition)` is the "go to scene" call used by every menu and stage. It does nothing while a load is pending (`g_LevelLoader` `0x802AC5D0` bit 0) or while `D_802A01B4` is set. Otherwise it writes the scene to `D_802AC5D4` and `D_802AC5DC`, the entry to `D_802AC5D8` and `D_802AC5E0`, the transition to `D_802AC5E4`, and sets bit 0. The scene loop `func_8023DB0C` fades, ends the overlay's processes and calls `func_8022691C(D_802AC5D4)`.
- **Scene to overlay** (verified): `func_8022691C(n)` reads two bytes at `0x8019B0D0 + 2n` (loaded from ROM `0x30800`). The bank's ROM start is `(b0 << 17) + (b1 << 11)`. The sub-overlay comes from the table at `0x8019C0D0` (`func_8022616C`). The overlay is decompressed to `0x80043000` and its entry is called. The scene numbering is separate from `gOverlayID`.

| Scenes (hex) | Bank (ROM start) | Overlay ids |
|---|---|---|
| `14`–`18` | 1 (`0x120000`) | 1–4 (likely) |
| `19`–`1D`, `24` | 2 | 5–10 (likely) |
| `1F`–`23`, `25`, `26` | 3 | 11–17 (likely; `23` loads 13) |
| `28`–`31` | 4 | 18–27 |
| `38`–`3E` | 5 | 28–34 |
| `48`–`56` | 6 | 35–49 |
| `58`–`61` | 7 | 50–59 |
| `68`–`74` | 8 | 60–72 |
| `78`, `79`, `7B`, `7C`, `7E`, `62` | 9 (`0x240000`) | 73, 74, 75, 76, 77, 78 (`7A` and `7D` are unused) |
| `88`–`89`, `8A`–`8B`, `8C`–`8D` | 10, 11, 12 | 79–84 (likely) |
| `90`–`99` | 14 (`0x2E0000`) | 86–95 (Battle arenas 1–10) |
| `9B`–`A4` | 13 (`0x2C0000`) | 85 (likely) |

  No scene uses ROM `0x220000`, so from bank 9 on the ROM slot is one higher than the bank number. Scene `0x23` is Battle setup (overlay 13, menu 3.3). Scene `0x24` is chosen at boot instead of `0x20` when port 0 holds Start (`func_8023A3E0`) (likely a debug entry).
- **Battle start** (verified): the stage-select code in 3.7 calls `func_8023AE6C(0x90 + index, 0x10, 0)`. The index comes from `+0xA0` of the Battle settings block `0x802AC660`, which the menu fills before that call. A request for an arena scene made from the title screen, without passing through Battle setup, crashed while loading the arena's files (2026-11-20). `BM64_DIRECTOR` therefore goes through scene `0x23`, lets its A presses pick the default arena (`0x90`), and rewrites the pending scene and `+0xA0` to the target arena (section 13). Writing the index to `+0xA0` before opening Battle setup also works (the menu then picks arena 3, scene `0x92`, by itself), but with that start the menu ran out of heap at tick 313 for arenas 2, 5, 8 and 10.
- **Heap exhaustion in Battle** (resolved as a director effect, 2026-11-20): reaching Battle setup through the title and main menu (`BM64_DIRECTOR_MENU`) runs all 10 arenas to the end with no allocation failure, on the same save and the same arena override. Only the warp from the title (scene `0x23` requested at tick 600 while overlay 9 runs) leaves too little heap. Original findings: in director runs, `func_802998EC` returns 0 for small requests (`0x2028`–`0x4040` bytes, files `0x119`, `0x11E`, `0x124`) and the decompressor `func_80292BD0` faults on the null destination. `func_8026CE28` now logs `Allocation failed: file … size …` first. Arenas 2, 3, 5 and 7 failed every time, 1–400 ticks after round-end loads (files `0x118`/`0x119`/`0x11D`/`0x11E`). Arena 9 ran full twice and failed once at a different file, so the point depends on the run (recording is not bit-identical between runs; the sound slot table follows the audio thread). Seeds 1, 2 and 3 on arena 3 all failed. Not caused by the director's arena swap (the preselect start failed too) or by the sound stack scrub (failed with it disabled).
  - Allocator (verified): GNU malloc (Haertel). `_heapbase` `0x802B3620` (`0x80300000`), `_heapinfo` `0x802B3624` (12-byte entry per 4 KB page; the table sits in the first page), `_heapindex` `0x802B3628`, `_heaplimit` `0x802B362C`, fragment free lists `0x802B3630 + 8 * log2(size)`, counters `0x802B3690`–`0x802B369C` (used count, used bytes, free count, free bytes), `__morecore` pointer `0x802A2CF0` → `func_8029B410` → sbrk `func_80225B00`. Requests over `0x800` bytes take whole pages (first fit, else grow the heap). sbrk caps the heap at 1 MB (`0x80300000`–`0x80400000`) and stores the new break even when it refuses, so after one refusal every later grow fails too. The break word is `D_8029F5A0`, and `free` (`func_8029A3F4`) gives the top of the heap back with a negative `__morecore` call, so the break also shrinks.
  - Heap size (patched 2026-11-22): `func_80225B00` is a `RECOMP_PATCH` that allows the break up to `0x80500000` (`BM64_HEAP_END`, host `bm64::debug::kHeapEnd`). Nothing in the game uses `0x80400000`–`0x80800000`: no code builds an address in it, and every tick dump checked had it all zero. The patch keeps the highest break it has granted at `0x80F01F20` (`bm64::coop::kHeapMark`, inside the snapshotted and hashed slot block). A run that stays under 1 MB allocates exactly as before.
  - Arena 3 (scene `0x92`), recording `runs/st/pre_92`: 600 KB used at tick 599 (title), 996 KB of 1000 KB with one free page by tick 700 (Battle setup), still full at 1100 and 1400 (battle). The failing request is the first 3-page block after the heap stops growing. About 300 KB is five never-written blocks allocated before the title (four of 9 pages, one of 38 pages, the size of a 320×240 16-bit image); they stay at the same addresses through every scene, which fits the game's own frame or render buffers that RT64 never writes back. Scene objects (1 KB, 512 B and smaller fragments, starting with a pointer and a type 1/6/8) fill about 112 pages in Battle setup and in the battle; the setup menu's fragments are all freed and reused when the battle loads, and the file cache (`D_800A7F30`, 16-byte entries) holds only arena 3's files, so nothing leaks across the scene change. Arena 3 loads four more files than arena 1 (`0x296`, `0x10C`, `0x10D`, `0x183`). The patches add no heap allocations besides the original boot stack (`func_802998EC(0x1000)`), and the free hook only forwards to `_free_internal`. Tools: `runs/re/heapwalk.py`, `largeblocks.py`, `fragpeek.py`, `fragdiff.py` (dumps from `BM64_DUMP_TICKS`).
- **Entry values** (verified): scene changes inside a stage use entry `0x12` (for example `ovl4_6` requests `0x2E` with `0x12`; bank 9 overlays request `0x79`, `0x7B` and `0x7C` with `0x12`). Most scenes also start with `0x10`. Scene `0x2E` does not: with `0x10` it faults in `func_80250338` on a player object that does not exist yet, and with `0x11` or `0x12` it loads overlay 24.
- **Unused scene ids** (verified): `0x7A` and `0x7D` point at bank 9 in the scene table but are not in bank 9's sub-overlay table (`06 00 78 01 00 79 02 00 7B 03 00 7C 04 00 7E 05 00 62`, count then (sub-overlay, scene) pairs). Requesting either faults before an overlay loads.

---

## 3. Threads, main loop and timing

- **Main loop** (verified): `func_80225840`, already re-implemented in `patches/required_patches.c`. One iteration is one game tick:
  0. `bm64_debug_tick()` (state hash, dumps), then the texture scroll `func_8022997C` if the previous tick submitted a frame (recomp: moved from the render thread, see section 9)
  1. `func_8023A104()`: wait for controller data and update input history (section 5)
  2. `Recomp_ProcessCheats()` (recomp addition; reads port 0 from `D_802AC410` after that sample, and only if `bm64_cheats_allowed()`)
  3. `Gfx_InitGfx()`
  4. `ThreadProc_RunQueuedThreads()` and `func_802381F8()` (start or yield to queued OS threads; in practice the render thread)
  5. `HuPrcCall()`: run all Hudson processes (all gameplay logic)
  6. `func_8022787C()`: per-framebuffer projection and viewport
  7. `func_802290CC()`: draw pass, which also advances animations (section 9)
  8. `func_8025E1D4()` (soft-reset effect), then `func_8026C208()` (audio housekeeping)
  9. `func_8023A208()`: `osContStartReadData` for the next tick
  10. `Gfx_EndRender()`: finish the display list, hand it to the render thread (and reset `D_802A5800`), then `func_80237F44()` blocks until the next vertical blank
- **Tick rate**: 60 Hz (verified). The loop waits for exactly one retrace through `ThreadProc_RunQueues`, and the recomp sets `gEXSetRefreshRate(60)`.
- **Fixed timestep** (verified): `HuPrcCall` decrements process sleep timers by exactly 1 per tick, in both the original and the patched version. `gThreadFrameCount` and `ThreadProc_GetCurrentFrameCount` (`0x80237F34`) have no callers or pointer references.
- **Clocks** (verified): the only clock read in main code was `g_initRandom(osGetTime())` at boot. That call is now `g_initRandom(bm64_session_seed())`; the host supplies the seed and the game does not read the clock. No overlay calls `osGetTime`, `osGetCount`, `osCreateThread`, `ThreadProc_Add`, EEPROM, Controller Pak or controller functions.
- **OS threads**: game thread (id 3, runs the main loop and all processes), render thread `Gfx_Render` (the only thread created through `ThreadProc_Add`, from `Gfx_CreateRenderThread`), the ThreadProc manager `ThreadProc_RunQueues` (retrace fan-out), plus libultra VI, PI, scheduler and audio threads. Thread names come from `zelda64::get_game_thread_name` in `src/main/main.cpp`.
- **Thread and queue objects in RDRAM** (verified with `osobjs.py` and dumps):
  - `OSThread`s: `gGameThread` `0x80020668` (boot BSS), pre-NMI thread `D_800BE868` (`func_8025E100`, created by `func_8025E16C`), the render thread inside `gThreadProcs` (`0x802A5A60`, 8 entries of `0x5D8`), the scheduler's thread inside `OSSched D_802A89B0`, `gThreadProcManagerThread` `0x802AAC48`, and the audio thread `D_802AD6B8` (`func_8026B584`).
  - ultramodern's `OSThread` keeps a **host pointer** (`context`) at `+0x20`, so every thread object differs between runs from tick 0.
  - Message queues: render `D_802A5330` (1 slot, non-blocking send from `Gfx_EndRender`), retrace `D_802A5348`, RSP done `D_802A53A0`, ThreadProc `D_802A8938`/`D_802A8958`, controller `D_802AC3E0`, audio `D_802AD1F8`, `D_802AD868`, `D_802AD8A0`, `D_802AFBA0`, pre-NMI `D_800BE848`.

### Hudson processes (HuPrc) and recomp coroutines

- **Process pool**: `D_80063370`, 0x50 entries of `struct Process` (`patches/process.h`). Each has an inline 0x800-byte MIPS stack unless a caller supplies one. Lists: active head `D_802AB200`, free head `D_802ABAA0`; current process `D_802AC344`; count `gProcessCount`.
- **Original design**: cooperative tasks with `setjmp`/`longjmp`. The recomp replaces that with native coroutines:
  - The MIPS side lives in `patches/process.c` (`HuPrcCall`, `HuPrcCreate`, `HuPrcSleep`, `HuPrcVSleep`, `HuPrcEnd`, and so on).
  - The host side is `src/game/process_coroutines.cpp` with minicoro (`src/game/minicoro.h`; ASM backend on x64).
  - Fixed array `g_slots[0x50]` of `ProcessSlot {state, process_id, entry_func, mips_sp, mco_coro* coro, recomp_context ctx}`.
  - The native stack is one slot in a fixed pool: 80 slots of 526016 bytes, reserved with `VirtualAlloc` at `0x200000000` (`process_coroutines.cpp`). A slot's stack address does not change when its process is destroyed and created again. Savestates copy from the saved stack pointer up to the top of that slot.
- **Yield points**: processes only yield inside `recomp_process_yield` (from `HuPrcSleep`, `HuPrcVSleep`, `HuPrcWaitCond`, `HuPrcEnd` and `HuPrcYieldSave`). Between ticks every process is suspended at one of these.
- **Boot-time processes** (created in `func_80225840`): `func_80236F54` (camera, priority `0x401`, 0x1000-byte stack) and `func_802334CC` (priority `0x402`).

---

## 4. Random numbers (verified)

- **Algorithm**: Knuth's subtractive generator (Numerical Recipes `ran3`: modulus 1e9, 55-entry table, stride 21).
- **State**: `g_RandomTable` at `0x802ACFE8` (56 words; the seed is stored at +0xDC) and `g_RandomIndex` at `0x802ACFE0`. Nothing else holds RNG state.
- **Functions**: `g_initRandom` (`0x8025E6A4`, seeds and shuffles 3 times), `g_shuffleRandomTable` (`0x8025E4A0`), `g_nextRandomRaw` (`0x8025E5EC`), `g_nextRandomDouble` (`0x8025E63C`), `g_getRandomNumber(max)` (`0x80234248`).
- **Seeding**: once, in `func_80225840`, from `bm64_session_seed()`. The host picks it: the recording's seed on replay, `BM64_SEED` or `bm64::session::set_seed` when a session supplied one, otherwise a random value. `osGetTime()` is no longer the seed. About 115 call sites across main code and overlays.

---

## 5. Input system (verified)

- **Raw pads**: `D_802AC410`, 4 × `OSContPad` (6 bytes: u16 buttons, s8 stick_x, s8 stick_y, u8 errno). Filled once per tick by `func_8023A104` through `osContGetReadData`. `osContStartReadData` is issued by `func_8023A208` with message queue `D_802AC3E0`. The 16 bytes at `D_802AC400` are four controller-status words. While online, the input filter overwrites them from the session player count (connected type 5 for players in the session, errno 8 for the rest), because each process otherwise reports whichever SDL devices it opened.
  - `func_8023A104` is also called once during boot by `func_8023A22C`, before the main loop.
  - `func_8023A104` is now re-implemented in `patches/input_hooks.c`. It calls the host's `bm64_input_filter(pads)` between `osContGetReadData` and the history update; this is the record, replay and netplay injection point.
- **History ring**: `D_802AC428`, 4 ports × 16 entries × 6 bytes (`0x60` per port). Each entry is u16 held, u16 newly pressed (`new & (new ^ previous)`), s8 stick_x, s8 stick_y. Write index per port: `D_802AC5A8[4]`. Stick deadzone: `D_802AC5B8`. Clear: `func_8023A26C`.
- **Accessor API** (port first, then history offset where present):

| Function | Behaviour |
|---|---|
| `func_802399AC(port)` | Newly pressed buttons, current tick |
| `func_80239E88`, `func_80239FF8` (port) | Held buttons, current tick (two variants) |
| `func_80239E28`, `func_80239D6C`, `func_80239CC8` | Pressed or held variants (unconfirmed) |
| `func_80239F70(port)` / `func_80239EE8(port)` | Stick X / Y with deadzone, current tick |
| `func_80239714(port, framesAgo)` / `func_802395A8(port, framesAgo)` | Stick X / Y with deadzone from history (likely X / Y) |
| `func_80239880(port, framesAgo, outVec3)` | Stick as angle and magnitude; used for bomber movement |
| `func_8023A038(port)` | Controller errno (0 means connected); used by menus to detect controllers |

- **Who reads input**:
  - Bomber control `func_8024E088`: port equals player index.
  - Camera C-buttons `func_80235934`: port 0, hardcoded at `0x80235974`.
  - Menu helper `func_8027F0C4` and `func_8027EDC8`: port 0, or all 4 ports ORed together.
  - Others: `func_8023AEDC` (from `func_8023BBCC`), `func_8023E1D8`, `func_802437C0`, `func_802437E0`, `func_8026CFF4`, `func_80295B80`, `func_80297A34`, `func_8023A3E0`, `func_802441F4`.
  - Overlay menus call `func_802399AC`, `func_8023A038` and `func_80239FF8` through stubs.
- **Host ports** (Phase 1): `recomp::get_n64_input` and `recomp::get_connected_device_info` cover ports 0–3. Port 0 is always connected because the keyboard feeds it. Each SDL controller is assigned to one port (Auto fills ports in connection order; the Controls tab assigns a device, or None). A port's buttons and stick come only from that port's controller and that port's bindings. Ports 1–3 report disconnected until a controller is assigned, which is what the battle setup menu uses (`func_8023A038` errno). Controller bindings are stored per port in `controls.json` under `ports`; an older single `controller` block is copied onto every port. Four-port support is implemented. On 2026-11-18 the user confirmed 2-player local play. 4-player and mixed human/AI matches were not played.
- **Cheats**: `Recomp_ProcessCheats` runs after `func_8023A104` and reads port 0 from `D_802AC410` (the filtered sample). It still writes `g_Players[0]`, lives and credits. `bm64_cheats_allowed()` is false while recording, replaying, or when `bm64::debug::set_cheats_disabled(true)` (also `BM64_DISABLE_CHEATS=1`). Netplay uses that flag.

---

## 6. Players (verified unless marked)

- **`g_Players`**: stride `0xC8` (200 bytes). The retail array is `0x800AEDF0` (5 slots). `tools/re/relocate_players.py` rewrites the main-code address setups so the game uses `0x80F00000`, and the player loops visit eight slots (`base + 0x640`). `D_800AF1D8` stays the next table: `func_802506B8` and `func_8024ABA8` still index it, and the script's checker rejects a patch that moves those uses. `g_ComputerPlayerMovement` (stride `0x10`) is at `0x80F00640`, eight entries. `func_80241BB0` writes a `0x14`-stride side table at `0x80F006C0`; boot copies the original `0x64` bytes from `0x802A07D4`. The 4-port controller assignment in `func_8023D0F8` still walks four words at `0x802AC660`. The bytes `0x80F00000`–`0x80F00780` are in the snapshot and the `slots` hash. A rollback that restored every other range and left the player array dirty faulted on the bomber's body pointer. `func_80243BF0` initialises the retail slots; boot also sets field `0x18` to `0x200` on all eight. Empty slots stay `controlType` 0, so the extra iterations do not spawn or simulate a bomber.
- **Pad ports and slots**: a bomber reads the controller port equal to its slot. The pad history `D_802AC428` holds four ports (write index `D_802AC5A8`), and its accessors do not bound-check, so slots 4–7 read past it. Accessors that take the port in `a0`: `func_802399AC`, `func_80239E88`, `func_80239FF8`, `func_80239E28`, `func_80239F70`, `func_80239EE8`, `func_80239714`, `func_802395A8` (`func_80239880` goes through the last two). `func_8023A038` returns an error code, and `func_80239D6C` and `func_80239CC8` take the port in `a1`. On story maps in a session, a port map in co-op rule bytes `0x58`–`0x5F` (`0x80F01EC0` + `0x58`) is applied at the entry of those eight accessors: `0` keeps the slot's own port, `0x80` reads nothing, anything else is port + 1. Stage setup gives the n-th co-op slot port n, and silences slots 1–3 on rival maps.
- **Field map** (offsets confirmed by accessors; the struct in `required_patches.c` agrees up to `0x28`):

| Offset | Field |
|---|---|
| `0x00` | spawn position xyz (floats) |
| `0x0C` | direction angle |
| `0x10` | flags (many accessors test bits) |
| `0x14` | hearts |
| `0x18` | u32, set by `func_80277D20(i, v)` to `v | 0x200` |
| `0x1A` | bombType (s16; bit0 red bombs, bit1 remote bombs) |
| `0x1C` | bomb count |
| `0x20` | fire power |
| `0x24` | controlType: 0 none, 1 human, 2 AI, 3 dead |
| `0x28` | `obj`, the bomber's LevelClass object. Its world position is at `obj->ObjectPointer(+0x40) + 0x10/0x14/0x18` |
| `0x2C` | held object |
| `0x30` | other object |
| `0x44` | animation |
| `0x48` | collision enabled (likely) |
| `0x4C` | read by `func_802442A8` (38 overlay calls) |
| `0x68` | flags; bit `0x8000` tested by `func_80250DC8` |
| `0x80` | written by `func_8025068C` for the main player |
| `0x94`, `0x98`, `0xA4`, `0xC0` | used by the battle and player update code |

- **Indexed accessors** (take a player index): `func_802443F4` "getPlayerObj(i)" (233 overlay plus 45 main call sites), `func_80250338` "getPlayerPos(i, outVec3)" (147 overlay), `func_80243D2C` hearts, `func_802442A8` field `0x4C`, `func_802442D0` fire, `func_80243F2C` "setControlType(i, v)", `func_80277D20` field `0x18`, `func_80244BA4`, `func_80245408`, `func_802452AC` (flag bits), `func_80243EB4`, `func_80243EF0` (field `0x68`), `func_802443CC` (anim), `g_Add`/`g_SubtractPlayerBombCount` and `FirePower`.
- **Main player index** `D_802AC894`: "the hero" in story mode.
  - Getter `func_80243DDC` "getMainPlayer"; setter `func_8024585C` "setMainPlayer".
  - Init sets it to -1 (`func_80243BF0`); stage setup sets it to 0.
  - Main-code functions that read it directly (27): `func_80243DEC`, `func_80243E60`, `func_802443A0`, `func_802450DC`, `func_80245280`, `func_802452E8`, `func_8024557C`, `func_802455A8`, `func_802456FC`, `func_802457D8`, `func_80245868`, `func_80245F74`, `func_80249AD4`, `func_8024A0DC`, `func_8024A824`, `func_8024D8C4`, `func_8025068C`, `func_802506B8`, `func_80250DC8`, `func_80250E2C`, `func_802510C8`, `func_802512C0`, `func_802512F8`, plus `func_80243BF0`, `func_80245CDC` and `func_8024585C` (writers). `func_80243DDC` is patched to `bm64_main_player()`, which returns this word unless a co-op override is set. The override stays off: a per-peer override would desync the shared camera block. The 22 other direct loads are hooks: after the `lw` they store `bm64_resolved_slot_at(rdram, lw address)` into that register. With no session it is the same word. The stores in `func_80243BF0`, `func_8024585C` and `func_80245CDC`, and the address formed in `func_80245868`, stay writers.
  - `getMainPlayer` asks `bm64_main_player()`. Recompiled `jal` is a plain C call that never writes `ctx->r31`, so a patch can't learn its caller from `$ra` (`__builtin_return_address(0)` logged `00000000` for every call, 2026-11-21). Instead, the "main-player call sites" block in `bm64.us.toml` (294 hooks, generated by `catalogue_main_player.py --emit` and spliced by `tools/re/_splice_site_hooks.py`) runs `bm64_main_site(call + 8)` just before each listed call, and `bm64_main_player` takes and clears that value. Unlisted calls pass 0. With no co-op session it returns `D_802AC894`. In a session the call site is looked up first in `src/game/bm64_player_sites.inc` (overlay id, or 0 for main code, plus site). A site that is not listed falls back to the running process entry in `src/game/bm64_player_cats.inc` (overlay id plus address, because overlay entries repeat). Enemy and boss entries get a sticky nearest living player, refreshed every 60 ticks and stored per process at `0x80F00780`. Any other category returns the leader. A category override byte at `0x80F00820` replaces that result when it is not `0xFF`. The direct-load hooks use that same resolver, and `func_80236F54` counts as a camera process.
  - Call sites are classified by `tools/re/catalogue_main_player.py` (626 sites, each with one category). Camera is the module at `0x80234578`–`0x80237890` (`func_80235D7C`, `func_80237664`, `func_80235234`). The six main-code hero routines `func_802506B8`, `func_80250DC8`, `func_80250E2C`, `func_802510C8`, `func_802512C0` and `func_802512F8` act on the bomber they read, so their sites are written to the site table as other (the leader); see "Main-player sequences" below. A function that also calls `setMainPlayer` is cutscene or script (`func_8023F26C`, `func_802553D4`, `func_8025DC60`, plus the rival-map overlays that do the same). Constant slot 1–3 inside maps 4.10, 5.7, 6.15, 8.13, 9.5 and 9.6 is rival-bomber control. A story or boss overlay that follows the main player without a constant slot is enemy or boss targeting. Menus, battle, and the remaining main-code readers are other. The per-overlay counts are in `docs/main-player-catalogue.txt`.
- **Call-site statistics** for `getPlayerObj` in overlays (`argclass.py`): 115 pass a computed value (mostly `getMainPlayer()`), 49 pass constant 0, 31 constant 1, 5 constant 2, 1 constant 3, and the rest pass loop variables. 54 of 95 overlays call it.
- **Stage player setup** `func_80245CDC`:
  - Sets the main player to 0.
  - Allocates a 0x1000-byte stack and creates the player manager process `func_8024FFF0` (priority `0x70`).
  - Calls `g_SpawnPlayer(i)` (`0x802459BC`) for every slot 0–7 with controlType ≠ 0.
  - `g_SpawnPlayer` creates the bomber from body file `0x49` (`func_80231274`) and then calls `g_AssignModelParts(model, file, part, part)` (`0x802297B8`) for parts 0–11. That frees the part's old file and loads `file` through `func_8026CE28`, which caches by file ID with a reference count. The parts file is `0x4A` + slot for slots 0–3 (white, black, red, blue). Slots 4–7 have no case and reuse the previous spawn's file from stack slot `0x38($sp)`. Co-op replaces it by party rank on every map (hook at `0x80245BC4`): white, black, red, blue. `func_8025DC60` assigns `0x4B` to the rival it spawns, but on screen the 4.10 rival is its own character, not a black bomber (user, 2026-11-22), so that file is not the rival's look.
  - Then runs `func_80237664` (camera reset, likely) and `func_802362D4(0x1F)`, and initialises per-stage globals (`D_802AC8BC`, `D_802ACA24`, `D_802AC95C`, …).
  - If `g_LevelLoader & 4`, it allocates enemies with `g_EnemyAllocate(3, 0x1C, 0xD2)`.
  - **Consequence**: marking more slots human before stage setup spawns more bombers.
- **Per-bomber logic**: `func_8024E088(i)` (0x1F68 bytes, driven by `func_8024FFF0`, which visits eight slots and runs a slot only when its controlType is human or AI). Human players read the port with their index through `func_80239880(i, …)`; AI reads `g_ComputerPlayerMovement` (`0x80F00640`, stride `0x10`, eight entries). Bomber object update: `func_802553D4` (0x71BC bytes). It also calls `setMainPlayer`, so the site catalogue files it under cutscene or script, but it runs as the rival's process for the whole fight on rival maps; co-op's cutscene check skips it.
- **Computer-bomber AI** (Battle CPU players and story rivals). Each AI slot has a record at `D_800BCFE8 + slot * 0x128`: `+0x0C` its own slot, `+0x10` the slot it is after, `+0x18` its own object, `+0x1C` the target's object (`getPlayerObj` = player `+0x28`), `+0x9C` its `g_ComputerPlayerMovement` entry. `func_802553D4` is the AI process, started with the record as user data.
  - Battle: `func_80251804(slot)` fills the record and draws the target with `func_80251708`, which uses `g_getRandomNumber(3)` (0–3, the raw value mod 4) directly as the slot. It rejects empty slots, itself, `func_80243EF0(slot) != 0`, teammates in team mode, and half the time the slot the rule block marks. The picker reads its own slot from the caller's `$s3`. So it can only pick slots 0–3.
  - Story rivals: `func_8025DC60` spawns the rival and writes the record itself, with target 0 (or `$v0` when `$s2` is 9; `0x8025DEF4`–`0x8025DF04`). `func_802553D4` calls the picker again only when the target's hearts reach 0 or the target is -1, and then only if `g_LevelLoader` bit `0x4` is set. So a story rival chases slot 0 for the whole fight.
  - The rival tends to stand still while its target is idle: on 4.10 with both humans idle it stood still for about 760 ticks (`coop_410d.bm64rec`, ticks 4660–5420), and walking only the target in a copy of the recording got it moving within 30 ticks, while walking only the other human did not. Single-player has pauses too, 100–380 ticks at a time.
- **Story maps that drive slots 1–3** (rival bombers or actors; call `setMainPlayer(1/2)` and/or `getPlayerObj(1..3)`): 4.10, 5.7, 6.15, 8.13 (slots 1, 2 and 3), 9.5, 9.6. With eight slots those maps still have room for co-op players in slots 0 and 4–7.
- **Main-player sequences** (verified 2026-11-20 from disassembly). Each reads `D_802AC894` and acts on that bomber, so none is a test of whether the player has reached the goal; that check lives in the stage overlays.
  - `func_80250E2C(a0)` (called with argument 1 from about 30 map overlays) is the hero's goal or warp sequence. It sets `D_802AC968[hero] = 1`, calls `func_80244354(hero, 0x10420, 1)`, and sleeps until flag `0x400` is set.
  - `func_802506B8` resets the per-player tables to −1, calls `func_80244354`, and waits on flags `0x800`, `0x80000` and `0x400`.
  - `func_80250DC8` calls `func_8024A0DC` on the hero unless flag `0x8000` is set.
  - `func_802510C8` calls `func_80247C50` (the hero's animation state) when flag `0x1000` is set.
  - `func_802512C0` calls `func_802511EC`, which places the hero at a grid position.
  - `func_802512F8` copies the hero's slot position onto its body.
- **Main-player box tests** (verified 2026-11-21). `func_802454F8(slot, out)` converts the bomber's body position (`g_Players[slot] + 0x28` → `+0x40` → `+0x10/14/18`) to a grid cell `{x, floor, z}` with `func_8026DCAC(x, y, z, buf, 2)`. The main-player tests cache the cell in `D_802ACEC8[slot]`, which has 5 entries of 12 bytes and is followed by `D_802ACF04`.
  - `func_802457D8(x0, z0, x1, z1)`: is the main player's cell in the inclusive x/z rectangle? Used for goals and exits (152 calls in 48 overlays; map 4.1's two exits are cells (5, 26) and (44, 22)).
  - `func_802456FC(x0, y, z0, x1, z1)`: the same, on floor `y` only (39 calls in 17 overlays).
  - `func_802455A8(x0, y0, z0, x1, y1, z1)`: a box with a floor range. No callers.
  - `func_80245430(cell, x0, z0, x1, z1)`: the rectangle test itself.
  - In a story co-op session the first two are patched to also accept any other human bomber (online-play-plan 6.7).
- **Getting hit** (verified): `func_80248148(player, kind)`. Kind 3 or 5 stores 0 to hearts (`+0x14`); any other kind subtracts one. The function returns immediately when `controlType` is already 3. When hearts are still non-zero it skips the rest (`bnez` at `0x80248284`). Hearts at zero fall into the sequence that stores `controlType` 3 at `0x80248374`. In a co-op session the `bnez` is hooked: a human is put back at the party checkpoint instead, unless that human is the last one standing and shared lives are one or fewer.
- **Story globals**: lives `D_802AC624`, credits `D_802AC628`. Stage HUD values are in struct `D_802AC610`: `+0x1C` gems, `+0x28` and `+0x30` timer values (from the HUD draw `func_80242270`).

---

## 7. Camera (verified unless marked)

- **Camera process** `func_80236F54`. Every tick it runs `func_80236D30` (update), takes the eye (`D_8008E648`) and the look-at target (`D_8008F3D8`), swapped if mode `D_8008F404` is 1, adds shake from `func_80236BA8`, calls `func_80227CD0` (guLookAt into the framebuffer matrix and the float view matrix `D_802A5418`), and then `HuPrcSleep1`. At start it spawns `func_80235D7C`.
- **Camera controller** `func_80235D7C`: follows `getPlayerObj(getMainPlayer())`; calls `func_80233650`, `func_8022FEAC`, `func_80235234`, `func_8023494C` and `func_80235934` (C-button rotation, port 0).
- **Simulation state** (about 260 bytes):
  - eye `0x8008E648` (12 bytes)
  - core block `0x8008F3D8`–`0x8008F430`
  - controller block `0x8008FFE8`–`0x8009007C`
  - tables in `0x8029FEA0`–`0x802A004C` and `0x802A5A50` (check whether they're mutable)
  - camera module code spans about `0x80234578`–`0x80237890`
- **Per-player copies** (2026-11-19): a session with two or more human bombers keeps each human's eye (`0x8008E648`, 16 bytes), core block (`0x8008F3D8`, `0x60` bytes), controller (`0x8008FFE8`, `0x98` bytes), table at `0x8029FEA0` (`0x1B0` bytes) and the 16 bytes at `0x802A5A50` in a block at `0x80F00840` (stride `0x2D0`; offsets `0x000`, `0x010`, `0x070`, `0x110`, `0x2C0`). The controller process and `func_80236D30` run once per human with that block swapped in, then the first human's block is copied back so live memory matches on every peer. Bomber movement (`func_8024E088`, call at `0x8024E880` with the slot in `s0`) gets the rotation from `func_80234858`; for a human other than the first, a hook returns that human's block copy of `0x8008F3F8` (block `+0x30`). `bm64_camera_frame` and that hook use the slot the local session player drives (see the port map under `g_Players`).
- **Gameplay reads the camera**: bomber movement is camera-relative: `func_8024E088` turns the stick by the rotation `func_80234858` copies from `0x8008F3F8` (pitch, yaw, roll in degrees). `func_80234858` is also called by `g_StopSound` (pan), the HUD and overlays. `func_8028A0B4`–`func_8028A2B0` (particles, likely billboards) read the eye. **Camera state is therefore simulation state.**
- **Bomb pool, not camera** (corrected 2026-11-19): the 48 entries of `0x48` bytes at `0x8008E658`, right after the eye, are the bomb pool. `func_80277F74` finds a free entry, `func_80277FC4` allocates one, `func_80278DBC` updates them each tick, and `func_80278F68` clears the pool and resets every player's fire power (`+0x20 = 2`). `func_80251C88(obj)` returns the entry whose `+0x14` is `obj`; it does not read the look-at point. An earlier version of this section treated the pool as camera state and copied it per player, which would have given each human their own bombs.
- **Perspective**: `func_80227C50`, called from `func_8023A3E0`, which also sets up the framebuffer contexts.
- **Overlay camera commands** (likely): `func_80234778`, `func_802346C8` and `func_80234434` are heavily called by stage and boss overlays.

---

## 8. Objects, enemies and items (partial)

- **Objects**: `LevelClass` (layout in `required_patches.c`): `+0x28` objectID, `+0x40` ObjectPointer (object world position at `+0x10`), `+0x44` collision pointer, `+0x6C` yVel. Initialised by `g_InitLevelClass` (`0x80231274`).
- **Enemies**: `g_EnemyAllocate` (`0x80287008`), `g_spawnEnemyFromAllocation` (`0x802867E4`). Items: `g_spawnItem` (`0x8027B7C4`). Particles: `g_SpawnParticleEffect` (`0x8028A81C`) and related.
- **Overlay logic**: mostly `HuPrcCreateChecked` processes that sleep and poll. Enemies find their target through `getPlayerObj(getMainPlayer())` and `getPlayerPos(...)`.
- **Rooms and models**: `func_80243A50` "Room_Load" (patched), `func_8026CE28` "load/alloc model by file ID" (patched).

---

## 9. Rendering

- **Display lists**: `gGfxWork` at `0x8014C8D0` (2 × `0x27000`). Framebuffers at `0x80100000` (2 × 320×240×16 bpp). RSP dram stack at `0x8019A8D0`; output buffer at `0x801E56C0`. Buffer IDs: `gFrontBufferID` and `gBackBufferID`.
- **Viewport contexts**: `gFrameBuffers[4]` of `FBInfo` (0x6C bytes each: viewport, enabled, sub-DL id, matrix, scissor). `D_802AC5C0` points at the main context.
- **Draw pass** `func_802290CC`: for each enabled context it walks model hierarchies through `func_802289C0`, calling per-node callbacks with `jalr` and `func_8022D58C` (matrix stack using `D_802A5418`). **It advances animations** (the recomp authors note that stubbing it freezes animation), so it mutates simulation state.
- **View matrix readers** (verified): the float lookAt at `D_802A5418` (16 floats, inside the hashed `gfx` range) is written by `func_80227CD0` and read only by `func_8022D58C`, which passes it to `func_8029CDA0` while building the model matrix. Nothing else in main code or the overlays references that range. `func_80232FF8` hides objects from projected depth using the camera tables around `0x802A0000` and `D_802A5804`, not `D_802A5418`. `BM64_CAMERA_BIAS` adds to the eye arguments of `func_80227CD0` and does not write the camera block. On `new.bm64rec` the biased replay matched `ovl`, `data`, `prc` and `game` for all 8,838 ticks. The differences were the integer `Mtx` inside `gFrameBuffers` (contexts at `0x800BD5C8` and `0x800BD638`, 0x40 bytes each), the float copy at `D_802A5418`, and the camera process's live stack frame on the malloc heap (base `0x80303000`, `sp` `0x80303EF8`), which still holds that eye when the process sleeps. Draw callbacks did not change simulation state. When each machine renders a different camera, leave those matrices and that stack frame out of the sync hash.
- **Render thread** `Gfx_Render` (patched) receives buffers through `D_802A5330` and builds scheduler tasks. The patches already contain `startGfxWait`/`waitForGfxFinish` (wait for display-list completion).
- **Dropped frames** (verified): `Gfx_EndRender` sends the finished frame to the 1-slot queue `D_802A5330` without blocking, so a frame is dropped whenever the render thread is still busy (for example during RT64 shader compiles or texture uploads after a map load). The game keeps running at 60 Hz regardless.
- **Double-buffer parity** (verified): the render thread flips `gFrontBufferID` (`0x8029F5E0`) for each frame it picks up, and the game builds into `gGfxWork[gFrontBufferID]`. After a dropped frame, `gBackBufferID`, `gGfxWorkPtr`, `gMasterDisplayList`, `gBufferEnds` and the matrix cursor `D_802A5550` point into the other buffer, so they differ between runs. No other game memory keeps gfx-work pointers.
- **Render thread writes into game memory** (verified; both now moved to the game thread by `patches/required_patches.c`):
  - `func_8022997C`, texture scroll: up to 6 entries in `D_80098C68` (stride `0x18`: object, vertex array, count, speed u/v, limit u/v, accumulator u/v). Registered by `func_80229C90` (called by stage setup in many map overlays and by `func_802553D4`) and freed by `func_80229BF4`. It adds the speed to the `s`/`t` texcoords of every vertex in the array, and the vertex arrays live in model data on the malloc heap. It used to run on the render thread after each drawn frame, so a dropped frame or a different render phase left a permanent UV offset in the heap until the map reloaded. It now runs once per submitted frame at the start of the next tick.
  - `D_802A5800`, frames since the render thread last picked up a frame: zeroed by `Gfx_Render`, incremented every tick by the boot process `func_802334CC`, which passes it to `func_80232FF8` for two object lists (`D_802A55F4`, `D_802A56F4`). For objects with flag bit 18 it toggles a blink phase (flag bit 30 and the model's visibility bit) only when the counter is 0. It is now zeroed by `Gfx_EndRender` when a frame is submitted.
  - The rest of `func_80232FF8` (mode `D_802A5804`) hides objects based on projected camera depth, which is camera-dependent but deterministic.

---

## 10. Sound and saves

- **Sound API**: `g_PlayGlobalSound` (`0x8026C660`), `g_PlayLocalSound` (`0x8026C51C`). The symbol `g_StopSound` (`0x8026C0A8`) is not a stop: its only callers are `g_PlayLocalSound` and `func_8026C208`, and the body computes a listener-relative pan from the camera (`func_802351DC`, `func_80234858`, `func_8022FEAC`). Stopping a voice by handle is `func_8026BE80`, which calls `func_80266930` unless the handle is -1. Per-tick housekeeping: `func_8026C208`. Volume scaling patches live in `required_patches.c`.
- **Audio memory** (verified): the audio heap is `0x8019C2E0`–`0x801E56C0` (`alHeapInit` from `func_8026A754`, size `0x493E0`, set up by `func_8026C77C`). Audio globals, queues, the audio thread and its stack are in main BSS `0x802AD120`–`0x802AFC20` (`D_802AD120` alHeap, `D_802AD8D8` audio thread stack and globals). Pointers into the sound table `D_802AD654` (in the audio heap) are `D_802AD64C`–`D_802AD675`.
- **Sound slot table** `D_800BD770` (8 countdown words) and `D_800BD778` (8 × `0x14`: position xyz, flag, handle) (verified): written by the play functions and the position setters `func_8026C40C`/`func_8026C454`. Each tick `func_8026C208` frees a slot (handle = -1) when `func_80266CD4` reports the sound has stopped. That check reads the state byte `+0x1B` of the sound's `D_802AD654` entry, which follows audio-thread progress, so the table can change a tick earlier or later between runs. `func_8022787C` mentions `D_800BD770` only as the end address of the four framebuffer contexts (`gFrameBuffers` is `0x70` × 4 bytes and ends there); it does not read a slot.
- **Audio queries gameplay does not make** (verified, main code and all 95 overlays): no call to `alSndpGetState`, `alSeqpGetState`, `alSeqpGetTempo`, `osAiGetLength` or `osAiGetStatus`. `alSeqGetTicks` is called only from the sequence player's `__postNextSeqEvent`. `func_80266CD4` is called only from `func_8026C208`. No gameplay function waits on a sound finishing or on the music position.
- **Handles can still escape into hashed memory.** `func_8026724C` picks a libultra voice from the audio heap (`alSndpSetSound` / `alSndpStop` secure calls). Hooks at the `jr ra` of `g_PlayGlobalSound` (`0x8026C6C8`) and `g_PlayLocalSound` (`0x8026C628`) replace that id with a host virtual handle before the caller stores the return value (`bm64_sound_leave` in `src/game/bm64_state.cpp`). Stores of that return value, after the delay slot:
  - `func_80247848` writes it to `D_802AC9E0` (in the hashed `game` range). The same function later loads it and passes it to `func_8026BE80`, then stores -1. `func_80245CDC` initialises the word to -1.
  - Overlay stores: 10.2 `func_ovl10_2_8004B690` at `0x8004DCF8`; 11.1 `func_ovl11_1_80047CDC` at `0x8004D4E8`; 2.3 `func_ovl2_3_800442AC` at `0x80048878` and `func_ovl2_3_80045020` through `s1`; 8.7 `func_ovl8_7_800462DC` at `0x8004A2C8`.
  - `func_80267A18` (inside the sound allocator) shifts the id from `func_8026724C` and stores it at offset `0x14` of its channel struct.
  The branch in `func_8026BE80` is only "handle == -1". A hook at its entry resolves the virtual id to the real voice before that test. `func_80267A18` still stores the real id from `func_8026724C` at offset `0x14` of the channel struct; that struct is in the audio heap, which the hash skips. The value in `D_802AC9E0` and in those overlay words is the virtual id, which follows the start key (tick, sound, source, position), so a difference there is a different start, not an audio-thread allocation.
- **Audio pointers on process stacks** (verified): sound calls leave audio-heap pointers in stack slots below the caller's `sp`. Their values depend on audio-heap allocation order, so they differ between runs.
- **Stack scrub after sound calls** (verified 2026-11-20): sound code also branches on audio-thread globals (`func_80268F94`, called from `func_8026C93C`), so the leftovers it writes below `sp` differ between the two SyncTest passes. A later frame that reserves that stack without writing every word moves those words above `sp`, into the hash. Depth 12 on the scene `0x90` director recording failed at tick 626 that way: `func_802506B8` (frame `0xD0`) sleeping through `HuPrcSleep1` → `HuPrcSleep` → `yield_to_scheduler` covered them. Hooks at the `jr ra` of every sound entry point with a stack frame call `bm64_scrub_stack(rdram, sp + frame)` (`src/game/process_coroutines.cpp`), which zeroes the current process's stack from its bottom up to the caller's `sp`. Under o32 nothing below the caller's `sp` is live. The hooks are the `bm64 sound stack scrub` block in `bm64.us.toml` plus the two `bm64_sound_leave` hooks. Leaf functions `func_8026C394`, `func_8026C40C` and `func_8026C454` have no frame and are not hooked.
- **Save records** (verified): the RAM image is `D_80063000`. `func_802947AC` is the commit used by the custom-bomber menu. It writes one EEPROM byte at address 0 when `D_802A1DB8` is set, then an 18-byte record at `18 * fileIndex + 1` (`fileIndex` is `D_802A1DB4`). The same file occupies 144 bytes in the RAM image, starting at `D_80063000 + 144 * fileIndex + 8`. Custom part *choices* are bytes in the menu overlay (loaded through `g_LoadCustomParts`, part slot 0–3). Which of the 18 bytes they land in was not identified.
- **Saves**: EEPROM, main code only. `func_80292DB8` reads, `func_80292E90` writes, through the secure calls `osEepromRead` / `osEepromLongRead` / `osEepromWrite` / `osEepromLongWrite`. `func_80294E54` probes the EEPROM and **reads the save** at boot, before the RNG is seeded (verified). Controller Pak menus exist in 2.6 and 3.6. Every reimplemented `osPfs*` entry point returns `PFS_ERR_NOPACK`; a sandbox session also forces that through `bm64_controller_pak_allowed()`.
- **Runtime save handling**: the save buffer lives in librecomp (`lib/N64ModernRuntime/librecomp/src/pi.cpp`). The build compiles a generated copy (`tools/runtime/prepare_sandbox_sources.py`) that can suppress the flush. `ultramodern::import_save_image` / `export_save_image` replace and copy that buffer. `bm64::session::begin_sandbox` installs an image and stops the flush, so the file from `get_save_file_path()` is not written. Replays do this with the image stored in the recording. `BM64_SANDBOX=1` sandboxes the loaded save; `BM64_SANDBOX_IMAGE` supplies a different one; `BM64_SANDBOX_EXPORT` writes the buffer out on exit.

---

## 11. RDRAM map (approximate)

| Range | Contents | Savestate |
|---|---|---|
| `0x80000400`–`0x8001BFC0` | Boot code | no (code) |
| `0x8001BFC0`–`0x80028460` | Boot BSS: OS threads and stacks, `gGameThread`, `gSectionSizes` | no (OS) |
| `0x80042000` | zerojump secure-call table | no |
| `0x80043000`–`0x80063000` | Resident sub-overlay code, data and BSS (largest overlay is `0xE320`) | data/BSS yes |
| `0x80063000`–`0x800BEA60` | Main game BSS, zeroed at boot: process pool `0x80063370` (`0x50` × `0x8A0`, each with an inline `0x800` MIPS stack at `+0x28`), camera `0x8008E648…`, texture scroll table `0x80098C68`, `g_Players` `0x800AEDF0`, model and anim work around `0x80098C18`/`0x800A0CF8`, `gFrameBuffers` `0x800BD5B0`, sound slot table `0x800BD770`, pre-NMI thread `0x800BE848` | yes, minus the sound slot table and the pre-NMI thread |
| `0x800BEA60`–`0x80100000` | Post-BSS gap. Many `lui` hits are stale `0x800C` bases. A full SyncTest of `test.bm64rec` did not need these bytes in the snapshot | no |
| `0x80100000`–`0x8014B000` | Framebuffers | no |
| `0x8014B000`–`0x8014C8D0` | File directory (`D_8014B000`, walked by `func_80226604`) | yes |
| `0x8014C8D0`–`0x8019A8D0` | gfx work (display lists) | no |
| `0x8019A8D0`–`0x80225800` | RSP stack, scene table `0x8019B0D0` (ROM `0x30800`) and the current bank's sub-overlay table `0x8019C0D0` (rewritten on every bank load), audio heap `0x8019C2E0`–`0x801E56C0`, RSP output buffer `0x801E56C0` | `0x8019B0D0`–`0x8019C2E0` yes; audio heap no; rest audit |
| `0x80225800`–`0x8029F5E0` | Main code and rodata | no |
| `0x8029F5E0`–`0x802A5300` | Main data (gfx globals, tables) | yes, minus `gFrontBufferID` and the two `OSScTask`s |
| `0x802A5300`–`0x802B36D0` | Main BSS: gfx state, ThreadProc tables `0x802A5A60`, process lists, input, RNG, `g_LevelLoader` `0x802AC5D0`, audio `0x802AD120`–`0x802AFC20`, malloc bookkeeping `0x802B3620` | partial; see below |
| `0x802B36D0`–`0x80500000` | malloc heap (models, files, allocated stacks); the ROM stops at `0x80400000`, the sbrk patch allows `0x80500000` | yes, up to the heap top (see `heap` below) |
| `0x80801000`–`0x81000000` | Recomp patch code and data ("extram", `patches/patches.ld`) | patch globals that are game state (for example `texture_scroll_pending` in `required_patches.c`) |

### State hash ranges (verified 2026-11-17)

The default `BM64_HASH_RANGES` list. With it, two replays of the same recording hash identically on every tick. Each gap was found by diffing dumps from two replays (`dumpdiff.py`).

| Name | Included | Excluded inside or next to it, and why |
|---|---|---|
| `ovl` | `0x80043000`–`0x80063000` | nothing |
| `bss` | `0x80063000`–`0x800BD770`, `0x800BD818`–`0x800BE848` | `0x800BD770`–`0x800BD818` sound slot table (follows audio-thread state); `0x800BE848`–`0x800BEA60` pre-NMI thread queue, `OSThread` (host pointer) and stack |
| `files` | `0x8014B000`–`0x8014C8D0`, `0x8019B0D0`–`0x8019C2E0` | nothing. The second span (scene and sub-overlay tables) was added 2026-11-20; logs made before then have a different `files` column |
| `data` | `0x8029F5E4`–`0x8029F608`, `0x8029F6D8`–`0x802A5300` | `0x8029F5E0` `gFrontBufferID` (flipped by the render thread); `0x8029F608`–`0x8029F6D8` `OSScTask` × 2 (render thread, scheduler) |
| `gfx` | `0x802A5300`–`0x802A5330`, `0x802A5368`–`0x802A5380`, `0x802A53BC`–`0x802A5550`, `0x802A5554`–`0x802A5A60` | `0x802A5330`–`0x802A5368` render and retrace queues; `0x802A5380`–`0x802A53BC` `gBufferEnds`, `gBackBufferID`, `gGfxWorkPtr`, `gMasterDisplayList` (buffer parity), `D_802A5394`/`D_802A5398` (render thread), RSP-done queue `D_802A53A0`; `0x802A5550` matrix cursor into gfx work (buffer parity) |
| — | — | `0x802A5A60`–`0x802AB200` `gThreadProcs` (render thread `OSThread` + stack), ThreadProc globals, retrace queues, `gThreadFrameCount`, `OSSched` + scheduler stack, ThreadProc manager thread + stack |
| `prc` | `0x802AB200`–`0x802AC3E0` | nothing (HuPrc lists and globals) |
| — | — | `0x802AC3E0`–`0x802AC400` controller message queue and buffer |
| `game` | `0x802AC400`–`0x802AD120`, `0x802AFC20`–`0x802B36D0` | `0x802AD120`–`0x802AFC20` audio globals, queues, audio thread + stack |
| `heap` | `0x802B36D0` up to the heap top | Above the top. The top is `bm64::debug::heap_top`: the heap mark rounded up to a page, at least `0x80400000` and at most `0x80500000`. Memory above it has never been handed out and is zero. Before 2026-11-22 the range ended at `0x80400000` |

- **Dead process stacks** are skipped in every range: memory below each suspended process's MIPS `sp`, the 16-byte o32 argument home at `sp`, and whole inline stacks that no live process uses. The live `sp` comes from the coroutine slot (`bm64::debug::get_process_stacks` in `process_coroutines.cpp`). Under the o32 ABI nothing the caller keeps lives below `sp + 0x10`. A callee may spill `a0`–`a3` into those 16 bytes, and a yield inside that call leaves audio-heap pointers and other scratch there. `new.bm64rec` SyncTest used to fail at tick 8448 on exactly that word: process 11 (`func_8023F26C`, suspended in `HuPrcSleep`) had `0` at `sp` on one pass and `0xB` on the other, with audio-heap pointers in the slots just below `sp`. The rest of the frame matched. `BM64_HASH_DEAD_STACKS=1` hashes the scratch anyway.
- **Snapshot, beyond the hash** (`src/game/bm64_state.cpp`): the hash ranges above, the file-loader lock `0x8029F580`–`0x8029F5E4` (the word just before `gFrontBufferID`; leaving the first pass's value makes the reload allocate from a different free-list bin), and the sound slot table `0x800BD770`–`0x800BD818`. It also keeps the ROM range last mapped at the sub-overlay slot `0x80043000` (from `recomp_load_overlays`) and calls `load_overlays` again on restore if it changed. RDRAM restore brings the old overlay's bytes back, but librecomp's function map would still point at the newer overlay's recompiled code. Arena 9 (scene `0x98`) failed depth 12 at tick 1893 that way: the forward pass left the results screen (overlay 17, mapped at tick 1798) for the arena (overlay 94, tick 1895), and the rerun of tick 1893 ran arena code. Snapshot version 3: the heap is stored up to the heap top, with the top written before it. Restore zeroes from the snapshot's top up to the live top, because at the snapshot's tick that memory had never been handed out. The post-BSS gap, `gGfxWork`, and the RSP/audio heap are not copied. Restoring audio globals `0x802AD120`–`0x802AFC20` without that heap sends the audio microcode to a bad jump target.
- **Still not hashed**: patch globals in `0x80801000+` (the boot timer, scroll-pending flag and restored overlay id are saved next to RDRAM instead). Freed malloc blocks are hashed as-is.

---

## 12. Recomp specifics

- **Patching**: MIPS C in `patches/*.c`, built with the clang MIPS target. `RECOMP_PATCH` replaces a function, `RECOMP_EXPORT` exports one to mods, and host functions are declared with `DECLARE_FUNC` (see `patches/patch_helpers.h`). `strict_patch_mode` is on, so patched names must exist in the symbols.
- **Host functions called from patches** also need a dummy address in `patches/syms.ld` (`0x8F000000+`; N64Recomp turns calls to those addresses into direct C calls). Without one, N64Recomp fails with "Undefined symbol … not found" and then "Failed to parse elf". The next free slot is `0x8F000194` (`bm64_camera_frame` is `0x8F000190`). `register_patches.cpp` includes the generated `RecompiledPatches/recomp_overlays.inl` and now depends on it (`OBJECT_DEPENDS` in `CMakeLists.txt`). Before that, a patch edit that moved functions linked a stale table, and the game stopped with "Failed to find function at 0x808…". A patch edit needs a second build before new host calls resolve, and `bm64.us.toml` changes need `N64Recomp.exe bm64.us.toml` before the C files rebuild.
- **`patches.h` renames**: `osRecvMesg`, `osContGetReadData`, `osGetTime` and friends become their `_recomp` host versions in patch code.
- **Hooks and instruction patches**: `bm64.us.toml` `[[patches.hook]]` injects C at an address. `[[patches.instruction]]` rewrites single instructions and can be generated by script at scale.
- **Mods**: the runtime supports mods with native libraries (`DynamicLibrary` in `lib/N64ModernRuntime/librecomp/src/mods.cpp`), but this project is a fork, so host code goes straight into `src/`.
- **Quicksave**: F5 saves and F7 loads through `bm64::state` (same session only; the fixed native stacks are not portable to another process). `src/game/quicksaving.cpp` stays `#if 0`. The old path only saved RDRAM and one thread context.
- **Leftovers**: `rom_decompression.cpp` (Star Fox 64) and `lib/sf64` are template leftovers; the BM64 ROM is loaded uncompressed.

---

## 13. Debug tooling (Phase 0)

Host side: `src/game/bm64_debug.cpp` (`include/bm64_debug.h`). Overlay UI: `src/ui/ui_debug_overlay.cpp`. Patch side: `patches/bm64_debug.h`, `patches/input_hooks.c`, plus hooks in the patched `func_80225840`.

- **Hooks** (`syms.ld` `0x8F00014C`–`0x8F000160`):

| Hook | When it runs |
|---|---|
| `bm64_debug_boot()` | First line of `func_80225840`, before the boot save read |
| `bm64_session_seed()` | The `g_initRandom` seed. Replay uses the recording; otherwise the host's session seed or a random one |
| `bm64_camera_bias(eye)` | Inside `func_80227CD0`, before `guLookAt`. Adds `BM64_CAMERA_BIAS` to the eye arguments |
| `bm64_debug_tick(gOverlayID)` | Top of every main-loop iteration |
| `bm64_cheats_allowed()` | Gates `Recomp_ProcessCheats` (off while recording, replaying, or when cheats are disabled for netplay) |
| `bm64_input_filter(pads)` | Inside `func_8023A104`, after the controller read |

- **Environment variables** (read once at boot; the full list is in `include/bm64_debug.h`):
  - `BM64_HASH_LOG=<path>` writes `tick,input_frame,overlay,total,<one column per range>` per tick (xxHash3).
  - `BM64_HASH_RANGES=<spec>` overrides the hashed ranges. The spec is comma-separated `[name:]start-end[+start-end...]`. The Phase 0 ranges, needed to compare with logs made before 2026-11-17, are `ovl:80043000-80063000,bss:80063000-800BEA60,main:8029F5E0-802B36D0,heap:802B36D0-80400000` together with `BM64_HASH_DEAD_STACKS=1`.
  - `BM64_HASH_DEAD_STACKS=1` also hashes dead process stack memory.
  - `BM64_DUMP_TICKS=0,1702,...` writes RDRAM `0x80000000`–`0x80800000` in N64 byte order to `BM64_DUMP_DIR` (default `dumps`) as `tick_<n>.bin`, plus `tick_<n>.mask` listing the dead stack ranges.
  - `BM64_RECORD=<path>` / `BM64_REPLAY=<path>` record or replay input. If both are set, the game shows an error and exits at startup (`bm64::debug::environment_error`, checked in `main`).
  - `BM64_SEED=<hex>` forces the RNG seed. Ignored while replaying, because the recording has its own seed. Netplay calls `bm64::session::set_seed` before boot instead of the clock.
  - `BM64_SANDBOX=1` keeps EEPROM in memory and does not flush it. `BM64_SANDBOX_IMAGE=<path>` starts from that image. `BM64_SANDBOX_EXPORT=<path>` writes the image on exit. A replay always sandboxes the recorded image, so the user's save is not written.
  - `BM64_CAMERA_BIAS=x,y,z` adds that offset to the eye passed into `func_80227CD0`. The camera block is not modified.
  - `BM64_DEBUG_OVERLAY=1` shows the debug overlay from startup (`tools/run-replay.ps1 -Overlay`).
  - `BM64_AUTOSTART=1` skips the launcher (`recompui::try_autostart_game`, called from the UI draw hook) if the ROM is valid.
  - `BM64_EXIT_ON_REPLAY_END=1` quits on the tick after the replay runs out of input; `BM64_EXIT_TICK=<n>` quits at tick n. Both close the hash log first.
  - `BM64_DIRECTOR=<hex scenes>` (needs `BM64_RECORD`) records an unattended coverage session: after `BM64_DIRECTOR_BOOT` ticks (default 600) it requests each scene in turn, `BM64_DIRECTOR_SECS` apart (default 60), then exits. Port 0 gets xorshift input seeded by `BM64_DIRECTOR_SEED` (hex): sticks −80..80, A a quarter of the time, B an eighth, each held 4–20 ticks. In menus (overlays 1–17) it pulses A every 30 ticks with the stick centred. Ports 1–3 are zero and only port 0 reports as connected. An arena goes through Battle setup (section 2). Scene `0x2E` gets entry `0x12`, every other scene `0x10`. Warping out of a running stage into the next one crashed (likely the heap; section 2), so `tools/run-synctest-scenes.ps1` boots once per scene.
  - `BM64_DIRECTOR_MENU=<tick:hexbuttons[:x:y],...>` replaces the input with these presses (each held 3 ticks, stick optional) up to the last one, then the normal director input resumes. With it set, an arena scene waits until overlay 13 (Battle setup) is up instead of warping there; the arena override still applies. `run-synctest-scenes.ps1 -ArenaMenus` uses `560:1000,820:1000,960:0:0:-80,1020:1000` with boot ticks 1100: Start during the title (overlay 9, from tick 509) opens overlay 11 (main menu, from tick 595); its cursor `0x802AD870` (copy at `0x80045ED8`) starts at 1 (Adventure) and moves with the stick only, not the D-pad; Start or A confirms once the menu is ready (around tick 960; earlier presses are ignored). Cursor 2 is Battle (overlay 13 at tick 1089); 1 goes to 16, 12 and then story; 3 went to 16 then 14; 4 went to 15.
  - `BM64_SYNCTEST_DUMP=<dir>` (with `BM64_SYNCTEST`): on a mismatch, writes `forward_<tick>.bin` for every frame of the window up to the failing one and `rerun_<tick>.bin` for the failing frame, each with a `.stacks` file (process id, stack bottom, `sp`). Keeping the images costs 8 MB per window frame.
  - `BM64_SYNCTEST=<n>` (`src/game/bm64_state.cpp`) runs each window as save, step, load, step, compare. Unset or `0` is off. `1`, or any non-numeric text that does not start with `0`, rolls back one tick. `2` through `12` save once, run that many ticks, load, and rerun those ticks, comparing the hash after each one. A value above 12 is clamped to 12, the same cap as `BM64_NET_ROLLBACK`, because the snapshot ring cannot roll back further in a session. Both passes skip the render-thread handoff. A replay reads the file again from the restored cursor; a live pad is reused from the sample stored on the forward pass. Exit prints `BM64 SYNCTEST PASS ticks N depth D` when every compared tick matched. A mismatch prints `BM64 SYNCTEST FAIL` and the ranges that differ, then exits 1. `BM64_SAVETEST` turns SyncTest off for that run.
- **Default hash ranges**: `ovl`, `bss`, `files`, `data`, `gfx`, `prc`, `game`, `heap`, `slots`, with OS, render-thread and audio objects and dead stacks left out. `slots` is `0x80F00000`–`0x80F01F20`: eight player slots, `g_ComputerPlayerMovement`, the `func_80241BB0` side table, sticky targets, category overrides, the per-player camera blocks (stride `0x2D0` at `0x80F00840`), and the `0x60`-byte co-op rule block (hidden flags, saved collision, party checkpoint, hearts, port map). A log from before the 2026-11-19 audit hashed a different `slots` span, so that column will not match a new replay. See "State hash ranges" in section 11.
- **Unattended replays**: `tools/run-replay.ps1 -Replay <file> -Out <dir> [-DumpTicks 0,1702] [-Ranges legacy|<spec>] [-TimeoutSec 600]` sets autostart, exit-on-replay-end, hash log and dumps, and kills the game if it is still running after the timeout. It writes `<dir>/hashes.csv`, `stdout.txt` and `dumps/`. A 9,591-tick replay takes about 165 s.
- **Coverage scripts**: `tools/run-director.ps1 -Scenes <hex list> -Out <dir> [-Secs 60] [-BootTicks 600] [-Seed <hex>]` records a director session (sandboxed save) to `<dir>/rec.bm64rec` with a hash log. `tools/run-synctest-coverage.ps1 -Scenes … -Out <dir> [-Depths 1,12]` records, then replays under SyncTest at each depth with `BM64_SYNCTEST_DUMP`, and writes `<dir>/summary.txt` (overlay ticks, PASS/FAIL lines). `tools/run-synctest-scenes.ps1 -Scenes … -Out <dir>` does that once per scene on a fresh boot and appends to `<dir>/results.txt`. Recordings stay under `runs/` (gitignored) because they embed the save.
- **Diagnosing a mismatch**: run two replays with `-DumpTicks` at the first differing tick from `compare_hashes.py`, then `dumpdiff.py`. To localise intermittent mismatches without guessing dump ticks, pass a `-Ranges` spec that splits one range into 4 KB columns.
- **Recording format** (little-endian): `"BM64INP\0"`, u32 version (2; replay also reads 1), u32 seed, u32 save_size, in version 2 u32 co-op players (0 = none, else 2–4) and u32 unlimited-lives flag, the save file bytes, then one 24-byte frame per `func_8023A104` call (4 × `{u16 button; s8 stick_x; s8 stick_y; u8 errno; u8 reserved}`). Port 0's reserved byte is the director's scene command (0 for none; `0xC0`–`0xC9` rewrites a pending arena request). Replay applies it on the same frame, and a SyncTest rerun that reuses stored pads applies it too. The boot-time read from `func_8023A22C` is the first frame. When a replay runs out, input goes live again and the overlay shows "REPLAY ENDED".
- **Replay**: `BM64_REPLAY` installs the recorded save with `bm64::session::begin_sandbox` and does not switch the save file. Controller Pak calls return `PFS_ERR_NOPACK` in every mode, and a sandbox forces that even if a pak implementation is added later. Only `osContGetReadData` data is replayed. The 16 status bytes at `0x802AC400` are not in the file; a replay writes them from which ports the recording actually moves or presses (port 0 always present, type 5, the others errno 8). Leaving the hardware query in place made the `game` hash follow whichever SDL devices that process had opened.
- **Floating point**: `RecompiledFuncs`, `PatchesLib`, `librecomp` and the executable are built with `-ffp-contract=off -fno-fast-math` (`bm64_deterministic_fp` in `CMakeLists.txt`; clang-cl gets the flags through `-Xclang`). The recompiled libraries are not built with `-march=nehalem` (that flag is only on the executable). x64 scalar float uses SSE2, so there is no x87 excess precision. A build that turns contraction back on, or enables fast-math, is not a supported cross-play peer. `tools/run-determinism.ps1` replays a recording twice, checks the user's save is unchanged, and replays once more with `BM64_CAMERA_BIAS`.
- **Overlay**: toggled with **F6**, or shown from startup with `BM64_DEBUG_OVERLAY=1`. It shows tick, input frame, loaded overlay ("4.10"), input mode, total and per-range hashes, main player, process count, RNG index, seed, camera eye and target, and slots P0–P4 (control type, hearts, bombs, fire, bomb type, position). It refreshes every 4 ticks and never captures input or the mouse, so game input keeps working.
  - When it works: from the moment the game starts (after "Start game" or autostart), including the boot intro, menus and gameplay. F6 does nothing on the launcher screen. The toggle shows or hides the panel immediately from the input thread; only the text refresh waits for game ticks. It renders like other RmlUi contexts (the UI draw hook renders whenever any context is shown). Menus opened afterwards (Esc) draw on top of it.
  - Logging: `[bm64 debug] Debug overlay created/shown/hidden`, and every 600 drawn UI frames a line with the panel's laid-out size and position, any visible document above it, and its first line of text (`recompui::note_debug_overlay_drawn`).
- **Reserved function keys** (verified 2026-11-17):
  - F1–F4: RT64 developer shortcuts (inspector, ray tracing, view RDRAM, texture replacements). These are active whenever the graphics config's `developer_mode` is true, which is this fork's default (`developer_mode_default` in `src/game/config.cpp`). RT64's SDL event filter swallows the key, so the game never sees it. F3 "view RDRAM" shows the native 4:3 framebuffer, which looks like an aspect-ratio switch.
  - F8: RmlUi debugger (only when the UI's debug mode is on).
  - F11 and Alt+Enter: fullscreen. Esc: in-game menu.
  - F5/F7: quicksave and quickload. They request a snapshot at the next main-loop step. Not checked from a focused window in the 2026-11-18 run.
  - Avoid F10 (Windows menu-bar key) and F12 (Windows debugger break key). F6 and F9 are otherwise free; F6 is the overlay.

---

## 14. Open questions

- Where the stage overlays test that the player reached the goal, and the death and transition flows. The main-code hero routines are described under "Main-player sequences".
- Which overlay functions are enemy processes, and how a process maps to its object (`Process.user_data`?).
- Contents of `0x800BEA60`–`0x80100000` and `0x801E56C0`–`0x80225800`. (The audio heap is `0x8019C2E0`–`0x801E56C0`.) SyncTest did not need them in the snapshot; they are still not mapped symbol by symbol.
- Mutability of the camera tables at `0x8029FEA0`–`0x802A004C`.
- World and boss identity of each map overlay (confirm by loading each map).
- Do the overlay words that store a sound handle (section 10) ever get used as an index into gameplay data, or only passed back to the sound allocator?
