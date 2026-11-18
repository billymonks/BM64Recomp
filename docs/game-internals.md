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
| `compare_hashes.py` | Compares two `BM64_HASH_LOG` files and reports the first differing tick per range |
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
  1. `Recomp_ProcessCheats()` (recomp addition; calls `osContGetReadData` directly)
  2. `func_8023A104()`: wait for controller data and update input history (section 5)
  3. `Gfx_InitGfx()`
  4. `ThreadProc_RunQueuedThreads()` and `func_802381F8()` (start or yield to queued OS threads; in practice the render thread)
  5. `HuPrcCall()`: run all Hudson processes (all gameplay logic)
  6. `func_8022787C()`: per-framebuffer projection and viewport
  7. `func_802290CC()`: draw pass, which also advances animations (section 9)
  8. `func_8025E1D4()` (soft-reset effect), then `func_8026C208()` (audio housekeeping)
  9. `func_8023A208()`: `osContStartReadData` for the next tick
  10. `Gfx_EndRender()`: finish the display list, hand it to the render thread, then `func_80237F44()` blocks until the next vertical blank
- **Tick rate**: 60 Hz (verified). The loop waits for exactly one retrace through `ThreadProc_RunQueues`, and the recomp sets `gEXSetRefreshRate(60)`.
- **Fixed timestep** (verified): `HuPrcCall` decrements process sleep timers by exactly 1 per tick, in both the original and the patched version. `gThreadFrameCount` and `ThreadProc_GetCurrentFrameCount` (`0x80237F34`) have no callers or pointer references.
- **Clocks** (verified): the only clock read in main code is `g_initRandom(osGetTime())` at boot. No overlay calls `osGetTime`, `osGetCount`, `osCreateThread`, `ThreadProc_Add`, EEPROM, Controller Pak or controller functions.
- **OS threads**: game thread (id 3, runs the main loop and all processes), render thread `Gfx_Render` (the only thread created through `ThreadProc_Add`, from `Gfx_CreateRenderThread`), the ThreadProc manager `ThreadProc_RunQueues` (retrace fan-out), plus libultra VI, PI, scheduler and audio threads. Thread names come from `zelda64::get_game_thread_name` in `src/main/main.cpp`.

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
- **Seeding**: once, in `func_80225840`, from `osGetTime()`. About 115 call sites across main code and overlays.

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
- **Draw pass** `func_802290CC`: for each enabled context it walks model hierarchies through `func_802289C0`, calling per-node callbacks with `jalr` and `func_8022D58C` (matrix stack using `D_802A5418`). **It advances animations** (the recomp authors note that stubbing it freezes animation), so it mutates simulation state. Whether any callback culls by camera hasn't been checked yet.
- **Render thread** `Gfx_Render` (patched) receives buffers through `D_802A5330` and builds scheduler tasks. The patches already contain `startGfxWait`/`waitForGfxFinish` (wait for display-list completion).

---

## 10. Sound and saves

- **Sound API**: `g_PlayGlobalSound` (`0x8026C660`), `g_PlayLocalSound` (`0x8026C51C`), `g_StopSound` (`0x8026C0A8`). They return and accept sound handles. Per-tick housekeeping: `func_8026C208`. Volume scaling patches live in `required_patches.c`. Whether gameplay reads sound status (for example waiting for a sound to end) hasn't been audited.
- **Saves**: EEPROM, main code only. `func_80292DB8` reads, `func_80292E90` writes. `func_80294E54` probes the EEPROM and **reads the save** at boot, before the RNG is seeded (verified). Controller Pak menus exist in 2.6 and 3.6.
- **Runtime save handling**: the save buffer lives in librecomp (`lib/N64ModernRuntime/librecomp/src/pi.cpp`). `ultramodern::change_save_file(subfolder, name)` switches to `saves/<subfolder>/<name>.bin` and reloads it; replays use this to run against a copy.

---

## 11. RDRAM map (approximate)

| Range | Contents | Savestate |
|---|---|---|
| `0x80000400`–`0x8001BFC0` | Boot code | no (code) |
| `0x8001BFC0`–`0x80028460` | Boot BSS: OS threads and stacks, `gGameThread`, `gSectionSizes` | no (OS) |
| `0x80042000` | zerojump secure-call table | no |
| `0x80043000`–`0x80063000` | Resident sub-overlay code, data and BSS (largest overlay is `0xE320`) | data/BSS yes |
| `0x80063000`–`0x800BEA60` | Main game BSS, zeroed at boot: process pool `0x80063370`, camera `0x8008E648…`, `g_Players` `0x800AEDF0`, model and anim work around `0x80098C18`/`0x800A0CF8` | yes (audit for OS objects) |
| `0x800BEA60`–`0x80100000` | Unknown; referenced by gfx and model code | audit |
| `0x80100000`–`0x8014B000` | Framebuffers | no |
| `0x8014C8D0`–`0x8019A8D0` | gfx work (display lists) | no |
| `0x8019A8D0`–`0x80225800` | RSP stack, data loaded from ROM at `0x8019B0D0`, model/file region around `0x801A0000`, RSP output buffer `0x801E56C0` | audit |
| `0x80225800`–`0x8029F5E0` | Main code and rodata | no |
| `0x8029F5E0`–`0x802A5300` | Main data (gfx globals, tables) | partial |
| `0x802A5300`–`0x802B36D0` | Main BSS: gfx state, ThreadProc tables `0x802A5A60`, process lists, input, RNG, `g_LevelLoader` `0x802AC5D0`, malloc bookkeeping `0x802B3620` | partial |
| `0x802B36D0`–`0x80400000` | malloc heap (models, files, allocated stacks) | yes |
| `0x80801000`–`0x81000000` | Recomp patch code and data ("extram", `patches/patches.ld`) | patch globals that are game state |

---

## 12. Recomp specifics

- **Patching**: MIPS C in `patches/*.c`, built with the clang MIPS target. `RECOMP_PATCH` replaces a function, `RECOMP_EXPORT` exports one to mods, and host functions are declared with `DECLARE_FUNC` (see `patches/patch_helpers.h`). `strict_patch_mode` is on, so patched names must exist in the symbols.
- **Host functions called from patches** also need a dummy address in `patches/syms.ld` (`0x8F000000+`; N64Recomp turns calls to those addresses into direct C calls). Without one, N64Recomp fails with "Undefined symbol … not found" and then "Failed to parse elf". The next free slot after the debug hooks is `0x8F000160`.
- **`patches.h` renames**: `osRecvMesg`, `osContGetReadData`, `osGetTime` and friends become their `_recomp` host versions in patch code.
- **Hooks and instruction patches**: `bm64.us.toml` `[[patches.hook]]` injects C at an address. `[[patches.instruction]]` rewrites single instructions and can be generated by script at scale.
- **Mods**: the runtime supports mods with native libraries (`DynamicLibrary` in `lib/N64ModernRuntime/librecomp/src/mods.cpp`), but this project is a fork, so host code goes straight into `src/`.
- **Quicksave**: `src/game/quicksaving.cpp` is disabled (`#if 0`). It only saved RDRAM and one thread context, which isn't enough with coroutines.
- **Leftovers**: `rom_decompression.cpp` (Star Fox 64) and `lib/sf64` are template leftovers; the BM64 ROM is loaded uncompressed.

---

## 13. Debug tooling (Phase 0)

Host side: `src/game/bm64_debug.cpp` (`include/bm64_debug.h`). Overlay UI: `src/ui/ui_debug_overlay.cpp`. Patch side: `patches/bm64_debug.h`, `patches/input_hooks.c`, plus hooks in the patched `func_80225840`.

- **Hooks** (`syms.ld` `0x8F00014C`–`0x8F00015C`):

| Hook | When it runs |
|---|---|
| `bm64_debug_boot()` | First line of `func_80225840`, before the boot save read |
| `bm64_session_seed(default)` | Wraps the `g_initRandom` seed |
| `bm64_debug_tick(gOverlayID)` | Top of every main-loop iteration |
| `bm64_cheats_allowed()` | Gates `Recomp_ProcessCheats` (off while recording or replaying) |
| `bm64_input_filter(pads)` | Inside `func_8023A104`, after the controller read |

- **Environment variables** (read once at boot):
  - `BM64_HASH_LOG=<path>` writes `tick,input_frame,overlay,total,ovl,bss,main,heap` per tick (xxHash3).
  - `BM64_HASH_RANGES=80063000-800BEA60,...` overrides the hashed ranges.
  - `BM64_RECORD=<path>` / `BM64_REPLAY=<path>` record or replay input.
  - `BM64_SEED=<hex>` forces the RNG seed.
- **Default hash ranges**: `ovl` `0x80043000`–`0x80063000`, `bss` `0x80063000`–`0x800BEA60`, `main` `0x8029F5E0`–`0x802B36D0`, `heap` `0x802B36D0`–`0x80400000`. `main` contains OS, gfx and audio objects and may vary with thread timing; refine during 3.1.
- **Recording format** (little-endian): `"BM64INP\0"`, u32 version (1), u32 seed, u32 save_size, the save file bytes, then one 24-byte frame per `func_8023A104` call (4 × `{u16 button; s8 stick_x; s8 stick_y; u8 errno; u8 reserved}`). The boot-time read from `func_8023A22C` is the first frame. When a replay runs out, input goes live again and the overlay shows "REPLAY ENDED".
- **Replay limits**: only `osContGetReadData` data is replayed. Controller status queries (`osContStartQuery`/`osContGetQuery`) and pak detection are live, so keep the same controllers connected.
- **Overlay**: F3 in-game. It shows tick, input frame, loaded overlay ("4.10"), input mode, total and per-range hashes, main player, process count, RNG index, seed, camera eye and target, and slots P0–P4 (control type, hearts, bombs, fire, bomb type, position). It refreshes every 4 ticks and never captures input.

---

## 14. Open questions

- Do any draw callbacks in `func_802290CC` skip offscreen objects (camera-dependent simulation)?
- Does any gameplay logic read sound state, so that audio timing could affect simulation?
- Exact meaning of `func_80250E2C`, `func_802506B8`, `func_80250DC8` and the death, goal and transition flows.
- Which overlay functions are enemy processes, and how a process maps to its object (`Process.user_data`?).
- Contents of `0x800BEA60`–`0x80100000` and `0x8019B0D0`–`0x80225800`; where the audio heap lives.
- Mutability of the camera tables at `0x8029FEA0`–`0x802A004C`.
- Which per-player companion arrays besides `g_ComputerPlayerMovement` must grow with the player array.
- World and boss identity of each map overlay (confirm by loading each map).
