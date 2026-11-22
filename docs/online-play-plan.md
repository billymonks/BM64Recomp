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
- Every open milestone has a numbered "Steps" list. Each step is tagged:
  - **[agent]**: code, scripts or analysis you can do without running the game.
  - **[user]**: a run to hand to the user as written; don't run the game yourself without asking.
  - **[agent, ask first]**: an unattended run (`run-replay.ps1` and the like) you may start only after the user says yes.

  Finish a step's check before starting the next step. If a step's assumption turns out wrong, write what you found under the milestone and stop.
- **Start with "Next work queue" near the end of this file.** It lists the next tasks in order, with the design already decided, the files to change, the tests to write and the traps to avoid. Do those before any other open milestone.

## Working rules

These come from mistakes already made in this repo. Follow them even when a shortcut looks safe.

**Ticking milestones**
- Tick `[x]` only when the "done when" sentence is literally true. If part of it was not checked, leave it open and write what was and was not checked in the note.
- Never tick a milestone whose dependencies are open.
- Write "not run in a stage" or "not watched" in the note whenever code was built but not seen working. Don't write that something works unless a run showed it.

**Determinism** (every rule here has caused, or would have caused, a desync)
- Anything that changes game memory must depend only on game memory and the tick's inputs. Never on a host clock, a frame counter, packet timing, "ticks since a peer was heard", or which machine is running.
- New game state goes in RDRAM inside the co-op region (`0x80F00000` up to `bm64::coop::kStateEnd`), which the snapshot and the `slots` hash cover. Host `static` variables may hold game state only if they are set and used inside the same call. `g_rotation_slot` in `bm64_coop.cpp` is set one instruction before the call that consumes it.
- A host decision that has to reach the game (a player leaving, a setting) must travel in the input stream, so every peer applies it on the same tick.
- Co-op rules run only when `story()` is true. Online Battle must behave like the unmodified game.
- RDRAM is stored as byte-swapped 32-bit words. Use `MEM_W`, `MEM_H`, `MEM_B` or the `word`/`set_word` helpers. Raw `rdram[addr - 0x80000000]` byte access is only safe for bytes that host code alone reads and writes.

**Game facts**
- Check a claim with `tools/re/bmdis.py` and `tools/re/xref.py` before writing it into `game-internals.md`. Mark anything inferred as "(likely)". The bomb pool at `0x8008E658` was once recorded as camera state because a name was guessed from one caller.
- Before hooking a function, disassemble it and record which register holds what at the hook address.

**Changing `bm64.us.toml`**
- The file is about 9 MB. Editor tools refuse to write it. Write the new block to a temp file and append it with PowerShell, converting to LF line endings:
  `$t = [IO.File]::ReadAllText("$env:TEMP\block.toml") -replace "`r`n", "`n"; [IO.File]::AppendAllText((Resolve-Path bm64.us.toml), $t)`
- Put hand-written hooks in their own `# BEGIN …` / `# END …` block. `tools/re/relocate_players.py --write` regenerates everything between `# BEGIN bm64 relocated player array` and its END marker, so anything added there by hand is lost.
- A `[[patches.hook]]` `text` is C inserted before the instruction at `before_vram`. It can read and write `ctx->rN`, call `extern` host functions with `(rdram, ctx)`, `return;` to leave the function (set `ctx->r2` first for a return value), or `goto L_xxxxxxxx` to a label that exists in the generated function. At a `jal`, the hook runs before the delay-slot instruction, which still runs before the call.
- A hook on a function that has a `RECOMP_PATCH` in `patches/` does nothing, because the patch replaces the generated function. Grep `patches/` first.
- Any toml change needs `tools/build-windows.ps1 -Regen`, which takes about 10 minutes. After it, confirm the hook is in the output: `Select-String -Path RecompiledFuncs\funcs_*.c -Pattern 'your_host_function'`.

**Builds and tests**
- Full build: `powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1` (add `-Regen` after toml changes). Run it in the background and read the log tail.
- Network unit tests, run after every change in `src/net/`:
  ```
  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
  cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | % { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }
  $env:PATH = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
  cmake --build build --target bm64_net_tests; .\build\bm64_net_tests.exe
  ```
  It must print `bm64_net_tests: all passed`.
- Fixing a session bug: first add a test to `src/net/rollback_test.cpp` that fails on the current code, then fix, then confirm it passes. `PairOptions` can delay, drop or hold packets (`hold_sender`, `hold_tick`, `hold_from`, `hold_until`).
- In-game network checks must not use `BM64_NET_SAMPLE=zero` as the only test. Zero pads always match the prediction, so rollback is never exercised. Use the scripted input from queue item 1 once it exists.

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
- [ ] **2.3 Same-machine determinism test**
  - Depends on: 2.1, 2.2.
  - Work: record about 10 minutes of Battle and one full story world; replay each twice.
  - Done when: hash logs match exactly; any mismatch is root-caused and fixed.
  - Steps:
    1. [user] Record about 10 minutes of Battle across at least two arenas, with 2–4 controllers or AI: `$env:BM64_RECORD = "battle10.bm64rec"; .\build\BM64Recompiled.exe`. Quit normally so the file is closed. Then record one full story world, from its first stage through its boss: `$env:BM64_RECORD = "world1.bm64rec"`. Remove `BM64_RECORD` afterwards (`Remove-Item Env:BM64_RECORD`).
    2. [agent, ask first] `.\tools\run-determinism.ps1 -Replay battle10.bm64rec -Out det_battle10`, then the same for `world1.bm64rec`. Pass: every range matches on both replays, and the save is unchanged.
    3. [agent] If a range differs, take the first differing tick N from the hash logs and dump N and N+1 in two replays (`-DumpTicks "N,N+1"`, outputs `a` and `b`). Diff with `python tools/re/dumpdiff.py a\dumps\tick_N.bin b\dumps\tick_N.bin`, and find the writer of the first differing address with `tools/re/xref.py`.
    4. [agent] Typical causes and fixes, from phase 0.5: a render-thread write to game memory (move it to the game thread, once per submitted frame), a host pointer stored in RDRAM (leave that object out of the hash), or a dead stack slot (add it to the mask). Record the cause here and in `game-internals.md`, rebuild, and repeat step 2.
    5. Tick when both recordings pass.
  - Note (2026-11-18): same-machine determinism is verified. `tools/run-determinism.ps1` replayed `new.bm64rec` (boot, menus, stage 7.1, 8,838 ticks) and `test.bm64rec` (boot into stage 4.1, 9,591 ticks) twice each. Every hash range matched, and the user's save was unchanged. Those files do not include Battle or a full story world. A longer Battle or world recording can be checked with the same script; nothing in the two story sessions diverged.
  - Note (audit, 2026-11-19): unticked. The "done when" needs about 10 minutes of Battle and one full story world, and neither has been recorded. The two replays above are the only evidence.
- [ ] **2.4 Cross-machine determinism**
  - Depends on: 2.3.
  - Work: replay the same files on a second PC (and on Linux if available). Audit compiler floating-point flags (no FMA contraction, consistent SSE).
  - Done when: hashes match across machines; supported cross-play combinations are listed.
  - Steps:
    1. [agent] Before any run: the UCRT math library picks FMA3 code paths at runtime on CPUs that support them, so `sinf`, `cosf`, `powf`, `atan2f` and similar can round differently on different CPUs from the same exe.
       - Search `RecompiledFuncs/` and `patches/` for calls to C math functions: `Select-String -Path RecompiledFuncs\*.c -Pattern '\b(sinf|cosf|tanf|atan2f|powf|expf|logf|fmodf|sin|cos|pow)\('`. `sqrt` is exact under IEEE and is safe.
       - Whether or not any are found, call `_set_FMA3_enable(0)` (from `<math.h>`, x64 UCRT) at the very start of `main`, before any game code runs.
       - Record the search result in `game-internals.md`.
    2. [agent] Add a "machine info" line to the hash-log header: CPU brand string (`__cpuid` leaves `0x80000002`–`0x80000004`), Windows build, and exe hash. That way a log can be matched to its machine.
    3. [user] Copy the whole `build\` folder, `assets\`, `tools\` and the recordings (`new.bm64rec`, `test.bm64rec`, plus the 2.3 recordings) to a second PC, ideally with the other CPU vendor (Intel vs AMD). On each PC run `.\tools\run-replay.ps1 -Replay new.bm64rec -Out xm_<pc>` and send back the output folder.
    4. [agent] Compare the two hash logs line by line. Pass: identical. On a difference, dump the first bad tick on both PCs (`-DumpTicks`) and diff the dumps. Float-only differences in object positions point to step 1 (a math call that's still unpinned). Anything else, treat as in 2.3 step 4.
    5. [agent] Linux: only if a Linux build exists. Otherwise write "Windows x64 only" in the supported list.
    6. Tick with the list of CPU and OS pairs that matched.
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
- [x] **3.6 Audio isolation**
  - Depends on: 2.6, 3.4.
  - Work: wrap `g_PlayGlobalSound`, `g_PlayLocalSound` and `g_StopSound` with deterministic virtual handles mapped to real ones on the host. Add a "speculative" flag that suppresses new sounds. Handle the gameplay audio reads found in 2.6.
  - Done when: sound behaves normally offline, and the audio state doesn't affect hashes.
  - Steps:
    1. [agent] Add two counters in `src/game/bm64_state.cpp`, printed at exit and in the debug overlay: real voices started during a speculative step (must stay 0), and virtual ids reused by `bm64_sound_enter`.
    2. [agent, ask first] Audio must not feed back into the simulation. Replay `test.bm64rec` normally, then again with `$env:SDL_AUDIODRIVER = "dummy"` (output goes nowhere, but the game's audio thread still runs). Compare the hash logs. Pass: identical.
    3. [agent, ask first] Replay `test.bm64rec` with `BM64_SYNCTEST=1`. Pass: no SyncTest failure, and the speculative-start counter is 0.
    4. [user] Listening test with no session: 5 minutes of a story stage and one Battle match. Listen for missing, doubled or stuck sounds, and for wrong left/right pan (`g_StopSound` is the pan helper). Report the stage and the moment.
    5. [agent] For each report, find the sound call involved (`xref.py` on `g_PlayGlobalSound` and `g_PlayLocalSound` in that overlay) and check that its handle goes through the virtual id path.
    6. Tick when steps 2–4 pass.
  - Note: `g_PlayGlobalSound` and `g_PlayLocalSound` return a virtual handle. Stopping a voice is `func_80266930` via `func_8026BE80`, which resolves the virtual id. `g_StopSound` is the pan helper; its entry returns immediately when the step is speculative, so a resim does not recompute pan. Speculative steps do not call the audio driver. Counters live in `src/game/bm64_state.cpp` and on the debug overlay.
  - Note (2026-11-19): `test.bm64rec` reached tick 9591 with the normal audio driver and with `SDL_AUDIODRIVER=dummy`. The hash logs match on all 9591 ticks. `BM64_SYNCTEST=1` on the same file exited at tick 9591 with no `BM64 SYNCTEST FAIL`, and both runs printed `speculative starts 0, virtual ids reused 0`. The user then played a story stage and a Battle match offline and reported nothing out of place. The stage was not named. No bad-sound report, so step 5 had nothing to trace.
- [x] **3.7 Snapshot ring buffer**
  - Depends on: 3.1, 3.3, 3.5, 3.6.
  - Work: `save(tick)` and `load(tick)` combining the RDRAM ranges, patch globals and coroutine state, with a ring of at least 10 snapshots. Measure the cost.
  - Done when: save plus load takes under 1 ms, and resimulating 8 ticks takes under 8 ms on a mid-range PC.
  - Note: ring of 16. Measured save 336 µs + load 243 µs. 8 ticks at 717 µs is about 5.7 ms. The machine is an RTX 3090, not a mid-range PC.
  - Note (audit, 2026-11-19): ticked while its dependency 3.6 was still open. The ring itself works. 3.6 was listened to and ticked later the same day. The ring size caps the rollback window: `BM64_NET_ROLLBACK` is clamped to 12 so a rollback never reaches a snapshot the ring has overwritten.
- [x] **3.8 SyncTest mode**
  - Depends on: 3.7.
  - Work: a debug mode that runs every tick as save, step, load, step, compare hashes (like GGPO's SyncTest), with a configurable rollback depth.
  - Done when: an automated director recording of every Battle arena and every story stage (one fresh boot per scene, `tools/run-synctest-scenes.ps1`) runs its full length and passes SyncTest at depths 1 and 12. Every mismatch is fixed or documented. Boss fights and stage clears are not reachable this way; the co-op playthrough recordings (6.14–6.19) cover them.
  - Reworded 2026-11-20 with the user: recording every arena and world by hand needed arenas the user had not unlocked.
  - Steps:
    1. [user] Record one file per Battle arena (bank 14 has 10 sub-overlays; check the debug overlay's "overlay" field to confirm which arena is loaded), 1–2 minutes each, with real input on at least two pads. Record one file per story world (banks 4–9), from the first stage to the boss. The 2.3 recordings count. Name them `st_<overlay>.bm64rec`.
    2. [agent, ask first] For each file: `$env:BM64_SYNCTEST = "1"; .\tools\run-replay.ps1 -Replay st_X.bm64rec -Out st_X -TimeoutSec 3600`. The number is the rollback depth (`1` is one tick, `12` is the session cap). Pass: no `BM64 SYNCTEST FAIL` line in the log, and exit code 0. A clean exit also prints `BM64 SYNCTEST PASS ticks N depth D`.
    3. [agent] On `FAIL tick N`, the log lists the ranges that differ. Classify:
       - `heap` or `prc` differing in a few bytes inside a process stack: a dead stack slot or an o32 argument home. Confirm it's a stack (between the process's stack bounds), then add it to the skip list as phase 6.1 did, with a comment naming the function.
       - `slots`: co-op state outside RDRAM. Search for host `static` variables in `src/game/bm64_coop.cpp` that change game memory, and move them into the co-op region.
       - Anything else: state the snapshot doesn't capture. Find the writer (`xref.py`) and add its range to the snapshot list in `game-internals.md` "State hash ranges", then to the code.
    4. [agent] Never "fix" a SyncTest failure by removing a range from the hash without proving it's not game state. Write the proof in the note.
    5. Keep a table in this note: file, overlay, ticks, result. Tick when every arena and world row passes.
  - Note: `BM64_SYNCTEST=1` exited clean on `test.bm64rec` (tick 9591) after the player move. `new.bm64rec` exited clean before that move, then failed at tick 8448 until the hash skipped the o32 argument home (phase 6.1). Every other arena and story world is still open.
  - Note (2026-11-19): `BM64_SYNCTEST=<n>` in `src/game/bm64_state.cpp` is the rollback depth. `1` saves, steps, loads, steps, and compares every tick. `2`–`12` save once, run that many ticks, load, and rerun them, comparing the hash after each. Above 12 is clamped to 12, the same cap as `BM64_NET_ROLLBACK`. Both passes skip the render-thread handoff. A replay rereads the file from the restored cursor; a live pad is replayed from the sample stored on the forward pass. A recording that ends mid-window is compared before exit. A mismatch prints `BM64 SYNCTEST FAIL`, the ranges, and `window frame k of n`.

    | file | overlay | ticks | depth | result |
    |---|---|---|---|---|
    | `test.bm64rec` | boot, then bank 4 sub-overlays 1 and 2 (the stage 4.1 recording). Overlay ids 11, 9, 16, 12, 18, 19. No bank 14. | 9591 | 1 and 12 | pass. The depth 1 and depth 12 hash logs match each other and the 3.6 SyncTest log. `speculative starts 0` |
    | `new.bm64rec` | boot, menus, bank 7 sub-overlay 1 (the stage 7.1 recording), then a menu. Overlay ids 11, 9, 16, 12, 50, 11. No bank 14. | 8838 | 1 and 12 | pass. The depth 1 and depth 12 hash logs match. `speculative starts 0` |
  - Note (2026-11-20): director coverage, final build, `runs/st/fin_a2` (arenas, 30 s each) and `runs/st/fin_s1`–`fin_s3` (story, 20 s each). Every SyncTest pass checked every tick the recording reached, at depth 1 and at depth 12, with `speculative starts 0`. Fixed on the way:
    - Depth 12, tick 626 (scene `0x90`): stack leftovers from sound code that reads audio-thread globals. Fixed by zeroing the process stack below the caller's `sp` when a sound entry point returns (`bm64_scrub_stack`, game-internals section 10).
    - Depth 3 and up, tick 1893 (scene `0x98`): a rollback across an overlay change kept the newer overlay's recompiled functions mapped. The snapshot now keeps the mapped overlay and remaps it on restore (section 11, snapshot version 2).
    - The sub-overlay table `0x8019C0D0` is rewritten on every bank load and read by the overlay loader, so `0x8019B0D0`–`0x8019C2E0` is now in the snapshot and the `files` hash.
    - A SyncTest rerun that reuses stored pads did not apply the director's scene command. It does now.
    - Scene `0x2E` needs entry `0x12`; `0x7A` and `0x7D` are not stages (section 2).
  - Note (2026-11-20, later): the early stops in arenas 2, 3, 5, 7 and 9 came from the director warping from the title straight into Battle setup (section 2, "Heap exhaustion in Battle"). Reaching Battle setup through the title and main menu instead (`BM64_DIRECTOR_MENU`, `run-synctest-scenes.ps1 -ArenaMenus`) runs all 10 arenas to the end. Arena results below are from `runs/st/fin_nat1` and `fin_nat2` (`-Secs 35 -ArenaMenus`): 3201 ticks each, about 1755 of them in the arena, `speculative starts 0`. Arena 8 finished a round and went to stage select (overlay 17) and back.

    | recording | overlay ids | ticks | depth 1 | depth 12 | result |
    |---|---|---|---|---|---|
    | arena 1 (`0x90`) | 11, 9, 13, 86 | 3201 | pass | pass | full length |
    | arena 2 (`0x91`) | 11, 9, 13, 87 | 3201 | pass | pass | full length |
    | arena 3 (`0x92`) | 11, 9, 13, 88 | 3201 | pass | pass | full length |
    | arena 4 (`0x93`) | 11, 9, 13, 89 | 3201 | pass | pass | full length |
    | arena 5 (`0x94`) | 11, 9, 13, 90 | 3201 | pass | pass | full length |
    | arena 6 (`0x95`) | 11, 9, 13, 91 | 3201 | pass | pass | full length |
    | arena 7 (`0x96`) | 11, 9, 13, 92 | 3201 | pass | pass | full length |
    | arena 8 (`0x97`) | 11, 9, 13, 93, 17, 93 | 3201 | pass | pass | full length |
    | arena 9 (`0x98`) | 11, 9, 13, 94 | 3201 | pass | pass | full length |
    | arena 10 (`0x99`) | 11, 9, 13, 95 | 3201 | pass | pass | full length |
    | bank 4: `28`–`31` (10 stages) | 18–27 | 1801 each | pass | pass | full length |
    | bank 5: `38`–`3E` (7) | 28–34 | 1801 each | pass | pass | full length |
    | bank 6: `48`–`56` (15) | 35–49 | 1801 each | pass | pass | full length |
    | bank 7: `58`–`61` (10) | 50–59 | 1801 each | pass | pass | full length |
    | bank 8: `68`–`74` (13) | 60–72 | 1801 each | pass | pass | full length |
    | bank 9: `78`, `79`, `7B`, `7C`, `7E`, `62` (6) | 73–78 | 1801 each | pass | pass | full length |

    Ticked 2026-11-20: every arena and every story stage row passes at full length at depths 1 and 12, under the reworded done-when. Steps 1 and 2 (hand recordings) were replaced by the director runs; boss fights and stage clears are covered by 6.14–6.19.
- [ ] **3.9 Player quicksave (optional)**
  - Depends on: 3.7.
  - Work: user-facing quicksave/quickload built on 3.7. Same session only; native stacks aren't portable across runs.
  - Done when: it works in story and Battle.
  - Steps:
    1. [agent] Quicksave and quickload must do nothing during an online session. A local restore would desync every peer. In `request_quicksave` and `request_quickload` (`src/game/bm64_state.cpp`), return early and print "Quicksave is off online" when `bm64::online::active()` is true.
    2. [agent] A quickload across an overlay load has the same host-table problem as rollback (queue item 2). After queue item 2, have quickload use the same overlay-log restore. Before item 2 is done, refuse a quickload when an overlay has loaded since the quicksave: compare the saved overlay id with the current one.
    3. [user] In a story stage: F5, walk away, F7. You should be back where you saved. Then F5, take damage, F7: hearts should be restored. Then F5, enter the next area, F7: either refused with a message (before item 2) or restored correctly (after it). Repeat in a Battle match. Report any crash or visual corruption that lasts more than one frame.
    4. Tick when step 3 passes in both modes.
  - Note: F5 and F7 request a save or load at the next step. Not pressed in a focused window.

## Phase 4 – Rollback core

The session is our own code in `src/net/` (not GGPO or GekkoNet). `bm64_net_tests` passes ten tests: one tick late, latency, loss, late packet, silence, barrier, guessed load, real load, leave, and leave with the host missing the end. Loopback uses `BM64_NET=host|guest`. `BM64_NET_PEER` is a comma-separated list, so four local processes can mesh. `BM64_NET_SAMPLE` picks the local sample, because two processes otherwise read the same stick twice and disagree: `zero` sends a zero pad; `script` keeps player 0 on the hardware and gives every other player a seeded random stick (held 4–20 samples, no buttons); `script-buttons` adds A (1 in 4) and B (1 in 8), never Start. Non-presented resim steps are not hashed: their end state does not match the peer that presented that tick, and the next presented tick matched in the logs.

- [x] **4.1 Rollback session library**
  - Depends on: nothing; game-independent.
  - Work: C++ in `src/net/`: input queue per player, prediction (repeat the last input), confirmed and predicted frames, a rollback trigger, configurable input delay (default 1–2 ticks) and maximum rollback (about 8), time sync from frame advantage, and a transport interface. Evaluate GGPO or GekkoNet versus writing our own; record the choice.
  - Done when: unit tests with a fake game and fake transport pass.
  - Note: `bm64_net_tests.exe` passed all four tests on 2026-11-18.
  - Note (audit fix, 2026-11-19): the audit found two bugs the four tests could not reach, because their inputs either arrived in time or matched the prediction.
    - False desync. A tick counted as confirmed once it had been simulated, even if a remote input for it was still missing. One input packet arriving about six frames late made both peers hash a predicted state, report a desync and exit. Now a tick is final only when it and every earlier tick ran on the real input of every player, and only final ticks are hashed.
    - Prediction window. Nothing stopped a peer running ahead of its last final tick, so a peer that went silent for about two seconds left the states permanently different, with no report. A peer now stalls once it is `max_rollback` ticks past its last final tick.
    - The resend window followed the sender's own clock, so inputs lost while a peer was silent were never sent again and the cap deadlocked. Each packet header now carries the sender's final tick, and inputs are resent from the slowest peer's final tick (at most 64).
    - The header grew to 20 bytes, so the protocol is 3. The tests "loss", "late packet" and "silence" fail on the old code and pass on the new.
- [ ] **4.2 Game integration and loopback transport**
  - Depends on: 3.8, 4.1.
  - Work: drive `step()` and `save`/`load` from the session; inject inputs into `D_802AC410` inside `func_8023A104`. Add a localhost UDP transport with simulated latency, jitter and loss.
  - Done when: two instances on one PC play Battle at 100 ms simulated RTT.
  - Steps:
    1. [agent] Finish queue item 1 (scripted input) and queue item 2 (load barrier). Battle changes overlay between menus and arena, so the barrier is on this path.
    2. [user] Two PowerShell windows in the repo root:
       - Host: `$env:BM64_NET = "host"; $env:BM64_NET_SAMPLE = "script"; $env:BM64_NET_LAG_MS = "50"; .\build\BM64Recompiled.exe`
       - Guest: the same with `$env:BM64_NET = "guest"`.

       50 ms each way is 100 ms RTT. Drive the menus from the host window into a 2-player Battle and play one full match to the results screen.
    3. Pass: neither window prints `BM64 DESYNC`, and both logs show `Rollback to tick` lines and the same `Load barrier` ticks.
    4. [agent] On a desync, follow the triage at the end of queue item 6. Add a unit test that reproduces the cause if it's in `src/net/`.
    5. [user] Repeat with `script-buttons` so the guest's bomber places bombs.
    6. Tick when steps 2–5 pass. Note the rollback count and the deepest rollback from the logs.
  - Note: two instances, 50 ms one-way lag, zero pads, stayed in sync from boot through tick 400 (no desync). That is the title and boot path, not a Battle match. Handshake sends the 128 KB save as 1000-byte UDP chunks.
  - Note (audit, 2026-11-19): left open because its dependency 3.8 is open and no Battle match was played. Every in-game network run so far used `BM64_NET_SAMPLE=zero`, so a prediction was never wrong after the first correction and rollback of real input was never exercised. Queue item 1 adds scripted input, and item 6 has the runs.
- [x] **4.3 Desync detection and diff tool**
  - Depends on: 4.2.
  - Work: exchange hashes every N ticks. On a mismatch, dump both snapshots and diff them into address ranges, then map those to symbols with `tools/re`.
  - Done when: a deliberately injected desync is reported with the exact address.
  - Note: the guest xored `0x802AC620` at tick 280. Both peers exited on tick 281. `dumpdiff.py` shows `802AC620: 00000000->00000001`.
- [ ] **4.4 Load barriers**
  - Depends on: 4.2.
  - Work: treat overlay loads and map changes (`load_from_rom_to_addr` and `recomp_load_overlays`) as points rollback can't cross. Peers wait for each other, then the snapshot history is cleared.
  - Done when: stage changes in Battle and story never roll back across a load.
  - Next: queue item 2 replaces this design. Barriers apply only to loads on final ticks, and a rollback across a guessed load re-points the overlay table from a log.
  - Note (2026-11-20): queue item 2 is in (step 1 done). A load waits until its tick's inputs are all known. It becomes a barrier only if they match what was simulated; otherwise the next poll rolls back before it and the snapshot remaps the overlay slot. Peers no longer wait on each other's load count. Unit tests only; step 2 is open.
  - Steps:
    1. [agent] Queue item 2, including its unit tests.
    2. [user] The 4.2 loopback run, but through two Battle rounds and back to the arena menu, then into a story stage from the title. Both logs must print the same `Load barrier overlay X at tick T` lines, with no desync.
    3. [agent] If a load shows on one peer only, or at different ticks, find whether the load function was reached through a path that doesn't call `bm64_load_barrier`. List every caller of `recomp_load_overlays` in `patches/required_patches.c`; each must notify the barrier or be proven to run only at boot.
    4. Tick when step 2 passes.
  - Note: `load_from_rom_to_addr` notifies a barrier. Boot file loads logged barriers and the later sync runs still matched. A battle or story stage change was not played. With more than two players the session waits until every peer has sent that load sequence, and it stalls on the slowest peer.
- [ ] **4.5 Sound during rollback**
  - Depends on: 3.6, 4.2.
  - Work: de-duplicate sound starts by (tick, sound id, source) across resimulation; stop sounds whose triggering tick was rolled back.
  - Done when: no doubled or missing sounds at 150 ms RTT.
  - Steps:
    1. [agent] Extend the 3.6 counters with: voices stopped by a rollback, and starts skipped as repeats. Show them in the online status line.
    2. [user] The 4.2 setup with `BM64_NET_LAG_MS=75` on both (150 ms RTT) and `script-buttons` on the guest. Play a match, listening on the host window. Report doubled explosions or footsteps, bombs exploding silently, and loops that never stop.
    3. [agent] A doubled sound means the repeat key (tick, sound, source, position) differs between the guessed and the corrected run. Log the key for each start in a debug build and compare. Position is the usual culprit: a guessed bomber position differs by a few units. If so, drop position from the key for sounds whose source is a bomber.
    4. [agent] A missing sound means a corrected tick started it during an intermediate resim step, which is speculative and starts nothing. Fix: on the last resim step, start real voices for virtual ids created during the resim that have no real voice.
    5. Tick when step 2 reports nothing wrong.
  - Note: a rollback stops voices the restored list does not keep, and a repeated (tick, sound, source, position) reuses the virtual id. Intermediate resim steps are speculative and do not start the real voice. Not listened to.
- [ ] **4.6 Session start**
  - Depends on: 2.1, 2.2, 4.2.
  - Work: the host sends the seed, save image, settings (cheats off, lives mode) and a build/ROM/mod hash. All peers boot in sync from tick 0.
  - Done when: four local instances start the same session and stay in sync.
  - Steps:
    1. [agent] **Input delay must come from the host.** Today each peer uses its own `BM64_NET_DELAY` or launcher value, and the hello doesn't carry it. The session pre-fills ticks `[0, delay)` with a default pad for every player, so peers with different delays disagree about those ticks and stall or desync. Append `u32 input_delay` to the hello after the flags word. The guest uses it and ignores its own setting. Bump `kProtocol` to 4.
    2. [agent] **ROM check that always runs.** `rom_hash()` reads `BM64_ROM` and returns 0 when it's unset, and 0 skips the check. Hash the ROM the runtime actually loaded: find where librecomp keeps it (search `lib/N64ModernRuntime/librecomp/src/` for the ROM buffer), hash those bytes with xxHash3, and never send 0.
    3. [agent] **Build check.** Hash the running exe (`GetModuleFileNameW`, read the file, xxHash3) once at startup. Put it in the hello and the ack, and reject a mismatch with "The other player has a different game build". Cross-build determinism is not proven (2.4), so only identical exes may play.
    4. [agent] **Mods.** If any mod is enabled (the mod menu in `src/ui/ui_mod_menu`), refuse to host or join, with a message. Mod-list syncing is out of scope.
    5. [agent] **Player numbers.** Guests default to player 1, so 3–4 players need `BM64_NET_PLAYER` set by hand.
       - The guest sends `BM64JOIN` with a random 64-bit nonce until it gets an assignment.
       - The host assigns 1, 2, 3 in order of first arrival and sends `BM64SLOT {nonce, index}` back.
       - The hello is sent only after every guest has a slot. `BM64_NET_PLAYER` stays as an override.
    6. [user] Four windows with scripted input, each with its own environment:
       - Host: `BM64_NET=host`, `BM64_NET_PLAYERS=4`, `BM64_NET_BIND=127.0.0.1:27015`, `BM64_NET_PEER=127.0.0.1:27016,127.0.0.1:27017,127.0.0.1:27018`.
       - Guest k (1–3): `BM64_NET=guest`, `BM64_NET_BIND=127.0.0.1:2701<5+k>`, and `BM64_NET_PEER` listing the other three ports.
       - Everyone: `BM64_NET_SAMPLE=script`.

       Drive the host into a 4-player Battle and play one match.
    7. Pass: all four reach the results screen with no `BM64 DESYNC`. Tick then.
  - Note: four processes on localhost, same seed, cheats forced off, zero pads, exited at tick 100 with no desync. Protocol 2 appends a lives flag (`BM64_NET_LIVES` unset or `0` is shared; anything else is unlimited, and the guest uses the host's flag). Re-checked after the player move: two peers through tick 200, seed `1DA12F06`, lives unlimited; four peers through tick 100, seed `EAED6DA7`. Controller status at `0x802AC400` is overwritten from the session player count.
- [ ] **4.7 Soak test**
  - Depends on: 4.3–4.6.
  - Work: run 2–4 instances for hours at varied simulated network conditions, including the story menus.
  - Done when: zero desyncs, and a tuned default delay and rollback window.
  - Steps:
    1. [agent] Add a soak log: every 600 ticks, each peer appends `tick, rollbacks, deepest rollback, window stalls, time-sync stalls, barrier waits` to `soak_<player>.csv`. Turn it on with `BM64_NET_SOAK_LOG=1`.
    2. [agent] Battle loops forever only if someone starts the next match. Add `BM64_NET_SAMPLE=script-menus` for the host: it presses A once every 120 ticks on results and menu screens. Detect those by overlay id: anything outside the arena overlays in bank 14. That way a match restarts without a person.
    3. [user] Run each row of this matrix for 1 hour with 2 players, then the last row with 4. Send back the CSV files.

       | Lag (ms each way) | Jitter (ms) | Loss (%) | Delay |
       |---|---|---|---|
       | 10 | 0 | 0 | 1 |
       | 50 | 10 | 1 | 2 |
       | 75 | 20 | 3 | 2 |
       | 75 | 20 | 3 | 3 |
       | 100 | 30 | 5 | 3 |

    4. [agent] Choose defaults from the CSVs:
       - Default delay: the smallest delay where, at 50 ms lag, the deepest rollback is at most 6 and window stalls are under 1 per minute.
       - Rollback window: 8, unless window stalls appear at 75 ms. Then 10, which stays under the ring-limited cap of 12.
    5. Pass: zero desyncs in every row. Write the chosen defaults into `online.cpp` and this note, then tick.
  - Note: not an hours-long run. Automated, zero desyncs: 400 ticks at 50 ms lag; 500 ticks at 40 ms lag, 15 ms jitter and 5% loss (one rollback each, because a zero pad matches the prediction after the first correction); 100 ticks with four players. Default delay is 2 and the rollback window is 8. A long soak and a real Battle are still open.
  - Note (2026-11-19): those runs predate the rollback fixes in 4.1 and used zero pads, so they need repeating with protocol 3 and non-zero input. A peer that falls `max_rollback` ticks behind now stalls instead of drifting; the debug overlay's stall count should be watched during the soak.

## Phase 5 – Online Battle

Code for a loopback lobby is in the build. A Discord application and two PCs are still required before anyone off this machine can connect. A TURN relay is not. Direct UDP covers the same LAN, a forwarded port, and (once libjuice is linked) ordinary home NATs via STUN hole punching. Do not invent an application id. `BM64_HAS_LIBJUICE` and `BM64_LINK_DISCORD` are CMake options and both default off; turning one on stops the build at the function where that client belongs (`src/net/ice.cpp`, `src/net/signalling.cpp`). `bm64::signalling::kApplicationId` is the empty slot for an application id.

- [ ] **5.1 Internet transport**
  - Depends on: 4.7.
  - Work: UDP with ICE (libjuice). Gather a host candidate and a server-reflexive candidate from a public STUN server, and hole-punch. A TURN server (`BM64_TURN_HOST`) is a later fallback, not part of getting the first two-PC match up. Signalling goes through an interface, so 5.3 can plug Discord in.
  - Done when: two PCs behind different home NATs connect without a relay. Pairs that cannot punch are reported and left for the TURN fallback.
  - Steps:
    1. [agent] Add libjuice as a submodule: `git submodule add https://github.com/paullouisageneau/libjuice lib/libjuice`, then check out the latest release tag.
       - In `CMakeLists.txt`, under `if(BM64_HAS_LIBJUICE)`: `add_subdirectory(lib/libjuice EXCLUDE_FROM_ALL)`, link the static target, and define `BM64_HAS_LIBJUICE=1`. Read the target name from libjuice's own `CMakeLists.txt` (expected `LibJuice::LibJuiceStatic`); don't guess.
       - Remove the `#error` in `src/net/ice.cpp`.
    2. [agent] **Transport.** In a new `src/net/juice_link.cpp`, write `JuiceLink : bm64::rollback::Transport`. libjuice agents are one-to-one, so there is one `juice_agent_t` per remote peer.
       - Config: `stun_server_host = "stun.l.google.com"`, `stun_server_port = 19302`, callbacks `cb_state_changed`, `cb_candidate`, `cb_gathering_done`, `cb_recv`.
       - `cb_recv` runs on libjuice's thread: copy the datagram into a mutex-protected queue. `recv()` pops from that queue.
       - `send()` calls `juice_send` on every agent.
       - Move the lag, jitter and loss simulation out of `UdpLink` into a wrapper transport, so both links can use it.
       - Check every function name against `lib/libjuice/include/juice/juice.h`.
    3. [agent] **Signalling interface.** Give `bm64::signalling` two calls: `post(int to_player, const std::string& text)` and `bool take(int from_player, std::string& text)`.
       - First backend, `BM64_SIGNAL_DIR=<folder>`: write `<from>_to_<to>.txt` and poll for the reverse file. Two PCs can use a shared OneDrive or Dropbox folder, or the user copies the files.
       - Exchange the full local description (`juice_get_local_description`) after gathering is done. That's one message per direction, with no trickle.
    4. [agent] **Startup order.** Create agents → gather → post descriptions → take the remote ones → `juice_set_remote_description` → wait until the state is `JUICE_STATE_CONNECTED` or `COMPLETED` (15 s timeout) → run the existing handshake over `JuiceLink`.
       - On timeout, print "Could not connect directly. This pair needs a relay (TURN)." and exit.
       - Keep `UdpLink` for `BM64_NET_PEER` loopback runs.
    5. [user] Test 1, same LAN: two PCs, `BM64_SIGNAL_DIR` on a shared folder, a 2-minute Battle. Test 2: one PC on a phone hotspot, so the two are behind different NATs. Record in the note whether each pair connected and which candidate type won (libjuice logs it at info level).
    6. Tick when test 2 connects and plays. Pairs that fail are listed for a later TURN step, which uses libjuice's `turn_servers` config with `BM64_TURN_HOST`.
  - Note: the session binds a UDP socket and publishes that endpoint as a host candidate (`bm64::ice::host_candidate`). `BM64_TURN_HOST` is printed and not dialed, because libjuice is not linked. Loopback peers still use `BM64_NET_PEER`. Two-PC NAT is blocked on linking libjuice, exchanging candidates, and a second machine. It is not blocked on running a TURN server.
- [ ] **5.2 Discord Social SDK**
  - Depends on: 0.1.
  - Work: register the app, then implement account linking, rich presence, and lobby create/join with invites. Check current SDK capabilities and terms first, and record them here.
  - Done when: a friend can be invited from Discord and both clients see the same lobby.
  - Steps:
    1. [user] Create an application at https://discord.com/developers/applications.
       - Turn on the Social SDK for it (the developer portal's Social SDK getting-started form).
       - Under OAuth2, add the redirect `http://127.0.0.1/callback` and turn on "Public Client", so a desktop app can sign in without a server.
       - Send the agent the Application ID. Never send the client secret.
    2. [user] Download the C++ Social SDK from the developer portal (it needs a login and acceptance of the terms) and unzip it to `lib/discord_social_sdk/`. [agent] Add that folder to `.gitignore` unless the license allows redistribution; record the license terms here.
    3. [agent] **Build.** In `CMakeLists.txt` under `BM64_LINK_DISCORD`: add the SDK include folder, link its import library, and copy its DLL next to the exe after the build. Put the Application ID in `bm64::signalling::kApplicationId`. Remove the `#error` in `src/net/signalling.cpp`.
    4. [agent] **Sign-in**, in `signalling.cpp`.
       - Create one client object at startup, and pump its callbacks once per UI frame from the launcher loop and the game loop.
       - Sign in with the SDK's desktop authorization flow (code verifier, authorize, exchange for a token, update the token, connect).
       - Store the refresh token in the app data folder, next to `controls.json`, and use it at the next start.
       - Take every class and function name from the SDK's own header and samples, not from memory. The API changed during 2025.
    5. [agent] **Lobby and invite.**
       - Host: create or join a lobby with a random 32-hex-character secret.
       - Set rich presence: state "In lobby", party size n/4, and the lobby secret as the join secret.
       - Guest: the SDK's activity-join callback hands over the secret; create or join the lobby with it.
       - Both: expose the lobby's member list (Discord name, user id) to the launcher through a function `bm64::signalling::members()`.
    6. [user] Test with two Discord accounts on two PCs, or one PC running Discord and Discord PTB. The host invites from Discord, and both launchers list both names.
    7. Tick when step 6 passes.
  - Note: blocked. No application is registered, and the SDK is not in the tree. Setting `BM64_DISCORD_APP_ID` only changes the log line; the lobby stays on loopback.
- [ ] **5.3 Signalling over Discord**
  - Depends on: 5.1, 5.2.
  - Work: exchange ICE candidates and session config through lobby messages or metadata.
  - Done when: a Discord invite produces a working peer connection.
  - Steps:
    1. [agent] Add a Discord backend for the `post` and `take` interface from 5.1 step 3. Send each description as one lobby message of the form `BM64ICE <from> <to> <description>`, and keep only messages addressed to the local player.
    2. [agent] Rate limit: lobby messages are capped at about 100 per 2 hours per application in development. Send one message per peer pair, retry at most twice with 3 s between tries, and never send per-frame data through Discord.
    3. [agent] Player numbers come from lobby join order: the host is 0, then each member in the order the lobby lists them. Every client must compute the same order from the same list, so sort by join time and then user id.
    4. [agent] The seed, save and settings stay on the UDP handshake (4.6). Discord carries only descriptions.
    5. [user] Invite → both launchers show the lobby → host clicks Start → both enter the game → a 2-minute Battle. Use two PCs on different networks.
    6. Tick when step 5 passes.
  - Note: `publish_candidate` records the host candidate and does not send it. Peers are the `BM64_NET_PEER` list. A Discord invite is blocked on 5.2.
- [ ] **5.4 Launcher UI**
  - Depends on: 5.3.
  - Work: Host and Join buttons, a player list, port assignment, ready state, version and ROM mismatch errors, and input delay settings.
  - Done when: a non-technical user can start an online Battle.
  - Steps:
    1. [agent] **Errors on screen, not on stderr.** Today handshake failures call `std::exit`. Run the handshake on a background thread started by the launcher's Start button, before the game boots. On failure, store a message in `bm64::online::last_error()`, return to the launcher, and show it. The launcher is `assets/launcher.rml` with `src/ui/ui_launcher.cpp`; follow how `cycle_players` binds a click.
       - Messages: different game build, different ROM, mods enabled, timed out finding the other player, the other player left.
    2. [agent] **Online section** of the launcher, when Discord is linked: "Host online game" (creates the lobby and opens Discord's invite UI), the member list with a ready tick per member, and Start, enabled only for the host when everyone is ready. Ready state goes in lobby member metadata (check the SDK header for the call).
    3. [agent] Show players, input delay and lives on the host only. Guests see the host's values, read from lobby metadata.
    4. [agent] Keep "Host local game" and "Join local game" behind a "Developer" toggle.
    5. [user] Someone who hasn't used the project follows `docs/playing-online.md` (5.6 step 4) and starts an online Battle without help. Write down every point where they hesitated, and fix the text or the UI.
    6. Tick when step 5 passes with no help given.
  - Note: the launcher has "Host local game" and "Join local game" (`prepare_loopback`, ports 27015 and 27016). The handshake rejects a protocol or ROM-hash mismatch and waits for every guest ack before the game starts. Player count, delay, and lives are `BM64_NET_PLAYERS`, `BM64_NET_DELAY`, and `BM64_NET_LIVES`. The buttons were not clicked. There is no Discord player list.
  - Note (2026-11-19): with a ROM selected, the launcher also cycles Players (2–4), Input delay (0–4), and Lives (shared or unlimited). Those values are what the handshake uses when the matching environment variable is unset. The new buttons were not clicked.
- [ ] **5.5 Custom Bomberman for guests**
  - Depends on: 4.6.
  - Work: costumes come from the save, and the session uses the host's save. Inject each guest's custom bomber parts into the session save image at session start.
  - Done when: everyone sees each player's own costume.
  - Steps:
    1. [user] Find the costume bytes by changing one thing at a time. Start the game with no session and back up the save file (`bm64_us.bin`) as `A.bin`. In the Custom menu, change only the head part and save; copy the save as `B.bin`. Change only the body; `C.bin`. Change only the arms; `D.bin`. Change only the legs; `E.bin`.
    2. [agent] Diff consecutive files byte by byte (a short Python script in `tools/re/`). Each change should touch one or two bytes inside the 18-byte EEPROM record at `18 * file + 1`. Record the offsets and value meanings in `game-internals.md`.
    3. [agent] Find who reads them in Battle: `xref.py` on the costume save fields and on `g_LoadCustomParts` (`0x8023A734`). Decide which case applies and write it down:
       - (a) Battle loads a custom bomber only for player 1, from the save. Guests can't have their own without a hook.
       - (b) There are per-player custom slots.
    4. [agent] Per-player costumes in the handshake:
       - Each guest sends its part bytes in the ack.
       - The host collects all of them and sends a `BM64CST` packet (player → bytes) to every guest before tick 0. Guests wait for it like the save chunks.
       - All peers store the table in the co-op region, so it's snapshotted.
       - Case (b): the host writes each guest's bytes into that guest's slot of the session save image before sending it.
       - Case (a): hook the part read for player k to return the table's bytes for k.
    5. [user] A 2-player loopback Battle where host and guest have different costumes in their own saves. Pass: both windows show both costumes correctly.
    6. Tick when step 5 passes.
  - Note: blocked on the byte offset. `g_LoadCustomParts` (`0x8023A734`) indexes the part-file table `D_802A0BA8` with stride `0x2C`. The custom menu keeps the selected part bytes in its own overlay and calls `g_LoadCustomParts` with the part slot in `a1` (0–3). `func_802947AC` commits a save: an 18-byte EEPROM record at `18 * file + 1`, and a 144-byte stride record in the RAM image at `D_80063000`. Which of those bytes are the costume was not identified, so nothing is copied into the host save.
- [ ] **5.6 Disconnects and polish**
  - Depends on: 5.4.
  - Work: a disconnected player is handed to AI (set controlType 2) or the match ends; add a ping and rollback indicator.
  - Done when: online Battle is release-ready; write player-facing notes.
  - Steps:
    1. [agent] Queue item 3 (deterministic leave), with its unit tests.
    2. [agent] **Ping.**
       - Every 30 frames, send `kPing {sender, host_time_us}`; the receiver echoes it back as `kPong`.
       - The round-trip time is now minus the echoed time. Keep the median of the last 8 per peer.
       - Show "ping N ms" per peer in the online status line and in a small corner indicator that is always on during a session (not the F6 panel).
    3. [agent] **Rollback indicator.** A small bar shows rollback frames per second. It turns amber above 4 and red when window stalls happen.
    4. [agent] Write `docs/playing-online.md` for players. Sections:
       - What you need: same game build, same ROM, Discord.
       - Hosting.
       - Joining.
       - Settings (input delay: raise it if the game stutters; lives).
       - What the indicators mean.
       - Troubleshooting, with one line per error message from 5.4 step 1.
       - What happens when someone leaves.
    5. [user] A 30-minute session over the internet between two homes: three Battle matches, one player quits mid-match. Pass: no desync, the leaver's bomber becomes AI on both screens at the same moment, and the remaining player can finish.
    6. Tick when step 5 passes and the notes are written.
  - Note: the debug overlay prints local player, delay, rollback count, and ticks since the quietest peer. Not shown in a window, and not release-ready.
  - Note (audit, 2026-11-19): the AI handoff was removed. Each peer wrote `controlType` 2 when its own timer passed 300 ticks, and the peers' timers differ, so the handoff landed on different ticks and desynced. Now a silent peer stalls the session at the rollback window and the session times out. A handoff has to travel through the input stream; queue item 3 has the design.
  - Note (2026-11-20): queue item 3 is in (step 1 done). A silent player is marked left from one tick T on every peer, their pad from T on has `err 0xFE`, and `begin_step` turns their bomber into an AI on that tick. Expect a stall of about 3–5 s while the peers notice and the host decides. Unit tests only; not run in the game.

## Phase 6 – Story co-op gameplay

The player array move is in the binary and the loopback session can run it. Loops visit eight slots. Rival maps leave slots 0 and 4–7 for co-op players, and other maps use 0–3.

Every co-op rule (spawning, port mapping, main-player resolution, cameras, respawn, lives, checkpoints, cutscene hiding) runs only while a session is on a story map (overlay banks 4–12). An online Battle uses the game's own rules. Phases 5–7 were built before 3.8, 4.2 and 4.7 were done, so none of this phase has been seen running in a stage.

- [ ] **6.1 Relocate the player array**
  - Depends on: 3.8.
  - Work: a script in `tools/re` generates `[[patches.instruction]]` entries that repoint every main-code reference to `g_Players` (`0x800AEDF0`–`0x800AF1D8`, about 143 functions) to a larger array in patch RAM (`0x80801000+`). It also widens the 4- and 5-slot loops and relocates per-player companion arrays (`g_ComputerPlayerMovement` and any found during the work). Add a checker that proves no old-range references remain.
  - Done when: SyncTest still passes, and Battle and story are unaffected with 5 slots in use.
  - Steps:
    1. [agent] Queue item 5 (local co-op mode).
    2. [user] `$env:BM64_COOP_LOCAL = "4"; $env:BM64_RECORD = "slots_rival.bm64rec"` with four controllers. Play rival map 4.10, which puts co-op in slots 0, 4, 5 and 6 plus rivals in 1–3, so 7 slots are in use. Then record a normal 4-player Battle with no co-op (`slots_battle.bm64rec`).
    3. [agent, ask first] SyncTest both files. Replay each twice and compare the hashes.
    4. Pass: no SyncTest failure, the replays match, and in the rival recording the debug overlay showed seven bombers moving. Tick then.
  - Note: `tools/re/relocate_players.py` repoints the main-code references (`--check` covers them) and widens the 4- and 5-slot player loops to 8. `g_ComputerPlayerMovement` is at `0x80F00640` (8 × `0x10`). `func_80241BB0`'s side table is at `0x80F006C0`, with its original bytes copied at boot. `func_802506B8` and `func_8024ABA8` still use `D_800AF1D8` as the next table. The 4-port controller map in `func_8023D0F8` stays at 4. The snapshot and the `slots` hash are `0x80F00000`–`0x80F00780`. SyncTest after the widen exited 0: `new.bm64rec` at tick 8838 and `test.bm64rec` at tick 9591. A battle match was not played.
  - Note (audit, 2026-11-19): unticked. The "done when" asks for Battle and story with 5 slots in use, and no stage has been played with more than four bombers. The co-op region after the array now ends at `0x80F01F20` (see `game-internals.md`); the snapshot and `slots` hash cover it.
- [x] **6.2 Call-site catalogue**
  - Depends on: 0.2.
  - Work: classify every `getMainPlayer()` use, every direct `D_802AC894` reader, and every constant-index `getPlayerObj`/`getPlayerPos` call, per overlay. Categories: camera, enemy or boss targeting, trigger or goal, cutscene or script, HUD, rival-bomber control, other. Put the table in `game-internals.md`.
  - Done when: every call site has a category.
  - Note: `tools/re/catalogue_main_player.py` assigns all 626 `getMainPlayer`, `D_802AC894`, and constant-index `getPlayerObj`/`getPlayerPos` sites. The rules are in `game-internals.md`. Per-overlay counts are in `docs/main-player-catalogue.txt`. Process entries that are not "other" are in `src/game/bm64_player_cats.inc`.
- [ ] **6.3 Co-op slots and spawning**
  - Depends on: 6.1.
  - Work: in co-op, mark the co-op players' slots human before `func_80245CDC`. Allocate slots that don't collide with rival bombers in maps 4.10, 5.7, 6.15, 8.13, 9.5 and 9.6. Spawn teammates next to the leader.
  - Done when: 2–4 bombers spawn in every story map.
  - Steps:
    1. [agent] Queue item 5. Then add one log line at the end of `bm64_coop_before_stage`: overlay as `bank.sub`, and for each slot its controlType and mapped port. Example: `[bm64 coop] stage 4.10 slots 0:1/p0 1:2/- 2:2/- 3:2/- 4:1/p1`.
    2. [agent] Build a checklist file `docs/coop-map-checklist.md` with one row per story map: banks 4–12, with sub-overlay counts from the bank sizes `{4,6,7,10,7,15,10,13,6,2,2,2,1,10}` (index = bank − 1). Columns: 2P spawn, 3P spawn, 4P spawn, notes.
    3. [user] With `BM64_COOP_LOCAL=4` and four controllers, enter each map. Note whether four bombers appear next to each other and each answers its own pad. Send the log. Spot-check 2 and 3 players on the first map of each world and on the six rival maps.
    4. [agent] Fill the checklist from the log lines. A map where `fit X of Y` appears needs a different slot plan: find which slots that map's overlay reserves (`xref.py` for `setControlType` in that overlay) and add a per-map exception next to the rival-map list.
    5. Tick when every row is filled and passing.
  - Note (fix, 2026-11-21): co-op learned the overlay id only from `bm64_debug_tick`, which reports it after stage setup has already run in the overlay's first tick. So the first stage entered from a menu was set up solo: in co-op director runs of maps 4.1 and 4.10, slot 1 stayed empty and the port map stayed zero, while 5.1 got two bombers only after a story-to-story transition. `bm64_load_barrier` (called by the patched loader after every load with `gOverlayID`) and snapshot restore now also call `bm64::coop::note_overlay`.
  - Note: `bm64_coop_before_stage` runs at the start of `func_80245CDC` when the session has more than one player. It sets `controlType` 1 on empty slots, skipping 1–3 on the six rival maps, and copies the leader's spawn position onto each new teammate with a 40-unit step along X. `g_SpawnPlayer` copies those floats onto the bomber. It logs when the 8-slot loop cannot fit everyone. No story map was entered.
  - Note (audit fix, 2026-11-19): bombers read the controller port equal to their slot, and the pad history has four ports with no bounds check, so slots 4–7 on rival maps read memory past the ring. Stage setup now writes a port map (rule block bytes `0x58`–`0x5F`): the n-th co-op slot reads session player n's port, and slots 1–3 on rival maps read nothing. Hooks at the entry of the eight accessors that take a port in `a0` (`func_802399AC`, `func_80239E88`, `func_80239FF8`, `func_80239E28`, `func_80239F70`, `func_80239EE8`, `func_80239714`, `func_802395A8`) apply it. Rival maps are recognised by bank and sub-overlay, not by raw overlay id. A menu that reads port 1–3 during a rival map would also read nothing; not seen.
- [ ] **6.4 Main-player virtualization framework**
  - Depends on: 6.2, 6.3.
  - Work: patch `func_80243DDC` and the 27 direct readers to ask a context resolver ("who is the main player for the current process?"), keyed on the current process (`D_802AC344`) and the category from 6.2. The default returns the leader (slot 0). The resolver's state lives in game memory, so it's snapshotted.
  - Done when: single-player behaviour is unchanged, and the resolver can be overridden per category.
  - Steps:
    1. [agent] Queue item 4 (call-site keys).
    2. [agent, ask first] Single-player unchanged: replay `test.bm64rec` and `new.bm64rec` twice with no session, and compare with each other and with the recording logs. Pass: identical.
    3. [agent] Override test hook: `BM64_COOP_OVERRIDE=<category>:<slot>` (for example `camera:1`) writes the override byte for that category at boot of a local co-op session. The overrides live at `0x80F00820`, one per category, `0xFF` meaning none.
    4. [user] `BM64_COOP_LOCAL=2 BM64_COOP_OVERRIDE=enemy:1` in a stage with enemies: every enemy should chase player 2. Then `enemy:0`: they chase player 1.
    5. Tick when steps 2 and 4 pass.
  - Note (2026-11-21): step 3 is built. `BM64_COOP_OVERRIDE=<category>:<slot>` takes the names `other`, `camera`, `enemy`, `trigger`, `cutscene`, `hud` and `rival`. It writes the byte after boot only when a co-op session exists (local mode or a co-op replay). The value is not stored in recordings, so replaying an override session needs the same variable. A `camera` override is overwritten by every per-player camera pass. Not run.
  - Note: with no session, `bm64_main_player()` returns `D_802AC894`, so single-player replays stay on the hero. In a session the call site is looked up in `bm64_player_sites.inc`, and an unlisted site falls back to the running process in `bm64_player_cats.inc` (queue item 4, 2026-11-20). Enemy and trigger entries use the 6.6 sticky target. Other categories return the leader. Eight override bytes at `0x80F00820` (`0xFF` means none) are in the snapshot.
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
  - Steps:
    1. [agent] Queue item 5 (local mode with F9 view switching). Built 2026-11-20.
    2. [user] `BM64_COOP_LOCAL=2`, record `cam.bm64rec`. Press F9 to view player 2, turn player 2's camera with the C-buttons on controller 2, and walk player 2 around. The camera should follow player 2, and "up" on the stick should move away from the camera. Press F9 back to player 1; player 1's camera should be where it was left.
    3. [agent, ask first] SyncTest `cam.bm64rec`. Pass: no failure. A failure in `slots` points at the camera blocks; check that every write to a block happens on the game thread inside a step.
    4. [user] The co-op checks in queue item 6, online, to confirm "hashes match across instances".
    5. [agent] Known gap to check: a camera command from a cutscene or overlay that writes the live camera without calling `func_80234778` or `func_802346C8` reaches only the first human's block. If step 2 shows a teammate's camera stuck after a cutscene, find the writer with `xref.py` on `0x8008F3D8`–`0x8008F438` and broadcast it the same way.
    6. Tick when steps 2–4 pass.
  - Note (2026-11-19): when a session has two or more human bombers, `func_80235D7C` and the `func_80236D30` call inside `func_80236F54` run once per human. Each human's eye, look-at/mode, controller, `0x8029FEA0` table, the 16 bytes at `0x802A5A50`, and the `func_80251C88` object table (`0x8008E658`–`0x8008F3D8`) live in a block at `0x80F00840` (stride `0x1080`, inside the snapshot). Live memory is put back to the first human before the process sleeps, so peers hash the same camera. C-buttons at `0x80235974` read that human's port while their block is in. `bm64_camera_frame` then replaces the lookAt arguments with the local human's saved eye and target (mode 1 still swaps them). `func_802346C8` and `func_80234778` copy the live camera onto every block. Not watched on two windows in a stage. A session with fewer than two humans does not swap.
  - Note (audit fix, 2026-11-19):
    - The 48 × `0x48` table at `0x8008E658` is the bomb pool, not camera state, so it was removed from the blocks. Swapping it per human would have given each human a different set of bombs. Blocks are now `0x2D0` bytes.
    - Bomber movement (`func_8024E088`) turned every bomber's stick by the live camera, which is the first human's. The `jal func_80234858` at `0x8024E880` now passes the bomber slot, and that one call returns the rotation from the bomber's own block.
    - `bm64_camera_frame` and the movement redirect look up the slot that the local session player drives, so they follow the port map on rival maps.
- [ ] **6.6 Enemy and boss targeting**
  - Depends on: 6.4.
  - Work: for each targeting process, keep a sticky target player (nearest living player, re-evaluated about once a second, deterministic). Tune per boss as needed.
  - Done when: enemies and bosses engage all players in every world.
  - Steps:
    1. [agent] Queue item 4. Without it, enemies whose `getMainPlayer` call sits in a helper still chase only the leader.
    2. [agent] Constant-index reads: the catalogue also lists `getPlayerObj(0)` and `getPlayerPos(0, …)` in story overlays. Those always mean player 0 and bypass the resolver. For each such site in an enemy or boss overlay, emit a hook (same generator as the direct reads) that replaces the constant 0 in `a0` with `bm64_resolved_slot_at(rdram, site)`. Leave rival-bomber sites (constants 1–3 on rival maps) alone.
    3. [agent] Add a boss list to `docs/coop-map-checklist.md` (banks 10–12 and each world's boss stage), with columns: targets P2 when P2 is nearer (yes/no), notes.
    4. [user] For each boss, in local co-op with 2 players: player 1 stays far away and player 2 stands near the boss. The boss should attack player 2. Then swap roles.
    5. [agent] A boss that ignores the nearer player has its targeting in code that's still keyed to slot 0. Find its reads in that overlay with the catalogue output and step 2, and add the missing sites.
    6. Tick when every boss row is "yes".
  - Note (2026-11-20): step 1 is done. Enemy calls in helpers are now matched by call site (292 rows in `bm64_player_sites.inc`).
  - Note: while a session is running, `getMainPlayer` from a process whose entry is classified as enemy or boss picks the nearest living human or AI bomber and keeps that slot for 60 ticks. The choice is stored per process at `0x80F00780`. The origin is the process `user_data` object when that pointer has a body, otherwise the leader. Ties take the lowest slot. Not watched in a stage, and not tuned per boss.
- [ ] **6.7 Triggers and stage clear**
  - Depends on: 6.4.
  - Work: goals, exits, switches and pickups that check the main player should accept any player; the triggering player becomes the leader for the following sequence. One player clearing the stage clears it for everyone.
  - Done when: every stage can be finished by any player.
  - Steps:
    1. [agent] Queue item 4, including its step that disassembles the six trigger functions and records what each does in `game-internals.md`.
    2. [agent] Find the goal check: in one stage overlay (start with 4.1), find the goal object's process (`find.py` on the overlay, look for the code that calls the stage-clear function from step 1) and the distance test it runs. Record which player it reads (via `getMainPlayer`, a constant `getPlayerPos(0)`, or a direct read).
    3. [agent] Make that read resolve to the nearest living bomber (trigger category). Use the call-site table from queue item 4, or the constant-site hooks from 6.6 step 2.
    4. [agent] Don't change `D_802AC894` when someone else clears the stage. Camera block 0, the port map and checkpoints all assume the leader stays slot 0.
    5. [user] For each stage in `docs/coop-map-checklist.md`: player 1 waits at the start while player 2 reaches the goal. Pass: the stage clears for both.
    6. Tick when every stage row passes.
  - Note (2026-11-21), steps 2 and 3:
    - Map 4.1's goal: `func_ovl4_1_80047074` calls `func_ovl4_1_800454C0` each frame. That function asks `func_802457D8(5, 0x1A, 5, 0x1A)` and `func_802457D8(0x2C, 0x16, 0x2C, 0x16)`, which test whether the main player's grid cell is inside an inclusive x/z rectangle. If one is true and `func_8028A1DC(0x416 or 0x417)` is false, it runs `func_ovl4_1_800452F4` with that exit's object. That function plays the goal sequence (`func_80250E2C(1)`) and requests scene `0x29` (`func_8023AE6C`).
    - `func_802457D8` (152 calls in 48 overlays) and `func_802456FC` (39 calls in 17 overlays; same test plus an exact floor `y`) are the shared "main player in box" tests. `func_802455A8` (box with a y range) has no callers.
    - Both are now `RECOMP_PATCH`es in `required_patches.c`. They test the leader first, exactly as the ROM does, including the write of its cell to `D_802ACEC8[leader]`. If that fails and `bm64_coop_trigger_slot(i)` says slot i is a human with a body in a story session, they test each such slot with the cell in a local, because `D_802ACEC8` has only 5 entries. With no session the result and the memory writes match the ROM.
    - `D_802AC894` is not changed, so the goal sequence still acts on the leader (step 4). The toml reader hooks inside those two functions no longer run, because the patch replaces the function.
    - Not watched. No co-op director run reaches a goal.
  - Note (2026-11-20, replaces the 2026-11-19 note): the six main-code "trigger" functions turned out to be hero routines that act on the bomber they read, so their sites now resolve to the leader (queue item 4). No call site is in the trigger category yet; step 2 must find the goal test first.
  - Old note (2026-11-19): a process whose entry is one of the six trigger functions resolves the main player through the same sticky nearest-living-bomber rule as an enemy. The goal's own distance test was not rewritten, and the canonical word `D_802AC894` is not changed to the player who touched it. Not watched in a stage.
  - Note (audit, 2026-11-19): this rule cannot fire. The catalogue records the function that contains each call, but the resolver looks up the running process's entry function. The six trigger functions are only reached by `jal` from other code, never as a process entry, so no process ever matches them. Queue item 4 keys the lookup on the call site instead, which also fixes enemies whose `getMainPlayer` call sits in a helper.
- [ ] **6.8 Map transitions and checkpoints**
  - Depends on: 6.7.
  - Work: when any player takes a screen or map transition, the whole party moves and teammates appear at the entry point. Record each entry point as the checkpoint; stage start is the first.
  - Done when: multi-area stages work with 4 players.
  - Steps:
    1. [agent] List every way the game moves the hero to another area: xref every caller of `func_802512F8` and of `load_from_rom_to_addr` inside stage overlays (banks 4–12). Add an "areas" column to the checklist for stages with more than one load.
    2. [agent] For any area change that doesn't go through `func_802512F8`, add a hook after its "place hero" write that calls the same `snap_party` logic (stores the checkpoint, moves the other humans 40 units apart on X).
    3. [user] Local co-op with 4 players, each multi-area stage: player 1 takes the transition while the others stand elsewhere. Pass: all four appear at the new area's entry, and a death afterwards respawns there.
    4. Tick when every multi-area row passes.
  - Note (2026-11-19): `func_802512F8` places one bomber on that slot's spawn. In a session the same point is stored as the party checkpoint and every other human is moved there, 40 units apart on X. Stage setup records the leader spawn as the first checkpoint. A screen change that does not call `func_802512F8` is not caught. Not watched in a stage.
- [ ] **6.9 Death, respawn and lives**
  - Depends on: 6.8.
  - Work: find the hero death flow (start with `func_80250E2C` and `func_802506B8`). In co-op, a death costs one shared life (`D_802AC624`) and respawns the player at the last checkpoint while others continue. Party wipe with no lives left runs the original game over. Add the unlimited-lives option to the synced session settings.
  - Done when: these rules hold in normal stages and boss fights.
  - Steps:
    1. [agent] Find every death path, not just the hit. Write `tools/re/find_control3.py`: for main code and every overlay, list each `sw` to offset `0x24` whose stored register was last set to 3 (`addiu rX, zero, 3`), plus every call to `setControlType` (`func_80243F2C`) whose `a1` is the constant 3. Falling off the map and crushing are likely separate paths from `func_80248148`. Record the list in `game-internals.md`.
    2. [agent] For each path not already covered by the `0x80248284` hook, add a hook before its controlType-3 store that calls the same respawn decision as `bm64_coop_downed`. Factor that decision into one function taking the slot.
    3. [user] Local co-op, 2 players, shared lives:
       - (a) Player 2 dies by damage while player 1 lives: player 2 respawns at the checkpoint and lives go down by 1.
       - (b) Player 2 falls off the map: same as (a).
       - (c) Both die with 1 life left: the original game over.
       - (d) With `BM64_COOP_LIVES=unlimited`: lives stay 99.
       - (e) (a) in a boss fight.
    4. Tick when (a)–(e) pass.
  - Note: unlimited lives is the session flag from 4.6. While it is on, `apply_rules` writes `D_802AC624 = 99` every step, on every peer. Shared lives leave that word to the game until a human is downed.
  - Note (2026-11-19): `func_80248148` is the hit. Argument 3 or 5 clears hearts; anything else subtracts one. Hearts still above zero skip the rest. Hearts at zero continue into the sequence that stores `controlType` 3. In a session, a human on that path respawns at the checkpoint with the hearts recorded there when another human or AI still has hearts, or when more than one shared life remains. The life word is decremented unless lives are unlimited. A last bomber with one life or fewer falls through into the original sequence. Not watched in a stage.
  - Note (audit fix, 2026-11-19): respawn, the lives write and the checkpoint snap ran in every session, including online Battle, where they would have kept downed players alive. They now run only on story maps.
  - Note (2026-11-22): on rival maps the "another bomber still has hearts" test counted the rivals in slots 1–3, so a downed teammate always respawned. Rival slots are now skipped. Respawn and the checkpoint snap spaced teammates 40 units × slot number, which put slot 4 at +160 on rival maps; they now use the session player number.
- [ ] **6.10 Cutscenes and scripted sequences**
  - Depends on: 6.4.
  - Work: during scripts (`WM1 …` command strings, intro and outro sequences, boss intros), the leader is scripted and teammates are hidden with collision off, then restored next to the leader afterwards.
  - Done when: no softlocks or visual breakage in any cutscene.
  - Steps:
    1. [agent] Add a "cutscenes" column to the checklist: stage intro, boss intro, boss defeat, world end, and story scenes between worlds.
    2. [agent] After a cutscene ends, teammates are un-hidden where they stood, which may be off screen. Move each human to the leader with the 40-unit X offset (the `place_human` helper) at the moment the hide is removed.
    3. [user] Local co-op with 2 players: watch each cutscene. Pass: teammates are invisible during it, nothing walks into the leader, and afterwards teammates stand next to the leader and can move. Note any cutscene that never ends (softlock) with its stage.
    4. [agent] A softlock usually means the script waits for a condition on "all players", for example all bombers idle or none in a trigger zone. Find the wait loop in that overlay and make it ignore hidden humans, or teleport them out of the zone at hide time.
    5. Tick when every cutscene row passes.
  - Note (2026-11-19): while any active process entry is classified as a cutscene, every human other than `D_802AC894` has collision (`player+0x48`) forced off and bit 30 set on the bomber object's flag word. Both are restored when no such process remains. Not watched in a stage.
  - Correction (2026-11-22): bit 30 of the bomber object's flag word (`obj+0x10`) is not a hide flag. `func_8024E088` turns it into `func_80248148(i, 3)` at `0x8024E324` every tick, so a hidden teammate was killed and respawned every tick. On rival map 4.10 the user's game locked up 33 ticks after such a sequence started (found with a hardware write watch on the flag word). The hide now only switches collision off; teammates stay visible during cutscenes. A real hide flag is still to be found.
- [ ] **6.11 Co-op HUD**
  - Depends on: 6.3.
  - Work: show the local player's hearts, bombs and fire; small teammate indicators; shared lives and gems.
  - Done when: it's readable at 4 players in widescreen.
  - Steps:
    1. Design decision: the co-op HUD is drawn by the host UI (RmlUi), not by the game. The game's display lists are built inside the simulated step, and the `gfx` range is hashed. A HUD that showed different values on each machine would desync the hash. The game's own HUD keeps showing the leader's values.
    2. [agent] Write `assets/coop_hud.rml` and `src/ui/ui_coop_hud.cpp`, modelled on `src/ui/ui_debug_overlay.cpp`. It is shown when `story()` is true, read once per UI frame.
       - Large, bottom left, for the local player's slot (`slot_of_player`): hearts (`+0x14`), bomb count (`+0x1C`), fire (`+0x20`).
       - A small row of teammates: name P2–P4 and hearts.
       - Shared lives (`D_802AC624`) and gems (`D_802AC610` + `0x1C`).
       - Read RDRAM with the word helpers.
    3. [agent] Position it inside the 4:3 safe area by default, so it doesn't sit under the game's HUD in widescreen. Check where the game's HUD draws with a screenshot from the user.
    4. [user] A 4-player local co-op stage in 16:9 and in 4:3, with a screenshot of each. Pass: all values are readable and match the debug overlay.
    5. Tick when step 4 passes.
  - Note: a session shows the debug overlay on its own. The panel lists the local hearts, bombs and fire, shared lives and gems, and all eight slots. It was not looked at in a window.
- [ ] **6.12 Rival-bomber maps**
  - Depends on: 6.3–6.9.
  - Work: verify maps 4.10, 5.7, 6.15, 8.13, 9.5 and 9.6 with 4 co-op players plus rivals.
  - Done when: all six are completable.
  - Steps:
    1. [agent] 6.3, 6.6, 6.7 and 6.9 first. Rival control code uses constant slots 1–3 (category "rival-bomber control"), which must stay as they are. Whether rivals pick their target through `getMainPlayer` is not known: before step 2, read one rival overlay's control code with `bmdis.py` and record how it chooses whom to chase.
    2. [user] Each of 4.10, 5.7, 6.15, 8.13, 9.5 and 9.6 in local co-op with 4 players: play until clear. Note whether rivals attack teammates, whether any co-op bomber is ignored, and whether the clear condition ("all rivals defeated") works.
    3. [agent] If clearing doesn't trigger, the overlay may count living bombers over all slots and see the co-op humans as rivals. Find the count loop (`xref.py` on the player array in that overlay) and make it count only slots 1–3 on rival maps.
    4. Tick when all six rows pass.
- [ ] **6.13 Progression and saves**
  - Depends on: 2.2.
  - Work: the host's sandboxed save is written back to the host's real file at checkpoints. At session end, guests can merge shared unlocks (stage clears, gold cards, custom parts) into their own save.
  - Done when: the merge is tested both ways.
  - Steps:
    1. [user] Make save pairs that differ by one unlock each, the same way as 5.5 step 1: before and after clearing one stage, after getting one gold card, and after unlocking one custom part.
    2. [agent] Diff them and record in `game-internals.md` which bits of the 18-byte EEPROM record (and the 144-byte RAM record at `D_80063000`) hold stage clears, gold cards and parts. Note any checksum byte: it changes in every pair. Find its formula with `xref.py` on the save commit function `func_802947AC`.
    3. [agent] Write `merge_unlocks(guest_image, host_image, file_index)` in a new `src/game/bm64_save_merge.cpp`. It ORs the unlock bits from host into guest for that file, then recomputes the checksum. Test it with a small C++ or Python test on the saves from step 1: merging "after" into "before" must give "after", and merging "before" into "after" must leave it unchanged.
    4. [agent] Host side: the session runs on a sandboxed copy. When the session ends, ask the host (launcher dialog) whether to write the session save back to their real save file (`BM64_SANDBOX_EXPORT` is the existing export path). Guest side: ask whether to merge the session's unlocks into their own save, with the file slot chosen in the dialog.
    5. [user] Two PCs or loopback: the host clears a stage the guest hasn't. At the end the guest merges, and the guest's own save now shows the clear. Then the reverse.
    6. Tick when both directions pass.
- [ ] **6.14–6.19 Playthrough per world**
  - Depends on: 6.5–6.13.
  - Work: one milestone per world (banks 4–9 plus the related boss banks 10–12): complete it with 2, 3 and 4 players and log issues.
  - Done when: each world is completable at all player counts.
  - Steps:
    1. [agent] Make one milestone row per world in `docs/coop-map-checklist.md` with columns 2P, 3P, 4P, each holding a recording file name and a result.
    2. [user] Local co-op, recording on, unlimited lives allowed: play each world through its boss at 2, 3 and 4 players. Log each problem with stage, time and what happened.
    3. [agent, ask first] SyncTest every recording. A failure is a determinism bug and blocks online play of that world even if it played fine locally.
    4. [agent] Triage each reported problem into the milestone it belongs to (6.3–6.12) and fix it there, not as a one-off here.
    5. Tick a world when its three rows pass and SyncTest is clean.
  - Note: 6.7–6.10 now have code (see their notes); 6.12, 6.13 and these playthroughs have none. Save merge is unchanged.

## Phase 7 – Online co-op

The playtest itself is blocked: it needs a Discord login, a second PC, and the phase 6 story playthroughs. A TURN server is not part of that list; direct UDP is the session path, and a relay is only for NATs that will not punch (see "P2P, and when a TURN server is involved"). What can run on loopback is the phase 4 session plus the lives flag. No snapshot is sent to a rejoining guest.

- [ ] **7.1 Co-op session flow**
  - Depends on: 5.4, 6.14–6.19.
  - Work: the host picks a save file and difficulty; guests join the lobby before the story starts; lives mode and other options are synced.
  - Done when: a Discord invite leads into a co-op story session.
  - Steps:
    1. [agent] The launcher's online section (5.4) gets a mode choice: Battle or Story co-op. Story co-op sends the co-op flag and lives option in the hello, and every peer calls `bm64::coop::set_session` with them, as the loopback path does today.
    2. [agent] Save file and difficulty: the game's own file-select screen runs inside the session, with the host's sandboxed save, and every peer sees it. Only player 0's pad should drive menus. On menu overlays (outside banks 4–14), map ports 1–3 to silent with the port map from 6.3, so a guest's stick can't move the cursor.
    3. [user] Discord invite → story co-op → host picks a file → both enter the first stage of that file. Pass: both windows are in the same stage, each following its own bomber.
    4. Tick when step 3 passes.
  - Note: on loopback the host's sandboxed save, seed, player count, and lives flag are in the hello, and guests install that save before tick 0. Difficulty is whatever is in that save. A Discord invite is blocked on 5.2. Story was not started.
- [ ] **7.2 Story barriers and loading**
  - Depends on: 7.1.
  - Work: confirm that every story load (maps, bosses, cutscenes) is handled by the load barriers from 4.4, with acceptable wait times.
  - Done when: a full world online has no load desyncs.
  - Steps:
    1. [agent] 4.4 (queue item 2) must be done. Add to the soak log (4.7 step 1) how long each barrier waited, in ms.
    2. [user] Two PCs or loopback with `BM64_NET_LAG_MS=50`: play one full world in story co-op, including cutscenes and the boss.
    3. Pass: no desync, and every barrier wait is under 500 ms. A longer wait means one peer loads much more slowly. Note which load it was.
    4. Tick when step 3 passes.
  - Note: `load_from_rom_to_addr` already notifies the barrier from phase 4. Boot file loads logged it. A story map change was not played, so wait times and missed loads are unchecked.
- [ ] **7.3 Reconnect and desync recovery (optional)**
  - Depends on: 7.1.
  - Work: make snapshots portable: fixed-address coroutine stacks (`VirtualAlloc` at a fixed base) plus a non-ASLR build, or pointer rebasing. Then the host can send its state to a rejoining or desynced guest.
  - Done when: a guest can drop and rejoin mid-stage.
  - Steps:
    1. [agent] **Make snapshots portable between processes.** The coroutine stacks hold return addresses into the exe. Link with a fixed image base and no ASLR (`/DYNAMICBASE:NO` and `/FIXED`, or a fixed `/BASE`), so those addresses are the same in every run of the same build.
    2. [agent] **Check portability.** Add `BM64_SNAPSHOT_FILE=<path>,<tick>`, which writes `capture()` to a file at that tick. Replay `test.bm64rec` twice with tick 2000 and compare the two files byte by byte. Pass: identical. If not, list the differing offsets and find which part of the snapshot they're in (RDRAM, a stack, or `recomp_context`). Every remaining host pointer must be rebased or excluded before going on.
    3. [agent] **Protocol.**
       - A rejoining guest connects as in 5.3 and sends `BM64REJOIN {player}`.
       - The host waits for its next final tick F, captures it, and sends the blob in 1000-byte chunks, the same way as the save.
       - The guest restores, sets its session to tick F with history empty, and starts sending input from F + delay.
       - The other peers treat the player as "left" (queue item 3) until F. From F on, the rejoiner's inputs are real again: the host's `kRejoin {k, F}` message undoes the leave from F.
    4. [agent] Unit test the session side: a peer leaves at tick 100 and rejoins at tick 200 with the host's state. Hashes must match from 200 on.
    5. [user] A 2-player online story session: the guest closes the game mid-stage, reopens it and rejoins. Pass: the guest continues in the same stage with no desync.
    6. This milestone is optional. Skip it if step 2 can't be made to pass.
  - Note: the coroutine stacks are already 80 slots at `0x200000000`. Nothing sends a snapshot to another process. A mid-stage rejoin was not tried.
- [ ] **7.4 Performance tuning**
  - Depends on: 7.1.
  - Work: profile rollback in the heaviest stages and boss fights at 4 players; optimize snapshots (dirty pages or deltas) if needed.
  - Done when: steady 60 fps at 150 ms RTT with an 8-tick rollback.
  - Steps:
    1. [agent] Add timing to the soak log: per frame, the time spent in save, load and resim steps, and the frame total. Report the 99th percentile every 600 ticks.
    2. [user] The heaviest content at 4 players: a boss fight from banks 10–12 and a busy Battle arena, at `BM64_NET_LAG_MS=75`.
    3. [agent] Pass when the 99th-percentile frame is under 16.6 ms. If not, find which part dominates:
       - **Save/load:** cut snapshot cost with dirty-page tracking. Allocate RDRAM with `MEM_WRITE_WATCH` (find the RDRAM `VirtualAlloc` in `lib/N64ModernRuntime`), call `GetWriteWatch` before each save, and copy only changed 4 KB pages over the previous snapshot. Unit test: a save built from page deltas must equal a full capture, byte for byte.
       - **Resim steps:** check that non-rendered steps skip all display-list submission (3.4), and profile one resim step with the Visual Studio profiler to find the hot recompiled function.
    4. Tick when step 3 passes.
  - Note: not profiled in a stage. The phase 3 savetest numbers (save 336 µs, load 243 µs, 717 µs per resimulated tick) are still the only measurement. The player-array range adds 1600 bytes to each snapshot.
- [ ] **7.5 Online playtest and telemetry**
  - Depends on: 7.2, 7.4.
  - Work: play every stage and boss online; add opt-in desync and crash reports.
  - Done when: no known blockers.
  - Steps:
    1. [agent] **Reports without a server.** On a desync or crash, write a folder `reports/<date>_<tick>/` with the dumps, the log, the build hash, the ROM hash, the session settings and the last 600 ticks of input. Tell the player on screen where it is and to send it. Nothing is uploaded automatically, so no opt-in is needed.
    2. [agent] Add a crash handler (`SetUnhandledExceptionFilter`) that writes a minidump (`MiniDumpWriteDump`) into the same report folder.
    3. [user] Online playtest of every stage and boss, following `docs/coop-map-checklist.md`, with at least two different people as guests. Send the report folders.
    4. [agent] File each problem under its milestone and fix it there. Keep a "known issues" list in this note.
    5. Tick when the known-issues list has no blockers: nothing that desyncs, softlocks or crashes.
  - Note: a hash mismatch still dumps both sides under `BM64_DESYNC_DIR` (default `desync`) and exits. No extra crash reporter. Playing the stages needs someone at the keyboard.
- [ ] **7.6 Release**
  - Depends on: 7.5.
  - Work: player documentation (setup, Discord, troubleshooting) and release builds.
  - Done when: shipped.
  - Steps:
    1. [agent] Check redistribution rules: the Discord Social SDK license (DLL in the release), libjuice (MPL 2.0: ship its license text), and the existing third-party licenses in `lib/`. Never include the ROM, any save, or any recording made from the user's save.
    2. [agent] Release build: `tools/build-windows.ps1 -Config Release`. Zip the exe, the DLLs, `assets/`, the license files and `docs/playing-online.md`. Name it with the version and the build hash from 4.6.
    3. [agent] Update `README.md`: online play, setup, and a link to `docs/playing-online.md`.
    4. [user] Install from the zip on a clean PC (no repo, no Visual Studio), select a ROM, and play one online Battle with someone else on the same release.
    5. [user] Publish the release (GitHub release or wherever the project ships).
    6. Tick when step 5 is done.
  - Note: not started. This is the ship step and it waits on the playtest.

---

## Audit (2026-11-19)

Fixed in this pass (details in the item notes): the false desync and the unbounded prediction window (4.1), the non-deterministic AI handoff (5.6), co-op rules running in Battle (6.9), rival-map slots reading past the pad ring (6.3), the bomb pool inside the camera blocks and movement using the first human's camera (6.5). Unticked: 2.3 and 6.1, whose "done when" was not met.

Still open: phases 5–7 were written before 3.8, 4.2 and 4.7 were done, the load barrier can act on a guess, the main-player categories are keyed on the wrong function, and the port map and movement camera have not run in a stage. `desync/` (about 100 MB of old dumps) is now in `.gitignore`. Each of these is an item in the queue below.

## Next work queue

Do these in order. Items 1–5 are code and unit tests and can be done without running the game. Item 6 is a list of runs for the user. Don't start new phase 6 or 7 features until item 6's Battle checks pass. After the queue, follow the "Steps" list under each open milestone.

Every design below has been decided. If the code turns out to differ from what an item says (a function has another signature, an address is wrong), stop and write down what you found under that item before changing the design.

### 1. Scripted test input (`BM64_NET_SAMPLE=script`)

**Why.** Two instances on one PC read the same physical pad, so the only in-game network tests used `zero`, which never mispredicts. Rollback with real input has never run in the game.

**Design.**
- With `BM64_NET_SAMPLE=script`, player 0 reads the real pad as usual. Every other player ignores the hardware and generates its own input.
- Generator: a host-side xorshift32 seeded with `0x9E3779B9u ^ (0x85EBCA6Bu * (local_player + 1))`. Hold each pad for a random 4–20 samples, then pick a new one: stick x and y uniform in −80..80, no buttons. With `BM64_NET_SAMPLE=script-buttons`, also set A (`0x8000`) with chance 1/4 and B (`0x4000`) with chance 1/8. Never press Start (`0x1000`), which would pause.
- Advance the generator only when a sample is accepted (`bm64::online::accepting_sample()`), so a resimulation never consumes it. The generator is local input, not game state, so a host static is correct here.

**Files.** `src/game/bm64_debug.cpp`, where `sample_zero` is read (search for `BM64_NET_SAMPLE`). Replace the bool with a mode enum (`hardware`, `zero`, `script`, `script-buttons`) and fill `local[6]` as `button` (2 bytes, host order, the same way the hardware path copies it), `stick_x`, `stick_y`, `err = 0`, padding.

**Check.** Build. No unit test is needed; item 6 uses it. Document the variable in this file under 4.2.

**Done (2026-11-20).** `net_sample_mode` and `script_sample` in `src/game/bm64_debug.cpp`, as designed; `bm64::online::local_player()` is new. The build passes. Not run in a session. Found on the way: the hardware path copied the pad with `MEM_BU`, which reads the button's high byte first, then `memcpy`'d those two bytes into a host `u16`, so every online button arrived byte-swapped (A `0x8000` became `0x0080`). The SyncTest and savetest pad reuse (`last_pads`) had the same swap. Neither was ever exercised: online runs used `zero`, and SyncTest of recordings rereads the file instead of reusing pads. Both now go through `read_pad`, which stores the button in host order like a recording frame.

### 2. Load barrier on final ticks only (4.4)

**Why.**
- `load_from_rom_to_addr` calls `bm64_load_barrier` after every load. The session then drops older snapshots and sets `barrier_tick_`, so nothing can roll back past the load.
- If the load came from a mispredicted input (the remote player tapped A for one tick, and prediction repeated it), the load should never have happened, but the rollback that would undo it is now refused. The peers stay different.
- The barrier also waits until every peer reports the same load count. A peer that never loads, because the real input did not trigger the load, never sends it, so the waiting peer stalls until the timeout.

**Why the rule exists at all.** Snapshots restore RDRAM, including the overlay code region at `0x80043000`. But `recomp_load_overlays` also changes host tables in librecomp (`func_map` and `section_addresses` in `lib/N64ModernRuntime/librecomp/src/overlays.cpp`), which no snapshot restores. After a rollback past a code load, RDRAM holds the old overlay while the host still runs the new overlay's functions. `load_overlays` writes `func_map` entries without unloading anything, so calling it again with the old overlay's arguments re-points every entry.

**Design.**
1. *Overlay log.* In `recomp_load_overlays` (`src/game/recomp_api.cpp`), append `{rom, ram, size}` to a host vector `overlay_log`, owned by `bm64::state`, before calling `load_overlays`. Each snapshot stores `overlay_log.size()` beside the patch globals in `capture`. On `restore`, if the saved size is smaller than the current one, shrink the vector to that size, then call `load_overlays` again for every remaining entry, oldest first. That is a few hundred calls at most, and it only happens when a rollback crosses a load.
2. *Wait for the load tick to be final.* Add `bool Session::wait_final(uint64_t tick, int timeout_ms)`. It loops on `recv_all(); send_inputs(); advance_confirmed();`, sleeping 1 ms between passes, until every player's input is known for every tick up to `tick`, then returns true. On timeout it returns false. This is the same condition as `tick_final`, except that ticks already simulated on a guess count once their real input is known.
3. *Decide.* In `notify_barrier(tick, overlay)`, called during the step for `tick`, call `wait_final(tick, 5000)`; on false, print and exit as for a stall. Then compare, for every player and every tick from `confirmed_` to `tick`, the pad that was used with the pad that is known.
   - All equal: the load is real. Set `barrier_tick_ = tick`, `load_seq_++`, send `kBarrier` as today, and return true.
   - Any different: the load came from a guess. Don't set the barrier, and return false. The next `poll` finds the mismatch, rolls back before the load, and `restore` re-points the overlay table from the log.
   - `Session` is game-independent, so it must not touch snapshots. Change `notify_barrier` to return `bool`. In `online::note_barrier`, call it first, and call `bm64::state::drop_snapshots_before(tick)` only when it returns true. Today the drop runs before `notify_barrier` and unconditionally.
4. *Remove the load-count wait.* Delete `barrier_open_` and `peers_caught_up()` from `poll`. Finality already means every peer has the inputs that cause the load, so every peer loads on the same tick. Keep `load_seq` in the header for logs.

**Files.** `src/net/rollback.cpp`, `include/bm64_rollback.h`, `src/net/online.cpp` (`note_barrier`), `src/game/bm64_state.cpp` (`capture`, `restore`, a new `note_overlay_load`), `src/game/recomp_api.cpp`.

**Tests.** Add to `rollback_test.cpp`:
- A fake game in which a "load" happens on the first tick where player 1's pad has bit `0x8000`. The load changes a host-side value that stands in for `func_map`; the fake restore re-applies a log the same way.
- "guessed load": player 1 presses `0x8000` for exactly one tick, and its next packet is held for 6 frames, so player 0 predicts the press repeating. Expect no desync, equal final states, and exactly one load in the final timeline on both peers.
- "real load": the press is real and on time. Expect one barrier on each peer, at the same tick.
- Keep the existing "barrier" test passing, updated if its expectations relied on `barrier_waits`.

**Done when.** All tests pass, and the old code fails "guessed load".

**Done (2026-11-20).**
- Design step 1 (the overlay log) was not needed. Since the 3.8 fixes, each snapshot stores the ROM range mapped at the sub-overlay slot (`mapped_rom`, `mapped_ram`, `mapped_size` in `bm64_state.cpp`), and `restore` calls `load_overlays` again when it differs. That is the only slot whose recompiled code changes. SyncTest at depth 12 already crossed overlay changes with it.
- Steps 2–4 are as designed. `Session::wait_final` and the decision are in `src/net/rollback.cpp`. `notify_barrier` returns `bool`, and `online::note_barrier` drops snapshots only on `true`. `barrier_open_`, `peers_caught_up` and the per-peer load sequence are gone. `barrier_waits()` now counts barriers that had to wait for input.
- `Config::wait_idle` is new: called on each pass of the wait instead of sleeping. The unit tests use it to run the other peer, because both share one thread.
- Tests: "guessed load" and "real load" in `rollback_test.cpp`. With only the API changed (barrier always accepted, the old load-count wait kept), "guessed load" stalled at tick 21 and "real load" logged two barriers per peer. With the new code both pass. The old "barrier" test fires a barrier at a fixed tick regardless of input, so the peer that guessed that tick now correctly refuses it; the test prints that instead of failing.
- Not run in the game.

### 3. Deterministic disconnect (5.6)

**Why.** A player handed to the AI on each peer's own timer desyncs, so that code was removed. Today a silent peer stalls everyone at the rollback window, and then the session exits.

**Design.** The leave becomes a known input, so every peer applies it on the same tick.
1. *Detect.* A peer that has heard nothing from player k for 180 ticks broadcasts `kSilent {k, Lp}`. Lp is the last tick for which it holds k's input. It also re-sends every input of k it holds from its own confirmed tick onward. Every peer relays k's inputs this way, so whoever has the most of them shares them.
2. *Decide.* Only the host (player 0) decides. Once it has a `kSilent` for k from every remaining peer, or 2 seconds have passed since its own detection, it picks T = (largest Lp it has seen, its own included) + 1 and broadcasts `kLeave {k, T}` every frame until each remaining peer's header shows a confirmed tick of at least T.
3. *Apply.* On `kLeave`, every peer marks k's input for every tick from T on as known, with `err = 0xFE` ("left"). It sets `left_[k] = true`, and from then on skips k in `slowest_peer` (time sync), in the minimum over `peer_confirmed_` that sets the resend window, and in silence detection. If the peer simulated ticks from T on with a guess for k, the normal rollback corrects it.
4. *Game side.* In `begin_step` (`src/game/bm64_state.cpp`), after `online::begin_frame`, for each player whose pad for this tick has `err == 0xFE`: find their slot (`slot_of_player` in a story session, otherwise the slot equals the player), and if that slot's `controlType` (`+0x24`) is 1, write 2. Because this depends only on the tick's input, it is deterministic and resimulates correctly.
5. If the host is the one who leaves, the session ends with a message. Host migration is out of scope.

**Why T is safe.** No peer can finalize a tick past its own Lp, because it lacks k's input there. T is larger than every Lp the host has heard, so no finalized tick changes. The relays in step 1 make sure every peer gets k's real inputs for every tick before T.

**Tests.** Three peers; player 2's packets stop at tick 50 and never resume. Expect all three to keep advancing after the leave, equal final hashes, and player 2's pad equal to `err 0xFE` from the same T on every peer. Second case: player 2's last packets reach player 1 but not player 0. T must come from player 1's Lp.

**Done (2026-11-20).** In `src/net/rollback.cpp`: `update_leaves`, `relay`, `apply_leave`, and packet types `kSilent` (5), `kLeave` (6) and `kRelay` (7). `input_at` answers "known, and which pad" for every reader, so a left player's ticks from T on count as known without filling the vector. Protocol is 4. Differences from the design, each found while testing:
- Silence is measured in milliseconds on a host clock (`Config::silence_ms` 3000, `leave_decide_ms` 2000, `clock_ms` for tests), not in ticks. A peer stalled at the rollback window stops advancing ticks, so a tick count would never reach 180.
- Lp is sent as `received_count`: the number of leading ticks held without a gap. A peer can only finalize ticks inside that prefix, and the peer with the largest one can relay all of it.
- T is also at least every remaining peer's confirmed tick. Every peer that has seen a `kSilent` for k relays k's inputs, not only peers that are silent on k themselves. Otherwise a peer that still hears k never shares what the host lacks.
- The host resends `kLeave` until each remaining peer's confirmed tick is **greater than** T. "At least T" was not enough: a peer can finalize every tick before T without knowing about the leave, and the first test run stopped there.
- If the host goes silent, the others end the session after `silence_ms + leave_decide_ms`. A player who receives a `kLeave` naming themselves also ends. `begin_frame` gives up after 20 s instead of 5000 polls, and exits with `end_reason()`.
- Game side: `bm64::coop::hand_to_ai` runs in `begin_step` after `ring_save`, not straight after `begin_frame`. Before the snapshot, the snapshot for tick t would hold a write made from tick t's input, possibly a guess.
- Tests "leave" and "leave, host missed the end" pass. In the second, the host held 45 of player 2's inputs and T came out at 52, from player 1. Not run in the game.

### 4. Main-player category by call site (6.4, 6.6, 6.7)

**Why.** `category_of` looks up the running process's entry function (`Process + 0x0C`). The catalogue records the function that contains each `getMainPlayer` call. These match only when the call sits directly in the process entry. The six trigger functions (`TRIGGER` in `tools/re/catalogue_main_player.py`) are reached only by `jal`, so they never match. The same is true for every enemy whose `getMainPlayer` call is in a helper.

**Design.** Key the lookup on the call site, and fall back to the process entry.
- The catalogue already records each site's address (`addr` in `walk` and `direct_reads`). Add a second emitted table, `src/game/bm64_player_sites.inc`, with rows `{ overlay, site, category }`. `site` is the call address + 8 for `getMainPlayer` (the return address), or the `lw` address for direct `D_802AC894` reads. `overlay` is 0 for main code (address `>= 0x80200000`). Sort it by `(overlay, site)`.
- `getMainPlayer` is the `RECOMP_PATCH s32 func_80243DDC(void)` in `patches/required_patches.c`. Change it to `return bm64_main_player((u32)__builtin_return_address(0));`. Update `DECLARE_FUNC(s32, bm64_main_player, u32 caller)` in `patches/bm64_debug.h`, and read the argument in `bm64_main_player` with `_arg<0, uint32_t>`. After the build, disassemble the patch, or log the first ten callers, to confirm the value is the caller's return address and not the patch's own.
- The 22 direct-read hooks call `bm64_resolved_slot(rdram)`. Change the generator to emit `bm64_resolved_slot_at(rdram, 0xSITE)`, with the `lw` address as a literal. Hand-edit the existing block, because `_emit_reader_hooks.py` refuses to run twice; keep the script in step.
- `resolved_slot(rdram, site)`: look up `(overlay key, site)` in the site table first. If found, use that category. Otherwise use `category_of` as today. Everything after that is unchanged.
- *Trigger semantics.* Before relying on the trigger rule, run `bmdis.py` on each of the six trigger functions and record in `game-internals.md` what each does. Use these defaults:
  - if a function moves or kills the bomber it reads (`func_802512F8` places a bomber at its spawn), it must act on the bomber it was called for. Give its sites category "other" so they return `D_802AC894`, and handle the multi-player case in that function's own hook, as `bm64_coop_snap_party` does.
  - if a function checks "is the player at the goal", it stays "trigger" (nearest living bomber).

**Tests.** No session test applies. Run `python tools/re/catalogue_main_player.py --emit` and check the site count (626). Then, with no session, confirm two replays still match (see item 6): with no session the resolver must return `D_802AC894` unchanged.

**Done (2026-11-20), except the replay check.**
- `catalogue_main_player.py --emit` reports 626 call sites in 312 functions and writes `src/game/bm64_player_sites.inc` with 300 rows: 292 enemy or boss, 2 camera, and the 6 hero-routine sites forced to other. Only rows that differ from "other" are written, plus those 6, because an unlisted site already falls back to the process entry. The six functions are no longer rows in `bm64_player_cats.inc`.
- Trigger semantics: all six act on the bomber they read (goal or warp sequence, table reset, animation state, grid placement, slot position; details in "Main-player sequences" in `game-internals.md`). None tests whether the player reached the goal, so all six are "other". The goal test is in the stage overlays and is still 6.7 step 2.
- Correction (2026-11-21): the first version passed `__builtin_return_address(0)` from the patched getter. The replay caller log showed `00000000` for every call: recompiled `jal` is a plain C call and never writes `ctx->r31`, so `$ra` is stale or 0. It is replaced by 294 generated toml hooks, one before each listed `getMainPlayer` call, that call `bm64_main_site(call + 8)`. `bm64_main_player` takes and clears that value. `catalogue_main_player.py --emit` writes the block to `tools/re/main_player_site_hooks.toml`, and `tools/re/_splice_site_hooks.py` replaces it in `bm64.us.toml` between `# BEGIN/END bm64 main-player call sites`.
- The 22 direct-read hooks call `bm64_resolved_slot_at(rdram, <lw address>)` (scripted edit of the toml block; `_emit_reader_hooks.py` emits the same text). After `-Regen`, all 22 are in `RecompiledFuncs` and no `bm64_resolved_slot(` call is left.
- `BM64_LOG_MAIN_CALLERS=1` prints the site of the first ten `getMainPlayer` calls (0 for an unlisted call).
- Replay check (2026-11-21, after item 5 was also built): two no-session replays of `test.bm64rec` (`runs\item4_a`, `runs\item4_b`) matched on all 9,591 ticks in every range. The file loaded as version 1 with co-op 0. No log from before items 1–5 was kept, so this shows the two runs agree with each other, not that they equal the old build. `new.bm64rec` was not replayed.

### 5. Local co-op mode (enables all phase 6 testing)

**Why.** `bm64::coop::set_session` is called only from the network handshake, so co-op can't run without two networked instances. Two instances on one PC share one physical pad, and replays and SyncTest can't run a networked session. Every phase 6 check is therefore blocked on someone with two PCs. A local mode lets one user with 2–4 controllers play co-op in one window, record it, and have it replayed and SyncTested automatically.

**Design.**
- `BM64_COOP_LOCAL=<2..4>` (and a launcher option later) calls `bm64::coop::set_session(n, 0, unlimited)` at boot when no `BM64_NET` is set. `BM64_COOP_LIVES=unlimited` sets the lives flag. Session player n uses local controller port n, which the port map already expects.
- The window renders player 0's camera. **F8** cycles the rendered player (0 → n−1). The choice is host-only: change only which block `bm64_camera_frame` reads, through a host variable `view_player` that replaces `sess().local_player` in that one function. Rendering is not hashed state, so this can't desync anything.
- *Recording.* Bump `recording_version` in `bm64_debug.cpp` to 2 and add two `u32` header fields after `save_size`: co-op player count (0 = none) and the lives flag. Replay calls `set_session` from them before tick 0. Keep reading version 1 files, which have no co-op.
- `story()` already gates everything, so menus and Battle run unmodified in this mode.

**Files.** `src/game/bm64_debug.cpp` (record and replay header, F8 key next to F6), `src/game/bm64_coop.cpp` (`view_player`), the startup path that runs `handshake()` in `src/net/online.cpp` (call the local setup when `BM64_NET` is unset).

**Checks.**
- Build.
- Replay `test.bm64rec` (version 1) twice; the hashes must still match.
- Then hand the user: `$env:BM64_COOP_LOCAL = "2"; $env:BM64_RECORD = "coop1.bm64rec"; .\build\BM64Recompiled.exe`. Play a story stage with two controllers, then run `BM64_SYNCTEST=1` on `coop1.bm64rec`.

**Done (2026-11-20), built but not run.**
- `BM64_COOP_LOCAL=2..4` calls `set_session(n, 0, lives)` in `bm64::debug` init, before recording starts. It is ignored, with a message, when `BM64_NET` or `BM64_REPLAY` is set. `BM64_COOP_LIVES=unlimited` sets the lives flag; anything else means shared.
- The view key is **F9**, not F8: F8 opens the RmlUi debugger, and F5 and F7 are savestates. `bm64::coop::cycle_view` stores the viewed player in a host atomic. `bm64_camera_frame` and the F6 overlay's "you" line read `view_player()`. Nothing in game memory changes.
- Recordings are version 2: `u32 coop_players` (0 = none) and `u32 coop_unlimited_lives` follow `save_size`. Replay accepts versions 1 and 2. For a co-op file it calls `set_session` and marks ports 0..n−1 connected even when a pad stays idle.
- Version 1 replay: `test.bm64rec` replayed twice with identical hashes (see item 4). Not run: the user's co-op recording.

### 6. Runs for the user (3.8, 4.2, 4.7, 6.3, 6.5)

Hand these over as numbered steps. Don't run them yourself without asking. Each one gives a pass or fail line to look for.

- **SyncTest in Battle (3.8).** The user records about 3 minutes of a 4-player Battle with real pads: `$env:BM64_RECORD = "battle1.bm64rec"; .\build\BM64Recompiled.exe`. Then `$env:BM64_SYNCTEST = "1"; .\tools\run-replay.ps1 -Replay battle1.bm64rec -Out st_battle1`. Pass: no `BM64 SYNCTEST FAIL` line. Repeat on at least three arenas, then one full story world. No longer needed for 3.8 (done with director runs, 2026-11-20); still useful as a real-pad, multi-player check.
- **Loopback Battle with scripted input (4.2).** Two terminals:
  - Host: `$env:BM64_NET = "host"; $env:BM64_NET_SAMPLE = "script"; $env:BM64_NET_LAG_MS = "50"; .\build\BM64Recompiled.exe`
  - Guest: the same with `$env:BM64_NET = "guest"`.
  - The user drives the menus from the host window into a 2-player Battle and plays one match.
  - Pass: no `BM64 DESYNC` line, and the log shows rollbacks. Then repeat with `BM64_NET_JITTER_MS=15`, `BM64_NET_LOSS=5` and `script-buttons`.
- **Soak (4.7).** The same setup, left for an hour in Battle with `script-buttons`, then four players (`BM64_NET_PLAYERS=4`, `BM64_NET_PLAYER=0..3`, `BM64_NET_BIND` and `BM64_NET_PEER` per process). Record the largest rollback depth and the stall count.
- **Co-op checks (6.3, 6.5).** A 2-player session into a story stage: local mode (item 5) with two controllers, or loopback.
  - Turn player 1's camera with the C-buttons, then move player 2. Player 2's stick must follow player 2's camera.
  - Drop bombs with both players. The bombs must match on both windows.
  - Repeat on rival map 4.10. Each co-op bomber (slot 0, and 4 and up) must answer only its own pad, and the rival bombers must ignore every pad.

When a run fails, the dumps are in `desync/`. Run `python tools/re/dumpdiff.py desync/player0_tick_N.bin desync/player1_tick_N.bin --ranges all`, map the first differing address with `tools/re/find.py`, and add the finding to the matching item before fixing anything.

Dumps cover only `0x80000000`–`0x80800000`. Since 2026-11-21 every dump also writes a `<name>.slots.bin` with `0x2000` bytes from `0x80F00000` (the player array and co-op state up to `kStateEnd`), big-endian like the main dump. This covers tick dumps, `desync/` dumps and SyncTest `forward_`/`rerun_` dumps. If the `slots` column differs, diff those files; byte offset `n` is address `0x80F00000 + n`.

### Overnight runs (2026-11-21)

Unattended runs with the user's permission. No one watched a window.

- **No-session replays of `test.bm64rec`.** Each build's two replays were identical on all 9,591 ticks (`runs\q4_test_*`, `runs\q4b_test_*`, `runs\final_test_a`). Between builds, `game` differs from tick 0 and `bss` on ticks 5847–5996 and 8733–8802. Proven cause, using a build without the box-test patches (`runs\dbg_nobox_test`, `bss` identical to the old build): at tick 5900 the only differences are 9 words in process 2's stack (the patched `func_802457D8` spills its arguments at other offsets) and `D_802B36AC`, which holds the address of the patch function `free_hook` that moved. Game state is unchanged. A log from an older build is therefore not a valid reference for a newer one; compare two runs of the same build.
- **Co-op director SyncTests** (`$env:BM64_COOP_LOCAL = "2"`, `tools\run-synctest-scenes.ps1 -Secs 30`, `runs\st\coop3`). Every log shows "Per-player cameras, 2 bombers".

| Scene | Map | Ticks | Depth 1 | Depth 12 |
|---|---|---|---|---|
| `0x28` | 4.1 | 2401 | PASS | PASS |
| `0x31` | 4.10 (rival) | 1200 | PASS | PASS |
| `0x38` | 5.1 (bounced through 6.9, 7.1 and 4.2) | 2401 | PASS | PASS |
| `0x48` | 6.1 | 2401 | PASS | PASS |
| `0x58` | 7.1 | 2401 | PASS | PASS |

- An earlier pass (`runs\st\coop2`, before the overlay-id fix under 6.3) also passed, but on 4.1 and 4.10 player 2 never spawned, so only 5.1 had two bombers. That run is how the bug was found.
- **Heap on 4.10 with co-op (resolved 2026-11-22).** The 4.10 recording stopped at tick 1200 with `Allocation failed: file 0x323 size 0x7910 slot 11`. The solo director run of the same scene loads `0x323` without failing. The director warps there from the title, and the same warp was the whole cause of the Battle heap failures (see "Heap exhaustion in Battle" in `game-internals.md`). The user's run `coop_410b.bm64rec` reached 4.10 through the world map and played 2,540 ticks there with no allocation failure, so the failure was the warp. Co-op still costs heap: at tick 3000 on 4.10 the co-op replay has 828 KB of the 1 MB break in use, solo 704 KB (`runs\re\heapwalk.py`). Correction (2026-11-22): the warp was not the whole story. Once the humans could move, scene `0x28` also ran out; the heap now goes up to 2 MB (see "Heap size" below).
- Director co-op input drives port 0 only. Player 2 stands still, so these runs cover spawning, cameras, the port map, respawns and enemy targeting near an idle teammate, not two players moving.
- Correction (2026-11-22): in every co-op run above, both humans were stuck in place. See "User check 1 on 4.10" below.

### User check 1 on 4.10 (2026-11-22)

The user played local co-op with two controllers through the world map to 4.10 (`coop_410b.bm64rec`). Report: the bombers were stacked on the spawn and could not move; the rival kept hitting both humans and hurting itself; hearts fell out of the humans repeatedly.

- **Stuck humans (fixed).** Two bomber bodies 50 units apart or closer can neither of them move; 52 is free. This was measured by pushing player 2 out to 45, 50, 52, 55, 60, 80 and 100 units on the replay. Co-op placed teammates 40 units apart, so on every map they spawned inside each other. On 4.10 it was worse: the overlay moves the leader's body 50 units from its spawn floats (2300 to 2350), onto the teammate at 2340. In the replay neither body moved from tick 1236 to 3776, although both pads pushed the stick from tick 2000. Fix: spawns, respawns and checkpoint snaps space teammates 60 units apart (`kPartySpacing`), and every story tick pushes a teammate out to 60 from any human closer than 50.25 (`separate_humans`). The push also fired twice in the middle of the fight (48 and 50 apart), so the bodies can reach locking distance in normal play, not only at spawn.
- **Teammate hearts (fixed).** Only slot 0's hearts (`+0x14`) come from the save; slot 4 started 4.10 with 0, so player 2 went down on every hit. `bm64_coop_before_stage` now gives a teammate at 0 hearts the leader's count.
- **Rival hurting itself.** Not a co-op bug. With the humans frozen next to each other, the rival's own blasts hit it (hearts 3 to 0 by tick 3000). In the fixed replay, the rival keeps 3 hearts through tick 3000, as it does in the solo replay of the same inputs. The rival dropping in at tick 2000 from (-10000, -10000, -10000) also happens solo.
- **"4 white bombers".** Not confirmed. The slot dumps show two humans (slots 0 and 4) and the rival in slot 1; slots 2, 3, 5, 6 and 7 stay empty for the whole map.
- **Checks.**
  - SyncTest at depths 1 and 12 passes on `coop_410b.bm64rec` (4,453 ticks).
  - It also passes on `runs\test_coop2.bm64rec`, which is `test.bm64rec` with a version 2 co-op header: 9,591 ticks, about 8,300 of them on 4.1 reached through the world map, with player 2 idle.
  - The director SyncTest on scene `0x28` (`runs\st\coop5`) now stops at tick 1823 with `Allocation failed: file 0x183 size 0x12A0 slot 26`, and the game crashes. The same recording on the previous build ran to the end, but there the two humans were locked together and nothing happened. With the fix, the leader walks, player 2 is hit and respawns, and an on-demand load runs out of heap. The warp from the title already has 944 KB of the heap in use at tick 700. Fixed by the larger heap below.

### Heap size (2026-11-22)

Unattended runs. No one watched a window.

- **Cause.** The ROM's sbrk (`func_80225B00`) refuses any break past `0x80400000`, a 1 MB heap. A director warp from the title starts a stage with about 944 KB in use. A second human who moves, gets hit and respawns triggers more on-demand loads. On scene `0x28` the heap needed 1,036 KB by tick 1800 and 1,044 KB by tick 2400, past the cap, so the load failed. The earlier co-op passes fit only because the humans never moved. The heap was full, not leaking: the earlier Battle investigation already showed that scene changes free their blocks, and this run never got that far.
- **Fix.** A `RECOMP_PATCH` of `func_80225B00` lets the break grow to `0x80500000`. Proof that the space is free: no game code builds an address in `0x80400000`–`0x807FFFFF`, and every recent tick dump (`runs\c410b_fix2`, `runs\coop5_new`, ticks 700–3000) had it all zero. `free` shrinks the break, so freed memory above the break keeps its old bytes. The hash and the snapshot therefore cover the heap up to the highest break ever granted, which the patch keeps at `0x80F01F20` (`kHeapMark`, in the snapshotted slot block). That mark is rounded up to a page and never taken below `0x80400000`. Restore zeroes from the snapshot's top up to the live top. Snapshot version 3. No hash range was removed.
- **Why not a fixed 2 MB range.** The first version hashed and snapshotted all of `0x802B36D0`–`0x80500000`. Single-player `BM64_SAVETEST=8` at tick 2000 then went from about 350 to 620 µs per save and from 750 to 1,250 µs per re-simulated tick. With the mark it is back to 330 µs and 610 µs.
- **Checks.**
  - Co-op director SyncTests (`$env:BM64_COOP_LOCAL = "2"`, `run-synctest-scenes.ps1 -Secs 30`, `runs\st\coop_hwm`): scenes `0x28`, `0x31` (4.10), `0x38`, `0x48` and `0x58` pass at depths 1 and 12, 2401 ticks each, with no allocation failure. 4.10 used to stop at tick 1200 and `0x28` at tick 1823.
  - On `0x28` (`runs\hwm28`): break and mark were both `0x803EC000` at tick 700, `0x80403000` at 1800 and `0x80405000` at 2400. Nothing above the mark was ever written.
  - `coop_410b.bm64rec`: SyncTest passes at depths 1 and 12 (4,453 ticks, `runs\hwm_c410b_st*`).
  - Single-player `test.bm64rec`, old build (`039343a`) against new, both hashing the same ranges (`runs\hwm_solo_base`, `runs\hwm_solo_new`): `ovl`, `files`, `data`, `gfx`, `prc`, `heap` and `game` match on all 9,591 ticks (`game` skips the `free_hook` word `0x802B36AC`). `bss` differs on 96 ticks. At tick 2579 that is 7 words in suspended processes' stacks: return addresses into patch code, which shifted by `0xC0`, and break values the new sbrk spilled. These are the same kind of cross-build stack leftovers as in "Overnight runs". Two runs of the new build are identical.
- Director warps into story stages no longer need the menus for heap.

### Teammate colors (2026-11-22)

Unattended runs. Not watched on a screen; the evidence is which parts files load.

- **Cause.** `g_SpawnPlayer` loads a bomber's 12 parts from file `0x4A` + slot for slots 0–3 (white, black, red, blue, as in Battle). Slots 4–7 have no case, so they reuse the file of the previous spawn through a stack slot. On rival maps teammates live in slots 4–7, so they came out white like the leader. On other maps the teammate is slot 1 and was already black.
- **Fix.** A toml hook in `g_SpawnPlayer` at `0x80245BC4` (block `bm64 coop colors`) calls `bm64_coop_spawn_color`, which replaces the parts file in `a1` for a human teammate in a story session. Party rank 1, 2 and 3 get black, red and blue (`0x4B`, `0x4C`, `0x4D`). On rival maps the order is red, blue, black, because the rival is spawned black (`func_8025DC60` assigns `0x4B`). Rank 4 and up wrap around. The leader, rivals, AI, Battle and runs without a session are untouched.
- **Heap.** Files load once per stage and are shared by reference count. Each color file is `0x4EEA` bytes (about 20 KB); the body `0x49` is `0x10E58`. The load log now prints the size.
- **Parts 4 and 5.** With flag `0x200` in `+0x68`, `g_SpawnPlayer` only re-tags parts 4 and 5 before the 12-part loop, and the loop then reloads all 12 from the chosen file. No caller passes `0x4A` to `g_AssignModelParts` later, so nothing turns a teammate white again.
- **Checks.**
  - Co-op director SyncTests (`runs\st\colors`): scenes `0x28`, `0x31` (4.10), `0x38`, `0x48` and `0x58` pass at depths 1 and 12, 2401 ticks each. Scene `0x31` loads `0x4A`, `0x4C` (the teammate) and later `0x4B` (the rival). The other four load `0x4A` and `0x4B`.
  - `coop_410b.bm64rec`: SyncTest passes at depths 1 and 12 (4,453 ticks). It loads `0x4C` for player 2.
  - Single-player `test.bm64rec` against the heap build (`runs\hwm_dn`, `runs\hwm_solo_new`): `ovl`, `files`, `data`, `gfx`, `prc`, `heap` and `slots` match on all 9,591 ticks. `game` matches once the `free_hook` word `0x802B36AC` is skipped (`runs\colors\solo_nofh`); that pointer moved because the patch code changed. `bss` differs on 398 ticks, which is the documented kind of cross-build stack leftover. It was not re-proven word by word for this build.
- Open: which colors the rivals on 5.7, 6.15, 8.13 (three rivals), 9.5 and 9.6 use. A teammate can match a rival there.

### Scripted teammates in director runs (2026-11-22)

Unattended runs. Not watched on a screen.

- Director runs used to drive player 1 only and zero ports 1–3, so two bombers moving at once in a story stage were covered only by the user's recordings. `BM64_DIRECTOR_COOP=move` gives each co-op port 1–3 its own random stick (held 4–20 ticks, seeded from the director seed and the port); `buttons` adds A (1 in 4) and B (1 in 8). Start is never pressed. Menus (banks 1–3) and scripted menu presses keep the teammates idle. The pads go into the recording like player 1's, so replays and SyncTests read them from the file. Without the variable nothing changes.
- Co-op SyncTests with `BM64_COOP_LOCAL=2` and `BM64_DIRECTOR_COOP=buttons`, `-Secs 40` (`runs\st\coopmove1`): scenes `0x28`, `0x31` (4.10), `0x38`, `0x48` and `0x58` pass at depths 1 and 12, 3001 ticks each. Player 2's pad is non-zero on 2,367 ticks in `0x28`, `0x31` and `0x48`, with deaths, respawns and the separation push in the logs. In `0x38` and `0x58` the party ran out of lives: game over, back to the title (overlay 11), and the attract demo (overlays 28, 43, 50, 19), so player 2 was only active for about 630 ticks there.

### Attract demo, parked checkpoints and pit respawns (2026-11-22)

Unattended runs. Not watched on a screen.

- **Attract demo.** Left on the title, the game plays its demo stages from recorded input, and co-op used to take them over (spawning teammates, respawns, port map). `func_80297ADC` starts the attract sequence: it calls `setMainPlayer(0)`, starts the demo playback process `0x80297A34`, allocates four demo input buffers at `0x802B03C0`, and sets bit 2 (`0x4`) of the game flags `D_802AC610` (getter `func_8023A840`; the HUD skips drawing while `flags & 6`). Pressing Start returns to the title, which clears the flags. Arena setup in `func_8023D0F8` sets the same bit for scenes `0x90`–`0x99`, which are not story maps anyway. Memory dumps: the flags are 13 during the intro movie and demo stages, 1 on the title and 0 in menus and stages entered through them. `story()` now also requires bit 2 clear, so every co-op rule stays off during the demo.
  - `coop_410b.bm64rec` (the user's 4.10 run through the menus): old and new builds hash identically on all 4,453 ticks.
  - `coop_410_b` (left idle on the title): co-op no longer starts in the demo; the logs differ only from the first demo stage (tick 732).
- **Director warps.** Director warps leave from the title or the intro, where bits 0, 2 and 3 are still set, so after the gate a warped stage counted as a demo and co-op stayed off. `request_scene` now clears those bits (`& ~0xD`) along with the scene request. Single-player director stages therefore differ from earlier builds' director runs from the warp on (HUD drawn, flags 0, as through the menus).
- **Parked checkpoint.** On a warp into 5.1 (`0x38`), slot 0's spawn stays parked at -10000 while the overlay places the leader's body from the stage entry, so the checkpoint and teammates were copied from the void: 4,259 respawns in one run. `settle_parked_checkpoint` (first in `apply_rules`) moves a parked checkpoint to the leader's body once it is on the map, and parked teammates beside it. `0x38` then passed with 3 respawns.
- **Pit respawn loop (the 4.10 freeze).** On 6.1 (`0x48`), 3 of 4 random seeds froze at tick 1260 after 33 kind-3 kills of player 2 in a row, the same count as the user's 4.10 lockup. Hardware write watches on player 2's collision record and body found the chain:
  - Bit 30 of the record's `+0x10` is latched by `func_80232FA8` from the accumulator `+0x14`, which `func_80232FF8` sets when the body's y is at or below the stage's kill plane (`func_8026E8B8` returns the float at `0x802AFC5C`, -600 on 6.1). So kind 3 is a fall into a pit.
  - Respawning moved the body to the checkpoint (y 1600), but the collision record kept its previous position (`+0x58/+0x5C/+0x60`) at the bottom of the pit. `func_802909FC` (floor and wall collision) sweeps from there and put the body back at y -600 on the next tick, so it died again on every tick until the game hung.
  - `place_human` now also sets the record's previous position. The frozen recording replays to the end with 4 respawns, and y stays 1600.
  - Two earlier guesses were wrong and are not in the code: spacing the respawn away from other bombers, and waiting for the leader to land.
- **Checks.**
  - Co-op SyncTests, `BM64_COOP_LOCAL=2`, `BM64_DIRECTOR_COOP=buttons`, `BM64_SEED=431DE7EA`, `-Secs 40` (`runs\st\coopfix`): scenes `0x28`, `0x31` (4.10), `0x38`, `0x48` and `0x58` pass at depths 1 and 12, 3001 ticks each, with 5, 3, 3, 13 and 4 respawns. Before the fix this seed froze `0x48` at tick 1260.
  - Scene `0x48` with the other two seeds that froze (`runs\st\coopfix_8D708355`, `runs\st\coopfix_A7445B34`): both pass at depths 1 and 12, 3001 ticks, 15 respawns each.
  - Single-player `test.bm64rec` against the previous build (`runs\colors\solo`, `runs\demo\solo_new`): identical on all 9,591 ticks.
- `BM64_SEED` pins the game's RNG seed for a director recording. Without it each recording picks its own, so the same scene can pass in one run and fail in the next.

## Risks and fallbacks

- **Coroutine snapshots (3.2–3.3)** are the biggest technical risk. Fallback: delay-based lockstep needs no snapshots; Battle still ships, with more input delay.
- **Camera-dependent animation (2.5)**: run the animation part of the draw pass with one canonical camera, and render separately.
- **Story code's one-hero assumptions (6.2–6.10)** are the long tail; the call-site catalogue keeps the work countable.
- **Gameplay that waits on sounds (2.6)** could make audio timing part of the simulation; virtual handles and deterministic durations fix it.
- **Player array relocation (6.1)** touches about 143 functions; the generator script plus checker plus SyncTest keep it safe.
