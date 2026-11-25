# Development notes

Practical notes for building, testing and debugging this port. Game facts are in [game-internals.md](game-internals.md); the roadmap and the current work queue are in [online-play-plan.md](online-play-plan.md) (start at "Next work queue").

## Building

- `tools/build-windows.ps1` builds everything (see `BUILDING.md`). Add `-Regen` after any change to `bm64.us.toml`, so the recompiled C is regenerated.
- A change under `patches/` takes two builds to land (an upstream quirk of the patch build).
- After restoring a file with `Copy-Item`, set its `LastWriteTime` to now. `Copy-Item` keeps the old timestamp, and the patch build then skips the file.
- Close any running `build\BM64Recompiled.exe` before building; the link step can't replace an open exe. To test while a game is open, copy the exe and pass the copy with `-Exe` to the run scripts.

## `bm64.us.toml`

- About 9 MB. Edit it only with scripts, never by hand in an editor. The working copy is CRLF while the index is LF, so a script must read and write without changing line endings (Python `open(..., newline='')`, or normalise to LF and restore CRLF).
- Hand-written hooks live in named `# BEGIN ...` / `# END ...` blocks, so a script can replace a whole block. Existing splice scripts are in `tools/re/` (`_splice_site_hooks.py`, `_emit_reader_hooks.py`, `catalogue_main_player.py --emit`).
- A `[[patches.hook]]` with `before_vram` at a `jal` is emitted before the call.
- A hook on a function that also has a `RECOMP_PATCH` does nothing: the patch replaces the whole function.

## Patches and host functions

- A recompiled `jal` is a plain C call and never writes `ctx->r31`. So `$ra` and `__builtin_return_address` are useless for finding a caller; use toml hooks at the call sites instead.
- A host function called from patches needs `DECLARE_FUNC` in a `patches/*.h`, a dummy address in `patches/syms.ld`, and an `extern "C" void name(uint8_t* rdram, recomp_context* ctx)` in `src/`. Dummy slots run upwards from `0x8F000000`; the last used is `0x8F00019C` (`bm64_coop_process_resume`), so the next free one is `0x8F0001A0`.

## Co-op state

- Co-op keeps its own state in RDRAM from `0x80F00000`: the player array (8 slots, stride `0xC8`, `controlType` at `+0x24`, object pointer at `+0x28`), then movement, HUD copies, process tables, category overrides, per-player camera blocks, rules, the heap high-water mark and camera locals. The layout and its end (`kStateEnd`) are in `include/bm64_coop.h`; that header is the source of truth.
- That range is inside snapshots and hashed as the `slots` range. Every dump also writes a `<name>.slots.bin` (`0x2000` bytes from `0x80F00000`, big-endian).
- The game heap may grow to `0x80500000`. Snapshots and hashes cover it up to the high-water mark.

## Running tests

- Recordings: `BM64_RECORD=<file>.bm64rec` records, `BM64_REPLAY=<file>` replays. Format version 2: magic, version, seed, save size, co-op player count, co-op unlimited-lives flag, the save image, then 24 bytes per input frame (4 ports × 6: buttons u16, stick x, stick y, error byte, director command on port 0). Version 1 files (no co-op fields) still replay. The recorder flushes every frame.
- `tools/run-replay.ps1 -Replay <file> -Out <dir>` replays unattended and writes `hashes.csv`. `-DumpTicks`, `-Ranges`, `-Exe`, `-TimeoutSec`.
- `tools/run-determinism.ps1` replays one recording twice and compares hash logs and the save.
- `tools/run-director.ps1 -Scenes <hex,...> -Out <dir>` records unattended director runs (scene warps with scripted input).
- `tools/run-synctest-scenes.ps1 -Scenes "28,31,38,48,58" -Out <dir> [-Secs 40] [-Exe <path>]` records each scene and runs SyncTest at depths 1 and 12. For co-op set `BM64_COOP_LOCAL=2` and a teammate script (`BM64_DIRECTOR_COOP=move|buttons|camera`). Always pin the seed (`BM64_SEED=431DE7EA` in the recorded runs); without it each recording picks its own and a bug can pass by luck. Scene `0x31` is rival map 4.10.
- `tools/run-synctest-coverage.ps1` does the same over many scenes with `-Depths`.
- A SyncTest failure prints `BM64 SYNCTEST FAIL`. Never fix one by removing a hash range without proof that the bytes are not game state; known build-to-build differences (idle-process stack leftovers, a stored patch-code pointer) are documented in `online-play-plan.md`.
- Compare dumps with `python tools/re/dumpdiff.py a.bin b.bin --ranges all`, then map addresses with `tools/re/find.py`.
- A manual run should redirect output with `Start-Process ... -RedirectStandardOutput log.txt -RedirectStandardError err.txt`. PowerShell's `>` redirection left the log empty in practice.

## Environment variables

Read in `src/` (check there for exact semantics):

- Session and input: `BM64_AUTOSTART`, `BM64_RECORD`, `BM64_REPLAY`, `BM64_EXIT_TICK`, `BM64_EXIT_ON_REPLAY_END`, `BM64_SEED`, `BM64_ROM`, `BM64_DISABLE_CHEATS`, `BM64_DEBUG_OVERLAY`.
- Save sandbox: `BM64_SANDBOX`, `BM64_SANDBOX_IMAGE`, `BM64_SANDBOX_EXPORT`, `BM64_SAVETEST`, `BM64_SAVETEST_START`.
- Hashing and dumps: `BM64_HASH_LOG`, `BM64_HASH_RANGES`, `BM64_HASH_DEAD_STACKS`, `BM64_DUMP_DIR`, `BM64_DUMP_TICKS`, `BM64_SYNCTEST`, `BM64_SYNCTEST_DUMP`, `BM64_INJECT_DESYNC`, `BM64_DESYNC_DIR`, `BM64_HEAP_END`, `BM64_HEAP_MARK`, `BM64_SLOT_PROBE`.
- Director: `BM64_DIRECTOR`, `BM64_DIRECTOR_BOOT`, `BM64_DIRECTOR_SECS`, `BM64_DIRECTOR_SEED`, `BM64_DIRECTOR_MENU`, `BM64_DIRECTOR_COOP`.
- Co-op: `BM64_COOP_LOCAL` (number of local players), `BM64_COOP_LIVES`, `BM64_COOP_OVERRIDE`, `BM64_VIEW_PLAYER`, `BM64_COOP_TRACE`, `BM64_CAM_TRACE`, `BM64_CAMERA_BIAS`, `BM64_LOG_MAIN_CALLERS`. F9 cycles which player's camera the window shows; F6 toggles the debug overlay.
- Network: `BM64_NET` (`host`/`guest`), `BM64_NET_PLAYERS`, `BM64_NET_PLAYER`, `BM64_NET_BIND`, `BM64_NET_PEER`, `BM64_NET_SAMPLE` (`zero`, `script`, `script-buttons`), `BM64_NET_LAG_MS`, `BM64_NET_JITTER_MS`, `BM64_NET_LOSS`, `BM64_NET_DELAY`, `BM64_NET_ROLLBACK`, `BM64_NET_LIVES`, `BM64_TURN_HOST`, `BM64_DISCORD_APP_ID`.

## Debugging crashes and hangs

- Windows logs crashes and hangs in the Application event log: `Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=(Get-Date).AddHours(-1)} | ? Message -match 'BM64Recompiled'`. "Application Error" gives the fault offset; "Application Hang" means the window stopped responding.
- Map a fault offset to a function and line with `build\BM64Recompiled.pdb`: load the exe at base `0x140000000` with `dbghelp` (`SymLoadModuleExW`, `SymFromAddr`, `SymGetLineFromAddr64`; a short Python `ctypes` script works). Recompiled functions resolve to a line in `RecompiledFuncs/funcs_N.c`, whose comments carry the original MIPS addresses.
- A hang that only happens live: ask for a Task Manager memory dump (Details, right-click, Create memory dump file) before the window is closed.
- To reproduce a live report, replay its recording with `BM64_EXIT_TICK` past the end (input goes live and idle), or append scripted frames to a copy of the recording.

## Known open issues

- Director warps to map 9.6 hang right after loading, even single-player; 9.6 needs a real playthrough.
- One replay crashed once while loading 4.10 and could not be reproduced.
- Players 5–8 repeat the four Battle colors.
- Pending manual checks are listed at the top of "Next work queue" in `online-play-plan.md`.
