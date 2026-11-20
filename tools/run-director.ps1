<#
.SYNOPSIS
  Records an unattended coverage session: skips the launcher, sandboxes the save, and lets BM64_DIRECTOR
  warp through a list of scenes with seeded random input on port 0. Exits when the last scene's time is up.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run-director.ps1 -Scenes "90,91,92" -Secs 60 -Out runs\st\battle_a

  Writes <Out>\rec.bm64rec, <Out>\hashes.csv (overlay id per tick) and <Out>\stdout.txt. Scene numbers are hex
  (see "Scenes" in docs/game-internals.md). The recording embeds the save image, so keep it out of releases.
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenes,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Secs = 60,
    [int]$BootTicks = 600,
    [string]$Seed = "",
    [string]$Menu = "",
    [string]$Exe = "build\BM64Recompiled.exe",
    [int]$TimeoutSec = 0
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $Repo
try {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $outFull = (Resolve-Path $Out).Path
    $count = ($Scenes -split ",").Count
    if ($TimeoutSec -le 0) {
        $TimeoutSec = [int](($BootTicks / 60) + $count * $Secs * 1.5 + 120)
    }

    Remove-Item Env:BM64_REPLAY -ErrorAction SilentlyContinue
    $env:BM64_AUTOSTART = "1"
    $env:BM64_SANDBOX = "1"
    $env:BM64_RECORD = Join-Path $outFull "rec.bm64rec"
    $env:BM64_HASH_LOG = Join-Path $outFull "hashes.csv"
    $env:BM64_DIRECTOR = $Scenes
    $env:BM64_DIRECTOR_SECS = "$Secs"
    $env:BM64_DIRECTOR_BOOT = "$BootTicks"
    $env:BM64_DIRECTOR_SEED = $Seed
    $env:BM64_DIRECTOR_MENU = $Menu

    $p = Start-Process -FilePath (Resolve-Path $Exe).Path -WorkingDirectory $Repo -PassThru `
        -RedirectStandardOutput (Join-Path $outFull "stdout.txt") -RedirectStandardError (Join-Path $outFull "stderr.txt")
    $exited = $p.WaitForExit($TimeoutSec * 1000)
    if (-not $exited) {
        Write-Warning "Recording still running after $TimeoutSec s; killing it"
        $p.Kill()
        $p.WaitForExit()
        $script:RunExit = 1
    }
    else {
        $null = $p.WaitForExit()
        $script:RunExit = if ($null -eq $p.ExitCode) { 0 } else { $p.ExitCode }
    }
    Write-Host "Recording finished (exit code $script:RunExit): $env:BM64_RECORD"
}
finally {
    foreach ($v in "BM64_AUTOSTART", "BM64_SANDBOX", "BM64_RECORD", "BM64_HASH_LOG", "BM64_DIRECTOR", "BM64_DIRECTOR_SECS", "BM64_DIRECTOR_BOOT", "BM64_DIRECTOR_SEED", "BM64_DIRECTOR_MENU") {
        Remove-Item "Env:$v" -ErrorAction SilentlyContinue
    }
    Pop-Location
}
if ($null -eq $script:RunExit) { $script:RunExit = 1 }
exit $script:RunExit
