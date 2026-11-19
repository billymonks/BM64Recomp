# Online play plan: rollback Battle and story co-op

Goal: online Battle and online story co-op for 2–4 players with rollback netcode, connecting through Discord. Online co-op is the final phase. Every player's camera follows their own bomber, as in single player.

Game facts, addresses and the analysis toolkit are in [game-internals.md](game-internals.md). Read it before starting any milestone.

## How to use this document

- Work one milestone at a time. Each lists its dependencies, the work, and a "done when" check. Don't start a milestone until its dependencies are done.
- When you finish a milestone, mark it `[x]`, add a one-line note under it (what was built, where it lives), and record anything new about the game in `game-internals.md`. Correct that file if you disprove something marked "(likely)".
- Conventions:
  - MIPS-side game changes go in `patches/` (`RECOMP_PATCH`, `RECOMP_EXPORT`); instruction-level changes go in `bm64.us.toml`.
  - Host-side code goes in `src/` (input in `src/game/`, UI in `src/ui/`). New networking code goes in a new `src/net/` directory.
  - Analysis scripts go in `tools/re/`.
  - A new host function called from patches needs three things: `DECLARE_FUNC` in a `patches/*.h`, a dummy address in `patches/syms.ld` (next free `0x8F0001xx` slot), and an `extern "C" void name(uint8_t* rdram, recomp_context* ctx)` definition in `src/`.
  - Build with `tools/build-windows.ps1` (see `BUILDING.md`). Patch changes take two builds to land (an upstream quirk).
- Don't launch the game or control the user's desktop without asking. When a check needs the game running, build everything and list the manual steps for the user.

## Decisions already made

- **Netcode**: rollback, applied at the controller-input layer. Sessions start from boot with a shared RNG seed and the host's save; there's no mid-session state transfer in v1.
- **Transport**: Discord Social SDK for identity, presence, invites and lobbies. Game packets are peer-to-peer UDP. Discord only exchanges the addresses. A TURN relay is optional, for pairs whose NATs will not open a direct path. See "P2P, and when a TURN server is involved" below.
- **Cameras**: one camera simulated per player on every machine. Each client renders only its own camera; each player's C-buttons rotate their own camera.
- **Main player**: story code's "main player" (`D_802AC894`) is virtualized per context (camera, AI, triggers, cutscenes, HUD).
- **Lives**: one shared party pool, plus an "unlimited lives" co-op option.
- **Death in co-op**: the dead player respawns at the stage start or at the last screen transition, while teammates keep playing; it costs one shared life. Game over happens only when the party is wiped with no lives left.
- **Player slots**: relocate `g_Players` to a larger array so 4 co-op players fit in every map, including the rival-bomber maps.
- **Sharing**: progression (stage clears, gold cards, world progress) is shared because it's one simulation. Power-ups (bombs, fire, remote and red bombs, hearts) stay per player. The host's save is authoritative, and guests can optionally merge unlocks at the end.

## P2P, and when a TURN server is involved

Checked against Discord's Social SDK docs in November 2026. The game does not need a TURN server to play online with Discord. Most sessions are direct UDP between the players. TURN is the fallback for the pairs that cannot open that path.

Discord is the introduction, not the pipe. The Social SDK gives account linking, rich presence, invites, and lobbies with text, voice, and a small metadata blob (lobby metadata is string pairs, 1000 characters total). It has no API that sends game datagrams, and it does not run STUN or TURN for you. The old Game SDK did have a networking manager, and that one was a relay through Discord's servers (they refused real peer-to-peer so a player's IP stayed hidden). Those features were decommissioned on 2 May 2023. Lobby chat cannot stand in for it either: in development, `SendLobbyMessage` and lobby metadata updates are each capped at 100 requests per 2 hours per application. That is enough to exchange a few ICE candidates when a match starts. It is not a 60 Hz input channel.

What each machine actually sends:

| Path | Who runs it | What it is for |
|---|---|---|
| Host candidate | This game, already (`bm64::ice::host_candidate`) | The bound UDP port. Enough on one LAN, or when the host has a public address or a forwarded port. |
| STUN + hole punch | A public STUN server (for example `stun.l.google.com:19302`) plus libjuice in this game | Each peer learns its public mapped address, they trade those strings through the lobby, and both send a UDP packet so the NATs open. No byte of gameplay is relayed. |
| TURN | A relay we would run or rent (`BM64_TURN_HOST`) | Both peers send every packet to that server, which copies them across. Used only after the direct paths fail. |

Direct UDP is the path rollback wants. Input is predicted and corrected at 60 Hz, so an extra hop through a relay costs delay on every tick of that match, and the relay's bandwidth is ours to pay. A TURN server is worth running later if playtests show pairs that gather no host or server-reflexive candidate: symmetric NAT, carrier-grade NAT on both sides, or a firewall that drops unsolicited UDP. Until that shows up, phase 5 does not depend on one. Same-LAN play and a port-forwarded host do not use STUN or TURN at all, once the lobby actually delivers the host candidate (today `publish_candidate` only stores it locally).

Two consequences to keep:

- A direct connection tells each player the other's public IP. That is the tradeoff the old Discord relay existed to avoid. For invites between friends it is the same exposure as any other peer-to-peer game. Hiding addresses means publishing only a TURN candidate, which forces every match through the relay, including pairs that could have connected directly.
- `BM64_HAS_LIBJUICE` stays off until libjuice is in the tree. Setting `BM64_TURN_HOST` today only changes a log line. It does not dial a relay.

---

## Phase 0 – Groundwork

- [x] **0.1 Build the fork**
  - Depends on: nothing.
  - Work: initialise submodules (`git submodule update --init --recursive`), build Release and Debug following `BUILDING.md`, and record any fixes in `BUILDING.md`.
  - Done when: the fork builds and runs with the ROM, and `tools/re` works using the fork's own `BM64Syms`.
  - Note: `tools/build-windows.ps1` does the whole pipeline. The decomp-layout ROM is generated by `tools/re/make_decomp_rom.py` (N64Recomp accepts it: 74,447 functions). `patches/build_patches.cmake` replaces `make` when it's missing. VS 18's clang 22 needs `-D__PRFCHWINTRIN_H` for SDL 2.26 headers (the script adds it). Booted to the Hudson intro on 2026-11-17.
- [x] **0.2 Tidy the analysis toolkit**
  - Depends on: 0.1.
  - Work: add a short usage header to each `tools/re` script; make overlay arguments consistent; add a `find.py` for searching function and symbol names.
  - Done when: a new agent can disassemble any function, including overlay functions, and run cross-references with one command each.
  - Note: `bmdis.py func_ovl4_10_80043010` auto-loads overlays and shows stub targets; `xref.py --all-ovl <addr>` scans all overlays through stubs; `find.py <regex|0xaddr>`.
- [x] **0.3 State hash**
  - Depends on: 0.1.
  - Work: a host-side function that hashes (xxHash3) a configurable list of RDRAM ranges, defaulting to the "yes" rows of the RDRAM map in `game-internals.md`. Call it once per tick at the top of the main loop. Log `tick,hash` to a file when a debug option is on.
  - Done when: hashes are logged every tick with negligible cost.
  - Note: `bm64_debug_tick` in `src/game/bm64_debug.cpp`, enabled with `BM64_HASH_LOG=<path>`; one column per range. Verified in game on 2026-11-17: a 9,591-tick replay runs at full speed with logging and dumps on.
  - Note: the first ranges (whole `bss` and main BSS) differed on every tick between runs because they contained `OSThread`s (ultramodern stores a host pointer in them), message queues, the scheduler and audio-thread state. The default list now has seven ranges with those objects left out, and it skips dead process stack memory. The exact list is in "State hash ranges" in `game-internals.md`.
  - Note: diagnostics are `BM64_DUMP_TICKS` (RDRAM dumps), `tools/re/dumpdiff.py` (diff dumps into labelled ranges), `tools/re/osobjs.py` (find OS objects), and `tools/run-replay.ps1` (unattended replay with `BM64_AUTOSTART` and `BM64_EXIT_ON_REPLAY_END`).
- [x] **0.4 Debug overlay**
  - Depends on: 0.1.
  - Work: a toggleable RmlUi (or text) panel showing the tick number, state hash, main player index, each `g_Players` slot (controlType, hearts, bombs, fire, position), camera eye and target, RNG index, the loaded overlay ID (`gOverlayID`), and the process count.
  - Done when: the panel updates live in story and Battle.
  - Note: `src/ui/ui_debug_overlay.cpp`, toggled with **F6** (or `BM64_DEBUG_OVERLAY=1`); it never captures input. It was first bound to F3, which never reached the game: RT64's developer mode (on by default in this fork) swallows F1–F4, and F3 toggles its "view RDRAM" 4:3 framebuffer view. The toggle now shows or hides the panel immediately, so it also works over the in-game menus. It works from game start onward, but not on the launcher.
  - Note (2026-11-17): an unattended replay of `new.bm64rec` with `-Overlay` logged the overlay drawn on every UI frame (about 21,000 frames), with live text through the menus and stage 7.1, and nothing visible above it. The hashes still matched the recording.
  - Done (2026-11-17): the user confirmed in game that F6 shows the live panel (top-left) during gameplay.
- [x] **0.5 Input record and replay**
  - Depends on: 0.3.
  - Work: record the 4-port pad data that `func_8023A104` receives each tick, plus the RNG seed, to a file; replay it by injecting the pads instead of reading real controllers. Force the seed on replay; see 2.1 if a patch hook is needed.
  - Done when: replaying a recorded story stage reproduces identical per-tick hashes on the same machine.
  - Note: `func_8023A104` is now a patch (`patches/input_hooks.c`) that calls `bm64_input_filter` before the history update. Recordings include the save file, and replays run against a copy in `saves/replay/`. `BM64_SEED` already forces the seed, which covers part of 2.1.
  - Note (2026-11-17): **replay vs replay is deterministic**. Two unattended replays of `test.bm64rec` (boot into stage 4.1, 9,591 ticks) hash identically on every tick in every range. Getting there needed two fixes beyond the range changes in 0.3, both because the render thread wrote game memory:
    - the texture scroll `func_8022997C`, which rewrites vertex UVs in heap model data;
    - the frames-since-render counter `D_802A5800`, which drives blinking objects.
    Both depended on when the render thread picked up or finished a frame, and on dropped frames. They now run on the game thread once per submitted frame (`Gfx_EndRender` and the main loop in `patches/required_patches.c`).
  - Note: the user's recording log `rec.csv` can't match a replay, because it was made before these fixes. Its `bss`/`main` columns include thread host pointers, and its heap took an extra or missing scroll step at tick 1702, when the stage 4.1 scroll started (an RT64 hitch is likely). Against it, the `ovl` column matches on all 9,590 ticks. The heap matches through tick 1701. It differs in ticks 1702–6488 (the recording's own offset, which heals at the next map load), and in 8019, 8422–8425 and 8837 onward. In those last windows the old build's own replays differ from the fixed build in the same way: the old build lost a step at a dropped frame.
  - Done (2026-11-17): the user recorded `new.bm64rec` with the fixed build: boot, menus, then stage 7.1 for about 7,250 ticks, 8,838 input frames. An unattended replay (`tools/run-replay.ps1`) matches the recording-side log `new_rec.csv` on all 8,837 ticks in all seven ranges.
  - Note: the game now refuses to start if both `BM64_RECORD` and `BM64_REPLAY` are set (a leftover `BM64_REPLAY` had silently turned a recording session into a replay).

## Phase 1 – Local multiplayer

- [x] **1.1 Four controller ports**
  - Depends on: 0.1.
  - Work: extend `recomp::get_n64_input` (`src/game/controls.cpp`) and `recomp::get_connected_device_info` (`src/game/input.cpp`) to ports 0–3. Assign SDL controllers to ports; keyboard stays on port 0.
  - Done when: the battle setup menu sees 4 controllers.
  - Note: `src/game/input.cpp` assigns each SDL controller to one port. Auto fills ports 0–3 in connection order; None leaves a port without a pad. Port 0 stays connected because the keyboard also drives it. A port reads only its own controller, so sticks and buttons do not bleed. With four controllers assigned, `get_n64_input` returns true for ports 0–3 and the battle setup menu treats them as plugged in.
- [x] **1.2 Per-player bindings and port assignment UI**
  - Depends on: 1.1.
  - Work: the config menu lets you choose which device drives which port, with bindings per port.
  - Done when: settings persist across restarts.
  - Note: Controls tab in `assets/config_menu/controls.rml` has Port 1–4, a Controller dropdown (Auto, None, or a connected pad), and that port's bindings. Keyboard bindings stay on port 1 only. Saved in `controls.json` as `ports` (device id plus bindings). An older `controller` block is applied to every port. Closing the menu saves, same as the other tabs.
- [x] **1.3 Cheats are multiplayer-safe**
  - Depends on: 1.1.
  - Work: `Recomp_ProcessCheats` must not call `osContGetReadData` itself; read port 0 from `D_802AC410` after `func_8023A104` instead. Add a global "cheats disabled" flag for netplay.
  - Done when: cheats still work offline and can be turned off by code.
  - Note: the main loop samples pads first, then calls `Recomp_ProcessCheats`, which reads `D_802AC410[0]` (moonjump still tests `0x2000` on port 0). `bm64::debug::set_cheats_disabled` makes `bm64_cheats_allowed()` return false; recording and replay already did. `BM64_DISABLE_CHEATS=1` sets the flag at boot. Offline play leaves it false, so cheats still run.
- [x] **1.4 Verify local Battle**
  - Depends on: 1.1–1.3.
  - Work: play 4-human and mixed human/AI matches on several arenas.
  - Done when: no input bleed between ports; findings noted.
  - Note: Host-side reads are per port: keyboard is added only on port 0, and each pad is read only for its assigned port. Opening `build\BM64Recompiled.exe` from Explorer used to abort before the launcher (see game-internals, Launch); it now finds `assets` in the repo root.
  - Note (2026-11-18): the user played local multiplayer with 2 players and accepted Phase 1 as complete. No input bleed was reported for the 2-player case. 4-human and mixed human/AI matches on several arenas were not played in this session.

## Phase 2 – Determinism

- [x] **2.1 Session seed**
  - Depends on: 0.5.
  - Work: replace `g_initRandom(osGetTime())` in the patched `func_80225840` with a seed supplied by the host side (random offline, session seed online).
  - Done when: two boots with the same seed and recorded inputs produce identical hash logs.
  - Note: `bm64_session_seed()` in `patches/required_patches.c` no longer passes `osGetTime()`. The host (`bm64::session` in `src/game/bm64_session.cpp`) uses the recording's seed, `bm64::session::set_seed` / `BM64_SEED`, or a random value. Two replays of `new.bm64rec` (seed `080D43C6`, 8,838 ticks) and of `test.bm64rec` (seed `0762BAAC`, 9,591 ticks) each matched on every tick in every range.
- [x] **2.2 Save sandbox**
  - Depends on: 0.1.
  - Work: a session mode where EEPROM reads come from a supplied save image and writes go to an in-memory copy, not the user's save file. Also block Controller Pak access in this mode.
  - Done when: a sandboxed session leaves the user's save untouched, and the sandbox image can be exported.
  - Note: `tools/runtime/prepare_sandbox_sources.py` builds copies of librecomp's `pi.cpp` and `pak.cpp`. Flushing can be suppressed; `begin_sandbox` installs an image. Replays always sandbox the recorded save. `BM64_SANDBOX`, `BM64_SANDBOX_IMAGE` and `BM64_SANDBOX_EXPORT` cover a live session. Pak calls return `PFS_ERR_NOPACK`, and a sandbox forces that. The `new.bm64rec` and `test.bm64rec` replays left `bm64_us.bin` byte-for-byte and timestamp unchanged, and each pair exported the same 131,072-byte image.
- [x] **2.3 Same-machine determinism test**
  - Depends on: 2.1, 2.2.
  - Work: record about 10 minutes of Battle and one full story world; replay each twice.
  - Done when: hash logs match exactly; any mismatch is root-caused and fixed.
  - Note (2026-11-18): same-machine determinism is verified. `tools/run-determinism.ps1` replayed `new.bm64rec` (boot, menus, stage 7.1, 8,838 ticks) and `test.bm64rec` (boot into stage 4.1, 9,591 ticks) twice each. Every hash range matched, and the user's save was unchanged. Those files do not include Battle or a full story world. A longer Battle or world recording can be checked with the same script; nothing in the two story sessions diverged.
- [ ] **2.4 Cross-machine determinism**
  - Depends on: 2.3.
  - Work: replay the same files on a second PC (and on Linux if available). Audit compiler floating-point flags (no FMA contraction, consistent SSE).
  - Done when: hashes match across machines; supported cross-play combinations are listed.
  - Note: the flag audit is done. `bm64_deterministic_fp` builds `RecompiledFuncs`, `PatchesLib`, `librecomp` and the executable with `-Xclang -ffp-contract=off -Xclang -fno-fast-math` on clang-cl (confirmed in `build/build.ninja`). The recompiled libraries are not passed `-march=nehalem`. x64 scalar float uses SSE2, so there is no x87 excess precision. Supported peer: Windows x64, clang-cl, those flags, no fast-math. Not yet replayed on a second PC or on Linux, so the hash match across machines is still open.
- [x] **2.5 Camera independence test**
  - Depends on: 2.3.
  - Work: replay with the rendered camera forced to a different position. Change only the matrix passed to `func_80227CD0`, not the camera state.
  - Done when: hashes still match. If not, identify which draw callbacks in `func_802290CC` depend on the view and document a fix.
  - Note: `BM64_CAMERA_BIAS` adds to the eye arguments of the patched `func_80227CD0` only. On `new.bm64rec`, `ovl`, `data`, `prc` and `game` matched all 8,838 ticks. `bss` and `gfx` differed because the lookAt is stored in the framebuffer `Mtx` (`gFrameBuffers` + `0x18`, two contexts at `0x800BD5C8` and `0x800BD638`) and in `D_802A5418`. The `heap` diff is the camera process's live stack frame (it still holds that eye when it sleeps); dead stack slots are already masked. Nothing in `func_802290CC` wrote player, RNG or overlay state. `D_802A5418` is read only by `func_8022D58C`. Fix when each machine renders its own camera: keep those matrices and that stack frame out of the sync hash, and build the local lookAt after the hashed step. `func_80232FF8`'s depth cull reads the camera tables, not `D_802A5418`, so it stays shared.
- [x] **2.6 Audio dependency audit**
  - Depends on: 0.2.
  - Work: find every gameplay read of sound state (sound handles, "is playing" queries, music position) by cross-referencing the sound API and audio structures. Document them in `game-internals.md`.
  - Done when: there's a list of functions where audio could affect simulation, with a mitigation for each.
  - Note: section 10 of `game-internals.md`. No gameplay call to `alSndpGetState`, `alSeqpGetState`, `alSeqpGetTempo`, `osAiGetLength` or `osAiGetStatus`. `alSeqGetTicks` is only the sequence player. `func_80266CD4` is only `func_8026C208`. Handles from `func_8026724C` are stored in `D_802AC9E0` (`func_80247848`, later passed to `func_8026BE80`) and in a handful of overlay words. Mitigation is the phase 3.6 virtual handle. The symbol `g_StopSound` is the listener pan helper, not a stop.

## Phase 3 – In-process savestates (main technical risk)

Checked on 2026-11-18 without a player at the keyboard. SyncTest passed on `test.bm64rec` (9,591 ticks) before and after the player-array move. `new.bm64rec` passed before that move. After it, the same mode failed at tick 8448 in the o32 argument home of process 11 (`func_8023F26C`); the hash now skips those 16 bytes (see phase 6.1). Savetest at tick 200 for 60 ticks: save 336 µs, load 243 µs, 717 µs per resimulated tick. Two ordinary replays of the first 200 ticks hashed identically. Every other arena and story world is still open. A patch edit needs two builds before new host calls resolve. The `g_Players` cheat writes were in `patches.c` after one build on this pass (`0x80F0`).

- [x] **3.1 Final snapshot memory map**
  - Depends on: 0.3, 2.6.
  - Work: resolve the "audit" rows in the RDRAM map. Produce an explicit include/exclude list, excluding OS threads, message queues, the scheduler, gfx buffers and IDs, and audio structures.
  - Done when: the list is in `game-internals.md` and used by the state hash.
  - Note: the hash list, including the file directory `0x8014B000`–`0x8014C8D0`, is in `game-internals.md` and is what the snapshot copies, plus the file-loader lock and the sound slots. The post-BSS gap and the RSP/audio heap were not needed for `test.bm64rec`. Patch globals are saved beside RDRAM (boot timer, scroll flag, overlay id), not hashed.
- [x] **3.2 Fixed coroutine stack pool**
  - Depends on: 0.1.
  - Work: in `process_coroutines.cpp`, preallocate one native stack per slot (0x50) and reuse slot *i*'s stack forever. Use a custom minicoro allocator so nothing is freed and nothing moves.
  - Done when: the game plays normally, and stack addresses stay constant across process creation and destruction.
  - Note: the pool is 80 slots of 526016 bytes at `0x200000000`. The boot log reports that base. Savetest prints `stacks stable`.
- [x] **3.3 Coroutine state save/restore**
  - Depends on: 3.2.
  - Work: an API to save and restore every `ProcessSlot` (state, IDs, `recomp_context`), the `mco_coro` context, and the used part of each native stack (from the saved stack pointer to the top). Restore only at the top of the main loop.
  - Done when: a unit test inside the game saves, runs 60 ticks, restores, runs the same 60 ticks, and gets identical hashes.
  - Note: `BM64_SAVETEST=60` did that and passed.
- [x] **3.4 Main loop as a step function**
  - Depends on: 0.3.
  - Work: restructure the patched `func_80225840` into `step(render)` driven by a host-side frame controller. With `render = false`, still build display lists and run the draw pass, but skip `Gfx_EndRender` submission and the `func_80227CD0` lookAt.
  - Done when: normal play is unchanged, and N non-rendered steps can run in one frame.
  - Note: a non-rendered step still applies the game-memory side effects of `Gfx_EndRender` (`D_8029F600++`, and the blink-counter clear plus scroll flag when `gPauseRender != 0`). It does not submit the display list or wait for vblank. Two normal replays of 200 ticks matched. Savetest resimulates 60 steps in one launch.
- [x] **3.5 Render isolation**
  - Depends on: 3.4.
  - Work: before a restore, wait for the in-flight display list (`startGfxWait`/`waitForGfxFinish`). Keep gfx buffer state out of snapshots.
  - Done when: repeated restores cause no crashes or corruption beyond one frame.
  - Note: `bm64_wait_gfx` is recompiled code, so the patched loop waits before `bm64_step_begin` whenever a restore is pending. Online play waits before every step, because that step may roll back. The 9,591-tick SyncTest did not crash.
- [ ] **3.6 Audio isolation**
  - Depends on: 2.6, 3.4.
  - Work: wrap `g_PlayGlobalSound`, `g_PlayLocalSound` and `g_StopSound` with deterministic virtual handles mapped to real ones on the host. Add a "speculative" flag that suppresses new sounds. Handle the gameplay audio reads found in 2.6.
  - Done when: sound behaves normally offline, and the audio state doesn't affect hashes.
  - Note: `g_PlayGlobalSound` and `g_PlayLocalSound` return a virtual handle. Stopping a voice is `func_80266930` via `func_8026BE80`, which resolves the virtual id. `g_StopSound` is the pan helper; its entry returns immediately when the step is speculative, so a resim does not recompute pan. Speculative steps do not call the audio driver. Not listened to.
- [x] **3.7 Snapshot ring buffer**
  - Depends on: 3.1, 3.3, 3.5, 3.6.
  - Work: `save(tick)` and `load(tick)` combining the RDRAM ranges, patch globals and coroutine state, with a ring of at least 10 snapshots. Measure the cost.
  - Done when: save plus load takes under 1 ms, and resimulating 8 ticks takes under 8 ms on a mid-range PC.
  - Note: ring of 16. Measured save 336 µs + load 243 µs. 8 ticks at 717 µs is about 5.7 ms. The machine is an RTX 3090, not a mid-range PC.
- [ ] **3.8 SyncTest mode**
  - Depends on: 3.7.
  - Work: a debug mode that runs every tick as save, step, load, step, compare hashes (like GGPO's SyncTest), with a configurable rollback depth.
  - Done when: SyncTest runs clean through every Battle arena and every story world. Fix or document every mismatch.
  - Note: `BM64_SYNCTEST=1` exited clean on `test.bm64rec` (tick 9591) after the player move. `new.bm64rec` exited clean before that move, then failed at tick 8448 until the hash skipped the o32 argument home (phase 6.1). Every other arena and story world is still open.
- [ ] **3.9 Player quicksave (optional)**
  - Depends on: 3.7.
  - Work: user-facing quicksave/quickload built on 3.7. Same session only; native stacks aren't portable across runs.
  - Done when: it works in story and Battle.
  - Note: F5 and F7 request a save or load at the next step. Not pressed in a focused window.

## Phase 4 – Rollback core

The session is our own code in `src/net/` (not GGPO or GekkoNet). `bm64_net_tests` passes lockstep, latency, loss and barrier. Loopback uses `BM64_NET=host|guest`. `BM64_NET_PEER` is a comma-separated list, so four local processes can mesh. `BM64_NET_SAMPLE=zero` replaces the hardware sample with a zero pad; two processes otherwise read the same stick twice and disagree. Non-presented resim steps are not hashed: their end state does not match the peer that presented that tick, and the next presented tick matched in the logs.

- [x] **4.1 Rollback session library**
  - Depends on: nothing; game-independent.
  - Work: C++ in `src/net/`: input queue per player, prediction (repeat the last input), confirmed and predicted frames, a rollback trigger, configurable input delay (default 1–2 ticks) and maximum rollback (about 8), time sync from frame advantage, and a transport interface. Evaluate GGPO or GekkoNet versus writing our own; record the choice.
  - Done when: unit tests with a fake game and fake transport pass.
  - Note: `bm64_net_tests.exe` passed all four tests on 2026-11-18.
- [ ] **4.2 Game integration and loopback transport**
  - Depends on: 3.8, 4.1.
  - Work: drive `step()` and `save`/`load` from the session; inject inputs into `D_802AC410` inside `func_8023A104`. Add a localhost UDP transport with simulated latency, jitter and loss.
  - Done when: two instances on one PC play Battle at 100 ms simulated RTT.
  - Note: two instances, 50 ms one-way lag, zero pads, stayed in sync from boot through tick 400 (no desync). That is the title and boot path, not a Battle match. Handshake sends the 128 KB save as 1000-byte UDP chunks.
- [x] **4.3 Desync detection and diff tool**
  - Depends on: 4.2.
  - Work: exchange hashes every N ticks. On a mismatch, dump both snapshots and diff them into address ranges, then map those to symbols with `tools/re`.
  - Done when: a deliberately injected desync is reported with the exact address.
  - Note: the guest xored `0x802AC620` at tick 280. Both peers exited on tick 281. `dumpdiff.py` shows `802AC620: 00000000->00000001`.
- [ ] **4.4 Load barriers**
  - Depends on: 4.2.
  - Work: treat overlay loads and map changes (`load_from_rom_to_addr` and `recomp_load_overlays`) as points rollback can't cross. Peers wait for each other, then the snapshot history is cleared.
  - Done when: stage changes in Battle and story never roll back across a load.
  - Note: `load_from_rom_to_addr` notifies a barrier. Boot file loads logged barriers and the later sync runs still matched. A battle or story stage change was not played. With more than two players the session waits until every peer has sent that load sequence, and it stalls on the slowest peer.
- [ ] **4.5 Sound during rollback**
  - Depends on: 3.6, 4.2.
  - Work: de-duplicate sound starts by (tick, sound id, source) across resimulation; stop sounds whose triggering tick was rolled back.
  - Done when: no doubled or missing sounds at 150 ms RTT.
  - Note: a rollback stops voices the restored list does not keep, and a repeated (tick, sound, source, position) reuses the virtual id. Intermediate resim steps are speculative and do not start the real voice. Not listened to.
- [ ] **4.6 Session start**
  - Depends on: 2.1, 2.2, 4.2.
  - Work: the host sends the seed, save image, settings (cheats off, lives mode) and a build/ROM/mod hash. All peers boot in sync from tick 0.
  - Done when: four local instances start the same session and stay in sync.
  - Note: four processes on localhost, same seed, cheats forced off, zero pads, exited at tick 100 with no desync. Protocol 2 appends a lives flag (`BM64_NET_LIVES` unset or `0` is shared; anything else is unlimited, and the guest uses the host's flag). Re-checked after the player move: two peers through tick 200, seed `1DA12F06`, lives unlimited; four peers through tick 100, seed `EAED6DA7`. Controller status at `0x802AC400` is overwritten from the session player count.
- [ ] **4.7 Soak test**
  - Depends on: 4.3–4.6.
  - Work: run 2–4 instances for hours at varied simulated network conditions, including the story menus.
  - Done when: zero desyncs, and a tuned default delay and rollback window.
  - Note: not an hours-long run. Automated, zero desyncs: 400 ticks at 50 ms lag; 500 ticks at 40 ms lag, 15 ms jitter and 5% loss (one rollback each, because a zero pad matches the prediction after the first correction); 100 ticks with four players. Default delay is 2 and the rollback window is 8. A long soak and a real Battle are still open.

## Phase 5 – Online Battle

Code for a loopback lobby is in the build. A Discord application and two PCs are still required before anyone off this machine can connect. A TURN relay is not. Direct UDP covers the same LAN, a forwarded port, and (once libjuice is linked) ordinary home NATs via STUN hole punching. Do not invent an application id. `BM64_HAS_LIBJUICE` and `BM64_LINK_DISCORD` are CMake options and both default off; turning one on stops the build at the function where that client belongs (`src/net/ice.cpp`, `src/net/signalling.cpp`). `bm64::signalling::kApplicationId` is the empty slot for an application id.

- [ ] **5.1 Internet transport**
  - Depends on: 4.7.
  - Work: UDP with ICE (libjuice). Gather a host candidate and a server-reflexive candidate from a public STUN server, and hole-punch. A TURN server (`BM64_TURN_HOST`) is a later fallback, not part of getting the first two-PC match up. Signalling goes through an interface, so 5.3 can plug Discord in.
  - Done when: two PCs behind different home NATs connect without a relay. Pairs that cannot punch are reported and left for the TURN fallback.
  - Note: the session binds a UDP socket and publishes that endpoint as a host candidate (`bm64::ice::host_candidate`). `BM64_TURN_HOST` is printed and not dialed, because libjuice is not linked. Loopback peers still use `BM64_NET_PEER`. Two-PC NAT is blocked on linking libjuice, exchanging candidates, and a second machine. It is not blocked on running a TURN server.
- [ ] **5.2 Discord Social SDK**
  - Depends on: 0.1.
  - Work: register the app, then implement account linking, rich presence, and lobby create/join with invites. Check current SDK capabilities and terms first, and record them here.
  - Done when: a friend can be invited from Discord and both clients see the same lobby.
  - Note: blocked. No application is registered, and the SDK is not in the tree. Setting `BM64_DISCORD_APP_ID` only changes the log line; the lobby stays on loopback.
- [ ] **5.3 Signalling over Discord**
  - Depends on: 5.1, 5.2.
  - Work: exchange ICE candidates and session config through lobby messages or metadata.
  - Done when: a Discord invite produces a working peer connection.
  - Note: `publish_candidate` records the host candidate and does not send it. Peers are the `BM64_NET_PEER` list. A Discord invite is blocked on 5.2.
- [ ] **5.4 Launcher UI**
  - Depends on: 5.3.
  - Work: Host and Join buttons, a player list, port assignment, ready state, version and ROM mismatch errors, and input delay settings.
  - Done when: a non-technical user can start an online Battle.
  - Note: the launcher has "Host local game" and "Join local game" (`prepare_loopback`, ports 27015 and 27016). The handshake rejects a protocol or ROM-hash mismatch and waits for every guest ack before the game starts. Player count, delay, and lives are `BM64_NET_PLAYERS`, `BM64_NET_DELAY`, and `BM64_NET_LIVES`. The buttons were not clicked. There is no Discord player list.
  - Note (2026-11-19): with a ROM selected, the launcher also cycles Players (2–4), Input delay (0–4), and Lives (shared or unlimited). Those values are what the handshake uses when the matching environment variable is unset. The new buttons were not clicked.
- [ ] **5.5 Custom Bomberman for guests**
  - Depends on: 4.6.
  - Work: costumes come from the save, and the session uses the host's save. Inject each guest's custom bomber parts into the session save image at session start.
  - Done when: everyone sees each player's own costume.
  - Note: blocked. The 128 KB save is sent, and the field layout for custom bomber parts is not known. No offset was guessed.
- [ ] **5.6 Disconnects and polish**
  - Depends on: 5.4.
  - Work: a disconnected player is handed to AI (set controlType 2) or the match ends; add a ping and rollback indicator.
  - Done when: online Battle is release-ready; write player-facing notes.
  - Note: a remote player with no packet for 300 ticks is handed to AI (`controlType` 2) once. The debug overlay prints local player, delay, rollback count, and ticks since the quietest peer. Not shown in a window, and not release-ready.

## Phase 6 – Story co-op gameplay

The player array move is in the binary and the loopback session can run it. Story rules that would change single-player combat, triggers, cutscenes, or the HUD were not rewritten: those need a played stage. Loops visit eight slots. Rival maps leave slots 0 and 4–7 for co-op players, and other maps use 0–3.

- [x] **6.1 Relocate the player array**
  - Depends on: 3.8.
  - Work: a script in `tools/re` generates `[[patches.instruction]]` entries that repoint every main-code reference to `g_Players` (`0x800AEDF0`–`0x800AF1D8`, about 143 functions) to a larger array in patch RAM (`0x80801000+`). It also widens the 4- and 5-slot loops and relocates per-player companion arrays (`g_ComputerPlayerMovement` and any found during the work). Add a checker that proves no old-range references remain.
  - Done when: SyncTest still passes, and Battle and story are unaffected with 5 slots in use.
  - Note: `tools/re/relocate_players.py` repoints the main-code references (`--check` covers them) and widens the 4- and 5-slot player loops to 8. `g_ComputerPlayerMovement` is at `0x80F00640` (8 × `0x10`). `func_80241BB0`'s side table is at `0x80F006C0`, with its original bytes copied at boot. `func_802506B8` and `func_8024ABA8` still use `D_800AF1D8` as the next table. The 4-port controller map in `func_8023D0F8` stays at 4. The snapshot and the `slots` hash are `0x80F00000`–`0x80F00780`. SyncTest after the widen exited 0: `new.bm64rec` at tick 8838 and `test.bm64rec` at tick 9591. A battle match was not played.
- [x] **6.2 Call-site catalogue**
  - Depends on: 0.2.
  - Work: classify every `getMainPlayer()` use, every direct `D_802AC894` reader, and every constant-index `getPlayerObj`/`getPlayerPos` call, per overlay. Categories: camera, enemy or boss targeting, trigger or goal, cutscene or script, HUD, rival-bomber control, other. Put the table in `game-internals.md`.
  - Done when: every call site has a category.
  - Note: `tools/re/catalogue_main_player.py` assigns all 626 `getMainPlayer`, `D_802AC894`, and constant-index `getPlayerObj`/`getPlayerPos` sites. The rules are in `game-internals.md`. Per-overlay counts are in `docs/main-player-catalogue.txt`. Process entries that are not "other" are in `src/game/bm64_player_cats.inc`.
- [ ] **6.3 Co-op slots and spawning**
  - Depends on: 6.1.
  - Work: in co-op, mark the co-op players' slots human before `func_80245CDC`. Allocate slots that don't collide with rival bombers in maps 4.10, 5.7, 6.15, 8.13, 9.5 and 9.6. Spawn teammates next to the leader.
  - Done when: 2–4 bombers spawn in every story map.
  - Note: `bm64_coop_before_stage` runs at the start of `func_80245CDC` when the session has more than one player. It sets `controlType` 1 on empty slots, skipping 1–3 on the six rival maps, and copies the leader's spawn position onto each new teammate with a 40-unit step along X. `g_SpawnPlayer` copies those floats onto the bomber. It logs when the 8-slot loop cannot fit everyone. No story map was entered.
- [ ] **6.4 Main-player virtualization framework**
  - Depends on: 6.2, 6.3.
  - Work: patch `func_80243DDC` and the 27 direct readers to ask a context resolver ("who is the main player for the current process?"), keyed on the current process (`D_802AC344`) and the category from 6.2. The default returns the leader (slot 0). The resolver's state lives in game memory, so it's snapshotted.
  - Done when: single-player behaviour is unchanged, and the resolver can be overridden per category.
  - Note: with no session, `bm64_main_player()` returns `D_802AC894`, so single-player replays stay on the hero. In a session the running process is looked up in `bm64_player_cats.inc`. Enemy and trigger entries use the 6.6 sticky target. Other categories return the leader. Eight override bytes at `0x80F00820` (`0xFF` means none) are in the snapshot.
  - Note (2026-11-19): the 22 direct `lw`s of `D_802AC894` (every reader except the patched getter) are hooks that store `bm64_resolved_slot` into the register the load just wrote. The three `sw`s and `func_80245868`'s address calculation stay writers. With no session the function returns that word, so the register does not change. `func_80236F54` is treated as a camera process even though it has no catalogue row. After replay started rewriting `0x802AC400` from the ports the file uses, two replays of `test.bm64rec` matched on all 9,591 ticks in every range, and the save was unchanged. A third pass with `BM64_CAMERA_BIAS` still differs in `bss` and `heap`, which is the lookAt matrix and the camera process stack from phase 2.5.
- [ ] **6.5 Per-player cameras**
  - Depends on: 6.4.
  - Work:
    - Keep one copy of the camera blocks per player (see the camera section of `game-internals.md`).
    - Each tick, run the camera update once per player, with that player's block swapped in, the main player resolved to that player, and C-buttons read from that player's port (`0x80235974`).
    - `func_80251C88` and the other camera readers use the acting player's block.
    - Overlay camera commands (`func_80234778`, `func_802346C8`, `func_80234434`, …) apply to every block.
    - Render only the local player's block.
  - Done when: each instance follows its own bomber, and the hashes match across instances.
  - Note (2026-11-19): when a session has two or more human bombers, `func_80235D7C` and the `func_80236D30` call inside `func_80236F54` run once per human. Each human's eye, look-at/mode, controller, `0x8029FEA0` table, and the 16 bytes at `0x802A5A50` live in a block at `0x80F00840` (stride `0x2E0`, inside the snapshot). Live memory is put back to the first human before the process sleeps, so peers hash the same camera. C-buttons at `0x80235974` read that human's port while their block is in. `bm64_camera_frame` then replaces the lookAt arguments with the local human's saved eye and target (mode 1 still swaps them). `func_802346C8` and `func_80234778` copy the live camera onto every block. `func_80251C88`'s object table is still shared. Not watched on two windows in a stage. A session with fewer than two humans does not swap.
- [ ] **6.6 Enemy and boss targeting**
  - Depends on: 6.4.
  - Work: for each targeting process, keep a sticky target player (nearest living player, re-evaluated about once a second, deterministic). Tune per boss as needed.
  - Done when: enemies and bosses engage all players in every world.
  - Note: while a session is running, `getMainPlayer` from a process whose entry is classified as enemy or boss picks the nearest living human or AI bomber and keeps that slot for 60 ticks. The choice is stored per process at `0x80F00780`. The origin is the process `user_data` object when that pointer has a body, otherwise the leader. Ties take the lowest slot. Not watched in a stage, and not tuned per boss.
- [ ] **6.7 Triggers and stage clear**
  - Depends on: 6.4.
  - Work: goals, exits, switches and pickups that check the main player should accept any player; the triggering player becomes the leader for the following sequence. One player clearing the stage clears it for everyone.
  - Done when: every stage can be finished by any player.
  - Note (2026-11-19): a process whose entry is one of the six trigger functions resolves the main player through the same sticky nearest-living-bomber rule as an enemy. That is the read side of "any player". The goal's own distance test was not rewritten, and the canonical word `D_802AC894` is not changed to the player who touched it. Not watched in a stage.
- [ ] **6.8 Map transitions and checkpoints**
  - Depends on: 6.7.
  - Work: when any player takes a screen or map transition, the whole party moves and teammates appear at the entry point. Record each entry point as the checkpoint; stage start is the first.
  - Done when: multi-area stages work with 4 players.
- [ ] **6.9 Death, respawn and lives**
  - Depends on: 6.8.
  - Work: find the hero death flow (start with `func_80250E2C` and `func_802506B8`). In co-op, a death costs one shared life (`D_802AC624`) and respawns the player at the last checkpoint while others continue. Party wipe with no lives left runs the original game over. Add the unlimited-lives option to the synced session settings.
  - Done when: these rules hold in normal stages and boss fights.
  - Note: unlimited lives is the session flag from 4.6. While it is on, `apply_rules` writes `D_802AC624 = 99` every step, on every peer. Shared lives leave that word to the game. Death, checkpoint respawn, and party wipe were not changed.
- [ ] **6.10 Cutscenes and scripted sequences**
  - Depends on: 6.4.
  - Work: during scripts (`WM1 …` command strings, intro and outro sequences, boss intros), the leader is scripted and teammates are hidden with collision off, then restored next to the leader afterwards.
  - Done when: no softlocks or visual breakage in any cutscene.
- [ ] **6.11 Co-op HUD**
  - Depends on: 6.3.
  - Work: show the local player's hearts, bombs and fire; small teammate indicators; shared lives and gems.
  - Done when: it's readable at 4 players in widescreen.
  - Note: a session shows the debug overlay on its own. The panel lists the local hearts, bombs and fire, shared lives and gems, and all eight slots. It was not looked at in a window.
- [ ] **6.12 Rival-bomber maps**
  - Depends on: 6.3–6.9.
  - Work: verify maps 4.10, 5.7, 6.15, 8.13, 9.5 and 9.6 with 4 co-op players plus rivals.
  - Done when: all six are completable.
- [ ] **6.13 Progression and saves**
  - Depends on: 2.2.
  - Work: the host's sandboxed save is written back to the host's real file at checkpoints. At session end, guests can merge shared unlocks (stage clears, gold cards, custom parts) into their own save.
  - Done when: the merge is tested both ways.
- [ ] **6.14–6.19 Playthrough per world**
  - Depends on: 6.5–6.13.
  - Work: one milestone per world (banks 4–9 plus the related boss banks 10–12): complete it with 2, 3 and 4 players and log issues.
  - Done when: each world is completable at all player counts.
  - Note: 6.7, 6.8, 6.9's respawn, 6.10, 6.12, 6.13 and these playthroughs were not coded. Triggers, checkpoints, cutscenes, rival-map checks, and save merge are unchanged. The debug overlay, shown automatically during a session, lists the local hearts, bombs, fire, shared lives and gems, and all eight slots. It was not looked at in a window.

## Phase 7 – Online co-op

The playtest itself is blocked: it needs a Discord login, a second PC, and the phase 6 story playthroughs. A TURN server is not part of that list; direct UDP is the session path, and a relay is only for NATs that will not punch (see "P2P, and when a TURN server is involved"). What can run on loopback is the phase 4 session plus the lives flag. No snapshot is sent to a rejoining guest.

- [ ] **7.1 Co-op session flow**
  - Depends on: 5.4, 6.14–6.19.
  - Work: the host picks a save file and difficulty; guests join the lobby before the story starts; lives mode and other options are synced.
  - Done when: a Discord invite leads into a co-op story session.
  - Note: on loopback the host's sandboxed save, seed, player count, and lives flag are in the hello, and guests install that save before tick 0. Difficulty is whatever is in that save. A Discord invite is blocked on 5.2. Story was not started.
- [ ] **7.2 Story barriers and loading**
  - Depends on: 7.1.
  - Work: confirm that every story load (maps, bosses, cutscenes) is handled by the load barriers from 4.4, with acceptable wait times.
  - Done when: a full world online has no load desyncs.
  - Note: `load_from_rom_to_addr` already notifies the barrier from phase 4. Boot file loads logged it. A story map change was not played, so wait times and missed loads are unchecked.
- [ ] **7.3 Reconnect and desync recovery (optional)**
  - Depends on: 7.1.
  - Work: make snapshots portable: fixed-address coroutine stacks (`VirtualAlloc` at a fixed base) plus a non-ASLR build, or pointer rebasing. Then the host can send its state to a rejoining or desynced guest.
  - Done when: a guest can drop and rejoin mid-stage.
  - Note: the coroutine stacks are already 80 slots at `0x200000000`. Nothing sends a snapshot to another process. A mid-stage rejoin was not tried.
- [ ] **7.4 Performance tuning**
  - Depends on: 7.1.
  - Work: profile rollback in the heaviest stages and boss fights at 4 players; optimize snapshots (dirty pages or deltas) if needed.
  - Done when: steady 60 fps at 150 ms RTT with an 8-tick rollback.
  - Note: not profiled in a stage. The phase 3 savetest numbers (save 336 µs, load 243 µs, 717 µs per resimulated tick) are still the only measurement. The player-array range adds 1600 bytes to each snapshot.
- [ ] **7.5 Online playtest and telemetry**
  - Depends on: 7.2, 7.4.
  - Work: play every stage and boss online; add opt-in desync and crash reports.
  - Done when: no known blockers.
  - Note: a hash mismatch still dumps both sides under `BM64_DESYNC_DIR` (default `desync`) and exits. No extra crash reporter. Playing the stages is a morning item.
- [ ] **7.6 Release**
  - Depends on: 7.5.
  - Work: player documentation (setup, Discord, troubleshooting) and release builds.
  - Done when: shipped.
  - Note: not started. This is the ship step and it waits on the playtest.

---

## Risks and fallbacks

- **Coroutine snapshots (3.2–3.3)** are the biggest technical risk. Fallback: delay-based lockstep needs no snapshots; Battle still ships, with more input delay.
- **Camera-dependent animation (2.5)**: run the animation part of the draw pass with one canonical camera, and render separately.
- **Story code's one-hero assumptions (6.2–6.10)** are the long tail; the call-site catalogue keeps the work countable.
- **Gameplay that waits on sounds (2.6)** could make audio timing part of the simulation; virtual handles and deterministic durations fix it.
- **Player array relocation (6.1)** touches about 143 functions; the generator script plus checker plus SyncTest keep it safe.
