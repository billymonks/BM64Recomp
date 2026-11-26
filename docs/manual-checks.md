# Manual checks

Hands-on checks for the top of "Next work queue" in [online-play-plan.md](online-play-plan.md). Each one records your inputs and writes a log, so a problem can be replayed and investigated afterwards without you having to play it again.

## How the commands work

Every command below is one line you can run as it is, in any PowerShell window. Each one:
- moves into the repo folder,
- clears every `BM64_*` variable left over from an earlier run,
- sets only that check's variables,
- starts the game with its log redirected to files in the repo folder.

So the order you click them in, or what you ran before, doesn't matter. Each check writes its own files; running the same check again overwrites only that check's files.

## Before every check

- Close any open copy of the game.
- Checks 1–3 need two controllers connected. Controller 1 is player 1 (white) and controller 2 is player 2 (black).
- Keys: **F9** switches the window between players' cameras, and **F6** toggles the debug overlay (tick, map number and so on).

When a check is done, close the game and report the pass or fail line plus anything odd.

---

## Round 1 results (2026-11-25)

| Check | Result | What happened next |
|---|---|---|
| 1. Camera controls on 4.1 | Pass, except billboards | Flat objects (pots and so on) face player 1's camera while the window shows player 2's. Still open. |
| 2. Player 2 clears 4.1 | Pass, with problems | Objects and enemies near player 2 were hidden or missing; during the orb cutscene player 2 was hit on every tick (the repeated sound) and lost the party's lives; afterwards player 2 was put at the start. All three fixed. |
| 3. 4.10 rival turns to player 2 | Pass | The camera only followed player 1, and F9 showed the same camera. Fixed. |
| 4. Loopback online Battle | Fail | The guest gave up after 5 seconds because the host's game was still starting. Fixed; the host's exit error when closing the window is fixed too. |

The details are in "Manual checks 1–4" in [online-play-plan.md](online-play-plan.md).

---

## Round 2 results (2026-11-26)

| Check | Result | What happened next |
|---|---|---|
| 2b. 4.1 played by player 2 | Objects fine; billboards, enemies, orb lights and camera height not | Billboards fixed: flat objects now face the camera the window shows. After an orb cutscene, player 1 now stays where they stood. Still open: enemies only appear near player 1, player 2 isn't frozen on a light, and player 2's camera sometimes goes high. |
| 3b. 4.10 camera | Camera stuck for player 2 | Fixed: player 2's camera follows player 2 during the fight. |
| 4b. Loopback Battle | Both copies closed on "single battle" | Fixed: a rollback that stopped a menu sound crashed the host. Unattended runs now go into the arena and play without a desync. |

The details are in "Manual checks round 2" in [online-play-plan.md](online-play-plan.md).

---

## Round 3

Same checks with new file names, so the earlier runs stay.

### Check 2c: map 4.1 again

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_COOP_LOCAL = "2"; $env:BM64_RECORD = "coop_goal41_3.bm64rec"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput coop_goal41_3_log.txt -RedirectStandardError coop_goal41_3_err.txt
```

1. On map 4.1, press **F9** to view player 2's camera, and turn it with player 2's C-buttons.
2. Let player 2 step on an orb light while player 1 stands somewhere else.

**Pass:**
- the pots and other flat objects face player 2's camera as it turns;
- after the orb cutscene, player 1 is where they stood and player 2 is at the light.

Also worth noting: which enemies fail to appear near player 2 when player 1 is far away (expected not to yet; say which ones and where), and whether the camera also tilts up in the same spots in single player.

### Check 3c: the 4.10 camera

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_COOP_LOCAL = "2"; $env:BM64_RECORD = "coop_rival_3.bm64rec"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput coop_rival_3_log.txt -RedirectStandardError coop_rival_3_err.txt
```

**Pass:** during the rival fight, **F9** shows player 2's camera following player 2, with the fight's zoom.

### Check 4c: a full online Battle match

Host:

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_AUTOSTART = "1"; $env:BM64_NET = "host"; $env:BM64_NET_PLAYERS = "2"; $env:BM64_NET_SAMPLE = "script-buttons"; $env:BM64_NET_LAG_MS = "50"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput net_host4_log.txt -RedirectStandardError net_host4_err.txt
```

Guest:

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_AUTOSTART = "1"; $env:BM64_NET = "guest"; $env:BM64_NET_PLAYERS = "2"; $env:BM64_NET_SAMPLE = "script-buttons"; $env:BM64_NET_LAG_MS = "50"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput net_guest4_log.txt -RedirectStandardError net_guest4_err.txt
```

In the host window, go into a 2-player Battle and play one whole match through to the results screen.

**Pass:** no `BM64 DESYNC`, no crash, and the F6 overlay's rollback count goes up.

---

## Round 2 (done)

Run these on the current build. The recordings and logs are named `*2` so round 1's stay.

### Check 2b: map 4.1 with player 2 doing the work

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_COOP_LOCAL = "2"; $env:BM64_RECORD = "coop_goal41_2.bm64rec"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput coop_goal41_2_log.txt -RedirectStandardError coop_goal41_2_err.txt
```

1. Enter map 4.1 (F6 shows `overlay 4.1`). Leave player 1 at the start the whole time.
2. Press **F9** to follow player 2, and play the stage with player 2: the four orbs, the second area, the exit.

**Pass:**
- enemies, the orbs' glow and every object near player 2 are visible, with no invisible walls;
- during the cutscene after the fourth orb there is no repeated hit sound, and the lives counter doesn't drop;
- after the cutscene player 2 is back where they stood, not at the start;
- player 2 reaching the exit clears the stage.

### Check 3b: the 4.10 camera

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_COOP_LOCAL = "2"; $env:BM64_RECORD = "coop_rival_2.bm64rec"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput coop_rival_2_log.txt -RedirectStandardError coop_rival_2_err.txt
```

1. Go to map 4.10 and start the rival fight.
2. Press **F9** to view player 2's camera, and walk player 2 away from player 1.

**Pass:** F9 shows a different camera for each player, and player 2's camera follows player 2 during the fight, so player 2 can't walk off screen.

### Check 4b: two copies connected on this PC, playing Battle

Two copies of the game talk over loopback with 50 ms of fake lag. You play player 1 in the host window; the guest plays player 2 with random scripted input. One controller is enough. Run the host line, then the guest line; the guest can now start up to about 30 seconds later.

Host:

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_AUTOSTART = "1"; $env:BM64_NET = "host"; $env:BM64_NET_PLAYERS = "2"; $env:BM64_NET_SAMPLE = "script"; $env:BM64_NET_LAG_MS = "50"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput net_host2_log.txt -RedirectStandardError net_host2_err.txt
```

Guest:

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_AUTOSTART = "1"; $env:BM64_NET = "guest"; $env:BM64_NET_PLAYERS = "2"; $env:BM64_NET_SAMPLE = "script"; $env:BM64_NET_LAG_MS = "50"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput net_guest2_log.txt -RedirectStandardError net_guest2_err.txt
```

1. The host window stays black until the guest connects; that's the handshake waiting. Both windows should then reach the title screen.
2. In the **host** window, go through the menus into a 2-player Battle and play one full match. Player 2 wanders randomly.
3. Press **F6** in either window: the overlay shows `net p0/2 delay 2 rollbacks N silent 0`.

**Pass:**
- neither copy exits with a `BM64 DESYNC` line (the dumps would go to `desync\`);
- the rollbacks counter goes up during the match;
- closing the windows afterwards shows no error box.

If it fails, the logs now say how long after the handshake each copy's first frame and first packet came (`First frame ... ms`, `First packet from player ... ms`).

Then the same with jitter, packet loss and random A/B presses (close both windows first):

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_AUTOSTART = "1"; $env:BM64_NET = "host"; $env:BM64_NET_PLAYERS = "2"; $env:BM64_NET_SAMPLE = "script-buttons"; $env:BM64_NET_LAG_MS = "50"; $env:BM64_NET_JITTER_MS = "15"; $env:BM64_NET_LOSS = "5"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput net_host3_log.txt -RedirectStandardError net_host3_err.txt
```

```powershell
Set-Location C:\Users\willi\Documents\GitHub\BM64Recomp\bm64recomp; Get-ChildItem Env:BM64_* | Remove-Item; $env:BM64_AUTOSTART = "1"; $env:BM64_NET = "guest"; $env:BM64_NET_PLAYERS = "2"; $env:BM64_NET_SAMPLE = "script-buttons"; $env:BM64_NET_LAG_MS = "50"; $env:BM64_NET_JITTER_MS = "15"; $env:BM64_NET_LOSS = "5"; Start-Process .\build\BM64Recompiled.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput net_guest3_log.txt -RedirectStandardError net_guest3_err.txt
```

Things worth noting in either run:
- player 2's random stick moving menu cursors, or joining or leaving the Battle setup on its own;
- the Battle setup offering only one human player;
- stutter, or the windows freezing for a moment.

Once both Battle runs pass, the next step is the one-hour soak (milestone 4.7), with the same setup left running.

### Billboards (done)

The round 2 screenshots showed the pots turning edge-on as player 2's camera turned. Fixed for round 3; check 2c covers it.
