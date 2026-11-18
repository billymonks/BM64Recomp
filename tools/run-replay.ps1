<#
.SYNOPSIS
  Runs an unattended replay: skips the launcher, replays a recording, logs hashes, and exits when the replay ends.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run-replay.ps1 -Replay test.bm64rec -Out runs\a -DumpTicks 0,1,1702
  powershell -ExecutionPolicy Bypass -File tools\run-replay.ps1 -Replay test.bm64rec -Out runs\legacy -Ranges legacy
  powershell -ExecutionPolicy Bypass -File tools\run-replay.ps1 -Replay new.bm64rec -Out runs\overlay -Overlay

  Writes <Out>\hashes.csv, <Out>\stdout.txt and <Out>\dumps\tick_<n>.bin. -Ranges "legacy" uses the original Phase 0
  ranges (ovl, bss, main, heap) and hashes dead stack memory, so the log can be compared with logs made before
  the range list changed.
#>
param(
    [Parameter(Mandatory = $true)][string]$Replay,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$DumpTicks = "",
    [string]$Ranges = "",
    [string]$Exe = "build\BM64Recompiled.exe",
    [switch]$Overlay,
    [int]$TimeoutSec = 600
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $Repo
try {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $outFull = (Resolve-Path $Out).Path
    $env:BM64_HASH_DEAD_STACKS = ""
    if ($Ranges -eq "legacy") {
        $Ranges = "ovl:80043000-80063000,bss:80063000-800BEA60,main:8029F5E0-802B36D0,heap:802B36D0-80400000"
        $env:BM64_HASH_DEAD_STACKS = "1"
    }

    $env:BM64_AUTOSTART = "1"
    $env:BM64_EXIT_ON_REPLAY_END = "1"
    $env:BM64_REPLAY = (Resolve-Path $Replay).Path
    $env:BM64_HASH_LOG = Join-Path $outFull "hashes.csv"
    $env:BM64_DUMP_TICKS = $DumpTicks
    $env:BM64_DUMP_DIR = Join-Path $outFull "dumps"
    $env:BM64_HASH_RANGES = $Ranges
    $env:BM64_DEBUG_OVERLAY = if ($Overlay) { "1" } else { "" }
    Remove-Item Env:BM64_RECORD -ErrorAction SilentlyContinue

    $p = Start-Process -FilePath (Resolve-Path $Exe).Path -WorkingDirectory $Repo -PassThru `
        -RedirectStandardOutput (Join-Path $outFull "stdout.txt") -RedirectStandardError (Join-Path $outFull "stderr.txt")
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Write-Warning "Replay still running after $TimeoutSec s; killing it"
        $p.Kill()
        $p.WaitForExit()
        exit 1
    }
    Write-Host "Replay finished (exit code $($p.ExitCode)); log: $env:BM64_HASH_LOG"
}
finally {
    foreach ($v in "BM64_AUTOSTART", "BM64_EXIT_ON_REPLAY_END", "BM64_REPLAY", "BM64_HASH_LOG", "BM64_DUMP_TICKS", "BM64_DUMP_DIR", "BM64_HASH_RANGES", "BM64_HASH_DEAD_STACKS", "BM64_DEBUG_OVERLAY") {
        Remove-Item "Env:$v" -ErrorAction SilentlyContinue
    }
    Pop-Location
}
