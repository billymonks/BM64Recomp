<#
.SYNOPSIS
  One fresh boot per scene: for each scene, records a BM64_DIRECTOR session that warps to that scene only,
  then runs SyncTest on it (tools\run-synctest-coverage.ps1). Warping out of a running stage into another
  ran out of game heap after a few stages (2026-11-20), so each recording visits a single scene.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run-synctest-scenes.ps1 -Scenes "90,91" -Secs 30 -Out runs\st\cov

  Writes <Out>\<scene>\ per scene and appends each scene's summary to <Out>\results.txt.

  -ArenaMenus reaches Battle setup through the title and main menu instead of warping there. Warping
  from the title left too little game heap for arenas 2, 3, 5, 7 and 9 (2026-11-20). Use it for 90-99.
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenes,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Secs = 30,
    [switch]$ArenaMenus,
    [string]$Exe = "build\BM64Recompiled.exe"
)

# Start on the title, Start again for the main menu, stick down to Battle, Start. The director's A
# pulses then take the defaults through Battle setup (overlay 13) by tick 1100.
$menuArgs = if ($ArenaMenus) { @("-Menu", "560:1000,820:1000,960:0:0:-80,1020:1000", "-BootTicks", "1100") } else { @() }

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($scene in ($Scenes -split ",")) {
    $dir = Join-Path $Out $scene
    try {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-synctest-coverage.ps1") -Scenes $scene -Secs $Secs -Out $dir -Exe $Exe @menuArgs | Out-Null
    }
    catch {
        Write-Warning "Scene ${scene}: $_"
    }
    $summary = Join-Path $dir "summary.txt"
    $text = if (Test-Path $summary) { Get-Content $summary } else { @("no summary") }
    Add-Content (Join-Path $Out "results.txt") (@("=== scene $scene") + $text)
}
