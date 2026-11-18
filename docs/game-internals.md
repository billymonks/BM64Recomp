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

---

## 3. Threads, main loop and timing

- **Main loop** (verified): `func_80225840`, already re-implemented in `patches/required_patches.c`. One iteration is one game tick:
  0. `bm64_debug_tick()` (state hash, dumps), then the texture scroll `func_8022997C` if the previous tick submitted a frame (recomp: moved from the render thread, see section 9)
  1. `Recomp_ProcessCheats()` (recomp addition; calls `osContGetReadData` directly)
  2. `func_8023A104()`: wait for controller data and update input history (section 5)
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
  - The native stack is `max(stack_size, 512 KB)`, allocated by `mco_create` and freed by `mco_destroy`. Stacks are not at stable addresses across a process's death and re-creation (matters for savestates).
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

- **Raw pads**: `D_802AC410`, 4 × `OSContPad` (6 bytes: u16 buttons, s8 stick_x, s8 stick_y, u8 errno). Filled once per tick by `func_8023A104` through `osContGetReadData`. `osContStartReadData` is issued by `func_8023A208` with message queue `D_802AC3E0`.
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
- **Recomp limitation**: `recomp::get_n64_input` (`src/game/controls.cpp`) returns false for `controller_num != 0`, and `recomp::get_connected_device_info` (`src/game/input.cpp`) reports only port 0. Local multiplayer doesn't work until both are extended.
- **Cheats**: `Recomp_ProcessCheats` reads `osContGetReadData` itself and writes `g_Players[0]`, lives and credits. This must be disabled or synced in netplay.

---

## 6. Players (verified unless marked)

- **`g_Players`**: `0x800AEDF0`, stride `0xC8` (200 bytes). **Capacity is 5 slots (0–4)**: another table begins at `0x800AF1D8` (= slot 5) and is used by `func_802506B8`. `func_80243BF0` initialises slots 0–3; spawn loops cover 5.
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
  - Main-code functions that read it directly (27): `func_80243DEC`, `func_80243E60`, `func_802443A0`, `func_802450DC`, `func_80245280`, `func_802452E8`, `func_8024557C`, `func_802455A8`, `func_802456FC`, `func_802457D8`, `func_80245868`, `func_80245F74`, `func_80249AD4`, `func_8024A0DC`, `func_8024A824`, `func_8024D8C4`, `func_8025068C`, `func_802506B8`, `func_80250DC8`, `func_80250E2C`, `func_802510C8`, `func_802512C0`, `func_802512F8`, plus `func_80243BF0`, `func_80245CDC` and `func_8024585C` (writers).
- **Call-site statistics** for `getPlayerObj` in overlays (`argclass.py`): 115 pass a computed value (mostly `getMainPlayer()`), 49 pass constant 0, 31 constant 1, 5 constant 2, 1 constant 3, and the rest pass loop variables. 54 of 95 overlays call it.
- **Stage player setup** `func_80245CDC`:
  - Sets the main player to 0.
  - Allocates a 0x1000-byte stack and creates the player manager process `func_8024FFF0` (priority `0x70`).
  - Calls `g_SpawnPlayer(i)` (`0x802459BC`) for every slot 0–4 with controlType ≠ 0.
  - Then runs `func_80237664` (camera reset, likely) and `func_802362D4(0x1F)`, and initialises per-stage globals (`D_802AC8BC`, `D_802ACA24`, `D_802AC95C`, …).
  - If `g_LevelLoader & 4`, it allocates enemies with `g_EnemyAllocate(3, 0x1C, 0xD2)`.
  - **Consequence**: marking more slots human before stage setup spawns more bombers.
- **Per-bomber logic**: `func_8024E088(i)` (0x1F68 bytes, driven by `func_8024FFF0`). Human players read the port with their index through `func_80239880(i, …)`; AI reads `g_ComputerPlayerMovement` (`0x802ACF08`, stride `0x10`). Bomber object update: `func_802553D4` (0x71BC bytes).
- **Story maps that drive slots 1–3** (rival bombers or actors; call `setMainPlayer(1/2)` and/or `getPlayerObj(1..3)`): 4.10, 5.7, 6.15, 8.13 (slots 1, 2 and 3), 9.5, 9.6. Map 8.13 leaves one free slot under the 5-slot cap.
- **Main-player sequences**: `func_80250E2C` (called with argument 1 from about 30 map overlays; camera, sound and sleeps, keyed on `D_802AC894`) and `func_802506B8` (iterates per-player tables). These are candidates for goal, death or transition flows (likely).
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
- **Gameplay reads the camera**: bomber movement is camera-relative. `func_80251C88` reads the look-at point (called from `func_802553D4` and `func_802530A8`). The eye is also read by `func_80277F74`, `func_80277FC4`, `func_80278DBC`, `func_80278F68` (likely bomb and throw actions) and `func_8028A0B4`–`func_8028A2B0` (particles, likely billboards). **Camera state is therefore simulation state.**
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
- **Handles can still escape into hashed memory.** `func_8026724C` picks a libultra voice from the audio heap (`alSndpSetSound` / `alSndpStop` secure calls) and that id is what `g_PlayGlobalSound` returns. Stores of that return value, after the delay slot:
  - `func_80247848` writes it to `D_802AC9E0` (in the hashed `game` range). The same function later loads it and passes it to `func_8026BE80`, then stores -1. `func_80245CDC` initialises the word to -1.
  - Overlay stores: 10.2 `func_ovl10_2_8004B690` at `0x8004DCF8`; 11.1 `func_ovl11_1_80047CDC` at `0x8004D4E8`; 2.3 `func_ovl2_3_800442AC` at `0x80048878` and `func_ovl2_3_80045020` through `s1`; 8.7 `func_ovl8_7_800462DC` at `0x8004A2C8`.
  - `func_80267A18` (inside the sound allocator) shifts the id from `func_8026724C` and stores it at offset `0x14` of its channel struct.
  The branch in `func_8026BE80` is only "handle == -1". Which voice number came back does not change that branch, but the number itself sits in hashed RAM until it is cleared, so two machines can disagree on the hash for that window without disagreeing on gameplay. **Mitigation (phase 3.6):** `g_PlayGlobalSound` and `g_PlayLocalSound` return a deterministic virtual handle, and the host maps it to the real voice. Until then, a hash diff in `D_802AC9E0` or those overlay words during a sound is an audio-id difference, not a simulation one, unless a future caller starts indexing gameplay data with the handle.
- **Audio pointers on process stacks** (verified): sound calls leave audio-heap pointers in stack slots below the caller's `sp`. Their values depend on audio-heap allocation order, so they differ between runs.
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
| `0x800BEA60`–`0x80100000` | Unknown; referenced by gfx and model code | audit |
| `0x80100000`–`0x8014B000` | Framebuffers | no |
| `0x8014C8D0`–`0x8019A8D0` | gfx work (display lists) | no |
| `0x8019A8D0`–`0x80225800` | RSP stack, data loaded from ROM at `0x8019B0D0`, audio heap `0x8019C2E0`–`0x801E56C0`, RSP output buffer `0x801E56C0` | no (audio heap); rest audit |
| `0x80225800`–`0x8029F5E0` | Main code and rodata | no |
| `0x8029F5E0`–`0x802A5300` | Main data (gfx globals, tables) | yes, minus `gFrontBufferID` and the two `OSScTask`s |
| `0x802A5300`–`0x802B36D0` | Main BSS: gfx state, ThreadProc tables `0x802A5A60`, process lists, input, RNG, `g_LevelLoader` `0x802AC5D0`, audio `0x802AD120`–`0x802AFC20`, malloc bookkeeping `0x802B3620` | partial; see below |
| `0x802B36D0`–`0x80400000` | malloc heap (models, files, allocated stacks) | yes |
| `0x80801000`–`0x81000000` | Recomp patch code and data ("extram", `patches/patches.ld`) | patch globals that are game state (for example `texture_scroll_pending` in `required_patches.c`) |

### State hash ranges (verified 2026-11-17)

The default `BM64_HASH_RANGES` list. With it, two replays of the same recording hash identically on every tick. Each gap was found by diffing dumps from two replays (`dumpdiff.py`).

| Name | Included | Excluded inside or next to it, and why |
|---|---|---|
| `ovl` | `0x80043000`–`0x80063000` | nothing |
| `bss` | `0x80063000`–`0x800BD770`, `0x800BD818`–`0x800BE848` | `0x800BD770`–`0x800BD818` sound slot table (follows audio-thread state); `0x800BE848`–`0x800BEA60` pre-NMI thread queue, `OSThread` (host pointer) and stack |
| `data` | `0x8029F5E4`–`0x8029F608`, `0x8029F6D8`–`0x802A5300` | `0x8029F5E0` `gFrontBufferID` (flipped by the render thread); `0x8029F608`–`0x8029F6D8` `OSScTask` × 2 (render thread, scheduler) |
| `gfx` | `0x802A5300`–`0x802A5330`, `0x802A5368`–`0x802A5380`, `0x802A53BC`–`0x802A5550`, `0x802A5554`–`0x802A5A60` | `0x802A5330`–`0x802A5368` render and retrace queues; `0x802A5380`–`0x802A53BC` `gBufferEnds`, `gBackBufferID`, `gGfxWorkPtr`, `gMasterDisplayList` (buffer parity), `D_802A5394`/`D_802A5398` (render thread), RSP-done queue `D_802A53A0`; `0x802A5550` matrix cursor into gfx work (buffer parity) |
| — | — | `0x802A5A60`–`0x802AB200` `gThreadProcs` (render thread `OSThread` + stack), ThreadProc globals, retrace queues, `gThreadFrameCount`, `OSSched` + scheduler stack, ThreadProc manager thread + stack |
| `prc` | `0x802AB200`–`0x802AC3E0` | nothing (HuPrc lists and globals) |
| — | — | `0x802AC3E0`–`0x802AC400` controller message queue and buffer |
| `game` | `0x802AC400`–`0x802AD120`, `0x802AFC20`–`0x802B36D0` | `0x802AD120`–`0x802AFC20` audio globals, queues, audio thread + stack |
| `heap` | `0x802B36D0`–`0x80400000` | nothing |

- **Dead process stacks** are skipped in every range: memory below each suspended process's MIPS `sp`, and whole inline stacks that no live process uses. The live `sp` comes from the coroutine slot (`bm64::debug::get_process_stacks` in `process_coroutines.cpp`). Under the o32 ABI nothing valid lives below `sp`, but stale slots there hold audio-heap pointers that differ between runs. `BM64_HASH_DEAD_STACKS=1` hashes them anyway.
- **Not hashed and not yet audited**: `0x800BEA60`–`0x80100000`, `0x8019A8D0`–`0x8019C2E0`, and patch globals in `0x80801000+`. Freed malloc blocks are hashed as-is.

---

## 12. Recomp specifics

- **Patching**: MIPS C in `patches/*.c`, built with the clang MIPS target. `RECOMP_PATCH` replaces a function, `RECOMP_EXPORT` exports one to mods, and host functions are declared with `DECLARE_FUNC` (see `patches/patch_helpers.h`). `strict_patch_mode` is on, so patched names must exist in the symbols.
- **Host functions called from patches** also need a dummy address in `patches/syms.ld` (`0x8F000000+`; N64Recomp turns calls to those addresses into direct C calls). Without one, N64Recomp fails with "Undefined symbol … not found" and then "Failed to parse elf". The next free slot after the debug hooks is `0x8F000164`.
- **`patches.h` renames**: `osRecvMesg`, `osContGetReadData`, `osGetTime` and friends become their `_recomp` host versions in patch code.
- **Hooks and instruction patches**: `bm64.us.toml` `[[patches.hook]]` injects C at an address. `[[patches.instruction]]` rewrites single instructions and can be generated by script at scale.
- **Mods**: the runtime supports mods with native libraries (`DynamicLibrary` in `lib/N64ModernRuntime/librecomp/src/mods.cpp`), but this project is a fork, so host code goes straight into `src/`.
- **Quicksave**: `src/game/quicksaving.cpp` is disabled (`#if 0`). It only saved RDRAM and one thread context, which isn't enough with coroutines.
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
| `bm64_cheats_allowed()` | Gates `Recomp_ProcessCheats` (off while recording or replaying) |
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
- **Default hash ranges**: `ovl`, `bss`, `data`, `gfx`, `prc`, `game`, `heap`, with OS, render-thread and audio objects and dead stacks left out. See "State hash ranges" in section 11.
- **Unattended replays**: `tools/run-replay.ps1 -Replay <file> -Out <dir> [-DumpTicks 0,1702] [-Ranges legacy|<spec>] [-TimeoutSec 600]` sets autostart, exit-on-replay-end, hash log and dumps, and kills the game if it is still running after the timeout. It writes `<dir>/hashes.csv`, `stdout.txt` and `dumps/`. A 9,591-tick replay takes about 165 s.
- **Diagnosing a mismatch**: run two replays with `-DumpTicks` at the first differing tick from `compare_hashes.py`, then `dumpdiff.py`. To localise intermittent mismatches without guessing dump ticks, pass a `-Ranges` spec that splits one range into 4 KB columns.
- **Recording format** (little-endian): `"BM64INP\0"`, u32 version (1), u32 seed, u32 save_size, the save file bytes, then one 24-byte frame per `func_8023A104` call (4 × `{u16 button; s8 stick_x; s8 stick_y; u8 errno; u8 reserved}`). The boot-time read from `func_8023A22C` is the first frame. When a replay runs out, input goes live again and the overlay shows "REPLAY ENDED".
- **Replay**: `BM64_REPLAY` installs the recorded save with `bm64::session::begin_sandbox` and does not switch the save file. Controller Pak calls return `PFS_ERR_NOPACK` in every mode, and a sandbox forces that even if a pak implementation is added later. Only `osContGetReadData` data is replayed. Controller status queries (`osContStartQuery`/`osContGetQuery`) stay live, so keep the same controllers connected.
- **Floating point**: `RecompiledFuncs`, `PatchesLib`, `librecomp` and the executable are built with `-ffp-contract=off -fno-fast-math` (`bm64_deterministic_fp` in `CMakeLists.txt`; clang-cl gets the flags through `-Xclang`). The recompiled libraries are not built with `-march=nehalem` (that flag is only on the executable). x64 scalar float uses SSE2, so there is no x87 excess precision. A build that turns contraction back on, or enables fast-math, is not a supported cross-play peer. `tools/run-determinism.ps1` replays a recording twice, checks the user's save is unchanged, and replays once more with `BM64_CAMERA_BIAS`.
- **Overlay**: toggled with **F6**, or shown from startup with `BM64_DEBUG_OVERLAY=1`. It shows tick, input frame, loaded overlay ("4.10"), input mode, total and per-range hashes, main player, process count, RNG index, seed, camera eye and target, and slots P0–P4 (control type, hearts, bombs, fire, bomb type, position). It refreshes every 4 ticks and never captures input or the mouse, so game input keeps working.
  - When it works: from the moment the game starts (after "Start game" or autostart), including the boot intro, menus and gameplay. F6 does nothing on the launcher screen. The toggle shows or hides the panel immediately from the input thread; only the text refresh waits for game ticks. It renders like other RmlUi contexts (the UI draw hook renders whenever any context is shown). Menus opened afterwards (Esc) draw on top of it.
  - Logging: `[bm64 debug] Debug overlay created/shown/hidden`, and every 600 drawn UI frames a line with the panel's laid-out size and position, any visible document above it, and its first line of text (`recompui::note_debug_overlay_drawn`).
- **Reserved function keys** (verified 2026-11-17):
  - F1–F4: RT64 developer shortcuts (inspector, ray tracing, view RDRAM, texture replacements). These are active whenever the graphics config's `developer_mode` is true, which is this fork's default (`developer_mode_default` in `src/game/config.cpp`). RT64's SDL event filter swallows the key, so the game never sees it. F3 "view RDRAM" shows the native 4:3 framebuffer, which looks like an aspect-ratio switch.
  - F8: RmlUi debugger (only when the UI's debug mode is on).
  - F11 and Alt+Enter: fullscreen. Esc: in-game menu.
  - F5/F7: quicksave and quickload, currently compiled out (`#if 0` in `src/game/input.cpp`).
  - Avoid F10 (Windows menu-bar key) and F12 (Windows debugger break key). F6 and F9 are otherwise free; F6 is the overlay.

---

## 14. Open questions

- Exact meaning of `func_80250E2C`, `func_802506B8`, `func_80250DC8` and the death, goal and transition flows.
- Which overlay functions are enemy processes, and how a process maps to its object (`Process.user_data`?).
- Contents of `0x800BEA60`–`0x80100000` and `0x8019B0D0`–`0x8019C2E0`, `0x801E56C0`–`0x80225800`. (The audio heap is `0x8019C2E0`–`0x801E56C0`.)
- Mutability of the camera tables at `0x8029FEA0`–`0x802A004C`.
- Which per-player companion arrays besides `g_ComputerPlayerMovement` must grow with the player array.
- World and boss identity of each map overlay (confirm by loading each map).
- Do the overlay words that store a sound handle (section 10) ever get used as an index into gameplay data, or only passed back to the sound allocator?
