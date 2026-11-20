<#
.SYNOPSIS
  One fresh boot per scene: for each scene, records a BM64_DIRECTOR session that warps to that scene only,
  then runs SyncTest on it (tools\run-synctest-coverage.ps1). Warping out of a running stage into another
  ran out of game heap after a few stages (2026-11-20), so each recording visits a single scene.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run-synctest-scenes.ps1 -Scenes "90,91" -Secs 30 -Out runs\st\cov

  Writes <Out>\<scene>\ per scene and appends each scene's summary to <Out>\results.txt.
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenes,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Secs = 30
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($scene in ($Scenes -split ",")) {
    $dir = Join-Path $Out $scene
    try {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-synctest-coverage.ps1") -Scenes $scene -Secs $Secs -Out $dir | Out-Null
    }
    catch {
        Write-Warning "Scene ${scene}: $_"
    }
    $summary = Join-Path $dir "summary.txt"
    $text = if (Test-Path $summary) { Get-Content $summary } else { @("no summary") }
    Add-Content (Join-Path $Out "results.txt") (@("=== scene $scene") + $text)
}
