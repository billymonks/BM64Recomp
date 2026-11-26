<#
.SYNOPSIS
  Runs an unattended online session on this PC: 2-4 copies of the game connected over loopback, every
  pad scripted, until -Ticks. Then compares each guest's hash log with the host's, tick by tick.

.DESCRIPTION
  Player 0 (the host) reads its real pad, so leave the controllers alone, or the menus will move,
  unless -HostMenu scripts it; the other players use BM64_NET_SAMPLE (script-buttons by default). With nobody at the host, the session
  runs the title, the intro and the attract demo, which exercises rollback, loads and screen grabs.

  Writes <Out>\p<n>.txt, p<n>_err.txt and p<n>_hashes.csv per player, desync dumps in <Out>\desync, and
  <Out>\summary.txt. Check that nobody is using the game build before starting.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run-loopback.ps1 -Out runs\net\two -Players 2 -Jitter 15 -Loss 5
#>
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Players = 2,
    [int]$Ticks = 7200,
    [int]$LagMs = 50,
    [int]$Jitter = 0,
    [int]$Loss = 0,
    [string]$Sample = "script-buttons",
    # The host's pad as tick:hexbuttons[:x:y] presses (BM64_NET_HOST_MENU), e.g. into Battle setup:
    # "560:1000,820:1000,960:0:0:-80,1020:1000". -HostPulse then taps A every 30 ticks.
    [string]$HostMenu = "",
    [switch]$HostPulse,
    [string]$Exe = "build\BM64Recompiled.exe"
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $Repo
try {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $outFull = (Resolve-Path $Out).Path
    $exePath = (Resolve-Path $Exe).Path
    $procs = @()
    for ($p = 0; $p -lt $Players; $p++) {
        Get-ChildItem Env:BM64_* | Remove-Item
        $env:BM64_AUTOSTART = "1"
        $env:BM64_NET_PLAYERS = "$Players"
        $env:BM64_NET_PLAYER = "$p"
        $env:BM64_NET_SAMPLE = $Sample
        $env:BM64_NET_LAG_MS = "$LagMs"
        if ($Jitter -gt 0) { $env:BM64_NET_JITTER_MS = "$Jitter" }
        if ($Loss -gt 0) { $env:BM64_NET_LOSS = "$Loss" }
        $env:BM64_EXIT_TICK = "$Ticks"
        $env:BM64_DESYNC_DIR = Join-Path $outFull "desync"
        $env:BM64_HASH_LOG = Join-Path $outFull "p${p}_hashes.csv"
        $env:BM64_NET = if ($p -eq 0) { "host" } else { "guest" }
        if ($p -eq 0 -and $HostMenu -ne "") { $env:BM64_NET_HOST_MENU = $HostMenu }
        if ($p -eq 0 -and $HostPulse) { $env:BM64_NET_HOST_PULSE = "1" }
        if ($Players -gt 2) {
            $env:BM64_NET_BIND = "127.0.0.1:$(27015 + $p)"
            $peers = @()
            for ($q = 0; $q -lt $Players; $q++) { if ($q -ne $p) { $peers += "127.0.0.1:$(27015 + $q)" } }
            $env:BM64_NET_PEER = ($peers -join ",")
        }
        $procs += Start-Process -FilePath $exePath -WorkingDirectory $Repo -PassThru `
            -RedirectStandardOutput (Join-Path $outFull "p$p.txt") -RedirectStandardError (Join-Path $outFull "p${p}_err.txt")
        # The host first; guests a little later, as people would join.
        Start-Sleep -Seconds $(if ($p -eq 0) { 8 } else { 2 })
    }
    $timeoutMs = [int](($Ticks / 60.0) * 3 + 120) * 1000
    foreach ($pr in $procs) {
        $null = $pr.WaitForExit($timeoutMs)
        if (-not $pr.HasExited) { $pr.Kill() }
    }
    Get-ChildItem Env:BM64_* | Remove-Item

    $lines = @("$Players players, $Ticks ticks, lag $LagMs ms, jitter $Jitter ms, loss $Loss %, $Sample")
    for ($p = 0; $p -lt $Players; $p++) {
        $lines += "-- p$p"
        $lines += Select-String -Path (Join-Path $outFull "p$p.txt"), (Join-Path $outFull "p${p}_err.txt") `
            -Pattern "First frame|First packet|DESYNC|left|stopped|Stalled|Timed out|Exiting|table full|Could not" |
            Select-Object -First 8 | ForEach-Object { $_.Line }
        $rollbacks = (Select-String -Path (Join-Path $outFull "p$p.txt") -Pattern "Rollback to tick" | Measure-Object).Count
        $lines += "rollbacks: $rollbacks, exit code $($procs[$p].ExitCode)"
        $lines += Select-String -Path (Join-Path $outFull "p$p.txt") -Pattern "overlay drawn" | Select-Object -Last 1 |
            ForEach-Object { "last overlay line: " + ($_.Line -replace '.*line 0 "', '') }
    }
    for ($p = 1; $p -lt $Players; $p++) {
        $lines += "-- hashes p0 vs p$p, by tick"
        $lines += & python (Join-Path $PSScriptRoot "re\compare_hashes.py") --by-tick (Join-Path $outFull "p0_hashes.csv") (Join-Path $outFull "p${p}_hashes.csv") 2>&1
    }
    $lines | Set-Content (Join-Path $outFull "summary.txt")
    $lines | Out-Host
}
finally {
    Get-ChildItem Env:BM64_* -ErrorAction SilentlyContinue | Remove-Item
    Pop-Location
}
